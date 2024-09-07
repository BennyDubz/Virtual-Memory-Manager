/**
 * @author Ben Williams
 * @date June 19th, 2024
 * 
 * Implementation of freelist functions and creation
 */

#include <stdio.h>
#include <windows.h>
#include "./db_linked_list.h"
#include "./pagelists.h"
#include "../macros.h"
#include "./custom_sync.h"

/**
 * ###########################
 * GENERAL PAGE LIST FUNCTIONS
 * ###########################
 */


/**
 * Initializes all of the pages, and organizes them in memory such that they are reachable using the pfn_to_page
 * function in O(1) time. Returns the address of page_storage_base representing the base address of where all the pages
 * can be found from, minus the lowest pagenumber for simpler arithmetic in the other functions
 * 
 * Returns NULL given any error
 */
PAGE* initialize_pages(PULONG_PTR physical_frame_numbers, ULONG64 num_physical_frames) {

    ULONG64 lowest_pfn = 0xFFFFFFFFFFFFFFFF;
    ULONG64 highest_pfn = 0x0;

    for (ULONG64 frame_idx = 0; frame_idx < num_physical_frames; frame_idx++) {
        ULONG64 curr_pfn = physical_frame_numbers[frame_idx];
        
        lowest_pfn = curr_pfn < lowest_pfn ? curr_pfn : lowest_pfn;

        highest_pfn = curr_pfn > highest_pfn ? curr_pfn : highest_pfn;
    }

    // Now we can reserve the minimum amount of memory required for this scheme
    PAGE* page_storage_base = VirtualAlloc(NULL, (highest_pfn - lowest_pfn) * sizeof(PAGE), 
                                                MEM_RESERVE, PAGE_READWRITE);
    
    if (page_storage_base == NULL) {
        fprintf(stderr, "Unable to reserve memory for the pages\n");
        return NULL;
    }

    printf("Num physical frames: 0x%llX\n", num_physical_frames);
    
    // This makes it so we can easily shift to find even the lowest physical frame
    page_storage_base -= lowest_pfn;

    // Now, we can actually commit memory for each page
    for (ULONG64 frame_idx = 0; frame_idx < num_physical_frames; frame_idx++) {
        ULONG64 curr_pfn = physical_frame_numbers[frame_idx];
        
        void* page_region = VirtualAlloc(page_storage_base + curr_pfn, 
                                    sizeof(PAGE), MEM_COMMIT, PAGE_READWRITE);
        
        if (page_region == NULL) {
            fprintf(stderr, "Failed to allocate memory for page region in initialize_pages\n");
            return NULL;
        }

        PAGE* new_page = page_storage_base + curr_pfn;
       
        if (new_page == NULL) {
            fprintf(stderr, "Unable to allocate memory for page in initialize_pages\n");
            return NULL;
        }

        new_page->page_lock = PAGE_UNLOCKED;

        #if DEBUG_PAGELOCK
        InitializeCriticalSection(&new_page->dev_page_lock);
        #endif

        #if LIGHT_DEBUG_PAGELOCK
        new_page->origin_code = 0xFFFFFF;
        new_page->prev_code = 0xFFFFFF;
        new_page->two_ago = 0xFFFFFF;
        new_page->three_ago = 0xFFFFFF;
        new_page->four_ago = 0xFFFFFF;
        new_page->five_ago = 0xFFFFFF;
        #endif

        new_page->flink = NULL;
        new_page->blink = NULL;
        
    }

    return page_storage_base;
}


/**
 * Spins until the pagelock for the given page can be acquired and returns
 */
