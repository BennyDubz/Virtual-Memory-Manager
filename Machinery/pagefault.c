/**
 * @author Ben Williams
 * @date June 25th, 2024
 * 
 * All functions for handling the page fault at the highest level
 */

#include <stdio.h>
#include <assert.h>
#include "../globals.h"
#include "../macros.h"
#include "./trim.h"
#include "./disk_operations.h"
#include "./conversions.h"
#include "./debug_checks.h"
#include "../Datastructures/datastructures.h"
#include "./pagefault.h"
#include "./pagelist_operations.h"


/**
 * GLOBALS FOR PAGE FAULTING
 */


ULONG64 fault_count = 0;

/**
 * Function declarations
 */
static int handle_unaccessed_pte_fault(PTE local_pte, PAGE** result_page_storage);

static int handle_transition_pte_fault(PTE local_pte, PTE* accessed_pte, ULONG64* disk_idx_storage, PAGE** result_page_storage);

static int handle_disk_pte_fault(PTE local_pte, PTE* accessed_pte, ULONG64* disk_idx_storage, PAGE** result_page_storage);

static PAGE* find_available_page();

static int rescue_page(PAGE* page, ULONG64* disk_idx_storage);

static int modified_rescue_page(PAGE* page);

static int standby_rescue_page(PAGE* page, ULONG64* disk_idx_storage);

static void release_unneeded_page(PAGE* page);

static void zero_lists_add(PAGE* page);

static void free_frames_add(PAGE* page);


/**
 * Handles the page fault for the given virtual address
 * 
 * Returns SUCCESS if the access can be attempted again, ERROR if
 * the fault must either be re-attempted or if the virtual address's corresponding
 * PTE was modified by another thread in the meantime
 * 
 */
