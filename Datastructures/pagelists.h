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

    DB_LL_NODE* frame_listnode;
    ULONG64 frame_number:40;
    /**
     * Whether the page has been cleaned out before it was freed by the previous VA using it
     * 
     * We need to zero out pages that are going to a different process than the one it was at before
     */
    ULONG64 zeroed_out:1; 

} FREE_PAGE;

typedef struct {
    ULONG64 status:2;
    DB_LL_NODE* frame_listnode;
    PTE* pte;
    ULONG64 modified_again:1;
} MODIFIED_PAGE;

typedef struct {
    ULONG64 status:2;
    DB_LL_NODE* frame_listnode;
    PTE* pte;
    //BW: Make more space efficient later - calculate how many bits are needed based off hardware
    ULONG64 pagefile_idx:40; 
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
 * ###########################
 * GENERAL PAGE LIST FUNCTIONS
 * ###########################
 */


/**
 * Initializes all of the pages, and organizes them in memory such that they are reachable using the pfn_to_page
 * function in O(1) time. Returns the address of page_storage_base representing the base address of where all the pages
 * can be found from
 * 
 * Returns NULL given any error
 */
PAGE* initialize_pages(PULONG_PTR physical_frame_numbers, ULONG64 num_physical_frames);

    
/**
 * Returns TRUE if the page is in the free status, FALSE otherwise
 */
BOOL page_is_free(PAGE page);


/**
 * Returns TRUE if the page is in the modified status, FALSE otherwise
 */
BOOL page_is_modified(PAGE page);


/**
 * Returns TRUE if the page is in the standby status, FALSE otherwise
 */
BOOL page_is_standby(PAGE page);



/**
 * ######################################
 * FREE FRAMES LIST STRUCTS AND FUNCTIONS
 * ######################################
 */


// We want to have frames go into different cache slots if we allocate several at a time
#define NUM_FRAME_LISTS (CACHE_SIZE / PAGE_SIZE)

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

    // BW: Note the potential for race conditions keeping track of this!  
    ULONG64 list_lengths[NUM_FRAME_LISTS]; 

    ULONG64 curr_list_idx;
    // LOCK
} FREE_FRAMES_LISTS;
#endif


/**
 * Given the new pagetable, create free frames lists that contain all of the physical frames
 * 
 * Returns a memory allocated pointer to a FREE_FRAMES_LISTS struct, or NULL if an error occurs
 */
FREE_FRAMES_LISTS* initialize_free_frames(PAGE* page_storage_base, PULONG_PTR physical_frame_numbers, ULONG64 num_physical_frames);


/**
 * Returns a page off the free list, if there are any. Otherwise, returns NULL
 */
PAGE* allocate_free_frame(FREE_FRAMES_LISTS* free_frames);


/**
 * Zeroes out the memory on the physical frame so that it can be allocated to a new process without privacy loss
 * 
 * Returns SUCCESS if no issues, ERROR otherwise
 */
int zero_out_pte(PTE* pte);


/**
 * ###################################
 * MODIFIED LIST STRUCTS AND FUNCTIONS
 * ###################################
 */

#ifndef MODIFIED_LIST_T
#define MODIFIED_LIST_T
typedef struct {
    DB_LL_NODE* listhead;
    ULONG64 list_length;
    CRITICAL_SECTION lock;

} MODIFIED_LIST;

/**
 * Allocates memory for and initializes a modified list struct
 * 
 * Returns a pointer to the modified list or NULL upon error
 */
MODIFIED_LIST* initialize_modified_list();


/**
 * Adds the given page to the modified list
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 */
int modified_add_page(PAGE* page, MODIFIED_LIST* modified_list);


/**
 * Pops the oldest page from the modified list and returns it
 * 
 * Returns NULL upon any error or if the list is empty
 */
PAGE* modified_pop_page(MODIFIED_LIST* modified_list);


/**
 * Rescues the page associated with the given PTE in the modified list, if it is there
 * 
 * Returns a pointer to the rescued page, or NULL upon error or if the page cannot be found
 */
PAGE* modified_rescue_page(MODIFIED_LIST* modified_list, PTE* pte);

#endif


/**
 * ##################################
 * STANDBY LIST STRUCTS AND FUNCTIONS
 * ##################################
 */

#ifndef STANDBY_LIST_T
#define STANDBY_LIST_T

typedef struct {
    DB_LL_NODE* listhead;
    ULONG64 list_length;
    CRITICAL_SECTION lock;

} STANDBY_LIST;

/**
 * Allocates memory for and initializes a standby list struct
 * 
 * Returns a pointer to the standby list or NULL upon error
 */
STANDBY_LIST* initialize_standby_list();


/**
 * Adds the given page to the standby list
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 */
int standby_add_page(PAGE* page, STANDBY_LIST* standby_list);

/**
 * Pops the oldest page from the standby list and returns it
 * 
 * Returns NULL upon any error or if the list is empty
 */
PAGE* standby_pop_page(STANDBY_LIST* standby_list);


/**
 * Rescues the page associated with the given PTE in the standby list, if it is there
 * 
 * Returns a pointer to the rescued page, or NULL upon error or if the page cannot be found
 */
PAGE* standby_rescue_page(STANDBY_LIST* standby_list, PTE* pte);

#endif