void acquire_pagelock(PAGE* page, ULONG64 origin_code) {

    #if DEBUG_PAGELOCK
    EnterCriticalSection(&page->dev_page_lock);
    log_page_status(page);
    page->page_lock = PAGE_LOCKED;
    page->holding_threadid = GetCurrentThreadId();
    return;
    #endif

    unsigned old_lock_status;
    while(TRUE) {
        old_lock_status = InterlockedCompareExchange16(&page->page_lock, PAGE_LOCKED, PAGE_UNLOCKED);

        #if LIGHT_DEBUG_PAGELOCK
        if (old_lock_status == PAGE_UNLOCKED) {
            page->holding_threadid = GetCurrentThreadId();
            page->five_ago = page->four_ago;
            page->four_ago = page->three_ago;
            page->three_ago = page->two_ago;
            page->two_ago = page->prev_code;
            page->prev_code = page->origin_code;
            page->origin_code = origin_code;
        }
        #endif

        if (old_lock_status == PAGE_UNLOCKED) break;
    }
}


/**
 * Releases the pagelock for other threads to use
 */
void release_pagelock(PAGE* page, ULONG64 origin_code) {
    
    #if DEBUG_PAGELOCK
    log_page_status(page);
    if (page->holding_threadid != GetCurrentThreadId()) {
        DebugBreak();
    }
    page->page_lock = PAGE_UNLOCKED;
    page->holding_threadid = 0;
    LeaveCriticalSection(&page->dev_page_lock);
    return;
    #endif

    #if LIGHT_DEBUG_PAGELOCK
    if (page->holding_threadid != GetCurrentThreadId()) {
        DebugBreak();
    }
    page->holding_threadid = 0;
    page->five_ago = page->four_ago;
    page->four_ago = page->three_ago;
    page->three_ago = page->two_ago;
    page->two_ago = page->prev_code;
    page->prev_code = page->origin_code;
    page->origin_code = origin_code;
    #endif

    if (InterlockedCompareExchange16(&page->page_lock, PAGE_UNLOCKED, PAGE_LOCKED) != PAGE_LOCKED) {
        DebugBreak();
    };
}


/**
 * Tries to acquire the pagelock without any spinning. 
 * 
 * Returns TRUE if successful, FALSE otherwise
 */
BOOL try_acquire_pagelock(PAGE* page, ULONG64 origin_code) {
    #if DEBUG_PAGELOCK
    if (TryEnterCriticalSection(&page->dev_page_lock)) {
        log_page_status(page);
        page->page_lock = PAGE_LOCKED;
        page->holding_threadid = GetCurrentThreadId();
        return TRUE;
    } else {
        return FALSE;
    }
    #endif


    #if LIGHT_DEBUG_PAGELOCK
    if (InterlockedCompareExchange16(&page->page_lock, PAGE_LOCKED, PAGE_UNLOCKED) == PAGE_UNLOCKED) {
        page->holding_threadid = GetCurrentThreadId();
        page->five_ago = page->four_ago;
        page->four_ago = page->three_ago;
        page->three_ago = page->two_ago;
        page->two_ago = page->prev_code;
        page->prev_code = page->origin_code;
        page->origin_code = origin_code;
        return TRUE;
    } else {
        return FALSE;
    }
    #endif
    
    return InterlockedCompareExchange16(&page->page_lock, PAGE_LOCKED, PAGE_UNLOCKED) == PAGE_UNLOCKED;

}


/**
 * Inserts the page into the list using the shared list lock scheme alongside the page locks,
 * and releases the lock on the given page before returning
 */
void insert_page2(PAGE_LISTHEAD* list, PAGE* page) {
    PAGE* listhead = &list->listhead;
    PAGE* old_head;

    AcquireSRWLockShared(&list->shared_lock);

    if (try_acquire_pagelock(listhead, 49)) {
        old_head = listhead->flink;
        if (old_head == listhead || try_acquire_pagelock(old_head, 50)) {

            /**
             * At this point, we have all of the pagelocks necessary to insert the page into the list 
             */
            listhead->flink = page;
            old_head->blink = page;
            page->blink = listhead;
            page->flink = old_head;
            InterlockedIncrement64(&list->list_length);


            release_pagelock(listhead, 52);
            release_pagelock(page, 53);

            if (old_head != listhead) {
                release_pagelock(old_head, 54);
            }

            ReleaseSRWLockShared(&list->shared_lock);
            return;

        } else {
            release_pagelock(listhead, 51);
        }
    }

    ReleaseSRWLockShared(&list->shared_lock);

    /**
     * We have the policy that we can modify the flinks/blinks of the pages with the exclusive lock
     * **without** having to acquire the pagelocks
     */
    AcquireSRWLockExclusive(&list->shared_lock);

    old_head = listhead->flink;

    listhead->flink = page;
    old_head->blink = page;
    page->blink = listhead;
    page->flink = old_head;
    list->list_length++;

    ReleaseSRWLockExclusive(&list->shared_lock);
}


