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

#define ZERO_REFRESH_BOUNDARY  NUM_ZERO_SLOTS / 2
long zero_slot_statuses[NUM_ZERO_SLOTS];
ULONG64 total_available_zero_slots;
PULONG_PTR faulter_zero_base_addr;

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

    /**
     * We sort the pages by their respective PTE's virtual address. This means that the pages will be sorted in the order of the 
     * PTE locksections, allowing us to enter each PTE lock only once. The cost of sorting the array is much lower than the potential
     * cost of colliding on critical sections and entering and leaving the same critical section more than once
     */
    qsort(page_list, num_pages, sizeof(PAGE*), page_pte_compare);

    // The first and last field will always be zero for all the PTEs we update
    PTE pte_contents;
    pte_contents.complete_format = 0;

    PTE_LOCKSECTION* curr_locksection = pte_to_locksection(page_list[0]->pte);
    PTE_LOCKSECTION* next_locksection;

    EnterCriticalSection(&curr_locksection->lock);

    for (ULONG64 page_idx = 0; page_idx < num_pages; page_idx++) {
        curr_page = page_list[page_idx];
        curr_pte = curr_page->pte;

        PTE_LOCKSECTION* next_locksection = pte_to_locksection(curr_pte);

        // Switch to the next critical section if it is not the one we are already in
        if (next_locksection != curr_locksection) {
            LeaveCriticalSection(&curr_locksection->lock);

            curr_locksection = next_locksection;

            EnterCriticalSection(&next_locksection->lock);
        }

        if (is_transition_format(*curr_pte) == FALSE) DebugBreak();

        pte_contents.disk_format.pagefile_idx = curr_page->pagefile_idx;

        write_pte_contents(curr_pte, pte_contents);
    }

    LeaveCriticalSection(&curr_locksection->lock);
}


/**
 * Adds all of the pages from each page section to their free list buckets, and releases all the pagelocks
 */
static void free_frames_zerolist_add_batch(PAGE** page_list, ULONG64 num_pages) {
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

        db_insert_at_head(free_frames->listheads[curr_cache_color], curr_page->frame_listnode);

        curr_page->status = FREE_STATUS;
        curr_page->pte = NULL;

        release_pagelock(curr_page);
    }
    
    LeaveCriticalSection(curr_locksection);

    SetEvent(waiting_for_pages_event);
}


/**
 * Reacquires all of the remaining (not NULL) pages in each page section from the free frames list
 */
static void free_frames_zerolist_reacquire_pages(PAGE*** page_sections, ULONG64* page_section_counts) {
    PAGE** page_array;
    ULONG64 section_count;
    ULONG64 remaining_section_count;
    PAGE* curr_page;
    
    for (ULONG64 cache_slot = 0; cache_slot < NUM_CACHE_SLOTS; cache_slot++) {
        section_count = page_section_counts[cache_slot];

        // Ignore page sections where we do not have anything to add
        if (section_count == 0) continue;

        page_array = page_sections[cache_slot];
        DB_LL_NODE* free_frames_section_listhead = free_frames->listheads[cache_slot];
        remaining_section_count = section_count;

        // Add every page in the cache slot section to the free list section
        EnterCriticalSection(&free_frames->list_locks[cache_slot]);

        for (ULONG64 page_idx = 0; page_idx < section_count; page_idx++) {
            curr_page = page_array[page_idx];

            // The page was taken from the free list before we could zero it
            if (curr_page == NULL) {
                remaining_section_count--;
                continue;
            }

            db_remove_from_middle(free_frames_section_listhead, curr_page->frame_listnode);
            release_pagelock(curr_page);
        }

        free_frames->list_lengths[cache_slot] -= remaining_section_count;

        LeaveCriticalSection(&free_frames->list_locks[cache_slot]);
    }
}


/**
 * Adds all of the remaining (not NULL) pages in each section to the zero list and releases all of the pagelocks
 */
