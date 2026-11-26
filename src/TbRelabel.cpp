//  Correction des résultats d'un .bin (format bullet) par les tables Syzygy
//
//  appel : Zangdar tbrelabel <data.vf> <data.bin> <tables> [--partie]
//      data.vf  : parties du datagen, en viriformat
//      data.bin : positions tirées de data.vf par « bullet-utils viribinpack splat », avec exactement le filtre
//                 max_eval = 2500, filter_check, filter_tactical (sinon : erreur de désalignement)
//      tables   : répertoires Syzygy séparés par « : », jusqu'à 6 pièces
//      --partie : les positions à plus de 6 pièces d'une partie qui entre en table
//                 prennent le résultat de sa 1re position en table
//
//  Chaque partie du .vf est rejouée pour retrouver les positions que splat a gardées dans le .bin.
//  Le .bin est modifié sur place ; les compteurs sont écrits en JSON sur stdout.

#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "TbRelabel.h"
#include "Board.h"
#include "DataGen.h"
#include "MoveList.h"
#include "pyrrhic/tbprobe.h"

namespace {

// Filtre de splat
constexpr int MAX_EVAL = 2500;

// Positions corrigées : au plus 6 pièces, sans droit de roque
constexpr int TB_MEN = 6;

// Règle des 50 coups, en demi-coups
constexpr int LIMITE_50_COUPS = 100;

// Résultat du point de vue du camp au trait, comme l'octet de résultat du .bin
constexpr int PERTE  = 0;
constexpr int NULLE  = 1;
constexpr int GAIN   = 2;
constexpr int AMBIGU = -1;      // à ±1 demi-coup de la limite des 50 coups : laissé tel quel
constexpr int ECHEC  = -2;      // sonde des tables échouée
constexpr int AUCUNE = -3;      // (--partie) la partie n'est pas encore entrée en table

// viriformat : en-tête marlinformat de 32 octets, puis (coup, score) de 4 octets, terminé par (0, 0)
constexpr size_t VF_HEADER = 32;
constexpr size_t VF_MOVE   = 4;

// En-tête marlinformat : occupation (0-7), pièces (8-23), trait et e.p. (24), 50 coups (25), numéro du coup (26-27)
constexpr size_t HDR_PIECES    = 8;
constexpr size_t HDR_TRAIT_EP  = 24;     // bit 7 : Noirs au trait ; bits 0-6 : case e.p.
constexpr size_t HDR_50_COUPS  = 25;
constexpr size_t HDR_NUMERO    = 26;
constexpr int    HDR_EP_AUCUNE = 64;

// Lettre FEN de chaque type de pièce marlinformat ; 6 = tour qui peut roquer
constexpr char MARLIN_PIECE[7]   = { 'P', 'N', 'B', 'R', 'Q', 'K', 'R' };
constexpr int  MARLIN_ROOK_CASTLE = 6;

// Position bullet de 32 octets : occupation (0-7), pièces (8-23), score (24-25), résultat (26)
// « using » donne un nom à un type : Record = tableau de 32 octets
using Record = std::array<U08, 32>;
constexpr size_t REC_SCORE  = 24;
constexpr size_t REC_RESULT = 26;

// Compteurs nommés : c["sondées"]++ crée le compteur à 0 au premier usage ; clés triées
using Compteurs = std::map<std::string, long long>;

//==================================================================
//! \brief  Message d'erreur sur stderr
//! \return 1, code de retour de la commande
//------------------------------------------------------------------
int erreur(const std::string& msg)
{
    std::cerr << msg << std::endl;
    return 1;
}

//==================================================================
//! \brief  FEN de la position de départ d'une partie, lue dans l'en-tête marlinformat
//------------------------------------------------------------------
std::string header_fen(const U08* h)
{
    U64 occ;
    std::memcpy(&occ, h, sizeof(occ));

    // Pièces dans l'ordre des cases occupées, une par quartet (4 bits), deux par octet
    char pc[64]           = {};
    bool rook_castle[64]  = {};
    int  i                = 0;
    for (int sq = 0; sq < 64; sq++)
    {
        if (((occ >> sq) & 1) == 0)
            continue;

        const int  quartet = (h[HDR_PIECES + i / 2] >> (4 * (i & 1))) & 15;   // 4 bits de la i-ème pièce
        const int  code    = quartet & 7;                                     // type de pièce
        const bool noire   = quartet & 8;

        pc[sq] = MARLIN_PIECE[code];
        if (noire)
            pc[sq] = static_cast<char>(std::tolower(pc[sq]));
        rook_castle[sq] = (code == MARLIN_ROOK_CASTLE);
        i++;
    }

    // Placement des pièces
    std::string fen;
    for (int r = 7; r >= 0; r--)
    {
        int empty = 0;
        for (int f = 0; f < 8; f++)
        {
            const char c = pc[r * 8 + f];
            if (!c)
            {
                empty++;
                continue;
            }
            if (empty)
            {
                fen += static_cast<char>('0' + empty);
                empty = 0;
            }
            fen += c;
        }
        if (empty)
            fen += static_cast<char>('0' + empty);
        if (r)
            fen += '/';
    }

    // Trait
    const bool noirs_au_trait = (h[HDR_TRAIT_EP] >> 7) != 0;
    fen += noirs_au_trait ? " b " : " w ";

    // Roques : déduits des tours qui peuvent roquer
    std::string castle;
    if (rook_castle[H1])
        castle += 'K';
    if (rook_castle[A1])
        castle += 'Q';
    if (rook_castle[H8])
        castle += 'k';
    if (rook_castle[A8])
        castle += 'q';
    fen += castle.empty() ? "-" : castle;
    fen += ' ';

    // Prise en passant
    const int ep = h[HDR_TRAIT_EP] & 127;
    if (ep == HDR_EP_AUCUNE)
    {
        fen += '-';
    }
    else
    {
        fen += static_cast<char>('a' + ep % 8);
        fen += static_cast<char>('1' + ep / 8);
    }

    // Compteur des 50 coups, numéro du coup
    U16 fullmove;
    std::memcpy(&fullmove, h + HDR_NUMERO, sizeof(fullmove));
    fen += " " + std::to_string(h[HDR_50_COUPS]) + " " + std::to_string(fullmove);

    return fen;
}

//==================================================================
//! \brief  La position est-elle couverte par les tables ?
//------------------------------------------------------------------
bool en_table(const Board& b)
{
    return BB::count_bit(b.occupancy_all()) <= TB_MEN && !b.get_status().castling;
}

//==================================================================
//! \brief  Case e.p. au format de pyrrhic : 0 = aucune
//------------------------------------------------------------------
unsigned ep_pyrrhic(const Board& b)
{
    if (b.get_ep_square() == SQUARE_NONE)
        return 0;
    return b.get_ep_square();
}

//==================================================================
//! \brief  Sonde WDL de pyrrhic
//! \return TB_LOSS .. TB_WIN, ou TB_RESULT_FAILED
//------------------------------------------------------------------
unsigned sonde_wdl(const Board& b)
{
    const unsigned ep = ep_pyrrhic(b);

    return tb_probe_wdl(b.occupancy_c<WHITE>(),             b.occupancy_c<BLACK>(),
                        b.occupancy_p<PieceType::KING>(),   b.occupancy_p<PieceType::QUEEN>(),
                        b.occupancy_p<PieceType::ROOK>(),   b.occupancy_p<PieceType::BISHOP>(),
                        b.occupancy_p<PieceType::KNIGHT>(), b.occupancy_p<PieceType::PAWN>(),
                        ep, b.turn() == WHITE);
}

//==================================================================
//! \brief  Sonde DTZ de pyrrhic, compteur des 50 coups supposé nul
//! \param[out] success  0 si la sonde échoue
//------------------------------------------------------------------
int sonde_dtz(const Board& b, int& success)
{
    const unsigned ep = ep_pyrrhic(b);

    return tb_probe_dtz(b.occupancy_c<WHITE>(),             b.occupancy_c<BLACK>(),
                        b.occupancy_p<PieceType::KING>(),   b.occupancy_p<PieceType::QUEEN>(),
                        b.occupancy_p<PieceType::ROOK>(),   b.occupancy_p<PieceType::BISHOP>(),
                        b.occupancy_p<PieceType::KNIGHT>(), b.occupancy_p<PieceType::PAWN>(),
                        ep, b.turn() == WHITE, &success);
}

//==================================================================
//! \brief  Résultat de table pour le camp au trait, règle des 50 coups comprise
//! Un gain (ou une perte) n'est retenu que si le compteur des 50 coups plus la DTZ reste sous la limite.
//! \return GAIN, NULLE, PERTE, AMBIGU ou ECHEC
//------------------------------------------------------------------
int tb_result(const Board& b)
{
    const unsigned wdl = sonde_wdl(b);
    if (wdl == TB_RESULT_FAILED)
        return ECHEC;

    // Nulle, gain maudit ou perte bénie : nulle sous la règle des 50 coups
    if (wdl != TB_WIN && wdl != TB_LOSS)
        return NULLE;

    int success;
    const int dtz = sonde_dtz(b, success);
    if (success == 0)
        return ECHEC;

    // Demi-coups joués sans prise ni coup de pion, à la prochaine remise à zéro du compteur
    // La DTZ de pyrrhic peut être décalée d'un demi-coup : à ±1 de la limite, on ne tranche pas
    const int plies = b.get_fiftymove_counter() + std::abs(dtz);
    if (plies > LIMITE_50_COUPS + 1)
        return NULLE;
    if (plies >= LIMITE_50_COUPS - 1)
        return AMBIGU;

    return wdl == TB_WIN ? GAIN : PERTE;
}

//==================================================================
//! \brief  Le même résultat vu par l'autre camp : gain et perte s'échangent
//------------------------------------------------------------------
int resultat_adverse(int r)
{
    return GAIN - r;
}

//==================================================================
//! \brief  Résultat de table du point de vue des Blancs
//------------------------------------------------------------------
int tb_result_blancs(const Board& b)
{
    const int r = tb_result(b);
    if (r < 0 || b.turn() == WHITE)
        return r;
    return resultat_adverse(r);
}

//==================================================================
//! \brief  Lecture séquentielle d'un .vf chargé en mémoire
//------------------------------------------------------------------
struct LecteurVf
{
    std::vector<U08> data;
    size_t           off = 0;

