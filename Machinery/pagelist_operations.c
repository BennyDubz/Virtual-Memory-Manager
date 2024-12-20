/**
 * @author Ben Williams
 * @date July 19th, 2024
 * 
 * All operations required to zeroing out pages
 */

#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <windows.h>
#include "../Datastructures/datastructures.h"
#include "./pagelist_operations.h"
#include "./conversions.h"
#include "../globals.h"
#include "../macros.h"

#define ZERO_REFRESH_BOUNDARY  NUM_FAULTER_ZERO_SLOTS / 2
long zero_slot_statuses[NUM_FAULTER_ZERO_SLOTS];
ULONG64 total_available_zero_slots;
PULONG_PTR faulter_zero_base_addr;

/**
 * Returns an index to the page zeroing struct's status/page storage that is likely to be open
 * if the zeroing thread has kept up
 * 
 * This is done by doing an interlocked increment on the current index, and using the modulo operation to
 * ensure the list wraps back around
 */
ULONG64 get_zeroing_struct_idx() {
    return InterlockedIncrement64(&page_zeroing->curr_idx) % NUM_THREAD_ZERO_SLOTS;
}


/**
 * Used as the compare function for quicksorting an array of pages by their PTE's virtual address
 * 
 * This allows the pages to be sorted by PTE locksection, ensuring that we only have to enter each locksection once
 */
static int page_pte_compare(const void* page1, const void* page2) {
    PAGE* p1 = (PAGE*) page1;
    PAGE* p2 = (PAGE*) page2;

    if ((ULONG_PTR) p1->pte < (ULONG_PTR) p2->pte) {
        return -1;
    } else if ((ULONG_PTR) p1->pte == (ULONG_PTR) p2->pte) {
        return 0;
    } else {
        return 1;
    }
}


/**
 * Used as the compare function for quicksorting an array of pages by their cache color
 * 
 * This allows us to only enter the lock for each free-list and zero-list section once
 */
static int page_cache_color_compare(const void* page1, const void* page2) {
    PAGE* p1 = (PAGE*) page1;
    PAGE* p2 = (PAGE*) page2;

    if (page_to_pfn(p1) % NUM_CACHE_SLOTS < page_to_pfn(p2) % NUM_CACHE_SLOTS) {
        return -1;
    } else if (page_to_pfn(p1) % NUM_CACHE_SLOTS == page_to_pfn(p2) % NUM_CACHE_SLOTS) {
        return 0;
    } else {
        return 1;
    }
}


/**
 * Updates all of the PTEs in each section to reflect that their pages are being repurposed and they
 * are now on the disk
 */
static void update_pte_sections_to_disk_format(PAGE** page_list, ULONG64 num_pages) {
    PTE* curr_pte;
    PAGE* curr_page;
    PTE pte_copy;

    // The first and last field will always be zero for all the PTEs we update
    PTE pte_contents;
    pte_contents.complete_format = 0;

    for (ULONG64 page_idx = 0; page_idx < num_pages; page_idx++) {
        curr_page = page_list[page_idx];
        curr_pte = curr_page->pte;
        pte_copy = read_pte_contents(curr_pte);

        if (is_transition_format(*curr_pte) == FALSE) DebugBreak();

        pte_contents.disk_format.pagefile_idx = curr_page->pagefile_idx;

        /**
         * We need to force a write here, as we must not collide with the end of the trimming thread when pages are added
         * to the standby list. See more info there
         */
        while (InterlockedCompareExchange64((ULONG64*) curr_pte, pte_contents.complete_format, pte_copy.complete_format) != pte_copy.complete_format) {
            pte_copy = read_pte_contents(curr_pte);
        }
    }

}


/**
 * Adds all of the pages from each page section to their free list buckets, and releases all the pagelocks
 */
static void free_frames_add_batch(PAGE** page_list, ULONG64 num_pages) {
    PAGE* curr_page;

    /**
     * Here, we sort the page_list by each page's cache color - which is determined by the cache size and each page's
     * respective physical frame number. This allows us to enter each free_list/zero_list locksection only once.
     */
    qsort(page_list, num_pages, sizeof(PAGE*), page_cache_color_compare);

    create_chain_of_pages(page_list, num_pages, FREE_STATUS);

    /**
     * Since we are typically using standby pages to add to the free list, the PTEs should be cleared
     */
    for (ULONG64 release_idx = 0; release_idx < num_pages; release_idx++) {
        page_list[release_idx]->pte = NULL;
    }

    ULONG64 curr_cache_color = page_to_pfn(page_list[0]) % NUM_CACHE_SLOTS;
    ULONG64 page_cache_color;

    ULONG64 first_in_section_idx = 0;
    ULONG64 last_page_in_section_idx = 0;

    PAGE_LIST* curr_listhead = &free_frames->listheads[curr_cache_color];

    ULONG64 page_idx = 0;
    ULONG64 page_section_size;

    for (ULONG64 page_idx = 0; page_idx < num_pages; page_idx++) {
        page_cache_color = page_to_pfn(page_list[page_idx]) % NUM_CACHE_SLOTS;

        if (curr_cache_color != page_cache_color) {
            page_section_size = page_idx - first_in_section_idx;

            InterlockedAdd64(&free_frames->total_available, page_section_size);
            InterlockedAdd64(&total_available_pages, page_section_size);
            insert_page_section(curr_listhead, page_list[first_in_section_idx], page_list[page_idx - 1], page_section_size);
            

            // Adjust the indices, cache color, and locksection to the next section of pages
            first_in_section_idx = page_idx;
            curr_cache_color = page_cache_color;
            curr_listhead = &free_frames->listheads[curr_cache_color];
        }
    }
    
    page_section_size = num_pages - first_in_section_idx;

    InterlockedAdd64(&free_frames->total_available, page_section_size);
    InterlockedAdd64(&total_available_pages, page_section_size);
    insert_page_section(curr_listhead, page_list[first_in_section_idx], page_list[num_pages - 1], page_section_size);

    SetEvent(waiting_for_pages_event);
}


/**
 * Adds all of the remaining (not NULL) pages in each section to the zero list and releases all of the pagelocks
 */
static void zero_list_add_batch(PAGE** page_list, ULONG64 num_pages) {
    PAGE* curr_page;

    /**
     * Here, we sort the page_list by each page's cache color - which is determined by the cache size and each page's
     * respective physical frame number. This allows us to enter each free_list/zero_list locksection only once.
     */
    qsort(page_list, num_pages, sizeof(PAGE*), page_cache_color_compare);

    create_chain_of_pages(page_list, num_pages, FREE_STATUS);

    /**
     * Since we are typically using standby pages to add to the zero list, the PTEs should be cleared
     */
    for (ULONG64 release_idx = 0; release_idx < num_pages; release_idx++) {
        page_list[release_idx]->pte = NULL;
    }

    ULONG64 curr_cache_color = page_to_pfn(page_list[0]) % NUM_CACHE_SLOTS;
    ULONG64 page_cache_color;

    ULONG64 first_in_section_idx = 0;
    ULONG64 last_page_in_section_idx = 0;

    PAGE_LIST* curr_listhead = &zero_lists->listheads[curr_cache_color];

    ULONG64 page_idx = 0;
    ULONG64 page_section_size;

    for (ULONG64 page_idx = 0; page_idx < num_pages; page_idx++) {
        page_cache_color = page_to_pfn(page_list[page_idx]) % NUM_CACHE_SLOTS;

        if (curr_cache_color != page_cache_color) {
            page_section_size = page_idx - first_in_section_idx;

            InterlockedAdd64(&zero_lists->total_available, page_section_size);
            InterlockedAdd64(&total_available_pages, page_section_size);
            insert_page_section(curr_listhead, page_list[first_in_section_idx], page_list[page_idx - 1], page_section_size);

            // Adjust the indices, cache color, and locksection to the next section of pages
            first_in_section_idx = page_idx;
            curr_cache_color = page_cache_color;
            curr_listhead = &zero_lists->listheads[curr_cache_color];

        }
    }
    
    page_section_size = num_pages - first_in_section_idx;

    InterlockedAdd64(&zero_lists->total_available, page_section_size);
    InterlockedAdd64(&total_available_pages, page_section_size);

    insert_page_section(curr_listhead, page_list[first_in_section_idx], page_list[num_pages - 1], page_section_size);

    SetEvent(waiting_for_pages_event);
}


