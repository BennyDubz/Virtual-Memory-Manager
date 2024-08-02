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
static int handle_valid_pte_fault(PTE local_pte, PTE* accessed_pte, ULONG64 access_type);

static int handle_unaccessed_pte_fault(PTE local_pte, PAGE** result_page_storage);

static int handle_transition_pte_fault(PTE local_pte, PTE* accessed_pte, PAGE** result_page_storage);

static int handle_disk_pte_fault(PTE local_pte, PTE* accessed_pte, PAGE** result_page_storage);

static PAGE* find_available_page(PTE local_pte);

static int rescue_page(PAGE* page);

static int modified_rescue_page(PAGE* page);

static int standby_rescue_page(PAGE* page);

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
int pagefault(PULONG_PTR virtual_address, ULONG64 access_type) {
    InterlockedIncrement64(&fault_count);
    
    if (fault_count % KB(128) == 0) {
        printf("Curr fault count 0x%llX\n", fault_count);
        printf("\t Phys page standby ratio: %f Zeroed: 0x%llX Free: 0x%llX Standby: 0x%llX Mod 0x%llX Num disk slots %llx\n", (double)  standby_list->list_length / physical_page_count, zero_lists->total_available, free_frames->total_available, 
                                            standby_list->list_length, modified_list->list_length, disk->total_available_slots);
    }

    /**
     * Temporary ways to induce trimming and aging
     */    
    if ((total_available_pages < (physical_page_count / 4) && (fault_count % 32) == 0) 
            && modified_list->list_length < physical_page_count / 4) {
        SetEvent(trimming_event);
    }

    PTE* accessed_pte = va_to_pte(virtual_address);

    if (accessed_pte == NULL) {
        fprintf(stderr, "Unable to find the accessed PTE\n");
        return REJECTION_FAIL;
    }

    // We try to trim behind us
    if (fault_count % 8 == 0 && modified_list->list_length < physical_page_count / 4) {
        faulter_trim_behind(accessed_pte);
    }

    // Make a copy of the PTE in order to perform the fault
    PTE local_pte = read_pte_contents(accessed_pte);
    PAGE* allocated_page = NULL;
    CRITICAL_SECTION* pte_lock = &pte_to_locksection(accessed_pte)->lock;
    int handler_result;

    if (is_memory_format(local_pte)) {

        if ((handler_result = handle_valid_pte_fault(local_pte, accessed_pte, access_type)) != SUCCESS) {
            return handler_result;
        }

        return SUCCESSFUL_FAULT;

    } else if (is_used_pte(local_pte) == FALSE) {

        if ((handler_result = handle_unaccessed_pte_fault(local_pte, &allocated_page)) != SUCCESS) {
            return handler_result;
        }

    } else if (is_transition_format(local_pte)) {

        if ((handler_result = handle_transition_pte_fault(local_pte, accessed_pte, &allocated_page)) != SUCCESS) {
            return handler_result;
        }

    } else if (is_disk_format(local_pte)) {

        if ((handler_result = handle_disk_pte_fault(local_pte, accessed_pte, &allocated_page)) != SUCCESS) {
            return handler_result;
        }

    }

    // For whatever reason the relevant handler failed, so we should retry the fault
    if (allocated_page == NULL) {
        DebugBreak();
    }

    #if DEBUG_PAGELOCK
    custom_spin_assert(allocated_page->holding_threadid == GetCurrentThreadId());
    #endif
    custom_spin_assert(allocated_page->page_lock == PAGE_LOCKED);
    custom_spin_assert(allocated_page->status != ACTIVE_STATUS);
    


    /**
     * For disk reads on the same PTE (or PTE section), only a single faulting thread will end up here and will already have the PTE lock
     * 
     * For transition PTEs, we can edit them with only the pagelock and **do not** need the corresponding PTE lock. This is because
     * only one rescuer at a time will hold the relevant pagelock, and if a standby page is repurposed, the repurposer will hold the pagelock.
     * This means that only a single thread at a time would be able to change a transition PTE so long as they hold the pagelock, making its corresponding
     * PTE lock redundant. By not aquiring it, we can reduce contention on the PTE locks
     */
    if (is_used_pte(local_pte) == FALSE) {
        EnterCriticalSection(pte_lock);
    
        /**
         * More than one thread was trying to access this PTE at once, meaning that only one of them succeeded and mapped the page
         */
        if (ptes_are_equal(local_pte, *accessed_pte) == FALSE) {
            LeaveCriticalSection(pte_lock);
            release_unneeded_page(allocated_page);
            return RACE_CONDITION_FAIL;
        }

    }

    // We may need to edit the old PTE, in which case, we want a copy so we can still find it
    PAGE allocated_page_old = *allocated_page;

    // We may need to release stale pagefile slots and/or modify the page's pagefile information
    handle_end_of_fault_disk_slot(local_pte, allocated_page, access_type);

    /**
     * For unaccessed PTEs, we need to ensure they start off with a clean page
     * 
     * Note - this is treating the individual threads to be similar to different processes accessing the same
     * address space, which would not be the case normally. However, this allows us to demonstrate the infrastructure
     * required to zero out pages when needed. Typically, different threads within a process would not need to have zeroed out pages
     * if their previous owner was withn the same process - since the threads are a part of the same process, we do not have the
     * security concern of sharing data between processes. 
     */
    if (allocated_page->status != ZERO_STATUS && is_used_pte(local_pte) == FALSE) {
        zero_out_pages(&allocated_page, 1);
    }

    allocated_page->status = ACTIVE_STATUS;
    allocated_page->pte = accessed_pte;

    // We are writing to the page - this may communicate to the modified writer that they need to return pagefile space
    if (access_type == WRITE_ACCESS) {
        allocated_page->modified = PAGE_MODIFIED;
    }

    // Connect the PTE to the pfn through the CPU, and sets the approprate permissions
    if (connect_pte_to_page(accessed_pte, allocated_page, access_type) == ERROR) {
        DebugBreak();
    }

    // Transition PTEs do not need the PTE lock, but everyone else does and will hold it
    if (is_transition_format(local_pte) == FALSE) {
        LeaveCriticalSection(pte_lock);
    }


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

        custom_spin_assert(allocated_page_old.pagefile_idx != DISK_IDX_NOTUSED);
        pte_contents.disk_format.pagefile_idx = allocated_page_old.pagefile_idx;

        write_pte_contents(old_pte, pte_contents);
    }

    release_pagelock(allocated_page, 11);

    return SUCCESSFUL_FAULT;
}   