volatile ULONG64 wait_count = 0;
volatile ULONG64 successful_rescue_count = 0;
volatile ULONG64 successful_disk_read_count = 0;
int pagefault(PULONG_PTR virtual_address) {
    InterlockedIncrement64(&fault_count);

    if (fault_count % KB(128) == 0) {
        printf("Curr fault count 0x%llX\n", fault_count);
        printf("\tZeroed: 0x%llX Free: 0x%llX Standby: 0x%llX Mod 0x%llX\n", zero_lists->total_available, free_frames->total_available, 
                                            standby_list->list_length, modified_list->list_length);

    }

    /**
     * Temporary ways to induce trimming and aging
     */    
    if ((total_available_pages < (physical_page_count / 3) && (fault_count % 32) == 0)) {
        SetEvent(trimming_event);
    }

    if (standby_list->list_length > physical_page_count / 10 && (fault_count % 32) == 0) {
        SetEvent(zero_pages_event);
    }

    PTE* accessed_pte = va_to_pte(virtual_address);

    if (accessed_pte == NULL) {
        fprintf(stderr, "Unable to find the accessed PTE\n");
        return REJECTION_FAIL;
    }

    // We try to trim behind us
    if (fault_count % 16 == 0) {
        faulter_trim_behind(accessed_pte);
    }

    // Make a copy of the PTE in order to perform the fault
    PTE local_pte = read_pte_contents(accessed_pte);
    PAGE* allocated_page = NULL;
    ULONG64 disk_idx;
    CRITICAL_SECTION* pte_lock = &pte_to_locksection(accessed_pte)->lock;
    int handler_result;

    if (is_memory_format(local_pte)) {
        return SUCCESS;
    } else if (is_used_pte(local_pte) == FALSE) {

        if ((handler_result = handle_unaccessed_pte_fault(local_pte, &allocated_page)) != SUCCESS) {
            return handler_result;
        }
        // There is no disk index for this page, but this allows us to quickly check whether to return a
        // disk slot later
        disk_idx = DISK_IDX_NOTUSED;
    } else if (is_transition_format(local_pte)) {

        if ((handler_result = handle_transition_pte_fault(local_pte, accessed_pte, &disk_idx, &allocated_page)) != SUCCESS) {
            return handler_result;
        }

    } else if (is_disk_format(local_pte)) {

        if ((handler_result = handle_disk_pte_fault(local_pte, accessed_pte, &disk_idx, &allocated_page)) != SUCCESS) {
            return handler_result;
        }

    }

    // For whatever reason the relevant handler failed, so we should retry the fault
    if (allocated_page == NULL) {
        DebugBreak();
    }

    #if DEBUG_PAGELOCK
    assert(allocated_page->holding_threadid == GetCurrentThreadId());
    #endif
    assert(allocated_page->page_lock == PAGE_LOCKED);
    assert(allocated_page->status != ACTIVE_STATUS);
    


    /**
     * Only a single thread faulting on the disk PTE will end up here, and it will already 
     * have the PTE lock. Therefore, we can commit the changes here.
     * 
     * In all other cases, we will need to enter the PTE lock and check the format again.
     */
    if (is_disk_format(local_pte) == FALSE) {
        EnterCriticalSection(pte_lock);
    
        /**
         * We cannot make any permanent changes until we hit the commit point
         */
        if (ptes_are_equal(local_pte, *accessed_pte) == FALSE) {

            /**
             * Return the page to the free-list and return RACE_CONDITION_FAIL
             * as the PTE changed from under the thread
             * 
             *  BW: Later, we need to zero-out pages that were acquired in the failure case
             *  in another thread and re-add them to the free-list
             */ 

            LeaveCriticalSection(pte_lock);
            release_unneeded_page(allocated_page);
            return RACE_CONDITION_FAIL;
        }
    }

    BOOL zeroed = FALSE;
    if (allocated_page->status != ZERO_STATUS && is_used_pte(local_pte) == FALSE) {
        zeroed = TRUE;
        zero_out_pages(&allocated_page, 1);
    }

    // We may need to edit the old PTE, in which case, we want a copy so we can still find it
    PAGE allocated_page_old = *allocated_page;

    allocated_page->status = ACTIVE_STATUS;
    allocated_page->pte = accessed_pte;

    // BW: Temporary until we are differentiating between reads and writes and can try to re-trim them
    // quickly by saving the disk index
    allocated_page->pagefile_idx = DISK_IDX_NOTUSED;

    // BW: Again, we are returning ALL disk slots for now
    if (disk_idx != DISK_IDX_NOTUSED) {
        release_single_disk_slot(disk_idx);
    }

    // Connect the PTE to the pfn through the CPU 
    connect_pte_to_page(accessed_pte, allocated_page);

    // if (fault_count % 50) printf("zero length %llx\n", zero_lists->total_available);

    // if (is_disk_format(local_pte)) {
    //     DebugBreak();
    // }

    #ifdef DEBUG_CHECKING
    if ((is_transition_format(local_pte) == FALSE && allocated_page_old.status == STANDBY_STATUS) == FALSE) {

        if (pfn_is_single_allocated(page_to_pfn(allocated_page)) == FALSE) {
            DebugBreak();
        }
    }
    #endif

    LeaveCriticalSection(pte_lock);

    /**
     * We need to modify the other PTE associated with the standby page now that we are committing
     * 
     * The worst case is that other threads are spinning on the pagelock in transition format waiting for this to happen,
     * and they will need to retry the fault as their PTE will be in disk format
     */
    if (is_transition_format(local_pte) == FALSE && allocated_page_old.status == STANDBY_STATUS) {
        PTE* old_pte = allocated_page_old.pte;
        ULONG64 pfn = page_to_pfn(allocated_page);

        if (is_transition_format(*old_pte) == FALSE) {
            DebugBreak();
        }

        PTE pte_contents;
        pte_contents.complete_format = 0;
        // The other disk format specific entries are zero - so we do not need to set them
        pte_contents.disk_format.pagefile_idx = allocated_page_old.pagefile_idx;

        #if 0
        EnterCriticalSection(&pte_to_locksection(old_pte)->lock);
        write_pte_contents(old_pte, pte_contents);
        LeaveCriticalSection(&pte_to_locksection(old_pte)->lock);
        #endif
        write_pte_contents(old_pte, pte_contents);
    }

    release_pagelock(allocated_page);

    return SUCCESSFUL_FAULT;
}   