/**
 * Gets all of the pages from the global pages_to_zero_storage and uses interlocked operations on the pages_to_zero_map
 * to mark which pages we took, and writes the pages into page_storage as well as their frame numbers into pfn_storage
 * 
 * Returns the number of pages written
 */
static ULONG64 zero_list_get_pages_to_clear(PAGE** page_storage, ULONG64* pfn_storage) {
    ULONG64 page_count = 0;
    PAGE* curr_page;
    UCHAR old_val;
    for (ULONG64 i = 0; i < NUM_THREAD_ZERO_SLOTS; i++) {
        if (page_zeroing->status_map[i] == PAGE_SLOT_READY) {
            curr_page = page_zeroing->pages_to_zero[i];
            page_storage[page_count] = curr_page;
            pfn_storage[page_count] = page_to_pfn(curr_page);

            acquire_pagelock(curr_page, 22);
            page_count++;

            // Clear the status slot - its corresponding page can now be overwritten
            InterlockedAnd(&page_zeroing->status_map[i], PAGE_SLOT_OPEN);
            InterlockedDecrement64(&page_zeroing->total_slots_used);
        }
    }

    return page_count;
}


/**
 * Thread dedicated to taking pages from standby, zeroing them, and
 * adding them to the zero lists for quicker access
 */
// #define MAX_PAGES_PER_SECTION 64
LPTHREAD_START_ROUTINE thread_populate_zero_lists(void* parameters) {
    WORKER_THREAD_PARAMETERS* thread_params = (WORKER_THREAD_PARAMETERS*) parameters;

    MEM_EXTENDED_PARAMETER* vmem_parameters = (MEM_EXTENDED_PARAMETER*) thread_params->other_parameters;
    ULONG64 worker_thread_idx = thread_params->thread_idx;

    #if DEBUG_THREAD_STORAGE
    thread_information.thread_local_storages[worker_thread_idx].thread_id = GetCurrentThreadId();
    #endif

    PULONG_PTR thread_zeroing_address;

    thread_zeroing_address = VirtualAlloc2(NULL, NULL, PAGE_SIZE * NUM_THREAD_ZERO_SLOTS,
                        MEM_PHYSICAL | MEM_RESERVE, PAGE_READWRITE, vmem_parameters, 1);


    if (thread_zeroing_address == NULL) {
        fprintf(stderr, "Unable to reserve memory for thread_zeroing_address in thread_populate_zero_lists\n");
        return NULL;
    }


    
    PAGE** page_list = (PAGE**) malloc(sizeof(PAGE*) * NUM_THREAD_ZERO_SLOTS);


    if (page_list == NULL) {
        fprintf(stderr, "Unable to allocate memory for page_list in thread_populate_zero_lists\n");
        return NULL;
    }

    ULONG64* pfn_list = (ULONG64*) malloc(sizeof(ULONG64) * NUM_THREAD_ZERO_SLOTS);

    if (pfn_list == NULL) {
        fprintf(stderr, "Unable to allocate memory for pfn_list in thread_populate_zero_lists\n");
        return NULL;
    }

    
    HANDLE events[2];
    ULONG64 signaled_event;

    events[0] = zero_pages_event;
    events[1] = shutdown_event;
     
    while (TRUE) {
        signaled_event = WaitForMultipleObjects(2, events, FALSE, INFINITE);

        if (signaled_event == 1) {
            free(page_list);
            free(pfn_list);
            VirtualFree(thread_zeroing_address, 0, MEM_RELEASE);
            return NULL;
        }

        InterlockedOr(&page_zeroing->zeroing_ongoing, TRUE);

        ULONG64 num_to_zero = zero_list_get_pages_to_clear(page_list, pfn_list);

        if (num_to_zero == 0) {
            InterlockedAnd(&page_zeroing->zeroing_ongoing, FALSE);
            continue;
        }

        if (MapUserPhysicalPages(thread_zeroing_address, num_to_zero, pfn_list) == FALSE) {
            fprintf(stderr, "Failed to map zeroing address in thread_populate_zero_lists\n");
            DebugBreak();
        }

        memset(thread_zeroing_address, 0, PAGE_SIZE * num_to_zero);

        if (MapUserPhysicalPages(thread_zeroing_address, num_to_zero, NULL) == FALSE) {
            fprintf(stderr, "Failed to unmap zeroing address in thread_populate_zero_lists\n");
            DebugBreak();
        }

        InterlockedAnd(&page_zeroing->zeroing_ongoing, FALSE);

        // Get all of the pages from the standby list into our sections        
        zero_list_add_batch(page_list, num_to_zero);

        // We are unlikely to need to work immediately, we should potentially do some extra work while we are awake
        // In this case, we might actually signal ourselves to work again if we need to zero out more pages!
        if (page_zeroing->total_slots_used < NUM_THREAD_ZERO_SLOTS / 4) {
            potential_list_refresh(worker_thread_idx);
        }

    }

    DebugBreak();
    return NULL; // To please the compiler

}


/**
 * Used to initialize the virtual addresses, lists, and variables used for zeroing out pages
 */
int initialize_page_zeroing(MEM_EXTENDED_PARAMETER* vmem_parameters) {
    faulter_zero_base_addr = VirtualAlloc2(NULL, NULL, PAGE_SIZE * NUM_FAULTER_ZERO_SLOTS,
                        MEM_PHYSICAL | MEM_RESERVE, PAGE_READWRITE, vmem_parameters, 1);

    if (faulter_zero_base_addr == NULL) {
        fprintf(stderr, "Unable to allocate memory for faulter_zero_base_addr in initialize_page_zeroing\n");
        return ERROR;
    }

    // All slots start as open
    for (ULONG64 i = 0; i < NUM_FAULTER_ZERO_SLOTS; i++) {
        zero_slot_statuses[i] = ZERO_SLOT_OPEN;
    }

    total_available_zero_slots = NUM_FAULTER_ZERO_SLOTS;

    return SUCCESS;
}


/**
 * Finds num_slots open slots and writes them into zero_slot_indices_storage while setting
 * them to be ZERO_SLOT_USED
 * 
 * Returns the number of slots successfully allocated
 */
static ULONG64 acquire_batch_zero_slots(ULONG64* zero_slot_indices_storage, ULONG64 num_slots) {
    long read_old_val;
    volatile long* zero_slot;
    ULONG64 num_allocated = 0;

    for (ULONG64 slot_idx = 0; slot_idx < NUM_FAULTER_ZERO_SLOTS; slot_idx++) {
        zero_slot = &zero_slot_statuses[slot_idx];
        
        if (*zero_slot == ZERO_SLOT_OPEN) {
            read_old_val = InterlockedCompareExchange(zero_slot, ZERO_SLOT_USED, ZERO_SLOT_OPEN);
            
            // We successfully claimed the zero slot
            if (read_old_val == ZERO_SLOT_OPEN) {
                zero_slot_indices_storage[num_allocated] = slot_idx;
                InterlockedDecrement64(&total_available_zero_slots);
                num_allocated++;

                if (num_allocated == num_slots) {
                    return num_allocated;
                }
            }
        }
    }

    return num_allocated;
}


