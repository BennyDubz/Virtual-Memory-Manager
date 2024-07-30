/**
 * @author Ben Williams
 * @date July 19th, 2024
 * 
 * All operations required to zeroing out pages
 */

#define NUM_FAULTER_ZERO_SLOTS 256

#define NUM_THREAD_ZERO_SLOTS 1024

#define PAGE_SLOT_OPEN 0
#define PAGE_SLOT_CLAIMED 1
#define PAGE_SLOT_READY 2


#define ZERO_SLOT_OPEN 0
#define ZERO_SLOT_USED 1
#define ZERO_SLOT_NEEDS_FLUSH 2

#include "../Datastructures/datastructures.h"

#ifndef PAGE_ZEROING_STRUCT_T
#define PAGE_ZEROING_STRUCT_T
typedef struct {
    long status_map[NUM_THREAD_ZERO_SLOTS];
    PAGE* pages_to_zero[NUM_THREAD_ZERO_SLOTS];
    volatile ULONG64 curr_idx;
    volatile ULONG64 total_slots_used;
    volatile long zeroing_ongoing;
} PAGE_ZEROING_STRUCT;
#endif


/**
 * Returns an index to the page zeroing struct's status/page storage that is likely to be open
 * if the zeroing thread has kept up
 */
ULONG64 get_zeroing_struct_idx();


/**
 * Thread dedicated to taking pages from standby, zeroing them, and
 * adding them to the zero lists for quicker access
 */
LPTHREAD_START_ROUTINE thread_populate_zero_lists(void* parameters);


/**
 * Used to initialize the virtual addresses, lists, and variables used for zeroing out pages
 * 
 * Returns SUCCESS if we successfully initialized everything, ERROR otherwise
 */
int initialize_page_zeroing();


/**
 * Given the pages that need to be cleaned, zeroes out all of the contents on their physical pages
 */
void zero_out_pages(PAGE** pages, ULONG64 num_pages);


/**
 * Allocates and returns a single zeroed page, if there are any
 * 
 * Returns NULL if the list is empty
 */
PAGE* allocate_zeroed_frame();


/**
 * Allocates a batch of num_pages zeroed frames in order of cache slots if possible
 * 
 * Writes the pages into page_storage. Returns the number of pages successfully written/allocated
 */
ULONG64 allocate_batch_zeroed_frames(PAGE** page_storage, ULONG64 num_pages);


/**
 * Returns a page off the free list, if there are any. Otherwise, returns NULL
 */
PAGE* allocate_free_frame();


/**
 * Tries to allocate batch_size number of free frames and put them sequentially in page_storage
 * 
 * Returns the number of pages successfully allocated and put into the page_storage
 */
ULONG64 allocate_batch_free_frames(PAGE** page_storage, ULONG64 batch_size);


/**
 * To be called by the faulting thread when the total of free frames + zero frames is low
 * 
 * Takes many frames off of the standby list and uses some to populate the free frames list, while also
 * adding some to the zeroing-threads buffer. If there are enough pages for the zeroing thread to zero-out,
 * then it will be signalled
 */
void faulter_refresh_free_and_zero_lists();


/**
 * Pops and returns a pointer to the oldest page from the standby list and returns it
 * 
 * Returns NULL upon any error or if the list is empty
 */
PAGE* standby_pop_page();


/**
 * Tries to pop batch_size pages from the standby list. Returns the number of pages successfully allocated
 * 
 * Writes the pages' pointers into page_storage
 */
ULONG64 standby_pop_batch(PAGE** page_storage, ULONG64 batch_size);


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