/**
 * Handles a pagefault for a PTE that has never been accessed before
 * 
 * Writes the p, or NULL if it fails
 */
static int handle_unaccessed_pte_fault(PTE local_pte, PAGE** result_page_storage) {

    /**
     * In this case, all we need to do is get a valid page and return it - nothing else
     */
    PAGE* allocated_page = find_available_page();

    if (allocated_page == NULL) {
        return NO_AVAILABLE_PAGES_FAIL;
    }

    *result_page_storage = allocated_page;
    return SUCCESS;
}


/**
 * Handles a pagefault for a PTE in transition format
 * 
 * Writes the rescued page into result_page_storage, and the disk index into the disk_idx_storage if applicable,
 * otherwise, DISK_IDX_NOTUSED is written into it
 * 
 * Returns SUCCESS if there are no issues, or RESCUE_FAIL otherwise
 */
static int handle_transition_pte_fault(PTE local_pte, PTE* accessed_pte, ULONG64* disk_idx_storage, PAGE** result_page_storage) {
    ULONG64 pfn = local_pte.transition_format.frame_number;

    PAGE* page_to_rescue = pfn_to_page(pfn);

    // After we acquire the pagelock, if the page is rescuable then we should always be able to succeed
    acquire_pagelock(page_to_rescue);

    // We lost the race to rescue this PTE - whether it was stolen from under us or someone else saved it
    if (page_to_rescue->pte != accessed_pte || page_to_rescue->status == ACTIVE_STATUS) {
        release_pagelock(page_to_rescue);
        return RESCUE_FAIL;
    }

    // We now try to rescue the page from the modified or standby list
    if (rescue_page(page_to_rescue, disk_idx_storage) == ERROR) {
        DebugBreak();
        release_pagelock(page_to_rescue);
        return RESCUE_FAIL;
    }

    *result_page_storage = page_to_rescue;

    assert(page_to_rescue->page_lock == PAGE_LOCKED);
    assert(page_to_rescue->status != ACTIVE_STATUS);
    #if DEBUG_PAGELOCK
    assert(page_to_rescue->holding_threadid == GetCurrentThreadId());
    #endif

    return SUCCESS;    
}


/**
 * Handles a pagefault for a PTE that is in the disk format - it's contents
 * must be fetched from the disk
 * 
 * Returns a pointer to the page with the restored contents on it, and stores the
 * disk index that it was at in disk_idx_storage
 */
static int handle_disk_pte_fault(PTE local_pte, PTE* accessed_pte, ULONG64* disk_idx_storage, PAGE** result_page_storage) {
    CRITICAL_SECTION* pte_lock = &pte_to_locksection(accessed_pte)->lock;

    // We also now hold the allocated pagelock
    PAGE* allocated_page = find_available_page();

    if (allocated_page == NULL) {
        return NO_AVAILABLE_PAGES_FAIL;
    }

    ULONG64 disk_idx = local_pte.disk_format.pagefile_idx;

    EnterCriticalSection(pte_lock);

    assert(allocated_page->page_lock == PAGE_LOCKED);
    assert(allocated_page->status != ACTIVE_STATUS);

    if (ptes_are_equal(local_pte, *accessed_pte) == FALSE) {
        /**
         * Return the page to the free-list and return NULL
         * 
         *  BW: Later, we need to zero-out pages that were acquired in the failure case
         *  in another thread and re-add them to the free-list
         */ 
        LeaveCriticalSection(pte_lock);
        release_unneeded_page(allocated_page);
        return RACE_CONDITION_FAIL;
    }
    
    /**
     * We may instead grab the PTE lock here, and hold the lock through the disk read
     * until we make the final commit at the end of the parent pagefault function
     * 
     * We then compare the PTEs before continuing on with the disk read. This way only a single
     * thread will complete the disk read for a given PTE.
     */

    // Another thread could have rescued this frame ahead of us
    if (read_page_from_disk(allocated_page, disk_idx) == ERROR) {
        LeaveCriticalSection(pte_lock);

        // BW: Here, we would want to add it to the zero-out thread!
        release_unneeded_page(allocated_page);
        return DISK_FAIL;
    }

    *disk_idx_storage = disk_idx;
    *result_page_storage = allocated_page;

    assert(allocated_page->page_lock == PAGE_LOCKED);
    assert(allocated_page->status != ACTIVE_STATUS);


    return SUCCESS;
}