/**
 * Finds an open zero-slot and sets it to used. Writes the index of the zero slot into the storage
 * 
 * Returns SUCCESS if a slot is allocated and written into the storage, ERROR otherwise
 */
static int acquire_zero_slot(ULONG64* zero_slot_idx_storage) {
    long read_old_val;
    volatile long* zero_slot;

    // BW: This can be made much more efficient with a bitmap implementation
    for (ULONG64 slot_idx = 0; slot_idx < NUM_FAULTER_ZERO_SLOTS; slot_idx++) {
        zero_slot = &zero_slot_statuses[slot_idx];

        if (*zero_slot == ZERO_SLOT_OPEN) {
            read_old_val = InterlockedCompareExchange(zero_slot, ZERO_SLOT_USED, ZERO_SLOT_OPEN);
            
            // We successfully claimed the zero slot
            if (read_old_val == ZERO_SLOT_OPEN) {
                custom_spin_assert(*zero_slot == ZERO_SLOT_USED);
                *zero_slot_idx_storage = slot_idx;
                InterlockedDecrement64(&total_available_zero_slots);
                return SUCCESS;
            }
        }
    }

    // There were no slots available, we will need to refresh the list and try again
    return ERROR;
}


/**
 * Marks the zero slot to be refreshed, releasing it for the current thread
 */
static void release_batch_zero_slots(ULONG64* slot_indices, ULONG64 num_slots) {
    
    for (ULONG64 release_idx = 0; release_idx < num_slots; release_idx++) {
        ULONG64 slot_index = slot_indices[release_idx];
        custom_spin_assert(zero_slot_statuses[slot_index] == ZERO_SLOT_USED);

        custom_spin_assert(InterlockedIncrement(&zero_slot_statuses[slot_index]) == ZERO_SLOT_NEEDS_FLUSH);
    }
}


/**
 * Marks the zero slot to be refreshed, releasing it for the current thread
 */
static void release_zero_slot(ULONG64 slot_idx) {
    if (zero_slot_statuses[slot_idx] != ZERO_SLOT_USED) {
        DebugBreak();
    }

    custom_spin_assert(InterlockedIncrement(&zero_slot_statuses[slot_idx]) == ZERO_SLOT_NEEDS_FLUSH);
}


/**
 * Takes all of the zero slots in the list that need to be refreshed and handle them in a single
 * batch - reducing the amount of load we need on MapUserPhysicalPagesScatter
 */
PULONG_PTR refresh_zero_addresses[NUM_FAULTER_ZERO_SLOTS];
volatile long* refresh_zero_status_slots[NUM_FAULTER_ZERO_SLOTS];
long zero_refresh_ongoing = FALSE; // We use the long for interlocked operation parameters
static void refresh_zero_slots() {
    // Synchronize whether we or someone else is refreshing the zero slots
    long old_val = InterlockedOr(&zero_refresh_ongoing, TRUE);

    if (old_val == TRUE) {
        return;
    }

    ULONG64 num_slots_refreshed = 0;
    volatile long* zero_slot;
    PULONG_PTR zero_slot_addr;

    // Find all of the slots to clear
    for (ULONG64 slot_idx = 0; slot_idx < NUM_FAULTER_ZERO_SLOTS; slot_idx++) {
        zero_slot = &zero_slot_statuses[slot_idx];

        if (*zero_slot == ZERO_SLOT_NEEDS_FLUSH) {
            refresh_zero_status_slots[num_slots_refreshed] = zero_slot;

            zero_slot_addr = faulter_zero_base_addr + (slot_idx * PAGE_SIZE / sizeof(PULONG_PTR));
            refresh_zero_addresses[num_slots_refreshed] = zero_slot_addr;
            num_slots_refreshed++;
        }
    }

    if (MapUserPhysicalPagesScatter(refresh_zero_addresses, num_slots_refreshed, NULL) == FALSE) {
        fprintf(stderr, "Error unmapping zeroing VAs in refreshzero_slots\n");
        DebugBreak();
        return;
    }

    InterlockedAdd64(&total_available_zero_slots, num_slots_refreshed);

    // Finally clear all of the slots
    for (ULONG64 zero_status_refresh = 0; zero_status_refresh < num_slots_refreshed; zero_status_refresh++) {
        InterlockedAnd(refresh_zero_status_slots[zero_status_refresh], ZERO_SLOT_OPEN);
    }

    InterlockedAnd(&zero_refresh_ongoing, FALSE);
}


/**
 * Memsets all of the virtual addresses to zero - each for a size of PAGE_SIZE
 */
static void zero_out_addresses(PULONG_PTR* virtual_addresses, ULONG64 num_addresses) {
    for (ULONG64 i = 0; i < num_addresses; i++) {
        memset(virtual_addresses[i], 0, PAGE_SIZE);
    }
}


/**
 * Given the pages that need to be cleaned, zeroes out all of the contents on their physical pages
 */
void zero_out_pages(PAGE** pages, ULONG64 num_pages) {
    if (pages == NULL || num_pages == 0 || num_pages > NUM_FAULTER_ZERO_SLOTS || pages[0] == NULL ) {
        fprintf(stderr, "NULL or invalid arguments given to zero_out_pages\n");
        DebugBreak();
    }


    PULONG_PTR zero_slot_addresses[NUM_FAULTER_ZERO_SLOTS];
    ULONG64 zero_slot_indices[NUM_FAULTER_ZERO_SLOTS];
    ULONG64 pfns[NUM_FAULTER_ZERO_SLOTS];
    ULONG64 slots_allocated_this_loop = 0;
    ULONG64 total_slots_allocated = 0;

    while ((total_slots_allocated += 
            acquire_batch_zero_slots(&zero_slot_indices[total_slots_allocated], num_pages - total_slots_allocated)) != num_pages) {

        if (zero_refresh_ongoing == FALSE) {
            // If we have a race condition here, only one thread will refresh and the other will return almost immediately
            refresh_zero_slots();
        }

    }

    // Collect all of the addresses and the PFNs
    for (ULONG64 i = 0; i < total_slots_allocated; i++) {
        zero_slot_addresses[i] = faulter_zero_base_addr + (zero_slot_indices[i] * PAGE_SIZE / sizeof(PULONG_PTR));
        pfns[i] = page_to_pfn(pages[i]);
    }


    if (MapUserPhysicalPagesScatter(zero_slot_addresses, num_pages, pfns) == FALSE) {
        DebugBreak();
    }  

    zero_out_addresses(zero_slot_addresses, num_pages);

    release_batch_zero_slots(zero_slot_indices, total_slots_allocated);

    if (total_available_zero_slots < ZERO_REFRESH_BOUNDARY && zero_refresh_ongoing == FALSE) {
        refresh_zero_slots();
    }
    
}


/**
 * Tries to acquire the pagelock for the page at the tail of the list 
 * 
 * Returns the page if its lock was acquired, NULL otherwise
 */
static PAGE* try_acquire_list_tail_pagelock(PAGE_LIST* list) {
    PAGE* pagelist_tail = &list->tail;
    BOOL pagelock_acquired = FALSE;
    PAGE* potential_page = NULL;

    while (pagelock_acquired == FALSE) {
        potential_page = pagelist_tail->blink;

        if (potential_page->status == LIST_STATUS) return NULL;

        pagelock_acquired = try_acquire_pagelock(potential_page, 4);
        
        if (pagelock_acquired && potential_page != pagelist_tail->blink) {
            release_pagelock(potential_page, 16);
            pagelock_acquired = FALSE;
            continue;
        }

        break;
    }

    // If we failed to acquire a pagelock, we cannot return a reference to a page
    if (pagelock_acquired == FALSE) {
        return NULL;
    } else {
        return potential_page;
    }
}


