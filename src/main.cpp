#include <cstring>
#include <cstdlib>
#include <string>
#include "Uci.h"
#include "TranspositionTable.h"
#include "ThreadPool.h"
#include "Attacks.h"
#include "DataGen.h"
#include "Cuckoo.h"
#include "NNUE.h"
#include "TbRelabel.h"

// Globals
TranspositionTable  transpositionTable(HASH_SIZE);
ThreadPool          threadPool(1, false, true);

extern void init_bitmasks();

//=======================================================
//! \brief  Point d'entrée du programme
//!
//! Initialise les tables d'attaques et Cuckoo, puis lance
//! soit le bench, soit le datagen, soit la boucle UCI,
//! selon les arguments de la ligne de commande.
//!
//! \param[in]  argCount    nombre d'arguments
//! \param[in]  argValue    tableau des arguments
//!
//! \return Code de retour du programme
//-------------------------------------------------------
int main(int argCount, char* argValue[])
{
    verify_network();
    Attacks::init_masks();
    Cuckoo::init();
    NNUE::init();   // réordonne les couches L1-L3 du réseau (avant tout thread)

    //  Benchmark
    //  appel : Zangdar bench [depth] [nbr_threads] [hash_size]
    //          les arguments sont optionnels
    if (argCount > 1 && strcmp(argValue[1], "bench") == 0)
    {
        Uci uci;
        uci.bench(argCount, argValue);
    }

    //  DataGen
    //  appel : Zangdar datagen <nbr_threads> <max_fens_millions> <output_dir>
    else if (argCount > 1 && strcmp(argValue[1], "datagen") == 0)
    {
        // Les 3 arguments sont obligatoires
        if (argCount < 5)
        {
            std::cout << "usage : Zangdar datagen <nbr_threads> <max_fens_millions> <output_dir>" << std::endl;
            return 1;
        }

        DataGen(atoi(argValue[2]), atoi(argValue[3]), std::string{argValue[4]});
        std::cout << "fin datagen" << std::endl;
    }

    //  Correction des résultats d'un .bin par les tables Syzygy
    //  appel : Zangdar tbrelabel <data.vf> <data.bin> <tables> [--partie]
    else if (argCount > 1 && strcmp(argValue[1], "tbrelabel") == 0)
    {
        if (argCount < 5 || (argCount == 6 && strcmp(argValue[5], "--partie") != 0) || argCount > 6)
        {
            std::cout << "usage : Zangdar tbrelabel <data.vf> <data.bin> <tables rép1:rép2> [--partie]" << std::endl;
            return 1;
        }
        return tb_relabel(argValue[2], argValue[3], argValue[4], argCount == 6);
    }

    //  UCI
    else
    {
        Uci uci;
        uci.run();
    }

    return 0;
}