/**
 * Handles the fault for a fault on a valid PTE
 * 
 * This means that we likely need to adjust the permissions of the PTE, potentially throw out pagefile space,
 * and we can take the opportunity to reset the age to zero
 */
static int handle_valid_pte_fault(PTE local_pte, PTE* accessed_pte, ULONG64 access_type) {

    custom_spin_assert(local_pte.memory_format.protections != PTE_PROTNONE);

    CRITICAL_SECTION* pte_lock = &pte_to_locksection(accessed_pte)->lock;
    PULONG_PTR pte_va = pte_to_va(accessed_pte);
    DWORD old_protection_storage_notused;
    
    // Prepare the contents ahead of time depending on whether or not we are changing the permissions
    PTE pte_contents;
    pte_contents.complete_format = 0;
    pte_contents.memory_format.valid = VALID;
    pte_contents.memory_format.frame_number = local_pte.memory_format.frame_number;
    pte_contents.memory_format.age = 0;

    if (access_type == READ_ACCESS) {
        pte_contents.memory_format.protections = PTE_PROTREAD;
    } else {
        pte_contents.memory_format.protections = PTE_PROTREADWRITE;
    }

    PAGE* curr_page = pfn_to_page(local_pte.memory_format.frame_number);

    acquire_pagelock(curr_page, 33);

    EnterCriticalSection(pte_lock);

    if (ptes_are_equal(local_pte, *accessed_pte) == FALSE) {
        LeaveCriticalSection(pte_lock);
        release_pagelock(curr_page, 35);
        return RACE_CONDITION_FAIL;
    }

    // PAGE* curr_page = pfn_to_page(local_pte.memory_format.frame_number);

    // See if we need to change the permissions to PAGE_READWRITE
    if (access_type == WRITE_ACCESS && local_pte.memory_format.protections == PTE_PROTREAD) {
        ULONG64 pfn = page_to_pfn(curr_page);

        // All we want to do is change the permissions on the page - using MapUserPhysicalPages for this happens to be faster than
        // using VirtualProtect
        if (MapUserPhysicalPages (pte_to_va(accessed_pte), 1, &pfn) == FALSE) {

            fprintf (stderr, "handle_valid_pte_fault : could not map VA %p to pfn %llx\n", pte_to_va(accessed_pte), pfn);
            DebugBreak();

            return ERROR;
        }

        if (curr_page->pagefile_idx != DISK_IDX_NOTUSED) {
            release_single_disk_slot(curr_page->pagefile_idx);
            curr_page->pagefile_idx = DISK_IDX_NOTUSED;
        }

    } else if (access_type == WRITE_ACCESS) {
        custom_spin_assert(curr_page->pagefile_idx == DISK_IDX_NOTUSED);
    }

    release_pagelock(curr_page, 34);

    custom_spin_assert(pfn_to_page(accessed_pte->memory_format.frame_number)->status == ACTIVE_STATUS);

    write_pte_contents(accessed_pte, pte_contents);

    LeaveCriticalSection(pte_lock);

    return SUCCESS;
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
    PAGE* allocated_page = find_available_page(local_pte);

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
static int handle_transition_pte_fault(PTE local_pte, PTE* accessed_pte, PAGE** result_page_storage) {
    ULONG64 pfn = local_pte.transition_format.frame_number;

    PAGE* page_to_rescue = pfn_to_page(pfn);

    // After we acquire the pagelock, if the page is rescuable then we should always be able to succeed
    acquire_pagelock(page_to_rescue, 1);

    // We lost the race to rescue this PTE - whether it was stolen from under us or someone else saved it
    if (page_to_rescue->pte != accessed_pte || page_to_rescue->status == ACTIVE_STATUS) {
        release_pagelock(page_to_rescue, 2);
        return RESCUE_FAIL;
    }

    // We now try to rescue the page from the modified or standby list
    if (rescue_page(page_to_rescue) == ERROR) {
        DebugBreak();
        release_pagelock(page_to_rescue, 3);
        return RESCUE_FAIL;
    }

    *result_page_storage = page_to_rescue;

    custom_spin_assert(page_to_rescue->page_lock == PAGE_LOCKED);
    custom_spin_assert(page_to_rescue->status != ACTIVE_STATUS);
    #if DEBUG_PAGELOCK
    custom_spin_assert(page_to_rescue->holding_threadid == GetCurrentThreadId());
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
static int handle_disk_pte_fault(PTE local_pte, PTE* accessed_pte, PAGE** result_page_storage) {
    CRITICAL_SECTION* pte_lock = &pte_to_locksection(accessed_pte)->lock;

    // We also now hold the allocated pagelock
    PAGE* allocated_page = find_available_page(local_pte);

    if (allocated_page == NULL) {
        return NO_AVAILABLE_PAGES_FAIL;
    }

    ULONG64 disk_idx = local_pte.disk_format.pagefile_idx;

    EnterCriticalSection(pte_lock);

    custom_spin_assert(allocated_page->page_lock == PAGE_LOCKED);
    custom_spin_assert(allocated_page->status != ACTIVE_STATUS);

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

    *result_page_storage = allocated_page;

    custom_spin_assert(allocated_page->page_lock == PAGE_LOCKED);
    custom_spin_assert(allocated_page->status != ACTIVE_STATUS);


    return SUCCESS;
}


/**
 * Sometimes, we will want to refresh the free and zero lists
 */
#define STANDBY_LIST_REFRESH_PROPORTION 8
#define TOTAL_ZERO_AND_FREE_LIST_REFRESH_PROPORTION 20
#define FREE_LIST_REFRESH_PROPORTION 25
static void potential_faulter_list_refresh() {
    if (standby_list->list_length > physical_page_count / STANDBY_LIST_REFRESH_PROPORTION) {
        if (free_frames->total_available + zero_lists->total_available < physical_page_count / TOTAL_ZERO_AND_FREE_LIST_REFRESH_PROPORTION) {
            faulter_refresh_free_and_zero_lists();
        } 
        #if 1
        else if (free_frames->total_available < physical_page_count / FREE_LIST_REFRESH_PROPORTION) {
            faulter_refresh_free_frames();
        }
        #endif
    }
}


/**
 * Finds an available page from either the free or standby list and returns it
 * 
 * Returns NULL if there were no pages available at the time
 */
#define REDUCE_FREE_LIST_PRESSURE_PROPORTION 4
static PAGE* find_available_page(PTE local_pte) {

    PAGE* allocated_page;
    BOOL zero_first;

    
    // Since disk reads overwrite page contents, we do not need zeroed out pages.
    // and we would rather them be saved for accesses that do need zeroed pages
    if (is_disk_format(local_pte) || free_frames->total_available < NUM_CACHE_SLOTS * REDUCE_FREE_LIST_PRESSURE_PROPORTION) {
        zero_first = FALSE;
    } else {
        zero_first = TRUE;
    }

    if (zero_first) {
        // If we succeed on the zeroed list, this is the best case scenario as we don't have to do anything else
        if ((allocated_page = allocate_zeroed_frame()) != NULL) {

            potential_faulter_list_refresh();

            return allocated_page;
        }

        // If we succeed on the free list, we may still have to zero out the frame later
        if ((allocated_page = allocate_free_frame()) != NULL) {

            potential_faulter_list_refresh();

            #if DEBUG_PAGELOCK
            custom_spin_assert(allocated_page->holding_threadid == GetCurrentThreadId());
            #endif
            return allocated_page;
        }
    } else {
        // In this case - we don't need the pages to be zeroed already - so the free list is fine
        if ((allocated_page = allocate_free_frame()) != NULL) {

            potential_faulter_list_refresh();

            #if DEBUG_PAGELOCK
            custom_spin_assert(allocated_page->holding_threadid == GetCurrentThreadId());
            #endif
            return allocated_page;
        }

        // We still prefer the faster access to the zeroed list than anything on standby
        if ((allocated_page = allocate_zeroed_frame()) != NULL) {

            potential_faulter_list_refresh();

            return allocated_page;
        }
    }


    /**
     * Now we have to try to get a page from the standby list
     */
    if ((allocated_page = standby_pop_page(standby_list)) == NULL) {
        ResetEvent(waiting_for_pages_event);

        SetEvent(trimming_event);

        if (modified_list->list_length > 0) {
            SetEvent(modified_to_standby_event);
        }

        InterlockedIncrement64(&wait_count);
        WaitForSingleObject(waiting_for_pages_event, INFINITE);

        return NULL;
    }

    #if DEBUG_PAGELOCK
    custom_spin_assert(allocated_page->holding_threadid == GetCurrentThreadId());
    #endif

    potential_faulter_list_refresh();

    // The pagelock is acquired in standby_pop_page
    return allocated_page;
}


/**
 * Rescues the given page from the modified or standby list and stores its relevant disk index in the given pointer, if applicable
 * 
 * Returns SUCCESS if the page is rescued, ERROR otherwise
 */
static int rescue_page(PAGE* page) {

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
static int modified_rescue_page(PAGE* page) {
    // The page is not in the modified list, but is instead being written to disk, but we can still take it
    if (page->writing_to_disk == PAGE_BEING_WRITTEN && page->modified == PAGE_NOT_MODIFIED) {
        #if DEBUG_LISTS
        custom_spin_assert(page->frame_listnode->listhead_ptr == NULL);
        #endif
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
static int standby_rescue_page(PAGE* page) {
    EnterCriticalSection(&standby_list->lock);

    if (db_remove_from_middle(standby_list->listhead, page->frame_listnode) == ERROR) {
        LeaveCriticalSection(&standby_list->lock);
        DebugBreak();
        return ERROR;
    }

    standby_list->list_length--;
    InterlockedDecrement64(&total_available_pages);

    LeaveCriticalSection(&standby_list->lock);

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

    page->status = ZERO_STATUS;

    db_insert_node_at_head(relevant_listhead, page->frame_listnode);
    zero_lists->list_lengths[listhead_idx]++;
    
    InterlockedIncrement64(&total_available_pages);

    InterlockedIncrement64(&zero_lists->total_available);

    LeaveCriticalSection(&zero_lists->list_locks[listhead_idx]);
}


/**
 * Adds the given page to its proper slot in the free list
 */
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