/**
 * Pops a page from the list while taking advantage of the shared lock and pagelock scheme
 * 
 * Assumes you already own the pagelock of the tail page
 * 
 * Returns a pointer to the popped page if successful with its pagelock acquired, NULL otherwise. 
 */
PAGE* pop_page2(PAGE_LISTHEAD* list) {
    PAGE* listhead = &list->listhead;
    PAGE* page_to_pop;
    PAGE* new_tail;

    // Pre-emptively return NULL without having to acquire any locks. The list might repopulate immediately after, though
    if (list->list_length == 0) {
        return NULL;
    }

    
    AcquireSRWLockShared(&list->shared_lock);

    if (try_acquire_pagelock(listhead, 55)) {
        page_to_pop = listhead->blink;

        if (page_to_pop == listhead) {
            release_pagelock(listhead, 56);

            ReleaseSRWLockShared(&list->shared_lock);
            return NULL;
        }

        if (try_acquire_pagelock(page_to_pop, 57)) {
            
            new_tail = page_to_pop->blink;

            if (try_acquire_pagelock(new_tail, 58)) {
                
                /**
                 * Here, we have all three of the pagelocks required to correctly modify the list
                 */

                release_pagelock(listhead, 61);
                release_pagelock(new_tail, 62);

                ReleaseSRWLockShared(&list->shared_lock);
                return page_to_pop;
            } else {    
                release_pagelock(listhead, 59);
                release_pagelock(page_to_pop, 60);
            }

        } else {
            release_pagelock(listhead, 51);
        }
    }

    ReleaseSRWLockShared(&list->shared_lock);

    /**
     * We have the policy that we can modify the flinks/blinks of the pages with the exclusive lock
     * **without** having to acquire the pagelocks
     */
    AcquireSRWLockExclusive(&list->shared_lock);

    ReleaseSRWLockExclusive(&list->shared_lock);

    return NULL;
}


/**
 * Returns TRUE if the given page is in the list, FALSE otherwise
 */
static BOOL page_is_in_list(PAGE* page_to_find, PAGE** page_list, ULONG64 list_length) {
    if (list_length == 0) return FALSE;

    for (ULONG64 i = 0; i < list_length; i++) {
        if (page_list[i] == page_to_find) return TRUE;
    }

    return FALSE;
}


/**
 * Removes all of the given pages from the list, assumes that all of the pagelocks are held.
 * 
 * Takes advantage of the shared lock scheme.
 * 
 * This function is intended for use with rescuing a batch of pages from the modified/standby list, 
 * where the batch of pages might not be adjacent to eachother
 */
