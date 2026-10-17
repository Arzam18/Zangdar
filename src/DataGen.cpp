
//  Code inspiré par plusieurs codes puis adapté et commenté
//      Midnight, Illumina, Clover, Clarity, SmallBrain

// #define DEBUG_GEN

/* viriformat (voir DataGen.h)
 *  toutes les positions sont écrites, sans filtre : échecs, prises, scores extrêmes se filtrent au chargement
 *  score et résultat du point de vue des Blancs ; pas de tables : le tri par Syzygy se fait après coup
*/

#include <string>
#include <cstring>
#include <array>
#include <vector>
#include <algorithm>
#include <fstream>
#include <random>
#include <memory>
#include <csignal>

#include "Board.h"
#include "Search.h"
#include "DataGen.h"
#include "defines.h"
#include "TranspositionTable.h"
#include "ThreadPool.h"

// Pointeur global pour permettre au handler de signal d'arrêter proprement le datagen
static std::atomic<bool>* g_datagen_run = nullptr;

//===========================================================================
//! \brief  Handler de signal (SIGINT/SIGTERM) pour arrêter proprement le datagen
//---------------------------------------------------------------------------
static void datagen_signal_handler(int /*sig*/)
{
    if (g_datagen_run)
        g_datagen_run->store(false);
}

//===========================================================================
//! \brief  Constructeur
//! \param[in]  _nbr_threads    nombre de threads demandé
//! \param[in]  _max_fens       nombre de fens demandé, en millions
//! \param[in]  _output         répertoire de sortie des fichiers
//---------------------------------------------------------------------------
DataGen::DataGen(const U32 _nbr_threads, const U32 _max_fens, const std::string& _output)
{
    const U32 max_fens = std::clamp(_max_fens, 1U, 1000U) * 1'000'000U;

    printf("DataGen : nbr_threads=%u ; max_fens=%u millions ; output=%s \n", _nbr_threads, max_fens/1'000'000, _output.c_str());

    //================================================
    //  Initialisations
    //================================================

    // Pas de tables : les finales gardent le score de la recherche, le tri se fait après coup
    threadPool.set_useSyzygy(false);

    U32 nbr_threads = set_threads(_nbr_threads);

    //================================================
    //  Lancement de la génération par thread
    //================================================
    std::vector<std::thread> threads{};
    std::atomic<size_t> total_fens{0};           // nombre total de fens pour toutes les threads

    // shared_ptr pour que le thread detached (lecture stdin) ne référence jamais un objet détruit
    auto run = std::make_shared<std::atomic<bool>>(true);
    auto start_time = TimePoint::now();

    // Installer le handler de signal pour arrêt propre via Ctrl-C
    g_datagen_run = run.get();
    auto prev_sigint  = std::signal(SIGINT,  datagen_signal_handler);
    auto prev_sigterm = std::signal(SIGTERM, datagen_signal_handler);

    for (size_t i = 0; i < nbr_threads; i++)
    {
        std::string str_file(_output);
        str_file += "/data" + std::to_string(i) + ".vf";

        threads.emplace_back(
                    [this, i, str_file, &total_fens, run] {
            genfens(i, str_file, total_fens, *run);
        });
    }

    // thread de contrôle d'arrêt (detached : ne bloque pas la fin du programme)
    // il suffit d'entrer un caractère
    std::thread([run] {
        std::string input_line{};
        while (std::getline(std::cin, input_line))
        {
            if (input_line.empty())
                continue;
            run->store(false);
            break;
        }
    }).detach();

    // boucle d'affichage de l'avancée des résultats
    while (run->load())
    {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        auto elapsed = std::max(std::chrono::duration_cast<std::chrono::milliseconds>(TimePoint::now() - start_time).count(), decltype(start_time)::duration::rep(1));
        U64 fps = 1000 * total_fens / elapsed;
        int r   = (fps > 0 && total_fens < max_fens)
                ? static_cast<double>(max_fens - total_fens) / static_cast<double>(fps)
                : 0;
        int h   = r / 3600;
        int m   = (r - h*3600) / 60;
        int s   = r - h*3600 - m*60;
        std::cout << "total fens = " << total_fens/1'000'000.0 << " millions"
                   << " ; fens/s = " << fps
                   << " ; fens/s/t = " << 1000*total_fens/elapsed/nbr_threads
                   << " ; end in " << h << " hours " << m << " min " << s << " sec"
                   << std::endl;
        if (total_fens >= max_fens)
        {
            run->store(false);
        }
    }

    //================================================
    //  Fin du job
    //================================================
    for (auto& t : threads)
        t.join();

    // Restaurer les handlers de signal
    std::signal(SIGINT,  prev_sigint);
    std::signal(SIGTERM, prev_sigterm);
    g_datagen_run = nullptr;

    auto total_elapsed = std::max(std::chrono::duration_cast<std::chrono::milliseconds>(TimePoint::now() - start_time).count(), decltype(start_time)::duration::rep(1));
    int total_r = total_elapsed / 1000;
    int total_h = total_r / 3600;
    int total_m = (total_r - total_h*3600) / 60;
    int total_s = total_r - total_h*3600 - total_m*60;

    std::cout << "total fens generated = " << total_fens
               << " ; total time = " << total_h << " hours " << total_m << " min " << total_s << " sec"
               << std::endl;
}