static void zero_list_add_batch(PAGE*** page_sections, ULONG64* page_section_counts) {
    PAGE** page_array;
    ULONG64 section_count;
    ULONG64 remaining_section_count;
    PAGE* curr_page;
    
    for (ULONG64 cache_slot = 0; cache_slot < NUM_CACHE_SLOTS; cache_slot++) {
        section_count = page_section_counts[cache_slot];

        // Ignore page sections where we do not have anything to add
        if (section_count == 0) continue;

        page_array = page_sections[cache_slot];
        DB_LL_NODE* zero_list_section_listhead = zero_lists->listheads[cache_slot];
        remaining_section_count = section_count;
        
        // Add every page in the cache slot section to the free list section
        EnterCriticalSection(&zero_lists->list_locks[cache_slot]);

        for (ULONG64 page_idx = 0; page_idx < section_count; page_idx++) {
            curr_page = page_array[page_idx];

            // This page was used while it was on the free list, and not on the zero_lists list
            if (curr_page == NULL) {
                remaining_section_count--;
                continue;
            }

            db_insert_node_at_head(zero_list_section_listhead, curr_page->frame_listnode);
            release_pagelock(curr_page);
        }

        zero_lists->list_lengths[cache_slot] += remaining_section_count;

        LeaveCriticalSection(&zero_lists->list_locks[cache_slot]);
    }
}


/**
 * Thread dedicated to taking pages from standby, zeroing them, and
 * adding them to the zero lists for quicker access
 */
#define MAX_PAGES_TO_REPURPOSE 512
#define STANDBY_PROPORTION_DIVISOR 5
// #define MAX_PAGES_PER_SECTION 64
LPTHREAD_START_ROUTINE thread_populate_zero_lists(void* parameters) {
    MEM_EXTENDED_PARAMETER* vmem_parameters = (MEM_EXTENDED_PARAMETER*) parameters;
    PULONG_PTR thread_zeroing_address;

    // We dynamically adjust depending on whether or not we allow multiple VAs to map to the same page
    if (vmem_parameters == NULL) {
        thread_zeroing_address = VirtualAlloc(NULL, PAGE_SIZE * MAX_PAGES_TO_REPURPOSE, 
                        MEM_PHYSICAL | MEM_RESERVE, PAGE_READWRITE);
    } else {
        thread_zeroing_address = VirtualAlloc2(NULL, NULL, PAGE_SIZE * MAX_PAGES_TO_REPURPOSE,
                        MEM_PHYSICAL | MEM_RESERVE, PAGE_READWRITE, vmem_parameters, 1);
    }


    #if 0
    /**
     *  We will organize the PTEs we need to update into their various locksections 
    */
    PTE** ptes_to_update = (PTE***) malloc(sizeof(PTE*) * pagetable->num_locks);

    if (ptes_to_update == NULL) {
        fprintf(stderr, "Unable to allocate memory for pte updating in thread_populate_zero_lists\n");
        return;
    }

    ULONG64* pte_section_counts = (ULONG64*) malloc(sizeof(ULONG64) * pagetable->num_locks);

    if (pte_section_counts == NULL) {
        fprintf(stderr, "Unable to allocate memory for pte_section_counts in thread_populate_zero_lists\n");
        return;
    }

    for (ULONG64 section_num = 0; section_num < pagetable->num_locks; section_num++) {
        //BW: Think about how we can reduce memory cost here - even if we could theoretically trim an entire PTE section
        // PTE** pte_section_to_update = (PTE**) malloc(sizeof(PTE*) * MAX_PAGES_PER_SECTION);
        PTE** pte_section_to_update = (PTE**) malloc(sizeof(PTE*) * MAX_PAGES_TO_REPURPOSE);


        if (pte_section_to_update == NULL) {
            fprintf(stderr, "Unable to allocate memory for pte section updating in thread_populate_zero_lists\n");
            return;
        }

        ptes_to_update[section_num] = pte_section_to_update;
    }
    #endif

    /**
     * The pages, on the other hand, will be organized based off of their cache section
     */
    PAGE** page_list = (PAGE**) malloc(sizeof(PAGE*) * MAX_PAGES_TO_REPURPOSE);


    if (page_list == NULL) {
        fprintf(stderr, "Unable to allocate memory for page_list in thread_populate_zero_lists\n");
        return;
    }


    /**
     * 1. Wait for event to start zeroing
     * 
     * 2. Take pagelock for tail of standby list
     *    2a. Once this is done - essentially free reign to take pagelocks for
     *        MAX_PAGES_TO_REPURPOSE pages on standby list
     * 
     * 3. Organize all of them into the PTE sections and PAGE sections
     * 
     * 3. Pop all of them from the standby list
     * 
     *    3a. Modify all PTEs from each section
     * 
     * 4. Add them all to the free frames list, release pagelocks
     * 
     * 5. Zero out all of the pages (Except for ones that get taken from free frames)
     * 
     * 6. Reacquire pagelocks
     * 
     * 7. Add them all to to the zero lists
     */
    while (TRUE) {
        WaitForSingleObject(zero_pages_event, INFINITE);

        ULONG64 curr_standby_length = *(volatile ULONG64*) &standby_list->list_length;
        ULONG64 num_to_repurpose;

        if (MAX_PAGES_TO_REPURPOSE < curr_standby_length / STANDBY_PROPORTION_DIVISOR) {
            num_to_repurpose = MAX_PAGES_TO_REPURPOSE;
        } else {
            num_to_repurpose = curr_standby_length / STANDBY_PROPORTION_DIVISOR;
        }

        ULONG64 actual_pages_acquired;

        // Get all of the pages from the standby list into our sections
        // standby_zerolist_pop_batch(page_sections, ptes_to_update, pte_section_counts, page_section_counts, num_to_repurpose);
        actual_pages_acquired = standby_pop_batch(page_list, num_to_repurpose);

        // We failed to pop any pages off of the standby list
        if (actual_pages_acquired == 0) {
            continue;
        }

        // Update all of the PTEs to be in disk format
        update_pte_sections_to_disk_format(page_list, actual_pages_acquired);
        
        // Add the frames to the free-list and release the pagelocks
        free_frames_zerolist_add_batch(page_list, actual_pages_acquired);


        // printf("\t\t finished batch\n");

        /**
         * Now, we want to zero out all of the pages that we can - but since we do not have the pagelock
         * we need to be careful about not zeroing pages that have been reallocated to someone else (reference counting)
         */


        /**
         * We will re-acquire the pagelocks inside of here - and check one more time that they have not been reallocated
         */
        // free_frames_zerolist_reacquire_pages(page_sections, page_section_counts);


        // // Add the pages that we have re-acquired from the free frames list to the zero list, and release page locks
        // zero_list_add_batch(page_sections, page_section_counts);
    
    }


}