void unlink_batch_scattered_pages(PAGE_LISTHEAD* list, PAGE** pages_to_remove, ULONG64 num_pages) {
    if (num_pages == 0) {
        return;
    }

    PAGE* ahead;
    PAGE* behind;
    ULONG64 page_idx;
    PAGE* curr_page;
    
    // This helps us distinguish from pagelocks we acquire from neighboring nodes versus those we already hold
    BOOL acquired_unique_pagelock_ahead;
    BOOL acquired_unique_pagelock_behind;

    AcquireSRWLockShared(&list->shared_lock);

    for (page_idx = 0; page_idx < num_pages; page_idx++) {
        curr_page = pages_to_remove[page_idx];

        ahead = curr_page->flink;
        behind = curr_page->blink;

        acquired_unique_pagelock_ahead = try_acquire_pagelock(ahead, 79);

        // If we have already acquired the pagelock or it is somewhere in our list of pages ahead of us, then we can keep going
        // Note that the page_idx + 1 will never cause an overflow in page_is_in_list function as the given list_length is 0
        if (acquired_unique_pagelock_ahead || page_is_in_list(ahead, &pages_to_remove[page_idx + 1], num_pages - page_idx - 1)) {
            
            acquired_unique_pagelock_behind = try_acquire_pagelock(behind, 80);

            if (acquired_unique_pagelock_behind || page_is_in_list(ahead, &pages_to_remove[page_idx + 1], num_pages - page_idx - 1) 
                                || ahead == behind) {
                
                ahead->blink = behind;
                behind->flink = ahead;

                /**
                 * We need to check that we acquired pagelocks that were NOT in our list already before we release them
                 */
                if (acquired_unique_pagelock_ahead) {
                    release_pagelock(ahead, 82);
                }

                if (acquired_unique_pagelock_behind) {
                    release_pagelock(behind, 83);
                }

            // Only release the pagelock if it wasn't in our list
            } else if (acquired_unique_pagelock_ahead) {
                release_pagelock(ahead, 81);
                break;
            } else {
                break;
            }
            
        } else {
            break;
        }
    }

    ReleaseSRWLockShared(&list->shared_lock);

    if (page_idx == num_pages) {
        InterlockedAdd64(&list->list_length, - num_pages);
        return;
    }


    // We no longer need the other pagelocks in order to unlink the pages
    AcquireSRWLockExclusive(&list->shared_lock);

    // Remove the pages that we couldn't remove before
    while (page_idx < num_pages) {
        curr_page = pages_to_remove[page_idx];

        ahead = curr_page->flink;
        behind = curr_page->blink;

        ahead->blink = behind;
        behind->flink = ahead;

        page_idx++;
    }
    
    ReleaseSRWLockExclusive(&list->shared_lock);

    InterlockedAdd64(&list->list_length, - num_pages);
}   


/**
 * Unlinks the page from its list, and takes advantage of the shared lock and pagelock scheme to avoid colliding with other unlinkers
 * and threads inserting/popping from the ends of the list
 * 
 * Assumes that you already hold the lock for the page that you are trying to unlink
 */
void unlink_page2(PAGE_LISTHEAD* list, PAGE* page) {
    if (page->status == LISTHEAD_STATUS) {
        DebugBreak();
    }

    PAGE* ahead;
    PAGE* behind;

    AcquireSRWLockShared(&list->shared_lock);

    ahead = page->flink;
    behind = page->blink;

    if (try_acquire_pagelock(ahead, 63)) {
        if (ahead == behind || try_acquire_pagelock(behind, 64)) {

            /**
             * At this point, we have all of the pagelocks necessary to insert the page into the list 
             */
            ahead->blink = behind;
            behind->flink = ahead;
            InterlockedDecrement64(&list->list_length);

            release_pagelock(ahead, 65);

            if (ahead != behind) {
                release_pagelock(behind, 66);
            }

            ReleaseSRWLockShared(&list->shared_lock);
            return;

        } else {
            release_pagelock(ahead, 67);
        }
    }

    ReleaseSRWLockShared(&list->shared_lock);


    // We no longer need the other pagelocks in order to unlink this page
    AcquireSRWLockExclusive(&list->shared_lock);

    ahead = page->flink;
    behind = page->blink;

    ahead->blink = behind;
    behind->flink = ahead;
    list->list_length--;

    ReleaseSRWLockExclusive(&list->shared_lock);
}   


/**
 * Inserts the chain of pages between the beginning and end at the listhead,
 * where the beginning node will be closest to the head 
 * 
 * Takes advantage of the shared lock and pagelock scheme. Does NOT release the pagelocks for each node in the section
 */