//====================================================================
//! \brief  Lancement d'une génération de fens , dans une thread donnée
//! \param[in]      thread_id   identifiant de la thread
//! \param[in]      str_file    chemin du fichier de sortie des fens
//! \param[in,out]  total_fens  somme des fens collectées sur toutes les threads
//! \param[in]      run         true = continuer la génération, false = arrêter
//--------------------------------------------------------------------
void DataGen::genfens(int thread_id, const std::string& str_file,
                      std::atomic<size_t>& total_fens,
                      std::atomic<bool>& run)
{
    printf("thread id = %d : out = %s \n", thread_id, str_file.c_str());

    std::ofstream file;
    file.open(str_file, std::ios::app | std::ios::binary);
    if (file.is_open() == false)
    {
        std::cout << "file " << str_file << " not opened" << std::endl;
        return;
    }

    std::vector<ViriMove> moves;        // coups et scores de la partie en cours
    moves.reserve(MAX_GAME_PLIES);
    std::random_device rd;
    std::mt19937_64 generator(rd());

    // Configure le gestionnaire de temps
    // On va utiliser une limite par "node",
    // sauf dans "random_game" où on va utiliser une limite par "depth".
    Timer timer(false, 0, 0, 0, 0, 0, MAX_RANDOM_DEPTH, 0, 0);

    //================================================
    //  Boucle infinie
    //  On simule l'exécution de N parties
    //================================================

    // Allocation sur le tas : Search et History sont trop gros pour la pile d'un thread
    std::unique_ptr<Search> search = std::make_unique<Search>();
    search->index  = 0;

    // chaque thread a ses propres tables, une par camp, comme deux moteurs en match.
    // DATAGEN_HASH_SIZE Mo chacune : largement assez pour 5000 nœuds
    std::array<std::unique_ptr<TranspositionTable>, N_COLORS> tables;
    for (auto& tt : tables)
        tt = std::make_unique<TranspositionTable>(DATAGEN_HASH_SIZE);
    search->table = tables[WHITE].get();    // make_move précharge la TT, dès les coups aléatoires

    MoveList movelist;
    Board    board;
    int      drawCount = 0;
    int      winCount = 0;

    MOVE    move;
    I32     score;

    //====================================================
    // Boucle infinie sur une simulation de parties
    //----------------------------------------------------

    // run est vérifié uniquement ici, entre deux parties :
    // Ctrl-C (ou limite atteinte) → finit la partie en cours → écrit les fens → sort proprement.
    while (run.load())
    {

        board.initialisation();
        board.set_fen(START_FEN, false);
        for (auto& tt : tables)
            tt->update_age();                   // évite de vider les tables
        search->history.reset();

        //====================================================
        //  Initalisation random de l'échiquier
        //----------------------------------------------------


        // Nombre de plies aléatoires tiré par partie dans [MIN,MAX] : alterne le camp
        // qui joue le premier coup réel (supprime un biais de parité de trait) + diversifie.
        std::uniform_int_distribution<> ply_distribution{MIN_RANDOM_PLIES, MAX_RANDOM_PLIES};
        const size_t random_plies = static_cast<size_t>(ply_distribution(generator));

        auto random_ply = [&]<Color C>(size_t& current_ply) -> bool {
            board.legal_moves<C, MoveGenType::ALL>(movelist);
            if (movelist.size() == 0)
            {
                current_ply = 0;
                board.initialisation();
                board.set_fen(START_FEN, false);
                return false; // recommencer
            }
            // Tirage parmi les coups de SEE >= RANDOM_SEE ; roque, prise en passant et promotion toujours admis
            std::array<int, MAX_MOVES> candidates;
            int nbr_candidates = 0;
            for (size_t k = 0; k < movelist.size(); k++)
            {
                const MOVE m = movelist.mlmoves[k].move;
                if (Move::is_castling(m) || Move::is_enpassant(m) || Move::is_promoting(m) || board.fast_see(m, RANDOM_SEE))
                    candidates[nbr_candidates++] = static_cast<int>(k);
            }
            int index;
            if (nbr_candidates > 0)
            {
                std::uniform_int_distribution<> distribution{0, nbr_candidates - 1};
                index = candidates[distribution(generator)];
            }
            else
            {
                std::uniform_int_distribution<> distribution{0, int(movelist.size() - 1)};
                index = distribution(generator);
            }
            search->make_move<C, false>(board, movelist.mlmoves[index].move);

            // Empêche que le dernier ply soit un mat ou un pat
            if (++current_ply == random_plies)
            {
                board.legal_moves<~C, MoveGenType::ALL>(movelist);
                if (movelist.size() == 0)
                {
                    current_ply = 0;
                    board.initialisation();
                    board.set_fen(START_FEN, false);
                }
            }
            return true;
        };

        size_t current_ply = 0;
        while (current_ply < random_plies)
        {
            bool ok = board.turn() == WHITE
                ? random_ply.template operator()<WHITE>(current_ply)
                : random_ply.template operator()<BLACK>(current_ply);
            if (!ok)
                continue;
        }


        // Evaluation de la position, en fin des plies aléatoires
        // Si elle est trop déséquilibrée, on passe à une nouvelle partie

        // Initialisation NNUE une seule fois par partie (au lieu de à chaque data_search)
        search->nnue.start_search(board);

        move  = Move::MOVE_NONE;
        score = -SCORE_INFINITE;
        timer.setup(Color::WHITE);
        search->table = tables[board.turn()].get();

        if (board.turn() == WHITE)
            data_search<WHITE>(board, timer, *search, move, score);
        else
            data_search<BLACK>(board, timer, *search, move, score);
        if (std::abs(score) > MAX_RANDOM_SCORE)
        {
            continue;
        }


        //====================================================
        //  Boucle de jeu : la partie est jouée jusqu'au bout
        //----------------------------------------------------

        const std::array<U08, 32> start = pack_board(board, WDL_DRAW);
        moves.clear();
        U08 wdl      = WDL_DRAW;
        int game_ply = static_cast<int>(random_plies);
        drawCount    = 0;
        winCount     = 0;
        timer.setup(SOFT_NODE_LIMIT, HARD_NODE_LIMIT);

        while (game_ply < MAX_GAME_PLIES)
        {
            search->nodes   = 0;
            search->stopped = false;

            // Partie nulle ? (répétition, 50 coups, matériel insuffisant)
            if (board.is_draw(0) || board.is_insufficient_material())
                break;

            if (board.turn() == WHITE)
                board.legal_moves<WHITE, MoveGenType::ALL>(movelist);
            else
                board.legal_moves<BLACK, MoveGenType::ALL>(movelist);

            // Aucun coup possible : mat ou pat
            if (movelist.size() == 0)
            {
                if (board.is_in_check())
                    wdl = WDL_WIN[~board.turn()];
                break;
            }

            // Rebase le stack NNUE avant chaque recherche pour éviter l'overflow
            // (head_idx monte de 1 à chaque make_move de la boucle de jeu)
            search->nnue.rebase(board);
            search->table = tables[board.turn()].get();
            search->table->update_age();

            if (board.turn() == WHITE)
                data_search<WHITE>(board, timer, *search, move, score);
            else
                data_search<BLACK>(board, timer, *search, move, score);

            // Toutes les positions sont gardées ; score du point de vue des Blancs
            const int white_score = score * (1 - 2*board.turn());
            moves.push_back({ viri_move(move),
                              static_cast<I16>(std::clamp<int>(white_score, -SCORE_CLIP, SCORE_CLIP)) });

            // Adjudication de la partie
            const int  absScore = std::abs(score);
            const U08  winner   = white_score > 0 ? WDL_WIN[WHITE] : WDL_WIN[BLACK];

            //  Presque mat ?
            if (absScore >= MATE_IN_X)
            {
                wdl = winner;
                break;
            }

            if (absScore >= HIGH_SCORE) {                                // position gagnante
                winCount++;
                drawCount = 0;
            } else if (absScore <= LOW_SCORE && game_ply >= MIN_DRAW_PLY) { // position annulante
                drawCount++;
                winCount = 0;
            } else {
                drawCount = 0;
                winCount = 0;
            }

            // Gain ou Nulle quasiment assuré, inutile d'aller plus loin
            if (winCount >= MIN_WIN_COUNT) {
                wdl = winner;
                break;
            }
            else if (drawCount >= MIN_DRAW_COUNT) {
                break;
            }

            // Exécution du coup trouvé lors de la recherche
            if (board.turn() == WHITE)
                search->make_move<WHITE, true>(board, move);
            else
                search->make_move<BLACK, true>(board, move);
            game_ply++;
        }

        if (moves.empty())
            continue;

        // Ecriture de la partie : position de départ, coups, terminateur
        std::array<U08, 32> packed = start;
        packed[30] = wdl;
        constexpr ViriMove terminator{0, 0};
        file.write(reinterpret_cast<const char*>(packed.data()), packed.size());
        file.write(reinterpret_cast<const char*>(moves.data()), static_cast<std::streamsize>(moves.size() * sizeof(ViriMove)));
        file.write(reinterpret_cast<const char*>(&terminator), sizeof(terminator));

        total_fens.fetch_add(moves.size());   // total des positions de tous les threads
        file.flush();   // force l'écriture sur disque : protège contre Ctrl-C / crash

    } // fin des parties

    file.close();
}