    bool fini() const
    {
        return off >= data.size();
    }

    bool reste(size_t n) const
    {
        return off + n <= data.size();
    }

    //! \brief  En-tête de partie : avance de VF_HEADER octets
    const U08* header()
    {
        const U08* h = &data[off];
        off += VF_HEADER;
        return h;
    }

    //! \brief  Coup suivant (code viriformat, score des Blancs)
    void coup(U16& code, I16& score)
    {
        std::memcpy(&code,  &data[off],     sizeof(code));
        std::memcpy(&score, &data[off + 2], sizeof(score));
        off += VF_MOVE;
    }
};

//==================================================================
//! \brief  splat garde-t-il cette position ?
//! Il écarte les positions en échec, celles dont le coup joué est une prise ou une promotion,
//! et les scores extrêmes.
//------------------------------------------------------------------
bool gardee_par_splat(const Board& board, MOVE move, I16 score)
{
    if (board.is_in_check())
        return false;
    if (Move::is_capturing(move) || Move::is_promoting(move))
        return false;
    return std::abs(score) < MAX_EVAL;
}

//==================================================================
//! \brief  La position du .bin est-elle celle que l'on rejoue ?
//! Le .bin est écrit du point de vue du camp au trait : si les Noirs ont le trait,
//! l'échiquier est retourné (rangée 1 ↔ rangée 8) et le score opposé.
//------------------------------------------------------------------
bool alignee(const Record& rec, const Board& board, I16 score)
{
    U64 occ;
    I16 rec_score;
    std::memcpy(&occ,       rec.data(),             sizeof(occ));
    std::memcpy(&rec_score, rec.data() + REC_SCORE, sizeof(rec_score));

    const bool white  = board.turn() == WHITE;
    const U64  bocc   = board.occupancy_all();

    if (white)
        return occ == bocc && rec_score == score;
    return occ == BB::byte_swap(bocc) && rec_score == -score;
}

//==================================================================
//! \brief  Remplace le résultat d'une position du .bin, et compte le changement
//! \return true si le résultat a changé
//------------------------------------------------------------------
bool corrige(Record& rec, int nouveau, const std::string& prefixe, Compteurs& c)
{
    const int ancien = rec[REC_RESULT];
    if (nouveau == ancien)
        return false;

    c[prefixe + std::to_string(ancien) + "->" + std::to_string(nouveau)]++;
    rec[REC_RESULT] = static_cast<U08>(nouveau);
    return true;
}

//==================================================================
//! \brief  Message d'erreur sur stderr
//! \return false, pour les méthodes de Relabel
//------------------------------------------------------------------
bool echec(const std::string& msg)
{
    std::cerr << msg << std::endl;
    return false;
}

//==================================================================
//! \brief  Correction d'un .bin, une partie du .vf à la fois
//!
//! Pour chaque partie :
//!     rejoue_partie   rejoue les coups, lit dans le .bin les positions gardées par splat,
//!                     corrige celles qui sont en table, met les autres de côté (--partie)
//!     corrige_partie  (--partie) donne aux positions mises de côté le résultat de l'entrée en table
//!     reecrit_partie  réécrit dans le .bin les positions de la partie, si l'une a changé
//!
//! Les méthodes rendent false en cas d'erreur, après l'avoir écrite sur stderr.
//------------------------------------------------------------------
class Relabel
{
public:
    Relabel(LecteurVf& _lecteur, std::fstream& _fb, size_t _nbr_positions,
            bool _partie, const std::string& _vf, const std::string& _bin)
        : lecteur(_lecteur), fb(_fb), nbr_positions(_nbr_positions),
          vf(_vf), bin(_bin), partie(_partie)
    {
    }