/** it acquires the pagelock for the tail entry of the list, if there is a tail entry
 * 
 * Spins until
 * Returns the page whose lock was acquired, or NULL if the list was empty and it was unable
 */
static PAGE* acquire_list_tail_pagelock(PAGE_LIST* list) {
    PAGE* pagelist_tail = &list->tail;
    BOOL pagelock_acquired = FALSE;
    PAGE* potential_page = NULL;

    while (pagelock_acquired == FALSE) {
        potential_page = (PAGE*) pagelist_tail->blink;

        // Check if this list section is empty
        if (potential_page->status == LIST_STATUS) return NULL;

        pagelock_acquired = try_acquire_pagelock(potential_page, 4);

        if (pagelock_acquired && potential_page != pagelist_tail->blink) {
            release_pagelock(potential_page, 15);
            pagelock_acquired = FALSE;
            continue;
        }
    }

    return potential_page;
}


/**
 * Tries to acquire num_pages pagelocks starting at the tail of the given listhead, stores the acquired pages in page_storage,
 * and returns the number of pages whose locks were acquired
 */
static ULONG64 acquire_batch_list_tail_pagelocks(PAGE_LIST* list, PAGE** page_storage, ULONG64 num_pages) {
    PAGE* curr_page;
    
    /**
     * Acquire the pagelock for the very first page in the list, once we have done this, 
     * we can walk down the list and acquire the other needed pagelocks
     * 
     * The bottleneck for popping a batch of pages off of lists using this function will almost always
     * be at the head
     */
    BOOL pagelock_acquired = FALSE;

    PAGE* first_page = acquire_list_tail_pagelock(list);
    
    if (first_page == NULL) return 0;

    page_storage[0] = first_page;
    ULONG64 num_allocated = 1;
    curr_page = first_page;
    
    PAGE* potential_page = (PAGE*) curr_page->blink;

    while (num_allocated < num_pages && potential_page->status != LIST_STATUS) {
        
        pagelock_acquired = try_acquire_pagelock(potential_page, 5);

        if (pagelock_acquired && potential_page == (PAGE*) curr_page->blink) {
            curr_page = potential_page;
            page_storage[num_allocated] = potential_page;
            num_allocated++;
        } else if (pagelock_acquired) {
            release_pagelock(potential_page, 16);
        }

        potential_page = (PAGE*) curr_page->blink;
    }

    #if DEBUG_PAGELOCK
    for (int i = 0; i < num_allocated; i++) {
        PAGE* page = page_storage[i];
        custom_spin_assert(page->page_lock == PAGE_LOCKED);
        custom_spin_assert(page->holding_threadid == GetCurrentThreadId());
        custom_spin_assert(page->status == STANDBY_STATUS);
    }
    #endif

    return num_allocated;
}


/**
 * Allocates and returns a single zeroed page, if there are any
 * 
 * Returns NULL if the list is empty
 */
PAGE* allocate_zeroed_frame() {
    
    /**
     * We can do a quick check to see if there are no zero frames left before contending for locks
     * 
     * We might still be wrong sometimes if a frame is added in before we look, but reducing contention
     * is more helpful.
     */
    if (zero_lists->total_available == 0) {
        return NULL;
    }

    BOOL pagelock_acquired = FALSE;
    PAGE_LIST* curr_listhead;
    PAGE* potential_page = NULL;

    int curr_attempts = 0;
    
    ULONG64 local_index = ReadTimeStampCounter() % NUM_CACHE_SLOTS;

    custom_spin_assert(local_index < NUM_CACHE_SLOTS);

    while (curr_attempts < MAX_ATTEMPTS_FREE_OR_ZERO_LISTS) {
        curr_listhead = &zero_lists->listheads[local_index];

        // Check for empty list - we can quickly check here before acquiring the lock
        if (curr_listhead->list_length == 0) {
            curr_attempts ++;
            local_index = (local_index + 1) % NUM_CACHE_SLOTS;
            continue;
        }

        // If multiple threads are contending on the same sections, this will help alleviate contention
        if (curr_attempts < MAX_ATTEMPTS_FREE_OR_ZERO_LISTS) {
            potential_page = try_acquire_list_tail_pagelock(curr_listhead);
        } else {
            potential_page = acquire_list_tail_pagelock(curr_listhead);
        }

        // We failed to get a page from this section, it was empty
        if (potential_page == NULL) {
            local_index = (local_index + 1) % NUM_CACHE_SLOTS;
            curr_attempts++;
            continue;
        }

        // Remove the page from the list
        unlink_page(curr_listhead, potential_page);
       
        InterlockedDecrement64(&zero_lists->total_available);
        InterlockedDecrement64(&total_available_pages);

        custom_spin_assert(potential_page->pagefile_idx == DISK_IDX_NOTUSED);

        break;
    }
    
    return potential_page;
}


/**
 * Allocates a batch of num_pages zeroed frames in order of cache slots if possible
 * 
 * Writes the pages into page_storage. Returns the number of pages successfully written/allocated
 */
ULONG64 allocate_batch_zeroed_frames(PAGE** page_storage, ULONG64 num_pages) {
    // Zero lists frames are empty
    if (zero_lists->total_available == 0) {
        return 0;
    }

    ULONG64 num_consecutive_failures = 0;
    ULONG64 num_allocated = 0;
    ULONG64 local_index = ReadTimeStampCounter() % NUM_CACHE_SLOTS;
    PAGE* curr_page;
    PAGE_LIST* curr_listhead;


    /**
     * We will continue until we fail to acquire a page several consecutive times in a row, or until
     * we have acquired the number of pages that we want
     */
    while (num_consecutive_failures < MAX_ATTEMPTS_FREE_OR_ZERO_LISTS && num_allocated < num_pages) {
        curr_listhead = &zero_lists->listheads[local_index];

        // We make an opportunistic look at the number of pages before acquiring any locks
        if (curr_listhead->list_length == 0) {
            num_consecutive_failures++;
            local_index = (local_index + 1) % NUM_CACHE_SLOTS;

            continue;
        }

        if (num_consecutive_failures < MAX_ATTEMPTS_FREE_OR_ZERO_LISTS) {
            curr_page = try_acquire_list_tail_pagelock(curr_listhead);
        } else {
            curr_page = acquire_list_tail_pagelock(curr_listhead);
        }

        // We failed to acquire a page - this list is now empty
        if (curr_page == NULL) {

            num_consecutive_failures++;

            local_index = (local_index + 1) % NUM_CACHE_SLOTS;

            continue;
        }

        unlink_page(curr_listhead, curr_page);

        page_storage[num_allocated] = curr_page;
        num_consecutive_failures = 0;
        num_allocated++;
    }

    InterlockedAdd64(&zero_lists->total_available, - num_allocated);

    custom_spin_assert(zero_lists->total_available >= 0);
    
    InterlockedAdd64(&total_available_pages, - num_allocated);

    return num_allocated;
}


/**
 * Returns a page off the free list, if there are any. Otherwise, returns NULL
 */