//============================================================================
//! \brief  Recherche dans la position actuelle
//! Lance un iterative deepening avec aspiration windows
//! \param[in]      board   échiquier courant
//! \param[in]      timer   gestionnaire de temps/limite de profondeur
//! \param[in,out]  search  contexte de recherche (nodes, pv, etc.)
//! \param[out]     move    meilleur coup trouvé
//! \param[out]     score   score du meilleur coup
//----------------------------------------------------------------------------
template <Color C>
void DataGen::data_search(Board& board, Timer& timer, Search& search,
                          MOVE &move, I32 &score)
{
    move  = Move::MOVE_NONE;
    score = -SCORE_INFINITE;

    search.stopped    = false;
    search.nodes      = 0;
    search.best_depth = 0;

    for (int d = 0; d <= MAX_PLY; d++)
    {
        search.pv_scores[d] = -SCORE_INFINITE;
        search.pv_moves [d] = Move::MOVE_NONE;
    }


#if defined DEBUG_GEN
    timer.debug(WHITE);
    timer.start();
#endif

    //==================================================
    // iterative deepening
    //==================================================

    // start_search() est appelé une seule fois par partie, dans genfens()
    // L'accumulateur NNUE est maintenu incrémentalement par les make_move de la boucle de jeu.

    std::array<SearchInfo, STACK_SIZE> _info{};
    SearchInfo* si  = &(_info[STACK_OFFSET]);
    for (int i = 0; i < MAX_PLY+STACK_OFFSET; i++)
    {
        (si + i)->ply = i;
        (si + i)->cont_hist = &search.history.continuation_history[0][0];
    }

    int prev_score = -SCORE_INFINITE;

    for (search.iter_depth = 1; search.iter_depth <= std::max(1, timer.getSearchDepth()); search.iter_depth++)
    {
        // Recherche la position, avec des aspiration windows pour les profondeurs élevées
        const int iter_score = search.aspiration_window<C>(board, timer, si, prev_score);

        if (search.is_stopped())
            break;

        // L'itération s'est terminée sans problème
        search.best_depth = search.iter_depth;

        // Historique par profondeur
        search.pv_scores[search.iter_depth] = iter_score;
        search.pv_moves [search.iter_depth] = si->pv.line[0];

        prev_score = iter_score;

#if defined DEBUG_GEN
        I64 elapsed = timer.elapsedTime();

        std::cout << "time " << elapsed
                  << "  depth " << search.iter_depth
                  << "  nodes " << search.nodes
                  << "  score " << iter_score
                  << "  move " << Move::name(si->pv.line[0])
                  << std::endl;
#else
        I64 elapsed = 0;
#endif
        // Timer::update n'est pas utilisé car on est soit "par node", soit "par depth".

        // Limite souple de nœuds (ou profondeur) atteinte : on arrête
        if (timer.finishOnThisDepth(elapsed, search.iter_depth, search.nodes, search.pv_scores, search.pv_moves))
            break;

        search.seldepth = 0;
    }

    move  = search.pv_moves[search.best_depth];
    score = search.pv_scores[search.best_depth];
}