void insert_page_section2(PAGE_LISTHEAD* list, PAGE* beginning, PAGE* end, ULONG64 num_pages) {

    PAGE* listhead = &list->listhead;
    PAGE* old_head;

    AcquireSRWLockShared(&list->shared_lock);

    if (try_acquire_pagelock(listhead, 68)) {
        old_head = listhead->flink;
        if (old_head == listhead || try_acquire_pagelock(old_head, 69)) {

            /**
             * At this point, we have all of the pagelocks necessary to insert the page into the list 
             */
            listhead->flink = beginning;
            old_head->blink = end;
            beginning->blink = listhead;
            end->flink = old_head;

            InterlockedAdd64(&list->list_length, num_pages);

            release_pagelock(listhead, 70);

            if (old_head != listhead) {
                release_pagelock(old_head, 72);
            }

            ReleaseSRWLockShared(&list->shared_lock);
            return;

        } else {
            release_pagelock(listhead, 71);
        }
    }

    ReleaseSRWLockShared(&list->shared_lock);

    /**
     * We have the policy that we can modify the flinks/blinks of the pages with the exclusive lock
     * **without** having to acquire the pagelocks
     */
    AcquireSRWLockExclusive(&list->shared_lock);

    old_head = listhead->flink;

    listhead->flink = beginning;
    old_head->blink = end;
    beginning->blink = listhead;
    end->flink = old_head;
    
    InterlockedAdd64(&list->list_length, num_pages);

    ReleaseSRWLockExclusive(&list->shared_lock);
}


/**
 * Removes the section of pages from the list they are in,
 * where the beginning node is closest to the head and the end node is closest to the tail
 * 
 * Assumes that the pagelocks for the nodes including and between the beginning and the end are all held
 */
void remove_page_section2(PAGE_LISTHEAD* list, PAGE* beginning, PAGE* end, ULONG64 num_pages) {

    PAGE* ahead;
    PAGE* behind;

    AcquireSRWLockShared(&list->shared_lock);

    ahead = end->flink;
    behind = beginning->blink;

    if (try_acquire_pagelock(ahead, 73)) {

        if (ahead == behind || try_acquire_pagelock(behind, 74)) {

            behind->flink = ahead;
            ahead->blink = behind;

            InterlockedAdd64(&list->list_length, - num_pages);

            release_pagelock(ahead, 75);

            if (ahead != behind) {
                release_pagelock(behind, 76);
            }

            ReleaseSRWLockShared(&list->shared_lock);

            return;
        } else {
            release_pagelock(ahead, 77);
        }

    }

    ReleaseSRWLockShared(&list->shared_lock);

    AcquireSRWLockExclusive(&list->shared_lock);

    ahead = end->flink;
    behind = beginning->blink;

    behind->flink = ahead;
    ahead->blink = behind;

    InterlockedAdd64(&list->list_length, - num_pages);

    ReleaseSRWLockExclusive(&list->shared_lock);
}


/**
 * Inserts the given page to the head of the list
 */
void insert_page(PAGE* listhead, PAGE* page) {
    listhead->flink->blink = page;
    page->flink = listhead->flink;
    page->blink = listhead;
    listhead->flink = page;
}


/**
 * Pops a page from the tail of the list
 * 
 * Returns a pointer to the popped page, or NULL if the list is empty
 */
PAGE* pop_page(PAGE* listhead) {
    if (listhead->flink == listhead) {
        return NULL;
    }

    PAGE* page_to_pop = listhead->blink;

    page_to_pop->blink->flink = listhead;
    listhead->blink = page_to_pop->blink;

    if (page_to_pop->status == LISTHEAD_STATUS) {
        DebugBreak();
    }

    return page_to_pop;
}


/**
 * Unlinks the given page from its list
 */
void unlink_page(PAGE* page) {
    if (page->status == LISTHEAD_STATUS) {
        DebugBreak();
    }

    page->blink->flink = page->flink;
    page->flink->blink = page->blink;
}


/**
 * Removes the section of pages from the list they are in,
 * where the beginning node is closest to the head and the end node is closest to the tail
 */
void remove_page_section(PAGE* beginning, PAGE* end) {
    
    PAGE* closer_to_head = beginning->blink;
    PAGE* closer_to_tail = end->flink;

    closer_to_head->flink = closer_to_tail;
    closer_to_tail->blink = closer_to_head;
}


