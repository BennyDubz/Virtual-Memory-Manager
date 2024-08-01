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

        db_insert_node_at_head(free_frames->listheads[curr_cache_color], curr_page->frame_listnode);

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

        db_insert_node_at_head(zero_lists->listheads[curr_cache_color], curr_page->frame_listnode);

        zero_lists->list_lengths[curr_cache_color]++;
        InterlockedIncrement64(&zero_lists->total_available);
        InterlockedIncrement64(&total_available_pages);

        curr_page->status = FREE_STATUS;
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
    MEM_EXTENDED_PARAMETER* vmem_parameters = (MEM_EXTENDED_PARAMETER*) parameters;
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
 * Spins until it acquires the pagelock for the tail entry of the list, if there is a tail entry
 * 
 * Returns the page whose lock was acquired, or NULL if the list was empty and it was unable
 */
static PAGE* acquire_list_tail_pagelock(DB_LL_NODE* pagelist_listhead) {
    BOOL pagelock_acquired = FALSE;
    PAGE* potential_page = NULL;

    while (pagelock_acquired == FALSE) {
        potential_page = (PAGE*) pagelist_listhead->blink->item;

        // Check if this list section is empty
        if (potential_page == NULL) break;

        pagelock_acquired = try_acquire_pagelock(potential_page, 4);

        if (pagelock_acquired && potential_page != (PAGE*) pagelist_listhead->blink->item) {
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
static ULONG64 acquire_batch_list_tail_pagelocks(DB_LL_NODE* pagelist_listhead, PAGE** page_storage, ULONG64 num_pages) {
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
    DB_LL_NODE* curr_node = curr_page->frame_listnode;
    PAGE* potential_page = (PAGE*) curr_node->blink->item;

    while (num_allocated < num_pages && potential_page != NULL) {
        
        pagelock_acquired = try_acquire_pagelock(potential_page, 5);

        // Once we have the lock, we need to ensure that it is still inside the standby list
        if (pagelock_acquired && potential_page == (PAGE*) curr_node->blink->item) {
            curr_node = potential_page->frame_listnode;
            page_storage[num_allocated] = potential_page;
            num_allocated++;
        } else if (pagelock_acquired) {
            release_pagelock(potential_page, 16);
        }

        potential_page = (PAGE*) curr_node->blink->item;
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

    while (curr_attempts < NUM_CACHE_SLOTS) {
        // Check for empty list - we can quickly check here before acquiring the lock
        if (zero_lists->list_lengths[local_index] == 0) {
            curr_attempts ++;
            local_index = (local_index + 1) % NUM_CACHE_SLOTS;
            continue;
        }

        // By here, the **odds are better** that we will get a frame, but not guaranteed
        
        DB_LL_NODE* section_listhead = zero_lists->listheads[local_index];

        potential_page = acquire_list_tail_pagelock(section_listhead);

        // We failed to get a page from this section, it was empty
        if (potential_page == NULL) {
            local_index = (local_index + 1) % NUM_CACHE_SLOTS;
            curr_attempts++;
            continue;
        }

        EnterCriticalSection(&zero_lists->list_locks[local_index]);

        // Remove the page from the list
        db_pop_from_tail(section_listhead);
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

    ULONG64 oldest_failure_idx = INFINITE;
    ULONG64 num_allocated = 0;
    ULONG64 local_index = ReadTimeStampCounter() % NUM_CACHE_SLOTS;
    PAGE* curr_page;


    /**
     * We will continue until we have either looped around and failed on all of the lists,
     * or until we have allocated the number of pages that we want
     */
    while (local_index != oldest_failure_idx && num_allocated < num_pages) {
        if (zero_lists->list_lengths[local_index] == 0) {
            // If this is our first failure in this loop of grabbing pages, set the failure index
            if (oldest_failure_idx != INFINITE) {
                oldest_failure_idx = local_index;
            }

            local_index = (local_index + 1) % NUM_CACHE_SLOTS;

            continue;
        }
        // By here, the odds are better that we will get a frame, but not guaranteed

        DB_LL_NODE* cache_color_listhead = zero_lists->listheads[local_index];

        curr_page = acquire_list_tail_pagelock(cache_color_listhead);

        // We failed to acquire a page - this list is now empty
        if (curr_page == NULL) {

            if (oldest_failure_idx != INFINITE) {
                oldest_failure_idx = local_index;
            }

            local_index = (local_index + 1) % NUM_CACHE_SLOTS;

            continue;
        }

        // By here, the **odds are better** that we will get a  frame, but not guaranteed
        EnterCriticalSection(&zero_lists->list_locks[local_index]);

        db_pop_from_tail(cache_color_listhead);

        zero_lists->list_lengths[local_index]--;

        InterlockedDecrement64(&zero_lists->total_available);
        InterlockedDecrement64(&total_available_pages);

        LeaveCriticalSection(&zero_lists->list_locks[local_index]);

        page_storage[num_allocated] = curr_page;
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

    while (curr_attempts < NUM_CACHE_SLOTS) {
        // Check for empty list - we can quickly check here before acquiring the lock
        if (free_frames->list_lengths[local_index] == 0) {
            curr_attempts += 1;
            local_index = (local_index + 1) % NUM_CACHE_SLOTS;
            continue;
        }

        // By here, the **odds are better** that we will get a frame, but not guaranteed
        
        DB_LL_NODE* section_listhead = free_frames->listheads[local_index];

        potential_page = acquire_list_tail_pagelock(section_listhead);

        // We failed to get a page from this section, it was empty
        if (potential_page == NULL) {
            local_index = (local_index + 1) % NUM_CACHE_SLOTS;
            curr_attempts++;
            continue;
        }

        EnterCriticalSection(&free_frames->list_locks[local_index]);

        // Remove the page from the list
        db_pop_from_tail(section_listhead);
        free_frames->list_lengths[local_index]--;

        InterlockedDecrement64(&free_frames->total_available);
        InterlockedDecrement64(&total_available_pages);

        LeaveCriticalSection(&free_frames->list_locks[local_index]);

        custom_spin_assert(potential_page->pagefile_idx == DISK_IDX_NOTUSED);

        break;
    }
    
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

    ULONG64 oldest_failure_idx = INFINITE;
    ULONG64 num_allocated = 0;
    ULONG64 local_index = ReadTimeStampCounter() % NUM_CACHE_SLOTS;
    PAGE* curr_page;


    /**
     * We will continue until we have either looped around and failed on all of the lists,
     * or until we have allocated the number of pages that we want
     */
    while (local_index != oldest_failure_idx && num_allocated < batch_size) {
        if (free_frames->list_lengths[local_index] == 0) {
            // If this is our first failure in this loop of grabbing pages, set the failure index
            if (oldest_failure_idx != INFINITE) {
                oldest_failure_idx = local_index;
            }

            local_index = (local_index + 1) % NUM_CACHE_SLOTS;

            continue;
        }
        // By here, the odds are better that we will get a frame, but not guaranteed

        DB_LL_NODE* cache_color_listhead = free_frames->listheads[local_index];

        curr_page = acquire_list_tail_pagelock(cache_color_listhead);

        // We failed to acquire a page - this list is now empty
        if (curr_page == NULL) {

            if (oldest_failure_idx != INFINITE) {
                oldest_failure_idx = local_index;
            }

            local_index = (local_index + 1) % NUM_CACHE_SLOTS;

            continue;
        }

        // By here, the **odds are better** that we will get a  frame, but not guaranteed
        EnterCriticalSection(&free_frames->list_locks[local_index]);

        db_pop_from_tail(cache_color_listhead);

        free_frames->list_lengths[local_index]--;

        InterlockedDecrement64(&free_frames->total_available);
        InterlockedDecrement64(&total_available_pages);

        LeaveCriticalSection(&free_frames->list_locks[local_index]);

        page_storage[num_allocated] = curr_page;
        num_allocated++;
    }

    return num_allocated;
}


/**
 * To be called by the faulting thread when the total of free frames + zero frames is low
 * 
 * Takes many frames off of the standby list and uses some to populate the free frames list, while also
 * adding some to the zeroing-threads buffer. If there are enough pages for the zeroing thread to zero-out,
 * then it will be signalled
 */
#define NUM_PAGES_FAULTER_REFRESH 512
#define FREE_FRAMES_PORTION   (NUM_PAGES_FAULTER_REFRESH * 3 / 4)
#define ONLY_ONE_REFRESHER 1

#if ONLY_ONE_REFRESHER
long free_zero_refresh_ongoing = FALSE;
#endif

void faulter_refresh_free_and_zero_lists() {
    
    #if ONLY_ONE_REFRESHER
    if (InterlockedOr(&free_zero_refresh_ongoing, TRUE) == TRUE) {
        return;
    }
    #endif


    PAGE* pages_to_refresh[NUM_PAGES_FAULTER_REFRESH];

    ULONG64 num_allocated = standby_pop_batch(pages_to_refresh, NUM_PAGES_FAULTER_REFRESH);

    // The standby list is empty!
    if (num_allocated == 0) {
        SetEvent(trimming_event);

        #if ONLY_ONE_REFRESHER
        InterlockedAnd(&free_zero_refresh_ongoing, FALSE);
        #endif

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
        InterlockedAnd(&free_zero_refresh_ongoing, FALSE);
        #endif

        for (int i = 0; i < num_allocated; i++) {
            custom_spin_assert(pages_to_refresh[i]->holding_threadid != GetCurrentThreadId());
        }

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

            #if ONLY_ONE_REFRESHER
            InterlockedAnd(&free_zero_refresh_ongoing, FALSE);
            #endif

            for (int i = page_idx; i < num_allocated; i++) {
                custom_spin_assert(pages_to_refresh[i]->holding_threadid != GetCurrentThreadId());
            }

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


    // if (page_zeroing->total_slots_used >= NUM_THREAD_ZERO_SLOTS / 2) {
    //     SetEvent(zero_pages_event);
    // }

    SetEvent(zero_pages_event);

    #if ONLY_ONE_REFRESHER
    InterlockedAnd(&free_zero_refresh_ongoing, FALSE);
    #endif
}


/**
 * To be called by the faulting thread when the number of pages left in the standby cache is low 
 * 
 * Pops many pages off the standby list to be added onto the cache for quicker access
 */
void faulter_refresh_standby_cache() {
    return;
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

    PAGE* potential_page = acquire_list_tail_pagelock(standby_list->listhead);
    
    if (potential_page == NULL) return NULL;

    EnterCriticalSection(&standby_list->lock);

    #if DEBUG_PAGELOCK
    custom_spin_assert(potential_page->holding_threadid == GetCurrentThreadId());
    #endif
    // We remove the page from the list now
    custom_spin_assert(db_pop_from_tail(standby_list->listhead) == potential_page);
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

    ULONG64 num_allocated = acquire_batch_list_tail_pagelocks(standby_list->listhead, page_storage, batch_size);
    
    if (num_allocated == 0) return 0;

    /**
     * Now, we have the pagelocks of all of the pages that we need to use
     */
    EnterCriticalSection(&standby_list->lock);

    for (int i = 0; i < num_allocated; i++) {
        db_pop_from_tail(standby_list->listhead);
    }

    // db_remove_section(page_storage[0]->frame_listnode, page_storage[num_allocated - 1]->frame_listnode);
    standby_list->list_length -= num_allocated;

    InterlockedAdd64(&total_available_pages, - num_allocated);
    
    LeaveCriticalSection(&standby_list->lock);

    return num_allocated;
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