/**
 * @author Ben Williams
 * @date June 25th, 2024
 * 
 * All global variables used across the state machine as well as inside the datastructures
 */

#include "Datastructures/disk.h"
#include "Datastructures/pagetable.h"
#include "Datastructures/pagelists.h"

/**
 * GLOBAL VALUES OR POINTERS
 */

extern PAGE* page_storage_base;

/**
 * GLOBAL DATASTRUCTURES
 */
extern PAGETABLE* pagetable;

extern DISK* disk;

extern FREE_FRAMES_LISTS* free_frames;


/**
 * GLOBAL SYNCHRONIZATION
 */