/**
 * Inserts the chain of pages between the beginning and end at the listhead,
 * where the beginning node will be closest to the head 
 */
void insert_page_section(PAGE* listhead, PAGE* beginning, PAGE* end) {
    PAGE* old_head = listhead->flink;

    listhead->flink = beginning;
    beginning->blink = listhead;

    end->flink = old_head;
    old_head->blink = end;
}


/**
 * Returns TRUE if the page is in the free status, FALSE otherwise
 */
BOOL page_is_free(PAGE page) {
    return page.status == FREE_STATUS;
}


/**
 * Returns TRUE if the page is in the modified status, FALSE otherwise
 */
BOOL page_is_modified(PAGE page) {
    return page.status == MODIFIED_STATUS;
}


/**
 * Returns TRUE if the page is in the standby status, FALSE otherwise
 */
BOOL page_is_standby(PAGE page) {
    return page.status == STANDBY_STATUS;
}

/**
 * #######################################
 * ZEROED PAGES LIST STRUCTS AND FUNCTIONS
 * #######################################
 */

/**
 * Initializes the zeroed frame lists with all of the initial physical memory in the system
 * 
 * Returns a pointer to the zero lists if successful, NULL otherwise
 */
ZEROED_PAGES_LISTS* initialize_zeroed_lists(PAGE* page_storage_base, PULONG_PTR physical_frame_numbers, ULONG64 num_physical_frames) {
    ZEROED_PAGES_LISTS* zeroed_lists = (ZEROED_PAGES_LISTS*) malloc(sizeof(ZEROED_PAGES_LISTS));

    if (zeroed_lists == NULL) {
        fprintf(stderr, "Unable to allocate memory for zeroed_lists");
        return NULL;
    }

    for (int new_list = 0; new_list < NUM_CACHE_SLOTS; new_list++) {
        
        zeroed_lists->listheads[new_list].listhead.status = LISTHEAD_STATUS;
        zeroed_lists->listheads[new_list].listhead.flink = &zeroed_lists->listheads[new_list].listhead;
        zeroed_lists->listheads[new_list].listhead.blink = &zeroed_lists->listheads[new_list].listhead;

        zeroed_lists->listheads[new_list].list_length = 0;

        InitializeSRWLock(&zeroed_lists->listheads[new_list].shared_lock);
    }

    // Add all the physical frames to their respective zero lists
    for (int pfn_idx = 0; pfn_idx < num_physical_frames; pfn_idx++) {
        ULONG64 frame_number = physical_frame_numbers[pfn_idx];

        // Modulo operation based on the pfn to put it alongside other cache-colliding pages
        int listhead_idx = frame_number % NUM_CACHE_SLOTS;

        PAGE_LISTHEAD* relevant_listhead = &zeroed_lists->listheads[listhead_idx];

        PAGE* listhead_page = &relevant_listhead->listhead;
        
        PAGE* zero_frame = page_storage_base + frame_number;

        if (zero_frame == NULL) {
            fprintf(stderr, "Unable to find the page associated with the pfn in initialize_zeroed_lists\n");
            return NULL;
        }

        insert_page(listhead_page, zero_frame);

        zero_frame->status = ZERO_STATUS;

        relevant_listhead->list_length++;
    }
  
    zeroed_lists->total_available = num_physical_frames;

    return zeroed_lists;
}


/**
 * ##########################
 * FREE FRAMES LIST FUNCTIONS
 * ##########################
 */


/**
 * Creates the free frames list structure and its associated listheads and locks
 * 
 * Returns a memory allocated pointer to a FREE_FRAMES_LISTS struct, or NULL if an error occurs
 */
