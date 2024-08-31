/**
 * @author Ben Williams
 * @date July 19th, 2024
 * 
 * All operations required to zeroing out pages
 */

#define NUM_FAULTER_ZERO_SLOTS 256

#define NUM_THREAD_ZERO_SLOTS   (NUM_CACHE_SLOTS * 4)


/**
 * These are for the page zeroing thread's structures
 */
#define PAGE_SLOT_OPEN 0
#define PAGE_SLOT_CLAIMED 1
#define PAGE_SLOT_READY 2

/**
 * These are for faulters zeroing out pages themselves
 */
#define ZERO_SLOT_OPEN 0
#define ZERO_SLOT_USED 1
#define ZERO_SLOT_NEEDS_FLUSH 2


/**
 * These are proportions of the total number of available pages that help determine whether or not we 
 * take pages from the standby list to add to the free and zero lists
 */

// If (number of physical pages) / STANDBY_LIST_REFRESH_PROPORTION is more than the total amount of available pages, we will allow threads
// to take pages from the standby list
#define STANDBY_LIST_REFRESH_PROPORTION 10

// If (number of physical pages) / TOTAL_ZERO_AND_FREE_LIST_REFRESH_PROPORTION is more than the total amount of pages in the free and zero lists (and we have enough pages in standby),
// we will refresh both the free and zero lists with pages
#define TOTAL_ZERO_AND_FREE_LIST_REFRESH_PROPORTION 12

// If (number of physical pages) / (free or zero)_LIST_REFRESH_PROPORTION is more than the total amount of pages in the free lists or the zero lists (and we have enough pages in standby),
// we will refresh the respective lists with pages. 
// This allows us to make targeted refreshes on either the zero lists or the free lists if either are independently running low
#define FREE_LIST_REFRESH_PROPORTION 15
#define ZERO_LIST_REFRESH_PROPORTION 25



/** 
 * These determine the number of pages we take off the standby list to add to the zero/free lists when we are refreshing the lists
 */
#define NUM_PAGES_FAULTER_REFRESH   NUM_CACHE_SLOTS

// When refreshing both the free and zero lists, this proportion goes to the free lists
#define FREE_FRAMES_PORTION   NUM_PAGES_FAULTER_REFRESH / 2

// Whether or not only a single thread can refresh these lists at once. Currently experimenting with this as multiple refreshing threads might just create
// standby list-lock contention and overall slow things down. However, if we had a large amount of threads, they might overwhelm the refreshing
#define ONLY_ONE_REFRESHER 1

/**
 * Rather than search through every single cache-color / list section of the free/zero lists, we bail after a certain number of attempts to avoid large linear walks
 * when these lists are sparse. After this many attempts, we will opt to try looking for pages in a different list
 */
#define MAX_ATTEMPTS_FREE_OR_ZERO_LISTS    NUM_CACHE_SLOTS //max(NUM_CACHE_SLOTS / 5, 1)

/**
 * When looking for available pages without any preference for zeroed pages, 
 * we might opt to instead search the zero-list first anyway if there are not a lot of pages on the free list.
 * This helps us potentially reduce contention on the free list
 */
#define REDUCE_FREE_LIST_PRESSURE_AMOUNT    NUM_CACHE_SLOTS


#include "../Datastructures/datastructures.h"
#include "../hardware.h"

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
void refresh_free_and_zero_lists();


/**
 * Determines whether or not a faulter should refresh the zero and free lists with pages from the standby list using pre-defined macros 
 * and the total amount of available pages compared to the total number of physical pages
 * 
 * Takes the thread index of the calling thread as parameter so that we can access the thread local storage
 * 
 * In the future, this would be made more dynamic (perhaps based moreso on the rate of page usage, where they're being used, etc - rather than simple macros)
 */
void potential_list_refresh(ULONG64 thread_idx);


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
 * This should be called whenever threads fail to acquire any pages, and need to wait for pages to be available.
 * 
 * This signals the trimmer as well as the modified writer if appropriate so that we start replenishing pages.
 */
void wait_for_pages_signalling();


/**
 * Tries to find an available page from the zero lists, free lists, or standby. If found,
 * returns a pointer to the page. Otherwise, returns NULL. If zero_page_preferred is TRUE,
 * we will search the zero lists first - otherwise, we search the free list first. Both are preferable to standby.
 * 
 * If there are no available pages, the trimming thread is signaled and the modified writing thread is signaled if appropriate.
 * The calling thread will wait for a signal for available pages before returning NULL
 */
PAGE* find_available_page(BOOL zeroed_page_preferred);


/**
 * Tries to find num_pages pages across the zeroed/free/standby lists. 
 * If zeroed_pages_preferred is TRUE, we will search the zero list first. Otherwise, we search the free list first. 
 * 
 * Returns the number of pages written into the page_storage. We do not guarentee that we return any pages, and might return 0
 */
ULONG64 find_batch_available_pages(BOOL zeroed_pages_preferred, PAGE** page_storage, ULONG64 num_pages);


/**
 * Rescues all of the pages from both the modified and standby lists
 * 
 * Assumes that all pages' pagelocks are acquired, and that the pre-sorted pages are in the appropriate lists
 * with their correct statuses
 */
void rescue_batch_pages(PAGE** modified_rescues, ULONG64 num_modified_rescues, PAGE** standby_rescues, ULONG64 num_standby_rescues);


/**
 * Rescues the given page from the modified or standby list, if it is in either. 
 * 
 * Assumes the pagelock was acquired beforehand.
 * 
 * Returns SUCCESS if the page is rescued, ERROR otherwise
 */
int rescue_page(PAGE* page);


/**
 * Rescues the given page from the modified list, if it can be found
 * 
 * Returns SUCCESS if the rescue page was found and removed if applicable, ERROR otherwise
 */
int modified_rescue_page(PAGE* page);


/**
 * Rescues the given page from the standby list, if it can be found
 * 
 * Returns SUCCESS if the rescue page was found and removed, ERROR otherwise
 */
int standby_rescue_page(PAGE* page);


/**
 * Releases all of the pages by returning them to their original lists
 * 
 * This is only done when we acquire pages to resolve an unaccessed PTE's fault and may have
 * speculatively brought in several pages to map several PTEs ahead of them as well. However,
 * another thread had already mapped those PTEs so we no longer need those pages.
 * 
 * We return these pages back to the zero/free/standby lists as appropriate
 */
void release_batch_unneeded_pages(PAGE** pages, ULONG64 num_pages);


/**
 * Returns the given page to its appropriate list after it is revealed we no longer need it
 * 
 * Assumes that we have the pagelock for the given page, and releases it at the end of the function
 */
void release_unneeded_page(PAGE* page);


/**
 * Adds the given page to its proper slot in the zero list
 */
void zero_lists_add(PAGE* page);


/**
 * Adds the given page to its proper slot in the free list
 */
void free_frames_add(PAGE* page);


/**
 * Connects the linked list nodes inside of the pages together to form a single section that
 * can be added to a listhead very quickly
 * 
 * We also change the page's statuses to new_page_status since this is usually being done to add a 
 * section to a list very quickly. This can avoid an extra loop later.
 */
void create_chain_of_pages(PAGE** pages_to_chain, ULONG64 num_pages, ULONG64 new_page_status);


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