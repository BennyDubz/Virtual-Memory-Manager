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

// For testing while we implement the use of the shared lock in the modified list
#define SRW_MOD 1


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
    short page_lock;

    struct PAGE_STRUCT* flink;
    struct PAGE_STRUCT* blink;
   
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


typedef struct {
    // These lists and their contents might be hot, so at least trying to keep them in different cache lines
    // should help reduce collisons
    DECLSPEC_ALIGN(64)
    PAGE listhead;

    DECLSPEC_ALIGN(64)
    ULONG64 list_length;

    // Having both the shared lock and the normal critical section will help with the transition while implementing both
    DECLSPEC_ALIGN(64)
    SRWLOCK shared_lock;

    DECLSPEC_ALIGN(64)
    CRITICAL_SECTION lock;
} PAGE_LISTHEAD;


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
 * Spins until the pagelock for the given page can be acquired and returns
 */
void acquire_pagelock(PAGE* page, ULONG64 origin_code);


/**
 * Releases the pagelock for other threads to use
 */
void release_pagelock(PAGE* page, ULONG64 origin_code);


/**
 * Tries to acquire the pagelock without any spinning. 
 * 
 * Returns TRUE if successful, FALSE otherwise
 */
BOOL try_acquire_pagelock(PAGE* page, ULONG64 origin_code);


/**
 * Inserts the page into the list using the shared list lock scheme alongside the page locks,
 * and releases the lock on the given page before returning
 */
void insert_page2(PAGE_LISTHEAD* list, PAGE* page);


#if 0
/**
 * Pops a page from the list while taking advantage of the shared lock and pagelock scheme
 * 
 * Assumes you already own the pagelock of the tail page
 * 
 * Returns a pointer to the popped page if successful with its pagelock acquired, NULL otherwise. 
 */
PAGE* pop_page2(PAGE_LISTHEAD* list);
#endif


/**
 * Removes all of the given pages from the list, assumes that all of the pagelocks are held.
 * 
 * Takes advantage of the shared lock scheme.
 * 
 * This function is intended for use with rescuing a batch of pages from the modified/standby list, 
 * where the batch of pages might not be adjacent to eachother
 */
void unlink_batch_scattered_pages(PAGE_LISTHEAD* list, PAGE** pages_to_remove, ULONG64 num_pages);


/**
 * Unlinks the page from its list, and takes advantage of the shared lock and pagelock scheme to avoid colliding with other unlinkers
 * and threads inserting/popping from the ends of the list
 * 
 * Assumes that you already hold the lock for the page that you are trying to unlink
 */
void unlink_page2(PAGE_LISTHEAD* list, PAGE* page);


/**
 * Inserts the chain of pages between the beginning and end at the listhead,
 * where the beginning node will be closest to the head 
 * 
 * Takes advantage of the shared lock and pagelock scheme. Does NOT release the pagelocks for each node in the section
 */
void insert_page_section2(PAGE_LISTHEAD* list, PAGE* beginning, PAGE* end, ULONG64 num_pages);


/**
 * Removes the section of pages from the list they are in,
 * where the beginning node is closest to the head and the end node is closest to the tail
 * 
 * Assumes that the pagelocks for the nodes including and between the beginning and the end are all held
 */
void remove_page_section2(PAGE_LISTHEAD* list, PAGE* beginning, PAGE* end, ULONG64 num_pages);


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
    PAGE_LISTHEAD listheads[NUM_CACHE_SLOTS];
    
    volatile ULONG64 total_available;
    
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
    PAGE_LISTHEAD listheads[NUM_CACHE_SLOTS];   

    volatile ULONG64 total_available;
    
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


/**
 * Allocates memory for and initializes a modified list struct
 * 
 * Returns a pointer to the modified list or NULL upon error
 */
PAGE_LISTHEAD* initialize_modified_list();


/**
 * ##################################
 * STANDBY LIST STRUCTS AND FUNCTIONS
 * ##################################
 */


/**
 * Allocates memory for and initializes a standby list struct
 * 
 * Returns a pointer to the standby list or NULL upon error
 */
PAGE_LISTHEAD* initialize_standby_list();

#if 0
/**
 * Adds the given page to the standby list
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 */
int standby_add_page(PAGE* page, STANDBY_LIST* standby_list);

#endif