FREE_FRAMES_LISTS* initialize_free_frames() {
    FREE_FRAMES_LISTS* free_frames = (FREE_FRAMES_LISTS*) malloc(sizeof(FREE_FRAMES_LISTS));

    if (free_frames == NULL) {
        fprintf(stderr, "Unable to allocate memory for free frames lists");
        return NULL;
    }

    for (int new_list = 0; new_list < NUM_CACHE_SLOTS; new_list++) {
        free_frames->listheads[new_list].listhead.status = LISTHEAD_STATUS;
        free_frames->listheads[new_list].listhead.flink = &free_frames->listheads[new_list].listhead;
        free_frames->listheads[new_list].listhead.blink = &free_frames->listheads[new_list].listhead;
        free_frames->listheads[new_list].list_length = 0;

        InitializeSRWLock(&free_frames->listheads[new_list].shared_lock);
    }

    free_frames->total_available = 0;

    return free_frames;
}


/**
 * #######################
 * MODIFIED LIST FUNCTIONS
 * #######################
 */


/**
 * Allocates memory for and initializes a modified list struct
 * 
 * Returns a pointer to the modified list or NULL upon error
 */
PAGE_LISTHEAD* initialize_modified_list() {
    PAGE_LISTHEAD* modified_list = (PAGE_LISTHEAD*) malloc(sizeof(PAGE_LISTHEAD));

    if (modified_list == NULL) {
        fprintf(stderr, "Unable to allocate memory for modified list in initialize_modified_list\n");
        return NULL;
    }

    modified_list->listhead.status = LISTHEAD_STATUS;
    modified_list->listhead.flink = &modified_list->listhead;
    modified_list->listhead.blink = &modified_list->listhead;    

    modified_list->list_length = 0;

    InitializeSRWLock(&modified_list->shared_lock);

    return modified_list;
}

#if 0
/**
 * Adds the given page to the modified list (at the head)
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 */
int modified_add_page(PAGE* page, PAGE_LISTHEAD* modified_list) {
    if (page == NULL || modified_list == NULL) {
        fprintf(stderr, "NULL page or modified list given to modified_add_page\n");
        return ERROR;
    }

    insert_page(&modified_list->listhead, page);
    
    page->status = MODIFIED_STATUS;
    modified_list->list_length += 1;

    return SUCCESS;
}


/**
 * Pops the oldest page (tail) from the modified list and returns it
 * 
 * Returns NULL upon any error or if the list is empty
 */
PAGE* modified_pop_page(PAGE_LISTHEAD* modified_list) {
    if (modified_list == NULL) {
        fprintf(stderr, "NULL standby list given to modified_pop_page\n");
        return NULL;
    }

    PAGE* popped = pop_page(&modified_list->listhead);
    
    if (popped != NULL) {
        modified_list->list_length -= 1;
    }

    return popped;
}   
#endif


/**
 * ######################
 * STANDBY LIST FUNCTIONS
 * ######################
 */

DECLSPEC_ALIGN(64) PAGE_LISTHEAD actual_standby_list;

/**
 * Allocates memory for and initializes a standby list struct
 * 
 * Returns a pointer to the standby list or NULL upon error
 */
PAGE_LISTHEAD* initialize_standby_list() {
    #if 0
    PAGE_LISTHEAD* standby_list = &actual_standby_list; //(PAGE_LISTHEAD*) malloc(sizeof(PAGE_LISTHEAD));
    #else
    PAGE_LISTHEAD* standby_list = (PAGE_LISTHEAD*) malloc(sizeof(PAGE_LISTHEAD));
    #endif


    standby_list->listhead.status = LISTHEAD_STATUS;
    standby_list->listhead.flink = &standby_list->listhead;
    standby_list->listhead.blink = &standby_list->listhead;
    
    standby_list->list_length = 0;

    InitializeSRWLock(&standby_list->shared_lock);

    return standby_list;
}

#if 0
/**
 * Adds the given page to the standby list
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 */
int standby_add_page(PAGE* page, STANDBY_LIST* standby_list) {
    if (page == NULL || standby_list == NULL) {
        fprintf(stderr, "NULL page or standby list given to standby_add_page\n");
        return ERROR;
    }
    
    page->status = STANDBY_STATUS;

    insert_page(&standby_list->listhead, page);

    standby_list->list_length += 1;

    return SUCCESS;
}
#endif