/**
 * Finds an available page from either the free or standby list and returns it
 * 
 * Returns NULL if there were no pages available at the time
 */
static PAGE* find_available_page() {

    PAGE* allocated_page;

    // If we succeed on the zeroed list, this is the best case scenario as we don't have to do anything else
    if ((allocated_page = allocate_zeroed_frame()) != NULL) {

        if (zero_lists->total_available + zero_lists->total_available < physical_page_count / 5) {
            faulter_refresh_free_and_zero_lists();
        }

        // acquire_pagelock(allocated_page);
        return allocated_page;
    }

    // If we succeed on the free list, we may still have to zero out the frame later
    if ((allocated_page = allocate_free_frame()) != NULL) {
        
        // acquire_pagelock(allocated_page);
        #if DEBUG_PAGELOCK
        assert(allocated_page->holding_threadid == GetCurrentThreadId());
        #endif
        return allocated_page;
    }

    /**
     * Now we have to try to get a page from the standby list
     */
    
    if ((allocated_page = standby_pop_page(standby_list)) == NULL) {
        ResetEvent(waiting_for_pages_event);

        SetEvent(trimming_event);

        InterlockedIncrement64(&wait_count);
        WaitForSingleObject(waiting_for_pages_event, INFINITE);

        return NULL;
    }

    #if DEBUG_PAGELOCK
    assert(allocated_page->holding_threadid == GetCurrentThreadId());
    #endif

    if (standby_list->list_length > physical_page_count / 5) {
        faulter_refresh_free_and_zero_lists();
    }

    // The pagelock is acquired in standby_pop_page
    return allocated_page;
}

/**
 * Rescues the given page from the modified or standby list and stores its relevant disk index in the given pointer, if applicable
 * 
 * Returns SUCCESS if the page is rescued, ERROR otherwise
 */
static int rescue_page(PAGE* page, ULONG64* disk_idx_storage) {

    if (page_is_modified(*page)) {
        if (modified_rescue_page(page) == ERROR) {
            return ERROR;
        }
        *disk_idx_storage = DISK_IDX_NOTUSED;
        assert(page->page_lock == PAGE_LOCKED);
        assert(page->status == MODIFIED_STATUS);
        return SUCCESS;
    }

    if (page_is_standby(*page)) {
        if (standby_rescue_page(page, disk_idx_storage) == ERROR) {
            return ERROR;
        }
        assert(page->page_lock == PAGE_LOCKED);
        assert(page->status == STANDBY_STATUS);
        return SUCCESS;
    }

    return ERROR;
}


/**
 * Rescues the given page from the modified list, if it can be found
 * 
 * Returns SUCCESS if the rescue page was found and removed, ERROR otherwise
 */
static int modified_rescue_page(PAGE* page) {
    // The page is not in the modified list, but is instead being written to disk, but we can still take it
    if (page->writing_to_disk == PAGE_BEING_WRITTEN) {
        page->writing_to_disk = PAGE_NOT_BEING_WRITTEN;
        return SUCCESS;
    }

    EnterCriticalSection(&modified_list->lock);

    if (db_remove_from_middle(modified_list->listhead, page->frame_listnode) == ERROR) {
        LeaveCriticalSection(&modified_list->lock);
        DebugBreak();
        return ERROR;
    }

    modified_list->list_length--;

    LeaveCriticalSection(&modified_list->lock);

    return SUCCESS;
}