    bool rejoue_partie();
    void corrige_partie();
    void reecrit_partie();

    size_t positions_lues() const
    {
        return idx;
    }

    Compteurs& compteurs()
    {
        return comptes;
    }

private:
    bool note_entree();
    bool lit_position(I16 score);
    MOVE trouve_coup(U16 code);
    void joue(MOVE move);

    // Membres rangés par alignement décroissant, pour ne pas perdre de place

    // Échiquier ; accum en tête, car il est aligné sur 64 octets
    Accumulator       accum;                    // ne sert pas
    Board             board;
    MoveList          ml;                       // coups légaux, réutilisée d'un coup à l'autre

    // Fichiers
    LecteurVf&        lecteur;
    std::fstream&     fb;
    const size_t      nbr_positions;            // dans le .bin
    const std::string vf;                       // noms, pour les messages d'erreur
    const std::string bin;
    size_t            idx = 0;                  // positions du .bin lues
    Compteurs         comptes;

    // Partie en cours
    struct Avant                                // (--partie) position d'avant l'entrée en table
    {
        size_t k;                               // indice dans recs
        bool   white;                           // Blancs au trait
    };

    size_t              start  = 0;             // 1re position de la partie dans le .bin
    std::vector<Record> recs;                   // positions de la partie gardées par splat
    std::vector<Avant>  avant;
    int                 entree = AUCUNE;        // (--partie) résultat des Blancs à l'entrée en table
    bool                modif  = false;         // une position de la partie a changé

