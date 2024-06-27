/**
 * @author Ben Williams
 * @date June 25th, 2024
 * 
 * All global variables used across the state machine as well as inside the datastructures
 */

#include <windows.h>
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

extern STANDBY_LIST* standby_list;

extern MODIFIED_LIST* modified_list;


/**
 * GLOBAL SYNCHRONIZATION
 */

extern HANDLE aging_event;

extern HANDLE trimming_event;

extern HANDLE modified_writing_event;

extern ULONG64 num_child_threads;

extern HANDLE* threads;

