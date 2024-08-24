/**
 * @author Ben Williams
 * @date June 18th, 2024
 * 
 * Header for free list datastructure and functions
 * 
 */


// If this is 1, then we will use normal critical sections instead of just the bit
#define DEBUG_PAGELOCK 0
#define LIGHT_DEBUG_PAGELOCK 1


// For development while switching between generic linked list nodes and keeping the flink/blink inside the page
#define PAGE_LINKED_LISTS 1


// When toggled, we enable a shared lock for the modified list
#define MODIFIED_SHARED_LOCK 0

#include <windows.h>
#include "../hardware.h"
#include "./db_linked_list.h"
#include "./pagetable.h"

/**
 * #################################################
 * PAGE STRUCT DEFINITIONS USED ACROSS ALL PAGELISTS
 * #################################################
 */

#define LISTHEAD_STATUS 0
#define ZERO_STATUS 1
#define FREE_STATUS 2
#define MODIFIED_STATUS 3
#define STANDBY_STATUS 4
#define ACTIVE_STATUS 5

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

#if 0
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

    // We could use a single bit here if we decided to be more careful with our interlocked operations (not to crush other bits),
    // but I decided to use this for simplicity so that I could spend time on the actual use of the pagelocks
    long page_lock;
} PHYS_PAGE_FORMAT;

typedef struct {
    UCHAR status;

    /**
     * We want to be able to insert at the head and pop from the tail simultaneously just using the pagelocks,
     * 
     */
    long flink_lock;
    long blink_lock;

} LISTHEAD_PAGE_FORMAT;
#endif

typedef struct PAGE_STRUCT {
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

    // We could use a single bit here if we decided to be more careful with our interlocked operations (not to crush other bits),
    // but I decided to use this for simplicity so that I could spend time on the actual use of the pagelocks
    long page_lock;

    #if PAGE_LINKED_LISTS
    struct PAGE_STRUCT* flink;
    struct PAGE_STRUCT* blink;
    #else
    DB_LL_NODE* frame_listnode;
    #endif

    #if DEBUG_PAGELOCK
    CRITICAL_SECTION dev_page_lock;
    ULONG64 holding_threadid;

    // The debugger is just painfully slow on this program since we are try/catching exceptions for the simulation,
    // so it was extremely helpful to build a history of the page into the struct to use only when debugging
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


#if PAGE_LINKED_LISTS
/**
 * Inserts the given page to the head of the list
 */
void insert_page(PAGE* listhead, PAGE* page);


/**
 * Pops a page from the tail of the list
 * 
 * Returns a pointer to the popped page, or NULL if the list is empty
 */
PAGE* pop_page(PAGE* listhead);


/**
 * Unlinks the given page from its list
 */
void unlink_page(PAGE* page);


/**
 * Removes the section of pages from the list they are in,
 * where the beginning node is closest to the head and the end node is closest to the tail
 */
void remove_page_section(PAGE* beginning, PAGE* end);


/**
 * Inserts the chain of pages between the beginning and end at the listhead,
 * where the beginning node will be closest to the head 
 */
void insert_page_section(PAGE* listhead, PAGE* beginning, PAGE* end);
#endif


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
    #if PAGE_LINKED_LISTS
    PAGE listheads[NUM_CACHE_SLOTS];
    #else
    DB_LL_NODE* listheads[NUM_CACHE_SLOTS];
    #endif

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
    #if PAGE_LINKED_LISTS
    PAGE listheads[NUM_CACHE_SLOTS];
    #else
    DB_LL_NODE* listheads[NUM_CACHE_SLOTS];
    #endif

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
    #if PAGE_LINKED_LISTS
    PAGE listhead;
    #else
    DB_LL_NODE* listhead;
    #endif

    //BW: remember to adjust this to the 64 - sizeof(page)
    UCHAR buffer[64];

    volatile ULONG64 list_length;

    #if MODIFIED_SHARED_LOCK
    SRWLOCK shared_lock;
    #else
    SRWLOCK shared_lock;
    CRITICAL_SECTION lock;
    #endif

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
    #if PAGE_LINKED_LISTS
    PAGE listhead;
    #else
    DB_LL_NODE* listhead;
    #endif

    //BW: remember to adjust this to the 64 - sizeof(page)
    UCHAR buffer[64];

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