    const bool          partie;                 // option --partie
};

//==================================================================
//! \brief  Rejoue la partie suivante du .vf
//------------------------------------------------------------------
bool Relabel::rejoue_partie()
{
    if (!lecteur.reste(VF_HEADER))
        return echec(vf + " : fin de fichier tronquée");

    board.initialisation();
    board.set_fen(header_fen(lecteur.header()), false);

    start  = idx;
    entree = AUCUNE;
    modif  = false;
    recs.clear();
    avant.clear();

    while (true)
    {
        if (!lecteur.reste(VF_MOVE))
            return echec(vf + " : partie sans terminateur");

        U16 code;
        I16 score;
        lecteur.coup(code, score);
        if (code == 0 && score == 0)            // terminateur : fin de la partie
            break;

        if (!note_entree())
            return false;

        const MOVE move = trouve_coup(code);
        if (move == Move::MOVE_NONE)
            return echec(vf + " : coup illégal à l'octet " + std::to_string(lecteur.off - VF_MOVE));

        if (gardee_par_splat(board, move, score))
        {
            if (!lit_position(score))
                return false;
        }

        joue(move);
    }

    // La partie peut entrer en table à son dernier coup
    return note_entree();
}

//==================================================================
//! \brief  (--partie) Note le résultat de la 1re position en table de la partie
//------------------------------------------------------------------
bool Relabel::note_entree()
{
    if (!partie || entree != AUCUNE || !en_table(board))
        return true;

    entree = tb_result_blancs(board);
    if (entree == ECHEC)
        return echec(vf + " : sonde des tables échouée");
    return true;
}

//==================================================================
//! \brief  Lit dans le .bin la position courante, que splat a gardée
//! En table, son résultat est corrigé ; sinon elle est mise de côté pour --partie.
//------------------------------------------------------------------
bool Relabel::lit_position(I16 score)
{
    if (idx >= nbr_positions)
        return echec(bin + " : moins de positions que de positions filtrées");

    Record rec;
    fb.read(reinterpret_cast<char*>(rec.data()), rec.size());
    if (!alignee(rec, board, score))
        return echec(vf + " : désalignement avec " + bin + " à la position " + std::to_string(idx));

    if (en_table(board))
    {
        const int r = tb_result(board);
        if (r == ECHEC)
            return echec(vf + " : sonde des tables échouée");

        comptes["sondées"]++;
        if (r == AMBIGU)
            comptes["à ±1 de la limite"]++;
        else if (corrige(rec, r, "", comptes))
            modif = true;
    }
    else if (partie && entree == AUCUNE)
    {
        avant.push_back({ recs.size(), board.turn() == WHITE });
    }

    recs.push_back(rec);
    idx++;
    return true;
}

//==================================================================
//! \brief  Coup légal de la position qui correspond au code viriformat
//! \return Move::MOVE_NONE si aucun
//------------------------------------------------------------------
MOVE Relabel::trouve_coup(U16 code)
{
    board.legal_moves<MoveGenType::ALL>(ml);

    for (size_t k = 0; k < ml.size(); k++)
    {
        if (DataGen::viri_move(ml.mlmoves[k].move) == code)
            return ml.mlmoves[k].move;
    }
    return Move::MOVE_NONE;
}

//==================================================================
//! \brief  Joue le coup sur l'échiquier
//------------------------------------------------------------------
void Relabel::joue(MOVE move)
{
    if (board.turn() == WHITE)
        board.make_move<WHITE, false>(accum, move);
    else
        board.make_move<BLACK, false>(accum, move);
}

//==================================================================
//! \brief  (--partie) Donne le résultat de l'entrée en table aux positions d'avant
//------------------------------------------------------------------
void Relabel::corrige_partie()
{
    if (!partie || entree == AUCUNE)
        return;

    comptes["parties en table"]++;

    if (entree == AMBIGU)
    {
        comptes["parties à ±1 de la limite"]++;
        return;
    }

    for (const Avant& a : avant)
    {
        // Le .bin est du point de vue du camp au trait
        const int nouveau = a.white ? entree : resultat_adverse(entree);
        if (corrige(recs[a.k], nouveau, "partie ", comptes))
            modif = true;
    }
}

//==================================================================
//! \brief  Réécrit dans le .bin les positions de la partie, si l'une a changé
//------------------------------------------------------------------
void Relabel::reecrit_partie()
{
    if (!modif)
        return;

    fb.seekp(static_cast<std::streamoff>(start * sizeof(Record)));
    fb.write(reinterpret_cast<const char*>(recs.data()), static_cast<std::streamsize>(recs.size() * sizeof(Record)));

    // Indispensable : lecture et écriture partagent la même position dans le fichier
    fb.seekg(static_cast<std::streamoff>(idx * sizeof(Record)));
}

} // namespace

