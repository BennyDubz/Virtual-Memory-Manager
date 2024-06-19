/**
 * @author Ben Williams
 * @date June 18th, 2024
 * 
 * Header for free list datastructure and functions
 * 
 */

#include <windows.h>
#include "../../../hardware.h"
#include "../db_linked_list.h"
#include "../pagetable.h"

// We want to have frames go into different cache slots if we allocate several at a time
#define NUM_FRAME_LISTS (NUMBER_OF_PHYSICAL_PAGES * PAGE_SIZE) / CACHE_SIZE

/**
 * Note: Not sure if we need a free_frames_list type if we only keep one free frames list.
 * 
 * However, if we keep more than one free frames list (so that multiple threads can use with less contention)
 * then it might be helpful.
 */
typedef struct {
    DB_LL_NODE* frame_listheads[NUM_FRAME_LISTS];
    ULONG64 curr_list_idx;
    // LOCK (idk windows implementation)
} FREE_FRAMES_LISTS;


typedef struct {
    PTE* pte;
    ULONG64 zeroed_out:1;
    // Other information?
} FREE_FRAME;


// Define a few db linked list function wrappers that help implement the free list functionality


/**
 * Given the new pagetable, create free frames lists that contain all of the physical frames
 * 
 * Returns a memory allocated pointer to a FREE_FRAMES_LISTS struct, or NULL if an error occurs
 */
FREE_FRAMES_LISTS* initialize_free_frames(PTE* page_table, ULONG64 num_physical_frames);


/**
 * Return the physical frame number 
 */


/**
 * Zeroes out the memory on the physical frame so that it can be allocated to a new process without privacy loss
 * 
 * Returns SUCCESS if no issues, ERROR otherwise
 */
int zero_out_frame(PTE* page_table, ULONG64 frame_number);


/**
 * An array of free lists whose length corresponds to the size of the cache
 * 
 * 1. You'd want to walk the list of free lists to pull free frames, such that all are in different cache slots
 * 
 * 2. Say for a 64kb cache, it can hold 16 frames in the cache. We can have an array of 16 free lists corresponding
 *      to each cache slot
 */