PAGE* allocate_free_frame() {
    
    /**
     * We can do a quick check to see if there are no zero frames left before contending for locks
     * 
     * We might still be wrong sometimes if a frame is added in before we look, but reducing contention
     * is more helpful.
     */
    if (free_frames->total_available == 0) {
        return NULL;
    }

    BOOL pagelock_acquired = FALSE;
    PAGE* potential_page = NULL;
    PAGE_LIST* curr_listhead;

    int curr_attempts = 0;
    
    ULONG64 local_index = ReadTimeStampCounter() % NUM_CACHE_SLOTS;

    while (curr_attempts < MAX_ATTEMPTS_FREE_OR_ZERO_LISTS) {
        curr_listhead = &free_frames->listheads[local_index];

        // Check for empty list - we can quickly check here before acquiring the lock
        if (curr_listhead->list_length == 0) {
            curr_attempts += 1;
            local_index = (local_index + 1) % NUM_CACHE_SLOTS;
            continue;
        }
        
        // If multiple threads are contending on the same sections, this will help alleviate contention
        if (curr_attempts < MAX_ATTEMPTS_FREE_OR_ZERO_LISTS / 2) {
            potential_page = try_acquire_list_tail_pagelock(curr_listhead);
        } else {
            potential_page = acquire_list_tail_pagelock(curr_listhead);
        }

        // We failed to get a page from this section, it was empty
        if (potential_page == NULL) {
            local_index = (local_index + 1) % NUM_CACHE_SLOTS;
            curr_attempts++;
            continue;
        }

        // Remove the page from the list
        unlink_page(curr_listhead, potential_page);
       
        InterlockedDecrement64(&free_frames->total_available);
        InterlockedDecrement64(&total_available_pages);

        custom_spin_assert(potential_page->pagefile_idx == DISK_IDX_NOTUSED);

        break;
    }

    // printf("Num attempts: %lld %p\n", curr_attempts, potential_page);
    
    return potential_page;
}


/**
 * Tries to allocate batch_size number of free frames and put them sequentially in page_storage
 * 
 * Returns the number of pages successfully allocated and put into the page_storage, but does
 * NOT acquire the pagelocks ahead of time
 */
ULONG64 allocate_batch_free_frames(PAGE** page_storage, ULONG64 batch_size) {
     // Zero lists frames are empty
    if (free_frames->total_available == 0) {
        return 0;
    }

    ULONG64 num_consecutive_failures = 0;
    ULONG64 num_allocated = 0;
    ULONG64 local_index = ReadTimeStampCounter() % NUM_CACHE_SLOTS;
    PAGE_LIST* curr_listhead;
    PAGE* curr_page;


    /**
     * We will continue until we fail to acquire a page several consecutive times in a row, or until
     * we have acquired the number of pages that we want
     */
    while (num_consecutive_failures < MAX_ATTEMPTS_FREE_OR_ZERO_LISTS && num_allocated < batch_size) {
        curr_listhead = &free_frames->listheads[local_index];

        // We make an opportunistic look at the number of pages before acquiring any locks
        if (curr_listhead->list_length == 0) {
            num_consecutive_failures++;
            local_index = (local_index + 1) % NUM_CACHE_SLOTS;

            continue;
        }

        if (num_consecutive_failures < MAX_ATTEMPTS_FREE_OR_ZERO_LISTS / 2) {
            curr_page = try_acquire_list_tail_pagelock(curr_listhead);
        } else {
            curr_page = acquire_list_tail_pagelock(curr_listhead);
        }

        // We failed to acquire a page - this list is now empty
        if (curr_page == NULL) {

            num_consecutive_failures++;

            local_index = (local_index + 1) % NUM_CACHE_SLOTS;

            continue;
        }

        unlink_page(curr_listhead, curr_page);

        page_storage[num_allocated] = curr_page;
        num_consecutive_failures = 0;
        num_allocated++;
    }

    InterlockedAdd64(&free_frames->total_available, - num_allocated);
    InterlockedAdd64(&total_available_pages, - num_allocated);

    return num_allocated;
}


/**
 * Updates all of the threads list refresh statuses to the given parameter
 */
static void refresh_update_thread_storages(UCHAR list_refresh_status) {
    for (ULONG64 i = 0; i < thread_information.total_thread_count; i++) {
        thread_information.thread_local_storages[i].list_refresh_status = list_refresh_status;
    }
}


/**
 * To be called by the faulting thread when the total of free frames + zero frames is low
 * 
 * Takes many frames off of the standby list and uses some to populate the free frames list, while also
 * adding some to the zeroing-threads buffer. If there are enough pages for the zeroing thread to zero-out,
 * then it will be signaled
 */
#if ONLY_ONE_REFRESHER
long list_refresh_ongoing = FALSE;
#endif

static void refresh_free_and_zero_lists(PAGE** page_storage) {
    
    #if ONLY_ONE_REFRESHER
    if (InterlockedOr(&list_refresh_ongoing, TRUE) == TRUE) {
        return;
    }
    #endif

    ULONG64 num_allocated = standby_pop_batch(page_storage, NUM_PAGES_THREAD_REFRESH);

    // The standby list is empty!
    if (num_allocated == 0) {
        SetEvent(trimming_event);

        #if ONLY_ONE_REFRESHER
        InterlockedAnd(&list_refresh_ongoing, FALSE);
        #endif

        return;
    }

    // Update the old transition PTEs to be disk format
    update_pte_sections_to_disk_format(page_storage, num_allocated);

    // Add half the pages to the free list, and releases their pagelocks to be used freely
    free_frames_add_batch(page_storage, min(num_allocated, NUM_PAGES_THREAD_REFRESH / FREE_FRAMES_PROPORTION));

    // See if we still have pages remaining to add to the zeroing threads buffer
    if (num_allocated < (NUM_PAGES_THREAD_REFRESH / FREE_FRAMES_PROPORTION) + 1) { // + 1 to reflect the indices used
        // There are still very few pages on standby...
        SetEvent(trimming_event);

        #if ONLY_ONE_REFRESHER
        InterlockedAnd(&list_refresh_ongoing, FALSE);
        #endif

        return;
    }

    ULONG64 zeroing_start = NUM_PAGES_THREAD_REFRESH / FREE_FRAMES_PROPORTION;

    long old_slot_val;

    PAGE* curr_page;

    ULONG64 num_to_zero = 0;
    ULONG64 extra_to_free = 0;

    // Add the other half to be zeroed out!
    for (ULONG64 page_idx = zeroing_start; page_idx < num_allocated; page_idx++) {
        ULONG64 page_zeroing_idx = get_zeroing_struct_idx();

        // We try to claim the slot
        old_slot_val = InterlockedCompareExchange(&page_zeroing->status_map[page_zeroing_idx], PAGE_SLOT_CLAIMED, PAGE_SLOT_OPEN);
        
        // The zeroing thread has failed to keep up - we should just add these to the free frames list
        if (old_slot_val != PAGE_SLOT_OPEN) {
            SetEvent(zero_pages_event);
            ULONG64 remaining_pages = num_allocated - page_idx;
            free_frames_add_batch(&page_storage[page_idx], remaining_pages);
            extra_to_free = remaining_pages;
            break;
        }

        curr_page = page_storage[page_idx];

        curr_page->status = ZERO_STATUS;
        curr_page->pte = NULL;

        // Write the page to be zeroed into the struct
        page_zeroing->pages_to_zero[page_zeroing_idx] = curr_page;
        release_pagelock(curr_page, 17);

        // Increment the value to signal that the zeroing thread can use this slot to zero
        custom_spin_assert(InterlockedIncrement(&page_zeroing->status_map[page_zeroing_idx]) == PAGE_SLOT_READY);

        num_to_zero++;
        InterlockedIncrement64(&page_zeroing->total_slots_used);
    }


    custom_spin_assert(num_to_zero + zeroing_start + extra_to_free == num_allocated);

    SetEvent(zero_pages_event);

    #if ONLY_ONE_REFRESHER
    InterlockedAnd(&list_refresh_ongoing, FALSE);
    #endif

}