//============================================================================
//! \brief  Position au format marlinformat (32 octets, petit-boutiste)
//! \param[in]  board   position à encoder
//! \param[in]  wdl     résultat de la partie, du point de vue des Blancs
//----------------------------------------------------------------------------
std::array<U08, 32> DataGen::pack_board(const Board& board, U08 wdl)
{
    std::array<U08, 32> out{};

    const Bitboard occupancy = board.occupancy_all();
    std::memcpy(out.data(), &occupancy, sizeof(occupancy));

    // un quartet par pièce, cases croissantes : type (P=0 … K=5, 6 = tour qui peut roquer) | couleur << 3
    const U32 castling = board.get_status().castling;
    Bitboard  bb       = occupancy;
    int       i        = 0;
    while (bb)
    {
        const SQUARE sq = BB::pop_lsb(bb);
        const Piece  pc = board.piece_at(sq);
        U08 code = static_cast<U08>(Move::type(pc)) - 1;
        if (Move::type(pc) == PieceType::ROOK
            && (   (sq == H1 && (castling & CASTLE_WK)) || (sq == A1 && (castling & CASTLE_WQ))
                || (sq == H8 && (castling & CASTLE_BK)) || (sq == A8 && (castling & CASTLE_BQ))))
            code = 6;
        code |= static_cast<U08>(Move::color(pc)) << 3;
        out[8 + i/2] |= (i & 1) ? static_cast<U08>(code << 4) : code;
        i++;
    }

    const SQUARE ep = board.get_ep_square();
    out[24] = static_cast<U08>((board.turn() << 7) | (ep == SQUARE_NONE ? 64 : ep));
    out[25] = static_cast<U08>(std::min(board.get_fiftymove_counter(), 255));
    const U16 fullmove = static_cast<U16>(board.get_fullmove_counter());
    std::memcpy(&out[26], &fullmove, sizeof(fullmove));
    // out[28..29] : éval de la position de départ, inutilisée (0)
    out[30] = wdl;
    // out[31] : extra (0)
    return out;
}

