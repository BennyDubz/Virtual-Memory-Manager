/**
 * @author Ben Williams
 * @date June 18th, 2024
 * 
 * Header for free list datastructure and functions
 * 
 */

#include <windows.h>
#include "../hardware.h"
#include "./db_linked_list.h"
#include "./pagetable.h"

/**
 * #################################################
 * PAGE STRUCT DEFINITIONS USED ACROSS ALL PAGELISTS
 * #################################################
 */

#define FREE_STATUS 0
#define MODIFIED_STATUS 1
#define STANDBY_STATUS 2

#ifndef PAGE_T
#define PAGE_T
typedef struct {
    ULONG64 status:2;
    PTE* pte;
    DB_LL_NODE* frame_listnode;
    /**
     * Whether the page has been cleaned out before it was freed by the previous VA using it
     * 
     * We need to zero out pages that are going to a different process than the one it was at before
     */
    ULONG64 zeroed_out:1; 

} FREE_PAGE;

typedef struct {
    ULONG64 status:2;
    PTE* pte;
    DB_LL_NODE* frame_listnode;
    ULONG64 modified_again:1;
} MODIFIED_PAGE;

typedef struct {
    ULONG64 status:2;
    PTE* pte;
    DB_LL_NODE* frame_listnode;

} STANDBY_PAGE;

typedef struct {
    union {
        FREE_PAGE free_page;
        MODIFIED_PAGE modified_page;
        STANDBY_PAGE standby_page;
    };
} PAGE;
#endif

/**
 * ######################################
 * FREE FRAMES LIST STRUCTS AND FUNCTIONS
 * ######################################
 */


// We want to have frames go into different cache slots if we allocate several at a time
#define NUM_FRAME_LISTS (NUMBER_OF_PHYSICAL_PAGES * PAGE_SIZE) / CACHE_SIZE

#ifndef FREE_FRAMES_T
#define FREE_FRAMES_T
/**
 * An array of free lists whose length corresponds to the size of the cache
 * 
 * 1. You'd want to walk the list of free lists to pull free frames, such that all are in different cache slots
 * 
 * 2. Say for a 64kb cache, it can hold 16 frames in the cache. We can have an array of 16 free lists corresponding
 *      to each cache slot
 */
typedef struct {
    DB_LL_NODE* listheads[NUM_FRAME_LISTS];
    ULONG64 list_lengths[NUM_FRAME_LISTS];    
    ULONG64 curr_list_idx;
    // LOCK (idk windows implementation)
} FREE_FRAMES_LISTS;


typedef struct {
    PTE* pte;
    ULONG64 zeroed_out:1;
    // Other information?
} FREE_FRAME;
#endif

/**
 * Given the new pagetable, create free frames lists that contain all of the physical frames
 * 
 * Returns a memory allocated pointer to a FREE_FRAMES_LISTS struct, or NULL if an error occurs
 */
FREE_FRAMES_LISTS* initialize_free_frames(PTE* page_table, ULONG64 num_physical_frames);

/**
 * Returns a page off the free list, if there are any. Otherwise, returns NULL
 */
PAGE* allocate_free_frame(FREE_FRAMES_LISTS* free_frames);

/**
 * Zeroes out the memory on the physical frame so that it can be allocated to a new process without privacy loss
 * 
 * Returns SUCCESS if no issues, ERROR otherwise
 */
int zero_out_frame(PTE* page_table, ULONG64 frame_number);


/**
 * ###################################
 * MODIFIED LIST STRUCTS AND FUNCTIONS
 * ###################################
 */



/**
 * ##################################
 * STANDBY LIST STRUCTS AND FUNCTIONS
 * ##################################
 */