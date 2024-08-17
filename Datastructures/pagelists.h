/**
 * @author Ben Williams
 * @date June 18th, 2024
 * 
 * Header for free list datastructure and functions
 * 
 */


// If this is 1, then we will use normal critical sections instead of just the bit
#define DEBUG_PAGELOCK 0
#define LIGHT_DEBUG_PAGELOCK 0

#include <windows.h>
#include "../hardware.h"
#include "./db_linked_list.h"
#include "./pagetable.h"

/**
 * #################################################
 * PAGE STRUCT DEFINITIONS USED ACROSS ALL PAGELISTS
 * #################################################
 */

#define ZERO_STATUS 0
#define FREE_STATUS 1
#define MODIFIED_STATUS 2
#define STANDBY_STATUS 3
#define ACTIVE_STATUS 4


#define PAGE_UNLOCKED 0
#define PAGE_LOCKED 1

#define PAGE_NOT_BEING_WRITTEN 0
#define PAGE_BEING_WRITTEN 1

#define PAGE_NOT_BEING_TRIMMED 0
#define PAGE_BEING_TRIMMED 1

#define PAGE_NOT_MODIFIED 0
#define PAGE_MODIFIED 1


#ifndef PAGE_T
#define PAGE_T

typedef struct {
    ULONG64 status:3;
    ULONG64 pagefile_idx:40; 
    ULONG64 writing_to_disk:1;
    /**
     * We need this bit to avoid a specific race condition:
     * 
     * If we trim a page, begin popping pages from modified to eventually write to disk,
     * then it is rescued and trimmed again, it can end up in a disk batch twice. With this bit,
     * we are able to ensure that we do not re-add it to the disk_batch
     */
    ULONG64 modified:1;
    PTE* pte;
    long page_lock;
    #if DEBUG_PAGELOCK
    CRITICAL_SECTION dev_page_lock;
    ULONG64 holding_threadid;

    #elif LIGHT_DEBUG_PAGELOCK
    ULONG64 holding_threadid;
    PTE acquiring_pte_copy;
    ULONG64 acquiring_access_type;
    ULONG64 origin_code;
    ULONG64 prev_code;
    ULONG64 two_ago;
    ULONG64 three_ago;
    ULONG64 four_ago;
    ULONG64 five_ago;
    #endif
    DB_LL_NODE* frame_listnode;
} PAGE;


#if DEBUG_PAGELOCK
typedef struct {
    PAGE* actual_page;
    PAGE page_copy;
    PVOID stack_trace[8];
    ULONG64 pfn;
    PTE* linked_pte;
    PTE linked_pte_copy;
    ULONG64 thread_id;
} PAGE_LOGSTRUCT;
#endif

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
 * #######################################
 * ZEROED PAGES LIST STRUCTS AND FUNCTIONS
 * #######################################
 */

// We want to have frames go into different cache slots if we allocate several at a time
#define NUM_CACHE_SLOTS (CACHE_SIZE / PAGE_SIZE)

#ifndef ZERO_LISTS_T
#define ZERO_LISTS_T

/**
 * An array of zero lists whose length corresponds to the size of the cache
 * 
 * 1. You'd want to walk the list of zero lists to pull out a frame at a time - such that all are in different cache slots
 * 
 * 2. Say for a 64kb cache, it can hold 16 frames in the cache. We can have an array of 16 zero lists corresponding
 *      to each cache slot
 */
typedef struct {
    DB_LL_NODE* listheads[NUM_CACHE_SLOTS];

    volatile ULONG64 list_lengths[NUM_CACHE_SLOTS]; 
    volatile ULONG64 total_available;

    CRITICAL_SECTION list_locks[NUM_CACHE_SLOTS];
    
} ZEROED_PAGES_LISTS;
#endif


/**
 * Initializes the zeroed frame lists with all of the initial physical memory in the system
 * 
 * Returns a pointer to the zero lists if successful, NULL otherwise
 */
ZEROED_PAGES_LISTS* initialize_zeroed_lists(PAGE* page_storage_base, PULONG_PTR physical_frame_numbers, ULONG64 num_physical_frames);


/**
 * ######################################
 * FREE FRAMES LIST STRUCTS AND FUNCTIONS
 * ######################################
 */


#ifndef FREE_FRAMES_T
#define FREE_FRAMES_T
/**
 * An array of free lists whose length corresponds to the size of the cache
 */
typedef struct {
    DB_LL_NODE* listheads[NUM_CACHE_SLOTS];

    volatile ULONG64 list_lengths[NUM_CACHE_SLOTS]; 
    volatile ULONG64 total_available;

    CRITICAL_SECTION list_locks[NUM_CACHE_SLOTS];
    
} FREE_FRAMES_LISTS;
#endif


/**
 * Creates the free frames list structure and its associated listheads and locks
 * 
 * Returns a memory allocated pointer to a FREE_FRAMES_LISTS struct, or NULL if an error occurs
 */
FREE_FRAMES_LISTS* initialize_free_frames();


/**
 * ###################################
 * MODIFIED LIST STRUCTS AND FUNCTIONS
 * ###################################
 */

#ifndef MODIFIED_LIST_T
#define MODIFIED_LIST_T
typedef struct {
    DB_LL_NODE* listhead;
    volatile ULONG64 list_length;
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
    volatile ULONG64 list_length;
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

#endif