/**
 * To be called when a faulter needs to only refresh the free frames list, and not the zero list
 * 
 * This helps us reduce contention on the standby list and avoids the unnecessary work coming from zeroing out pages
 */
static void refresh_free_frames(PAGE** page_storage) {
    
    #if ONLY_ONE_REFRESHER
    if (InterlockedOr(&list_refresh_ongoing, TRUE) == TRUE) {
        return;
    }
    #endif

    ULONG64 num_allocated = standby_pop_batch(page_storage, NUM_PAGES_THREAD_REFRESH);

    // The standby list is empty!
    if (num_allocated == 0) {
        SetEvent(trimming_event);

        #if ONLY_ONE_REFRESHER
        InterlockedAnd(&list_refresh_ongoing, FALSE);
        #endif

        return;
    }

    // Update the old transition PTEs to be disk format
    update_pte_sections_to_disk_format(page_storage, num_allocated);

    // Add all the pages to their respective free lists
    free_frames_add_batch(page_storage, min(num_allocated, NUM_PAGES_THREAD_REFRESH));

    #if ONLY_ONE_REFRESHER
    InterlockedAnd(&list_refresh_ongoing, FALSE);
    #endif

}


/**
 * To be called when a faulter needs to only refresh the zeroed pages list, and not the free list
 * 
 * This helps us replenish the zero list as quickly as possible when necessary
 */
static void refresh_zero_lists(PAGE** page_storage) {
    
    #if ONLY_ONE_REFRESHER
    if (InterlockedOr(&list_refresh_ongoing, TRUE) == TRUE) {
        return;
    }
    #endif

    refresh_update_thread_storages(LIST_REFRESH_ONGOING);

    ULONG64 num_allocated = standby_pop_batch(page_storage, NUM_PAGES_THREAD_REFRESH);

    // The standby list is empty!
    if (num_allocated == 0) {
        SetEvent(trimming_event);

        #if ONLY_ONE_REFRESHER
        InterlockedAnd(&list_refresh_ongoing, FALSE);
        #endif

        return;
    }

    // Update the old transition PTEs to be disk format
    update_pte_sections_to_disk_format(page_storage, num_allocated);

    long old_slot_val;

    PAGE* curr_page;
    ULONG64 num_added_to_zero = 0;
    ULONG64 extra_to_free = 0;

    // Add the other half to be zeroed out!
    for (ULONG64 page_idx = 0; page_idx < num_allocated; page_idx++) {
        ULONG64 page_zeroing_idx = get_zeroing_struct_idx();

        // We try to claim the slot
        old_slot_val = InterlockedCompareExchange(&page_zeroing->status_map[page_zeroing_idx], PAGE_SLOT_CLAIMED, PAGE_SLOT_OPEN);
        
        // The zeroing thread has failed to keep up - we should just add these to the free frames list
        if (old_slot_val != PAGE_SLOT_OPEN) {
            SetEvent(zero_pages_event);
            ULONG64 remaining_pages = num_allocated - page_idx;
            free_frames_add_batch(&page_storage[page_idx], remaining_pages);
            extra_to_free = remaining_pages;
            break;
        }

        curr_page = page_storage[page_idx];

        curr_page->status = ZERO_STATUS;
        curr_page->pte = NULL;

        // Write the page to be zeroed into the struct
        page_zeroing->pages_to_zero[page_zeroing_idx] = curr_page;
        release_pagelock(curr_page, 17);

        // Increment the value to signal that the zeroing thread can use this slot to zero
        custom_spin_assert(InterlockedIncrement(&page_zeroing->status_map[page_zeroing_idx]) == PAGE_SLOT_READY);
        num_added_to_zero++;

        InterlockedIncrement64(&page_zeroing->total_slots_used);
    }

    SetEvent(zero_pages_event);

    custom_spin_assert(num_added_to_zero + extra_to_free == num_allocated);

    #if ONLY_ONE_REFRESHER
    InterlockedAnd(&list_refresh_ongoing, FALSE);
    #endif

}

/**
 * Determines whether or not a faulter should refresh the zero and free lists with pages from the standby list using pre-defined macros 
 * and the total amount of available pages compared to the total number of physical pages
 * 
 * In the future, this would be made more dynamic (perhaps based moreso on the rate of page usage, where they're being used, etc - rather than simple macros)
 */
void potential_list_refresh(ULONG64 thread_idx) {

    /**
     * We do this short scheme at the beginning of each list refresh so that we can avoid reading and writing to hot cache lines whenever we
     * are trying to access the global variables in this function
     */
    #if DEBUG_THREAD_STORAGE
    THREAD_LOCAL_STORAGE* storage = &thread_information.thread_local_storages[thread_idx];
    custom_spin_assert(thread_information.thread_local_storages[thread_idx].thread_id == GetCurrentThreadId());
    #endif

    if (thread_information.thread_local_storages[thread_idx].list_refresh_status == LIST_REFRESH_ONGOING) {
        return;
    }

    if (standby_list->list_length > physical_page_count / STANDBY_LIST_REFRESH_PROPORTION) {
        if (zero_lists->total_available < physical_page_count / ZERO_LIST_REFRESH_PROPORTION || 
            free_frames->total_available < physical_page_count / FREE_LIST_REFRESH_PROPORTION) {
            SetEvent(refresh_lists_event);
        }
    }
}


/**
 * Pops and returns a pointer to the oldest page from the standby list and returns it
 * 
 * Returns NULL upon any error or if the list is empty
 */
PAGE* standby_pop_page() {

    // Preemptively check that the list is empty
    if (standby_list->list_length == 0) {
        return NULL;
    }

    BOOL pagelock_acquired = FALSE;

    PAGE* potential_page = acquire_list_tail_pagelock(standby_list);
    
    if (potential_page == NULL) return NULL;

    #if DEBUG_PAGELOCK
    custom_spin_assert(potential_page->holding_threadid == GetCurrentThreadId());
    #endif

    // We remove the page from the list now
    unlink_page(standby_list, potential_page);
   
    InterlockedDecrement64(&total_available_pages);

    /**
     * By here, we have the required page and its lock. 
     * 
     * However, we have NOT made any irreversible changes. This will allow us to return the page to the standby list?
     */
    return potential_page;    
}   


/**
 * Tries to pop batch_size pages from the standby list. Returns the number of pages successfully allocated
 * 
 * Writes the pages' pointers into page_storage
 */
ULONG64 standby_pop_batch(PAGE** page_storage, ULONG64 batch_size) {

    // Pre-emptively check for the list length being empty before doing any more work
    if (standby_list->list_length == 0) return 0;

    ULONG64 num_allocated = acquire_batch_list_tail_pagelocks(standby_list, page_storage, batch_size);
    
    if (num_allocated == 0) return 0;

    PAGE* clostest_to_tail = page_storage[0];
    PAGE* farthest_from_tail = page_storage[num_allocated - 1];

    /**
     * Now, we have the pagelocks of all of the pages that we need to use
     */
    remove_page_section(standby_list, farthest_from_tail, clostest_to_tail, num_allocated);
    
    InterlockedAdd64(&total_available_pages, - num_allocated);
    
    return num_allocated;
}


/**
 * This should be called whenever threads fail to acquire any pages, and need to wait for pages to be available.
 * 
 * This signals the trimmer as well as the modified writer if appropriate so that we start replenishing pages.
 */
void wait_for_pages_signalling() {
    ResetEvent(waiting_for_pages_event);

    SetEvent(trimming_event);

    if (modified_list->list_length > 0) {
        SetEvent(modified_writer_event);
    }

    WaitForSingleObject(waiting_for_pages_event, INFINITE);
}