//==================================================================
//! \brief  Corrige par les tables Syzygy le résultat des positions d'un .bin issu d'un .vf
//! \param[in]  vf      fichier viriformat du datagen
//! \param[in]  bin     fichier bullet tiré du .vf par splat, modifié sur place
//! \param[in]  tables  répertoires Syzygy séparés par « : »
//! \param[in]  partie  étend la correction aux positions d'avant l'entrée en table
//! \return 0, ou 1 en cas d'erreur
//------------------------------------------------------------------
int tb_relabel(const std::string& vf, const std::string& bin, const std::string& tables, bool partie)
{
    if (!tb_init(tables) || TB_LARGEST < TB_MEN)
        return erreur("tables " + tables + " : " + std::to_string(TB_LARGEST) + " pièces au plus, "
                      + std::to_string(TB_MEN) + " requises");

    //------------------------------------------------ ouverture des fichiers

    std::ifstream fv(vf, std::ios::binary);
    if (!fv)
        return erreur(vf + " : lecture impossible");

    LecteurVf lecteur;
    lecteur.data.assign(std::istreambuf_iterator<char>(fv), std::istreambuf_iterator<char>());

    std::fstream fb(bin, std::ios::in | std::ios::out | std::ios::binary);
    if (!fb)
        return erreur(bin + " : ouverture impossible");

    fb.seekg(0, std::ios::end);
    const size_t nbr_positions = static_cast<size_t>(fb.tellg()) / sizeof(Record);
    fb.seekg(0);

    //------------------------------------------------ une partie à la fois

    Relabel relabel(lecteur, fb, nbr_positions, partie, vf, bin);

    while (!lecteur.fini())
    {
        if (!relabel.rejoue_partie())
            return 1;
        relabel.corrige_partie();
        relabel.reecrit_partie();
    }

    //------------------------------------------------ contrôles finaux et compteurs

    if (relabel.positions_lues() != nbr_positions)
        return erreur(vf + " : " + std::to_string(relabel.positions_lues()) + " positions filtrées contre "
                      + std::to_string(nbr_positions) + " dans " + bin);

    fb.flush();
    if (!fb)
        return erreur(bin + " : écriture échouée");

    Compteurs& c = relabel.compteurs();
    c["positions"] = static_cast<long long>(nbr_positions);

    std::cout << "{";
    bool first = true;
    for (const auto& [cle, valeur] : c)
    {
        if (!first)
            std::cout << ", ";
        std::cout << "\"" << cle << "\": " << valeur;
        first = false;
    }
    std::cout << "}" << std::endl;

    return 0;
}