/**
 * Used to initialize the virtual addresses, lists, and variables used for zeroing out pages
 */
int initialize_page_zeroing(MEM_EXTENDED_PARAMETER* vmem_parameters) {
    if (vmem_parameters == NULL) {
        faulter_zero_base_addr = VirtualAlloc(NULL, PAGE_SIZE * NUM_ZERO_SLOTS, 
                        MEM_PHYSICAL | MEM_RESERVE, PAGE_READWRITE);
    } else {
        faulter_zero_base_addr = VirtualAlloc2(NULL, NULL, PAGE_SIZE * NUM_ZERO_SLOTS,
                        MEM_PHYSICAL | MEM_RESERVE, PAGE_READWRITE, vmem_parameters, 1);
    }

    if (faulter_zero_base_addr == NULL) {
        fprintf(stderr, "Unable to allocate memory for faulter_zero_base_addr in initialize_page_zeroing\n");
        return ERROR;
    }

    // All slots start as open
    for (ULONG64 i = 0; i < NUM_ZERO_SLOTS; i++) {
        zero_slot_statuses[i] = ZERO_SLOT_OPEN;
    }

    total_available_zero_slots = NUM_ZERO_SLOTS;

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

    for (ULONG64 slot_idx = 0; slot_idx < NUM_ZERO_SLOTS; slot_idx++) {
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
    for (ULONG64 slot_idx = 0; slot_idx < NUM_ZERO_SLOTS; slot_idx++) {
        zero_slot = &zero_slot_statuses[slot_idx];

        if (*zero_slot == ZERO_SLOT_OPEN) {
            read_old_val = InterlockedCompareExchange(zero_slot, ZERO_SLOT_USED, ZERO_SLOT_OPEN);
            
            // We successfully claimed the zero slot
            if (read_old_val == ZERO_SLOT_OPEN) {
                assert(*zero_slot == ZERO_SLOT_USED);
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
        assert(zero_slot_statuses[slot_index] == ZERO_SLOT_USED);

        assert(InterlockedIncrement(&zero_slot_statuses[slot_index]) == ZERO_SLOT_NEEDS_FLUSH);
    }
}


/**
 * Marks the zero slot to be refreshed, releasing it for the current thread
 */
static void release_zero_slot(ULONG64 slot_idx) {
    if (zero_slot_statuses[slot_idx] != ZERO_SLOT_USED) {
        DebugBreak();
    }

    assert(InterlockedIncrement(&zero_slot_statuses[slot_idx]) == ZERO_SLOT_NEEDS_FLUSH);
}


/**
 * Takes all of the zero slots in the list that need to be refreshed and handle them in a single
 * batch - reducing the amount of load we need on MapUserPhysicalPagesScatter
 */
PULONG_PTR refresh_zero_addresses[NUM_ZERO_SLOTS];
volatile long* refresh_zero_status_slots[NUM_ZERO_SLOTS];
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
    for (ULONG64 slot_idx = 0; slot_idx < NUM_ZERO_SLOTS; slot_idx++) {
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
    if (pages == NULL || num_pages == 0 || num_pages > MAX_ZEROABLE || pages[0] == NULL ) {
        fprintf(stderr, "NULL or invalid arguments given to zero_out_pages\n");
        DebugBreak();
    }


    PULONG_PTR zero_slot_addresses[MAX_ZEROABLE];
    ULONG64 zero_slot_indices[MAX_ZEROABLE];
    ULONG64 pfns[MAX_ZEROABLE];
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

        pagelock_acquired = try_acquire_pagelock(potential_page);

        if (pagelock_acquired && potential_page != (PAGE*) pagelist_listhead->blink->item) {
            release_pagelock(potential_page);
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
        
        pagelock_acquired = try_acquire_pagelock(potential_page);

        // Once we have the lock, we need to ensure that it is still inside the standby list
        if (pagelock_acquired && potential_page == (PAGE*) curr_node->blink->item) {
            curr_node = potential_page->frame_listnode;
            page_storage[num_allocated] = potential_page;
            num_allocated++;
        }

        potential_page = (PAGE*) curr_node->blink->item;
    }

    #if DEBUG_PAGELOCK
    for (int i = 0; i < num_allocated; i++) {
        PAGE* page = page_storage[i];
        assert(page->page_lock == PAGE_LOCKED);
        assert(page->holding_threadid == GetCurrentThreadId());
        assert(page->status == STANDBY_STATUS);
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
    
    ULONG64 local_index = zero_lists->curr_list_idx;
    BOOL found_first_nonempty_list = FALSE;

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

        // We may update the list index if there was a chain of empty list sections
        if (found_first_nonempty_list == FALSE) {
            zero_lists->curr_list_idx = local_index;
            found_first_nonempty_list = TRUE;
        }

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
    ULONG64 local_index = zero_lists->curr_list_idx;
    BOOL found_first_nonempty_list = FALSE;
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

        /**
         * If this is the first cache slot that has entries in it, shift the
         * zero_lists' stored index
         * 
         * Acknowledge that threads may race to edit the curr_list_idx, but so long as it is 
         * being edited to an index that is likely to have more entries, we accept it
         */
        if (found_first_nonempty_list == FALSE) {
            zero_lists->curr_list_idx = local_index;
            found_first_nonempty_list = TRUE;
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
    
    ULONG64 local_index = free_frames->curr_list_idx;
    BOOL found_first_nonempty_list = FALSE;

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

        // We may update the list index if there was a chain of empty list sections
        if (found_first_nonempty_list == FALSE) {
            free_frames->curr_list_idx = local_index;
            found_first_nonempty_list = TRUE;
        }

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
    ULONG64 local_index = free_frames->curr_list_idx;
    BOOL found_first_nonempty_list = FALSE;
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

        /**
         * If this is the first cache slot that has entries in it, shift the
         * free_frames' stored index
         * 
         * Acknowledge that threads may race to edit the curr_list_idx, but so long as it is 
         * being edited to an index that is likely to have more entries, we accept it
         */
        if (found_first_nonempty_list == FALSE) {
            free_frames->curr_list_idx = local_index;
            found_first_nonempty_list = TRUE;
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
    assert(potential_page->holding_threadid == GetCurrentThreadId());
    #endif
    // We remove the page from the list now
    assert(db_pop_from_tail(standby_list->listhead) == potential_page);
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
void acquire_pagelock(PAGE* page) {

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
        if (old_lock_status == PAGE_UNLOCKED) break;
    }
}


/**
 * Releases the pagelock for other threads to use
 */
void release_pagelock(PAGE* page) {
    
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

    if (InterlockedCompareExchange(&page->page_lock, PAGE_UNLOCKED, PAGE_LOCKED) != PAGE_LOCKED) {
        DebugBreak();
    };
}


/**
 * Tries to acquire the pagelock without any spinning. 
 * 
 * Returns TRUE if successful, FALSE otherwise
 */
BOOL try_acquire_pagelock(PAGE* page) {
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
    return InterlockedCompareExchange(&page->page_lock, PAGE_LOCKED, PAGE_UNLOCKED) == PAGE_UNLOCKED;

}