/**
 * Tries to find an available page from the zero lists, free lists, or standby. If found,
 * returns a pointer to the page. Otherwise, returns NULL. If zero_page_preferred is TRUE,
 * we will search the zero lists first - otherwise, we search the free list first. Both are preferable to standby.
 * 
 */
PAGE* find_available_page(BOOL zeroed_page_preferred) {

    PAGE* allocated_page;
    BOOL zero_first;

    
    // Since disk reads overwrite page contents, we do not need zeroed out pages.
    // and we would rather them be saved for accesses that do need zeroed pages
    if (zeroed_page_preferred == FALSE || free_frames->total_available < REDUCE_FREE_LIST_PRESSURE_AMOUNT) {
        zero_first = FALSE;
    } else {
        zero_first = TRUE;
    }

    if (zero_first) {
        // If we succeed on the zeroed list, this is the best case scenario as we don't have to do anything else
        if ((allocated_page = allocate_zeroed_frame()) != NULL) {

            return allocated_page;
        }

        // If we succeed on the free list, we may still have to zero out the frame later
        if ((allocated_page = allocate_free_frame()) != NULL) {

            #if DEBUG_PAGELOCK
            custom_spin_assert(allocated_page->holding_threadid == GetCurrentThreadId());
            #endif
            return allocated_page;
        }
    } else {
        // In this case - we don't need the pages to be zeroed already - so the free list is fine
        if ((allocated_page = allocate_free_frame()) != NULL) {

            #if DEBUG_PAGELOCK
            custom_spin_assert(allocated_page->holding_threadid == GetCurrentThreadId());
            #endif
            return allocated_page;
        }

        // We still prefer the faster access to the zeroed list than anything on standby
        if ((allocated_page = allocate_zeroed_frame()) != NULL) {

            return allocated_page;
        }
    }


    /**
     * Now we have to try to get a page from the standby list
     */
    allocated_page = standby_pop_page(standby_list);

    #if DEBUG_PAGELOCK
    custom_spin_assert(allocated_page->holding_threadid == GetCurrentThreadId());
    #endif

    // If we failed to acquire a page, this is NULL
    return allocated_page;
}


/**
 * Tries to find num_pages pages across the zeroed/free/standby lists. 
 * If zeroed_pages_preferred is TRUE, we will search the zero list first. Otherwise, we search the free list first. 
 * 
 * Returns the number of pages written into the page_storage, if we find zero pages, we will wait for the signal for available pages
 * and return 0
 */
ULONG64 find_batch_available_pages(BOOL zeroed_pages_preferred, PAGE** page_storage, ULONG64 num_pages) {
    BOOL zero_first;
    ULONG64 total_allocated_pages = 0;

    
    // Since disk reads overwrite page contents, we do not need zeroed out pages.
    // and we would rather them be saved for accesses that do need zeroed pages
    if (zeroed_pages_preferred == FALSE && free_frames->total_available > REDUCE_FREE_LIST_PRESSURE_AMOUNT) {
        zero_first = FALSE;
    } else {
        zero_first = TRUE;
    }

    // Look in the zero lists first
    if (zero_first) {
        total_allocated_pages += allocate_batch_zeroed_frames(page_storage, num_pages);

        // If we have enough pages, we can return now
        if (total_allocated_pages == num_pages) {
            return num_pages;
        }

        total_allocated_pages += allocate_batch_free_frames(&page_storage[total_allocated_pages], num_pages - total_allocated_pages);

        if (total_allocated_pages == num_pages) {
            return num_pages;
        }

    } else {
        total_allocated_pages += allocate_batch_free_frames(page_storage, num_pages);

        if (total_allocated_pages == num_pages) {
            return num_pages;
        }

        total_allocated_pages += allocate_batch_zeroed_frames(&page_storage[total_allocated_pages], num_pages - total_allocated_pages);

        if (total_allocated_pages == num_pages) {
            return num_pages;
        }
    }

    // DebugBreak();

    // At this point, we need to take pages from the standby list
    total_allocated_pages += standby_pop_batch(&page_storage[total_allocated_pages], num_pages - total_allocated_pages);

    return total_allocated_pages;
}


/**
 * Takes the pages and unlinks them from the standby list
 */
static void rescue_batch_standby_pages(PAGE** standby_rescues, ULONG64 num_standby_rescues) {
    if (num_standby_rescues > 0) {
        unlink_batch_scattered_pages(standby_list, standby_rescues, num_standby_rescues);

        InterlockedAdd64(&total_available_pages, - num_standby_rescues);
    }
}


/**
 * Takes the pages and unlinks them from the modified list
 */
static void rescue_batch_modified_pages(PAGE** modified_rescues, ULONG64 num_modified_rescues) {
    PAGE* curr_page;
    if (num_modified_rescues == 0) return;

    
    

    /**
     * We need to separately keep track of the pages we are actually removing, since some of them might be being written to the disk
     * right now
     */
    PAGE* pages_to_remove[MAX_PAGES_RESCUABLE];
    ULONG64 num_pages_to_remove = 0;

    for (ULONG64 i = 0; i < num_modified_rescues; i++) {
        curr_page = modified_rescues[i];

        // The page is not in the modified list, but is instead being written to disk, but we can still take it
        if (curr_page->writing_to_disk == PAGE_BEING_WRITTEN && curr_page->modified == PAGE_NOT_MODIFIED) {
            continue;
        }

        pages_to_remove[num_pages_to_remove] = curr_page;
        num_pages_to_remove++;


    }

    // If there are no pages to pop, we will return immediately
    unlink_batch_scattered_pages(modified_list, pages_to_remove, num_pages_to_remove);
    
}


/**
 * Rescues all of the pages from both the modified and standby lists
 * 
 * Assumes that all pages' pagelocks are acquired, and that the pre-sorted pages are in the appropriate lists
 * with their correct statuses
 */
void rescue_batch_pages(PAGE** modified_rescues, ULONG64 num_modified_rescues, PAGE** standby_rescues, ULONG64 num_standby_rescues) {
    
    /**
     * These functions return immediately if there are no modified/standby rescues, but seperating them out
     * helps with analyzing lock contention in the perf traces
     */
    rescue_batch_modified_pages(modified_rescues, num_modified_rescues);

    rescue_batch_standby_pages(standby_rescues, num_standby_rescues);
}


/**
 * Rescues the given page from the modified or standby list, if it is in either. 
 * 
 * Assumes the pagelock was acquired beforehand.
 * 
 * Returns SUCCESS if the page is rescued, ERROR otherwise
 */
int rescue_page(PAGE* page) {

    if (page_is_modified(*page)) {
        if (modified_rescue_page(page) == ERROR) {
            return ERROR;
        }
        custom_spin_assert(page->page_lock == PAGE_LOCKED);
        custom_spin_assert(page->status == MODIFIED_STATUS);
        return SUCCESS;
    }

    if (page_is_standby(*page)) {
        if (standby_rescue_page(page) == ERROR) {
            return ERROR;
        }
        custom_spin_assert(page->page_lock == PAGE_LOCKED);
        custom_spin_assert(page->status == STANDBY_STATUS);
        return SUCCESS;
    }

    return ERROR;
}


/**
 * Rescues the given page from the modified list, if it can be found
 * 
 * Returns SUCCESS if the rescue page was found and removed, ERROR otherwise
 */
int modified_rescue_page(PAGE* page) {
    // The page is not in the modified list, but is instead being written to disk, but we can still take it
    if (page->writing_to_disk == PAGE_BEING_WRITTEN && page->modified == PAGE_NOT_MODIFIED) {
        #if DEBUG_LISTS
        custom_spin_assert(page->frame_listnode->listhead_ptr == NULL);
        #endif
        return SUCCESS;
    }

    unlink_page(modified_list, page);

    return SUCCESS;
}