//============================================================================
//! \brief  Coup au format viriformat : départ | arrivée << 6 | promotion << 12 | type << 14
//! Roque : le roi va sur la case de la tour
//----------------------------------------------------------------------------
U16 DataGen::viri_move(MOVE move)
{
    const U16 from = static_cast<U16>(Move::from(move));
    const U16 to   = static_cast<U16>(Move::dest(move));

    if (Move::is_castling(move))
    {
        const U16 rook = (to > from) ? from + 3 : from - 4;
        return static_cast<U16>(from | (rook << 6) | 0x8000);
    }

    const U16 data = static_cast<U16>(from | (to << 6));
    if (Move::is_promoting(move))
        return static_cast<U16>(data | ((Move::promoted_type(move) - PieceType::KNIGHT) << 12) | 0xC000);
    if (Move::is_enpassant(move))
        return static_cast<U16>(data | 0x4000);
    return data;
}

//===================================================
//! \brief  Calcule le nombre de threads à utiliser
//! Limité au nombre de processeurs disponibles
//! \param[in]  nbr     nombre de threads demandé
//!
//! \return Nombre de threads effectivement utilisables, entre 1 et MAX_THREADS
//---------------------------------------------------
U32 DataGen::set_threads(const U32 nbr)
{
    U32 nbr_threads = nbr;

    U32 processorCount = static_cast<U32>(std::thread::hardware_concurrency());
    if (processorCount == 0)
        processorCount = MAX_THREADS;

    // Limite le nombre de threads au nombre de processeurs
    nbr_threads     = std::min(nbr, processorCount);
    nbr_threads     = std::max(nbr_threads, 1U);
    nbr_threads     = std::min(nbr_threads, MAX_THREADS);

    return nbr_threads;
}

template void DataGen::data_search<WHITE>(Board& board, Timer& timer, Search& search, MOVE& move, I32& score);
template void DataGen::data_search<BLACK>(Board& board, Timer& timer, Search& search, MOVE& move, I32& score);