/**
 * Rescues the given page from the standby list, if it can be found
 * 
 * Returns SUCCESS if the rescue page was found and removed, ERROR otherwise
 */
static int standby_rescue_page(PAGE* page, ULONG64* disk_idx_storage) {
    EnterCriticalSection(&standby_list->lock);

    if (db_remove_from_middle(standby_list->listhead, page->frame_listnode) == ERROR) {
        LeaveCriticalSection(&standby_list->lock);
        DebugBreak();
        return ERROR;
    }

    standby_list->list_length--;
    InterlockedDecrement64(&total_available_pages);

    LeaveCriticalSection(&standby_list->lock);

    *disk_idx_storage = page->pagefile_idx;
    return SUCCESS;
}


/**
 * Returns the given page to its appropriate list after it is revealed we no longer need it
 * 
 * Assumes that we have the pagelock
 */
static void release_unneeded_page(PAGE* page) {
    if (page->status == STANDBY_STATUS) {
        EnterCriticalSection(&standby_list->lock);

        standby_add_page(page, standby_list);

        InterlockedIncrement64(&total_available_pages);

        LeaveCriticalSection(&standby_list->lock);
    } else if (page->status == MODIFIED_STATUS) {
        EnterCriticalSection(&modified_list->lock);

        modified_add_page(page, modified_list);

        InterlockedIncrement64(&total_available_pages);

        LeaveCriticalSection(&modified_list->lock);
    } else if (page->status == FREE_STATUS) {
        free_frames_add(page);
    } else if (page->status == ZERO_STATUS) {
        zero_lists_add(page);
    } else {
        DebugBreak();
    }

    assert(page->page_lock == PAGE_LOCKED);
    release_pagelock(page);

}


/**
 * Adds the given page to its proper slot in the zero list
 */
static void zero_lists_add(PAGE* page) {
    // Modulo operation based on the pfn to put it alongside other cache-colliding pages
    int listhead_idx = page_to_pfn(page) % NUM_CACHE_SLOTS;

    DB_LL_NODE* relevant_listhead = zero_lists->listheads[listhead_idx];

    #ifdef DEBUG_CHECKING
    int dbg_result;
    if ((dbg_result = page_is_isolated(page)) != ISOLATED) {
        DebugBreak();
    }
    #endif

    EnterCriticalSection(&zero_lists->list_locks[listhead_idx]);

    page->status = FREE_STATUS;

    db_insert_node_at_head(relevant_listhead, page->frame_listnode);
    zero_lists->list_lengths[listhead_idx]++;
    
    InterlockedIncrement64(&total_available_pages);

    InterlockedIncrement64(&zero_lists->total_available);

    LeaveCriticalSection(&zero_lists->list_locks[listhead_idx]);
}


/**
 * Adds the given page to its proper slot in the free list
= */
static void free_frames_add(PAGE* page) {
    // Modulo operation based on the pfn to put it alongside other cache-colliding pages
    int listhead_idx = page_to_pfn(page) % NUM_CACHE_SLOTS;

    DB_LL_NODE* relevant_listhead = free_frames->listheads[listhead_idx];

    #ifdef DEBUG_CHECKING
    int dbg_result;
    if ((dbg_result = page_is_isolated(page)) != ISOLATED) {
        DebugBreak();
    }
    #endif

    EnterCriticalSection(&free_frames->list_locks[listhead_idx]);

    page->status = FREE_STATUS;

    db_insert_node_at_head(relevant_listhead, page->frame_listnode);
    free_frames->list_lengths[listhead_idx]++;
    
    InterlockedIncrement64(&total_available_pages);

    InterlockedIncrement64(&free_frames->total_available);

    LeaveCriticalSection(&free_frames->list_locks[listhead_idx]);
}