/**
 * Rescues the given page from the standby list, if it can be found
 * 
 * Returns SUCCESS if the rescue page was found and removed, ERROR otherwise
 */
int standby_rescue_page(PAGE* page) {

    unlink_page(standby_list, page);

    InterlockedDecrement64(&total_available_pages);

    return SUCCESS;
}


/**
 * Releases all of the pages by returning them to their original lists
 * 
 * This is only done when we acquire pages to resolve an unaccessed PTE's fault and may have
 * speculatively brought in several pages to map several PTEs ahead of them as well. However,
 * another thread had already mapped those PTEs so we no longer need those pages.
 * 
 * We return these pages back to the zero/free/standby lists as appropriate
 */
void release_batch_unneeded_pages(PAGE** pages, ULONG64 num_pages) {

    /**
     * We have to sort the pages first so that we add them to the correct lists
     */
    ULONG64 num_standby_pages = 0;

    // As of right now, MAX_PAGES_READABLE is the maximum that we would speculate... this is temporary
    PAGE* standby_pages[MAX_PAGES_READABLE];

    ULONG64 num_zero_pages = 0;
    PAGE* zeroed_pages[MAX_PAGES_READABLE];

    ULONG64 num_free_pages = 0;
    PAGE* free_pages[MAX_PAGES_READABLE];

    PAGE* curr_page;
    for (ULONG64 i = 0; i < num_pages; i++) {
        curr_page = pages[i];

        if (curr_page->status == STANDBY_STATUS) {
            standby_pages[num_standby_pages] = curr_page;
            num_standby_pages++;
            continue;
        } else if (curr_page->status == ZERO_STATUS) {
            zeroed_pages[num_zero_pages] = curr_page;
            num_zero_pages++;
            continue;
        } else if (curr_page->status == FREE_STATUS) {
            free_pages[num_free_pages] = curr_page;
            num_free_pages++;
            continue;
        }

        custom_spin_assert(FALSE);
    }

    BOOL signal_waiters = (total_available_pages == 0);

    /**
     * We handle the standby pages first as there is a chance that these pages can be rescued still, preventing
     * unnecessary disk reads
     */
    if (num_standby_pages > 0) {
        create_chain_of_pages(standby_pages, num_standby_pages, STANDBY_STATUS);

        InterlockedAdd64(&total_available_pages, num_standby_pages);

        insert_page_section(standby_list, standby_pages[0], standby_pages[num_standby_pages - 1], num_standby_pages);

    }

    if (num_zero_pages > 0) {
        zero_list_add_batch(zeroed_pages, num_zero_pages);
    }

    if (num_free_pages > 0) {
        free_frames_add_batch(free_pages, num_free_pages);
    }


    if (signal_waiters) {
        SetEvent(waiting_for_pages_event);
    }

}


/**
 * Returns the given page to its appropriate list after it is revealed we no longer need it
 * 
 * Assumes that we have the pagelock
 */
void release_unneeded_page(PAGE* page) {
    if (page->status == STANDBY_STATUS) {
        InterlockedIncrement64(&total_available_pages);

        insert_page(standby_list, page);

    } else if (page->status == MODIFIED_STATUS) {
       DebugBreak();
    } else if (page->status == FREE_STATUS) {
        free_frames_add(page);
    } else if (page->status == ZERO_STATUS) {
        zero_lists_add(page);
    } else {
        DebugBreak();
    }

    custom_spin_assert(page->page_lock == PAGE_LOCKED);
    release_pagelock(page, 12);

}


/**
 * Adds the given page to its proper slot in the zero list
 */
void zero_lists_add(PAGE* page) {
    // Modulo operation based on the pfn to put it alongside other cache-colliding pages
    int listhead_idx = page_to_pfn(page) % NUM_CACHE_SLOTS;

    PAGE_LIST* head = &zero_lists->listheads[listhead_idx];
   
    #ifdef DEBUG_CHECKING
    int dbg_result;
    if ((dbg_result = page_is_isolated(page)) != ISOLATED) {
        DebugBreak();
    }
    #endif

    page->status = ZERO_STATUS;

    InterlockedIncrement64(&total_available_pages);

    InterlockedIncrement64(&zero_lists->total_available);

    insert_page(head, page);
}


/**
 * Adds the given page to its proper slot in the free list
 */
void free_frames_add(PAGE* page) {
    // Modulo operation based on the pfn to put it alongside other cache-colliding pages
    int listhead_idx = page_to_pfn(page) % NUM_CACHE_SLOTS;

    PAGE_LIST* head = &free_frames->listheads[listhead_idx];

    #ifdef DEBUG_CHECKING
    int dbg_result;
    if ((dbg_result = page_is_isolated(page)) != ISOLATED) {
        DebugBreak();
    }
    #endif

    page->status = FREE_STATUS;

    InterlockedIncrement64(&total_available_pages);

    InterlockedIncrement64(&free_frames->total_available);

    insert_page(head, page);
}


/**
 * Connects the linked list nodes inside of the pages together to form a single section that
 * can be added to a listhead very quickly
 * 
 * We also change the page's statuses to new_page_status since this is usually being done to add a 
 * section to a list very quickly. This can avoid an extra loop later.
 */
void create_chain_of_pages(PAGE** pages_to_chain, ULONG64 num_pages, ULONG64 new_page_status) {
    // We do not need to do anything
    if (num_pages == 1) {
        pages_to_chain[0]->status = new_page_status;
        return;
    }

    PAGE* curr_page;
    PAGE* next_page;

    for (ULONG64 i = 0; i < num_pages - 1; i++) {
        curr_page = pages_to_chain[i];
        curr_page->status = new_page_status;

        next_page = pages_to_chain[i + 1];

        curr_page->flink = next_page;
        next_page->blink = curr_page;
    } 
        
    // We need to modify the final page's status
    next_page->status = new_page_status;
}



/**
 * Thread dedicated to refreshing the free and zero lists using old standby pages
 */
LPTHREAD_START_ROUTINE* thread_free_and_zero_refresher(void* parameters) {
    WORKER_THREAD_PARAMETERS* thread_params = (WORKER_THREAD_PARAMETERS*) parameters;
    ULONG64 worker_thread_idx = thread_params->thread_idx;

    PAGE** page_storage = (PAGE**) malloc(sizeof(PAGE*) * NUM_PAGES_THREAD_REFRESH);

    if (page_storage == NULL) {
        fprintf(stderr, "Failed to allocate memory for page storage for list refreshing thread\n");
        return NULL;
    }

    HANDLE events[2];
    ULONG64 signaled_event;

    events[0] = refresh_lists_event;
    events[1] = shutdown_event;

    while (TRUE) {
        signaled_event = WaitForMultipleObjects(2, events, FALSE, INFINITE);

        if (signaled_event == 1) {
            free(page_storage);
            return NULL;
        }

        refresh_update_thread_storages(LIST_REFRESH_ONGOING);

        if (zero_lists->total_available < physical_page_count / ZERO_LIST_REFRESH_PROPORTION && 
            free_frames->total_available < physical_page_count / FREE_LIST_REFRESH_PROPORTION) {
            refresh_free_and_zero_lists(page_storage);
        }
        if (zero_lists->total_available < physical_page_count / ZERO_LIST_REFRESH_PROPORTION) {
            refresh_zero_lists(page_storage);
        } else {
            refresh_free_frames(page_storage);
        }

        refresh_update_thread_storages(LIST_REFRESH_NOT_ONGOING);
       
    }

    return NULL;
}