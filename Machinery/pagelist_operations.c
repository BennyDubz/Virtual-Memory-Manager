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

    // The first and last field will always be zero for all the PTEs we update
    PTE pte_contents;
    pte_contents.complete_format = 0;

    for (ULONG64 page_idx = 0; page_idx < num_pages; page_idx++) {
        curr_page = page_list[page_idx];
        curr_pte = curr_page->pte;

        if (is_transition_format(*curr_pte) == FALSE) DebugBreak();

        pte_contents.disk_format.pagefile_idx = curr_page->pagefile_idx;

        write_pte_contents(curr_pte, pte_contents);
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

    ULONG64 curr_cache_color = page_to_pfn(page_list[0]) % NUM_CACHE_SLOTS;
    ULONG64 next_cache_color;


    /**
     * We enter the first pages corresponding cache color locksection - we will hold onto this lock
     * until we reach a page who is in a different cache color. Because the list is sorted by cache color,
     * we will only enter and leave each critical section once
     */
    CRITICAL_SECTION* curr_locksection = &free_frames->list_locks[curr_cache_color];

    EnterCriticalSection(curr_locksection);

    for (ULONG64 page_idx = 0; page_idx < num_pages; page_idx++) {
    
        curr_page = page_list[page_idx];

        next_cache_color = page_to_pfn(curr_page) % NUM_CACHE_SLOTS;

        // If the cache colors are different, we will need to enter a new locksection
        if (next_cache_color != curr_cache_color) {
            LeaveCriticalSection(curr_locksection);

            curr_locksection = &free_frames->list_locks[next_cache_color];

            curr_cache_color = next_cache_color;

            EnterCriticalSection(curr_locksection);
        }

        insert_page(&free_frames->listheads[curr_cache_color], curr_page);

        free_frames->list_lengths[curr_cache_color]++;
        InterlockedIncrement64(&free_frames->total_available);
        InterlockedIncrement64(&total_available_pages);

        curr_page->status = FREE_STATUS;
        curr_page->pte = NULL;
        curr_page->pagefile_idx = DISK_IDX_NOTUSED;

        release_pagelock(curr_page, 13);
    }
    
    LeaveCriticalSection(curr_locksection);

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

    ULONG64 curr_cache_color = page_to_pfn(page_list[0]) % NUM_CACHE_SLOTS;
    ULONG64 next_cache_color;


    /**
     * We enter the first pages corresponding cache color locksection - we will hold onto this lock
     * until we reach a page who is in a different cache color. Because the list is sorted by cache color,
     * we will only enter and leave each critical section once
     */
    CRITICAL_SECTION* curr_locksection = &zero_lists->list_locks[curr_cache_color];

    EnterCriticalSection(curr_locksection);

    for (ULONG64 page_idx = 0; page_idx < num_pages; page_idx++) {
    
        curr_page = page_list[page_idx];

        next_cache_color = page_to_pfn(curr_page) % NUM_CACHE_SLOTS;

        // If the cache colors are different, we will need to enter a new locksection
        if (next_cache_color != curr_cache_color) {
            LeaveCriticalSection(curr_locksection);

            curr_locksection = &zero_lists->list_locks[next_cache_color];

            curr_cache_color = next_cache_color;

            EnterCriticalSection(curr_locksection);
        }

        insert_page(&zero_lists->listheads[curr_cache_color], curr_page);
       

        zero_lists->list_lengths[curr_cache_color]++;
        InterlockedIncrement64(&zero_lists->total_available);
        InterlockedIncrement64(&total_available_pages);

        curr_page->status = ZERO_STATUS;
        curr_page->pte = NULL;
        curr_page->pagefile_idx = DISK_IDX_NOTUSED;

        release_pagelock(curr_page, 14);
    }
    
    LeaveCriticalSection(curr_locksection);

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
    
     
    while (TRUE) {
        WaitForSingleObject(zero_pages_event, INFINITE);

        InterlockedOr(&page_zeroing->zeroing_ongoing, TRUE);

        ULONG64 num_to_zero = zero_list_get_pages_to_clear(page_list, pfn_list);

        if (num_to_zero == 0) continue;

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
        // standby_zerolist_pop_batch(page_sections, ptes_to_update, pte_section_counts, page_section_counts, num_to_repurpose);
        
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
static PAGE* try_acquire_list_tail_pagelock(PAGE* pagelist_listhead) {
    BOOL pagelock_acquired = FALSE;
    PAGE* potential_page = NULL;

    while (pagelock_acquired == FALSE) {
        potential_page = pagelist_listhead->blink;

        if (potential_page->status == LISTHEAD_STATUS) return NULL;


        pagelock_acquired = try_acquire_pagelock(potential_page, 4);
        
        if (pagelock_acquired && potential_page != pagelist_listhead->blink) {
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


/**
 * Spins until it acquires the pagelock for the tail entry of the list, if there is a tail entry
 * 
 * Returns the page whose lock was acquired, or NULL if the list was empty and it was unable
 */
static PAGE* acquire_list_tail_pagelock(PAGE* pagelist_listhead) {

    BOOL pagelock_acquired = FALSE;
    PAGE* potential_page = NULL;

    while (pagelock_acquired == FALSE) {
        potential_page = (PAGE*) pagelist_listhead->blink;

        // Check if this list section is empty
        if (potential_page->status == LISTHEAD_STATUS) return NULL;

        pagelock_acquired = try_acquire_pagelock(potential_page, 4);

        if (pagelock_acquired && potential_page != (PAGE*) pagelist_listhead->blink) {
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
static ULONG64 acquire_batch_list_tail_pagelocks(PAGE* pagelist_listhead, PAGE** page_storage, ULONG64 num_pages) {

    PAGE* curr_page;
    
    /**
     * Acquire the pagelock for the very first page in the list, once we have done this, 
     * we can walk down the list and acquire the other needed pagelocks
     * 
     * The bottleneck for popping a batch of pages off of lists using this function will almost always
     * be at the listhead
     */
    BOOL pagelock_acquired = FALSE;

    PAGE* first_page = acquire_list_tail_pagelock(pagelist_listhead);
    
    if (first_page == NULL) return 0;

    page_storage[0] = first_page;
    ULONG64 num_allocated = 1;
    curr_page = first_page;
    
    PAGE* potential_page = (PAGE*) curr_page->blink;

    while (num_allocated < num_pages && potential_page->status != LISTHEAD_STATUS) {
        
        pagelock_acquired = try_acquire_pagelock(potential_page, 5);

        // Once we have the lock, we need to ensure that it is still inside the standby list
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
    PAGE* potential_page = NULL;

    int curr_attempts = 0;
    
    ULONG64 local_index = ReadTimeStampCounter() % NUM_CACHE_SLOTS;

    custom_spin_assert(local_index < NUM_CACHE_SLOTS);

    while (curr_attempts < MAX_ATTEMPTS_FREE_OR_ZERO_LISTS) {
        // Check for empty list - we can quickly check here before acquiring the lock
        if (zero_lists->list_lengths[local_index] == 0) {
            curr_attempts ++;
            local_index = (local_index + 1) % NUM_CACHE_SLOTS;
            continue;
        }

        // By here, the **odds are better** that we will get a frame, but not guaranteed
        PAGE* section_listhead = &zero_lists->listheads[local_index];

        // If multiple threads are contending on the same sections, this will help alleviate contention
        if (curr_attempts < MAX_ATTEMPTS_FREE_OR_ZERO_LISTS) {
            potential_page = try_acquire_list_tail_pagelock(section_listhead);
        } else {
            potential_page = acquire_list_tail_pagelock(section_listhead);
        }

        // We failed to get a page from this section, it was empty
        if (potential_page == NULL) {
            local_index = (local_index + 1) % NUM_CACHE_SLOTS;
            curr_attempts++;
            continue;
        }

        EnterCriticalSection(&zero_lists->list_locks[local_index]);

        // Remove the page from the list
        pop_page(section_listhead);
       
        zero_lists->list_lengths[local_index]--;
        InterlockedDecrement64(&zero_lists->total_available);
        InterlockedDecrement64(&total_available_pages);

        LeaveCriticalSection(&zero_lists->list_locks[local_index]);

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


    /**
     * We will continue until we fail to acquire a page several consecutive times in a row, or until
     * we have acquired the number of pages that we want
     */
    while (num_consecutive_failures < MAX_ATTEMPTS_FREE_OR_ZERO_LISTS && num_allocated < num_pages) {
        // We make an opportunistic look at the number of pages before acquiring any locks
        if (zero_lists->list_lengths[local_index] == 0) {
            num_consecutive_failures++;
            local_index = (local_index + 1) % NUM_CACHE_SLOTS;

            continue;
        }

        PAGE* cache_color_listhead = &zero_lists->listheads[local_index];

        if (num_consecutive_failures < MAX_ATTEMPTS_FREE_OR_ZERO_LISTS) {
            curr_page = try_acquire_list_tail_pagelock(cache_color_listhead);
        } else {
            curr_page = acquire_list_tail_pagelock(cache_color_listhead);
        }

        // We failed to acquire a page - this list is now empty
        if (curr_page == NULL) {

            num_consecutive_failures++;

            local_index = (local_index + 1) % NUM_CACHE_SLOTS;

            continue;
        }

        EnterCriticalSection(&zero_lists->list_locks[local_index]);

        pop_page(cache_color_listhead);
        
        zero_lists->list_lengths[local_index]--;

        InterlockedDecrement64(&zero_lists->total_available);
        InterlockedDecrement64(&total_available_pages);

        LeaveCriticalSection(&zero_lists->list_locks[local_index]);

        page_storage[num_allocated] = curr_page;
        num_consecutive_failures = 0;
        num_allocated++;
    }

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

    int curr_attempts = 0;
    
    ULONG64 local_index = ReadTimeStampCounter() % NUM_CACHE_SLOTS;

    while (curr_attempts < MAX_ATTEMPTS_FREE_OR_ZERO_LISTS) {
        // Check for empty list - we can quickly check here before acquiring the lock
        if (free_frames->list_lengths[local_index] == 0) {
            curr_attempts += 1;
            local_index = (local_index + 1) % NUM_CACHE_SLOTS;
            continue;
        }

        PAGE* section_listhead = &free_frames->listheads[local_index];
        
        // If multiple threads are contending on the same sections, this will help alleviate contention
        if (curr_attempts < MAX_ATTEMPTS_FREE_OR_ZERO_LISTS / 2) {
            potential_page = try_acquire_list_tail_pagelock(section_listhead);
        } else {
            potential_page = acquire_list_tail_pagelock(section_listhead);
        }

        // potential_page = acquire_list_tail_pagelock(section_listhead);

        // potential_page = try_acquire_list_tail_pagelock(section_listhead);

        // We failed to get a page from this section, it was empty
        if (potential_page == NULL) {
            local_index = (local_index + 1) % NUM_CACHE_SLOTS;
            curr_attempts++;
            continue;
        }

        EnterCriticalSection(&free_frames->list_locks[local_index]);

        // Remove the page from the list
        pop_page(section_listhead);
       
        free_frames->list_lengths[local_index]--;

        InterlockedDecrement64(&free_frames->total_available);
        InterlockedDecrement64(&total_available_pages);

        LeaveCriticalSection(&free_frames->list_locks[local_index]);

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
    PAGE* curr_page;


    /**
     * We will continue until we fail to acquire a page several consecutive times in a row, or until
     * we have acquired the number of pages that we want
     */
    while (num_consecutive_failures < MAX_ATTEMPTS_FREE_OR_ZERO_LISTS && num_allocated < batch_size) {
        
        // We make an opportunistic look at the number of pages before acquiring any locks
        if (free_frames->list_lengths[local_index] == 0) {
            num_consecutive_failures++;
            local_index = (local_index + 1) % NUM_CACHE_SLOTS;

            continue;
        }

        // By here, the odds are better that we will get a frame, but not guaranteed
        PAGE* cache_color_listhead = &free_frames->listheads[local_index];

        if (num_consecutive_failures < MAX_ATTEMPTS_FREE_OR_ZERO_LISTS / 2) {
            curr_page = try_acquire_list_tail_pagelock(cache_color_listhead);
        } else {
            curr_page = acquire_list_tail_pagelock(cache_color_listhead);
        }

        // We failed to acquire a page - this list is now empty
        if (curr_page == NULL) {

            num_consecutive_failures++;

            local_index = (local_index + 1) % NUM_CACHE_SLOTS;

            continue;
        }

        // By here, the **odds are better** that we will get a  frame, but not guaranteed
        EnterCriticalSection(&free_frames->list_locks[local_index]);

        pop_page(cache_color_listhead);

        free_frames->list_lengths[local_index]--;

        InterlockedDecrement64(&free_frames->total_available);
        InterlockedDecrement64(&total_available_pages);

        LeaveCriticalSection(&free_frames->list_locks[local_index]);

        page_storage[num_allocated] = curr_page;
        num_consecutive_failures = 0;
        num_allocated++;
    }

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

static void refresh_free_and_zero_lists() {
    
    #if ONLY_ONE_REFRESHER
    if (InterlockedOr(&list_refresh_ongoing, TRUE) == TRUE) {
        return;
    }
    #endif

    refresh_update_thread_storages(LIST_REFRESH_ONGOING);

    PAGE* pages_to_refresh[NUM_PAGES_FAULTER_REFRESH];

    ULONG64 num_allocated = standby_pop_batch(pages_to_refresh, NUM_PAGES_FAULTER_REFRESH);

    // The standby list is empty!
    if (num_allocated == 0) {
        SetEvent(trimming_event);

        #if ONLY_ONE_REFRESHER
        InterlockedAnd(&list_refresh_ongoing, FALSE);
        #endif

        refresh_update_thread_storages(LIST_REFRESH_NOT_ONGOING);

        return;
    }

    // Update the old transition PTEs to be disk format
    update_pte_sections_to_disk_format(pages_to_refresh, num_allocated);

    // Add half the pages to the free list, and releases their pagelocks to be used freely
    free_frames_add_batch(pages_to_refresh, min(num_allocated, FREE_FRAMES_PORTION));


    // See if we still have pages remaining to add to the zeroing threads buffer
    if (num_allocated < FREE_FRAMES_PORTION + 1) { // + 1 to reflect the indices used
        // There are still very few pages on standby...
        SetEvent(trimming_event);

        #if ONLY_ONE_REFRESHER
        InterlockedAnd(&list_refresh_ongoing, FALSE);
        #endif

        refresh_update_thread_storages(LIST_REFRESH_NOT_ONGOING);

        return;
    }

    long old_slot_val;

    PAGE* curr_page;

    // Add the other half to be zeroed out!
    for (ULONG64 page_idx = FREE_FRAMES_PORTION; page_idx < num_allocated; page_idx++) {
        ULONG64 page_zeroing_idx = get_zeroing_struct_idx();

        // We try to claim the slot
        old_slot_val = InterlockedCompareExchange(&page_zeroing->status_map[page_zeroing_idx], PAGE_SLOT_CLAIMED, PAGE_SLOT_OPEN);
        
        // The zeroing thread has failed to keep up - we should just add these to the free frames list
        if (old_slot_val != PAGE_SLOT_OPEN) {
            SetEvent(zero_pages_event);
            ULONG64 remaining_pages = num_allocated - page_idx;
            free_frames_add_batch(&pages_to_refresh[page_idx], remaining_pages);

            break;
        }

        curr_page = pages_to_refresh[page_idx];

        curr_page->status = ZERO_STATUS;
        curr_page->pte = NULL;

        // Write the page to be zeroed into the struct
        page_zeroing->pages_to_zero[page_zeroing_idx] = curr_page;
        release_pagelock(curr_page, 17);

        // Increment the value to signal that the zeroing thread can use this slot to zero
        custom_spin_assert(InterlockedIncrement(&page_zeroing->status_map[page_zeroing_idx]) == PAGE_SLOT_READY);

        InterlockedIncrement64(&page_zeroing->total_slots_used);
    }

    SetEvent(zero_pages_event);

    #if ONLY_ONE_REFRESHER
    InterlockedAnd(&list_refresh_ongoing, FALSE);
    #endif

    refresh_update_thread_storages(LIST_REFRESH_NOT_ONGOING);
}


/**
 * To be called when a faulter needs to only refresh the free frames list, and not the zero list
 * 
 * This helps us reduce contention on the standby list and avoids the unnecessary work coming from zeroing out pages
 */
static void refresh_free_frames() {
    
    #if ONLY_ONE_REFRESHER
    if (InterlockedOr(&list_refresh_ongoing, TRUE) == TRUE) {
        return;
    }
    #endif

    refresh_update_thread_storages(LIST_REFRESH_ONGOING);

    PAGE* pages_to_refresh[NUM_PAGES_FAULTER_REFRESH];

    ULONG64 num_allocated = standby_pop_batch(pages_to_refresh, NUM_PAGES_FAULTER_REFRESH);

    // The standby list is empty!
    if (num_allocated == 0) {
        SetEvent(trimming_event);

        #if ONLY_ONE_REFRESHER
        InterlockedAnd(&list_refresh_ongoing, FALSE);
        #endif

        refresh_update_thread_storages(LIST_REFRESH_NOT_ONGOING);

        return;
    }

    // Update the old transition PTEs to be disk format
    update_pte_sections_to_disk_format(pages_to_refresh, num_allocated);

    // Add all the pages to their respective free lists
    free_frames_add_batch(pages_to_refresh, min(num_allocated, NUM_PAGES_FAULTER_REFRESH));

    #if ONLY_ONE_REFRESHER
    InterlockedAnd(&list_refresh_ongoing, FALSE);
    #endif

    refresh_update_thread_storages(LIST_REFRESH_NOT_ONGOING);
}


/**
 * To be called when a faulter needs to only refresh the zeroed pages list, and not the free list
 * 
 * This helps us replenish the zero list as quickly as possible when necessary
 */
static void refresh_zero_lists() {
    
    #if ONLY_ONE_REFRESHER
    if (InterlockedOr(&list_refresh_ongoing, TRUE) == TRUE) {
        return;
    }
    #endif

    refresh_update_thread_storages(LIST_REFRESH_ONGOING);

    PAGE* pages_to_refresh[NUM_PAGES_FAULTER_REFRESH];

    ULONG64 num_allocated = standby_pop_batch(pages_to_refresh, NUM_PAGES_FAULTER_REFRESH);

    // The standby list is empty!
    if (num_allocated == 0) {
        SetEvent(trimming_event);

        #if ONLY_ONE_REFRESHER
        InterlockedAnd(&list_refresh_ongoing, FALSE);
        #endif

        refresh_update_thread_storages(LIST_REFRESH_NOT_ONGOING);

        return;
    }

    // Update the old transition PTEs to be disk format
    update_pte_sections_to_disk_format(pages_to_refresh, num_allocated);

    long old_slot_val;

    PAGE* curr_page;

    // Add the other half to be zeroed out!
    for (ULONG64 page_idx = 0; page_idx < num_allocated; page_idx++) {
        ULONG64 page_zeroing_idx = get_zeroing_struct_idx();

        // We try to claim the slot
        old_slot_val = InterlockedCompareExchange(&page_zeroing->status_map[page_zeroing_idx], PAGE_SLOT_CLAIMED, PAGE_SLOT_OPEN);
        
        // The zeroing thread has failed to keep up - we should just add these to the free frames list
        if (old_slot_val != PAGE_SLOT_OPEN) {
            SetEvent(zero_pages_event);
            ULONG64 remaining_pages = num_allocated - page_idx;
            free_frames_add_batch(&pages_to_refresh[page_idx], remaining_pages);

            break;
        }

        curr_page = pages_to_refresh[page_idx];

        curr_page->status = ZERO_STATUS;
        curr_page->pte = NULL;

        // Write the page to be zeroed into the struct
        page_zeroing->pages_to_zero[page_zeroing_idx] = curr_page;
        release_pagelock(curr_page, 17);

        // Increment the value to signal that the zeroing thread can use this slot to zero
        custom_spin_assert(InterlockedIncrement(&page_zeroing->status_map[page_zeroing_idx]) == PAGE_SLOT_READY);

        InterlockedIncrement64(&page_zeroing->total_slots_used);
    }

    SetEvent(zero_pages_event);

    #if ONLY_ONE_REFRESHER
    InterlockedAnd(&list_refresh_ongoing, FALSE);
    #endif

    refresh_update_thread_storages(LIST_REFRESH_NOT_ONGOING);
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
        #if 0
        if (free_frames->total_available + zero_lists->total_available < physical_page_count / TOTAL_ZERO_AND_FREE_LIST_REFRESH_PROPORTION) {
            refresh_free_and_zero_lists();
        } 
        #if 1
        else if (free_frames->total_available < physical_page_count / INDIVIDUAL_LIST_REFRESH_PROPORTION) {
            refresh_free_frames();
        }
        #endif
        #endif
        if (zero_lists->total_available < physical_page_count / ZERO_LIST_REFRESH_PROPORTION && 
            free_frames->total_available < physical_page_count / FREE_LIST_REFRESH_PROPORTION) {
            refresh_free_and_zero_lists();
        }
        if (zero_lists->total_available < physical_page_count / ZERO_LIST_REFRESH_PROPORTION) {
            refresh_zero_lists();
        } else if (free_frames->total_available < physical_page_count / FREE_LIST_REFRESH_PROPORTION) {
            refresh_free_frames();
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

    PAGE* potential_page = acquire_list_tail_pagelock(&standby_list->listhead);
    
    if (potential_page == NULL) return NULL;

    EnterCriticalSection(&standby_list->lock);

    #if DEBUG_PAGELOCK
    custom_spin_assert(potential_page->holding_threadid == GetCurrentThreadId());
    #endif

    // We remove the page from the list now
    pop_page(&standby_list->listhead);
   
    standby_list->list_length--;

    LeaveCriticalSection(&standby_list->lock);

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

    ULONG64 num_allocated = acquire_batch_list_tail_pagelocks(&standby_list->listhead, page_storage, batch_size);
    
    if (num_allocated == 0) return 0;

    PAGE* clostest_to_tail = page_storage[0];
    PAGE* farthest_from_tail = page_storage[num_allocated - 1];

    /**
     * Now, we have the pagelocks of all of the pages that we need to use
     */
    EnterCriticalSection(&standby_list->lock);

    remove_page_section(farthest_from_tail, clostest_to_tail);
    
    standby_list->list_length -= num_allocated;

    InterlockedAdd64(&total_available_pages, - num_allocated);
    
    LeaveCriticalSection(&standby_list->lock);

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
 * Rescues all of the pages from both the modified and standby lists
 * 
 * Assumes that all pages' pagelocks are acquired, and that the pre-sorted pages are in the appropriate lists
 * with their correct statuses
 */
void rescue_batch_pages(PAGE** modified_rescues, ULONG64 num_modified_rescues, PAGE** standby_rescues, ULONG64 num_standby_rescues) {
    PAGE* curr_page;
    if (num_modified_rescues > 0) {
        ULONG64 num_removed = 0;
        EnterCriticalSection(&modified_list->lock);

        for (ULONG64 i = 0; i < num_modified_rescues; i++) {
            curr_page = modified_rescues[i];

            // The page is not in the modified list, but is instead being written to disk, but we can still take it
            if (curr_page->writing_to_disk == PAGE_BEING_WRITTEN && curr_page->modified == PAGE_NOT_MODIFIED) {
                continue;
            }

            unlink_page(modified_rescues[i]);
            
            num_removed++;
        }

        modified_list->list_length -= num_removed;

        LeaveCriticalSection(&modified_list->lock);
    }

    if (num_standby_rescues > 0) {
        EnterCriticalSection(&standby_list->lock);

        for (ULONG64 i = 0; i < num_standby_rescues; i++) {
            unlink_page(standby_rescues[i]);
        }

        standby_list->list_length -= num_standby_rescues;

        LeaveCriticalSection(&standby_list->lock);

        InterlockedAdd64(&total_available_pages, - num_standby_rescues);
    }
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

    EnterCriticalSection(&modified_list->lock);

    unlink_page(page);

    modified_list->list_length--;

    LeaveCriticalSection(&modified_list->lock);

    return SUCCESS;
}


/**
 * Rescues the given page from the standby list, if it can be found
 * 
 * Returns SUCCESS if the rescue page was found and removed, ERROR otherwise
 */
int standby_rescue_page(PAGE* page) {
    EnterCriticalSection(&standby_list->lock);

    unlink_page(page);

    standby_list->list_length--;
    InterlockedDecrement64(&total_available_pages);

    LeaveCriticalSection(&standby_list->lock);

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

        EnterCriticalSection(&standby_list->lock);

        insert_page_section(&standby_list->listhead, standby_pages[0], standby_pages[num_standby_pages - 1]);

        standby_list->list_length += num_standby_pages;
        InterlockedAdd64(&total_available_pages, num_standby_pages);

        LeaveCriticalSection(&standby_list->lock);

        for (ULONG64 i = 0; i < num_standby_pages; i++) {
            release_pagelock(standby_pages[i], 40);
        }

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
        EnterCriticalSection(&standby_list->lock);

        standby_add_page(page, standby_list);

        InterlockedIncrement64(&total_available_pages);

        LeaveCriticalSection(&standby_list->lock);
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

    PAGE* relevant_listhead = &zero_lists->listheads[listhead_idx];
   
    #ifdef DEBUG_CHECKING
    int dbg_result;
    if ((dbg_result = page_is_isolated(page)) != ISOLATED) {
        DebugBreak();
    }
    #endif

    EnterCriticalSection(&zero_lists->list_locks[listhead_idx]);

    page->status = ZERO_STATUS;

    insert_page(relevant_listhead, page);

    zero_lists->list_lengths[listhead_idx]++;
    
    InterlockedIncrement64(&total_available_pages);

    InterlockedIncrement64(&zero_lists->total_available);

    LeaveCriticalSection(&zero_lists->list_locks[listhead_idx]);
}


/**
 * Adds the given page to its proper slot in the free list
 */
void free_frames_add(PAGE* page) {
    // Modulo operation based on the pfn to put it alongside other cache-colliding pages
    int listhead_idx = page_to_pfn(page) % NUM_CACHE_SLOTS;

    PAGE* relevant_listhead = &free_frames->listheads[listhead_idx];

    #ifdef DEBUG_CHECKING
    int dbg_result;
    if ((dbg_result = page_is_isolated(page)) != ISOLATED) {
        DebugBreak();
    }
    #endif

    EnterCriticalSection(&free_frames->list_locks[listhead_idx]);

    page->status = FREE_STATUS;

    insert_page(relevant_listhead, page);

    free_frames->list_lengths[listhead_idx]++;
    
    InterlockedIncrement64(&total_available_pages);

    InterlockedIncrement64(&free_frames->total_available);

    LeaveCriticalSection(&free_frames->list_locks[listhead_idx]);
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
 * Tries to acquire the pagelocks for the flink and blink pages of the given page
 * 
 * Returns TRUE iff both neighboring pagelocks are acquired, FALSE otherwise.
 * 
 * In the case that we acquire one of the neighbor's locks but not the other's, then we will release
 * the first one before returning FALSE so that we do not have any neighboring locks held
 */
BOOL try_acquire_neighboring_pagelocks(PAGE* page, ULONG64 origin_code) {
    if (try_acquire_pagelock(page->flink, 42) == FALSE) {
        return FALSE;
    }

    if (try_acquire_pagelock(page->blink, 43) == FALSE) {
        release_pagelock(page->flink, 44);
        return FALSE;
    }

    return TRUE;
}


/**
 * Acquires the pagelocks for the flink and blink pages of the given page, and will spin as long
 * as necessary in order to do so
 */
void acquire_neighboring_pagelocks(PAGE* page, ULONG64 origin_code) {
    acquire_pagelock(page->flink, 45);
    acquire_pagelock(page->blink, 46);
}


/**
 * Releases the pagelocks for the flink and the blink of the given page
 */
void release_neighboring_pagelocks(PAGE* page, ULONG64 origin_code) {
    release_pagelock(page->flink, 47);
    release_pagelock(page->blink, 48);
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
        old_lock_status = InterlockedCompareExchange(&page->page_lock, PAGE_LOCKED, PAGE_UNLOCKED);

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

    if (InterlockedCompareExchange(&page->page_lock, PAGE_UNLOCKED, PAGE_LOCKED) != PAGE_LOCKED) {
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
    if (InterlockedCompareExchange(&page->page_lock, PAGE_LOCKED, PAGE_UNLOCKED) == PAGE_UNLOCKED) {
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

    return InterlockedCompareExchange(&page->page_lock, PAGE_LOCKED, PAGE_UNLOCKED) == PAGE_UNLOCKED;

}