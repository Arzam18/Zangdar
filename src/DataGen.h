#ifndef DATAGEN_H
#define DATAGEN_H

class DataGen;

#include "Board.h"
#include "Search.h"

// viriformat (crate viriformat 2.0.1, lu par ViriBinpackLoader de bullet) : par partie,
// la position de départ en marlinformat (32 o), puis (coup u16, éval i16 des Blancs) par demi-coup, puis 4 octets nuls
struct ViriMove {
    U16 move;
    I16 score;
};
static_assert(sizeof(ViriMove) == 4);

class DataGen
{
public:
    DataGen(const U32 _nbr_threads=4, const U32 _max_fens=1, const std::string &_output="./fens");

    //! \brief  Destructeur
    ~DataGen() {}

    static U16 viri_move(MOVE move);

private:
    void genfens(int thread_id, const std::string& str_file,
                 std::atomic<size_t>& total_fens,
                 std::atomic<bool>& run);
    U32 set_threads(const U32 nbr);
    template <Color color> void data_search(Board& board, Timer& timer, Search& search,
                     MOVE &move, I32 &score);

    static std::array<U08, 32> pack_board(const Board& board, U08 wdl);

    constexpr static int MIN_RANDOM_PLIES =     8;   // borne basse du tirage [MIN,MAX] par partie
    constexpr static int MAX_RANDOM_PLIES =     9;   // borne haute : 1 ply de plus alterne le camp du 1er coup réel
    constexpr static int RANDOM_SEE       =  -200;   // seuil SEE des coups aléatoires (comme Avalanche, pawnocchio)
    constexpr static int MAX_RANDOM_SCORE =  1000;   // score maximum accepté pour la position résultant
    constexpr static int MAX_RANDOM_DEPTH =     5;   // profondeur maximum pour la recherche

    constexpr static int HIGH_SCORE       = 2500;   // score minimum pour compter la position comme gagnante
    constexpr static int LOW_SCORE        =   10;   // score maximum pour compter la position comme annulante
    constexpr static int MIN_WIN_COUNT    =    5;   // nombre minimum de positions gagnantes pour adjuger la partie comme gagnée
    constexpr static int MIN_DRAW_COUNT   =   10;   // nombre minimum de positions nulles    pour adjuger la partie comme nulle
    constexpr static int MIN_DRAW_PLY     =   50;   // pas de nulle adjugée avant ce demi-coup (Avalanche, pawnocchio)
    constexpr static int MAX_GAME_PLIES   = 1000;   // garde-fou ; la règle des 50 coups arrête bien avant

    // HARD_NODE_LIMIT est juste là pour empécher une explosion de la recherche
    constexpr static int HARD_NODE_LIMIT  =   100'000;   // 20 x SOFT_NODE_LIMIT
    constexpr static int SOFT_NODE_LIMIT  =     5'000;

    // TT du datagen, une par camp : 16 Mo (comme pawnocchio) ; 4 Mo donnent le même débit
    constexpr static int DATAGEN_HASH_SIZE = 16;

    constexpr static U08 WDL_WIN[N_COLORS] = {2, 0};   // résultat du point de vue des Blancs : 2 gain, 1 nulle, 0 perte
    constexpr static U08 WDL_DRAW          = 1;
    constexpr static I16 SCORE_CLIP        = 32000;    // 32767 est réservé : position à exclure
};

#endif // DATAGEN_H
