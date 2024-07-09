/**
 * @author Ben Williams
 * @date June 25th, 2024
 * 
 * All functions for handling the page fault at the highest level
 */

#include <stdio.h>
#include "../globals.h"
#include "../macros.h"
#include "./trim.h"
#include "./disk_operations.h"
#include "./conversions.h"
#include "./debug_checks.h"
#include "../Datastructures/datastructures.h"
#include "./pagefault.h"


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

static PAGE* rescue_pte(PTE local_pte, PTE* accessed_pte, ULONG64* disk_idx_storage);

static int standby_rescue_page(PTE* accessed_pte, PAGE* rescue_page, ULONG64* disk_idx_storage);

static int modified_rescue_page(PTE* accessed_pte, PAGE* rescue_page);

static PAGE* standby_pop_page();

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

    if (fault_count % KB(64) == 0) {
        printf("Curr fault count 0x%llX\n", fault_count);
    }

    /**
     * Temporary ways to induce trimming and aging
     */
    // if ((total_available_pages < (physical_page_count / 2)) && (fault_count % 16) == 0) {
    //     SetEvent(aging_event);
    // }
    
    if ((total_available_pages < (physical_page_count / 3) && (fault_count % 2) == 0)) {
        SetEvent(trimming_event);
    }


    PTE* accessed_pte = va_to_pte(virtual_address);

    if (accessed_pte == NULL) {
        fprintf(stderr, "Unable to find the accessed PTE\n");
        return REJECTION_FAIL;
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
             * Return the page to the free-list and return ERROR
             * 
             *  BW: Later, we need to zero-out pages that were acquired in the failure case
             *  in another thread and re-add them to the free-list
             */ 

            LeaveCriticalSection(pte_lock);
            free_frames_add(allocated_page);
            return RACE_CONDITION_FAIL;
        }
    }


    allocated_page->active_page.status = ACTIVE_STATUS;
    allocated_page->active_page.pte = accessed_pte;

    // BW: Temporary until we are differentiating between reads and writes and can try to re-trim them
    // quickly by saving the disk index
    allocated_page->active_page.pagefile_idx = DISK_IDX_NOTUSED;

    // BW: Again, we are returning ALL disk slots for now
    if (disk_idx != DISK_IDX_NOTUSED) {
        release_disk_slot(disk_idx);
    }

    // Connect the PTE to the pfn through the CPU 
    connect_pte_to_page(accessed_pte, allocated_page);

    #ifdef DEBUG_CHECKING
    if (pfn_is_single_allocated(page_to_pfn(page_storage)) == FALSE) {
        DebugBreak();
    }
    #endif

    LeaveCriticalSection(pte_lock);

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
 * Handles a pagefault for a PTE that is in transition format - it is either
 * in the modified or standby list
 * 
 * Returns a pointer to the rescued page, or NULL if it could not be rescued.
 * 
 * Changes disk_idx_storage to be the disk index of the page if it was rescued from standby,
 * and is set to DISK_IDX_NOTUSED if it was rescued from modified to signify that it is invalid.
 */
static int handle_transition_pte_fault(PTE local_pte, PTE* accessed_pte, ULONG64* disk_idx_storage, PAGE** result_page_storage) {
    
    PAGE* rescued_page = rescue_pte(local_pte, accessed_pte, disk_idx_storage);

    if (rescued_page == NULL) {
        return RESCUE_FAIL;
    }

    *result_page_storage = rescued_page;

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

    PAGE* allocated_page = find_available_page();

    if (allocated_page == NULL) {
        return NO_AVAILABLE_PAGES_FAIL;
    }

    ULONG64 disk_idx = local_pte.disk_format.pagefile_idx;

    EnterCriticalSection(pte_lock);

    if (ptes_are_equal(local_pte, *accessed_pte) == FALSE) {
        /**
         * Return the page to the free-list and return NULL
         * 
         *  BW: Later, we need to zero-out pages that were acquired in the failure case
         *  in another thread and re-add them to the free-list
         */ 
        LeaveCriticalSection(pte_lock);
        free_frames_add(allocated_page);
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
    if (read_from_disk(allocated_page, disk_idx) == ERROR) {
        LeaveCriticalSection(pte_lock);

        // BW: Here, we would want to add it to the zero-out thread!
        free_frames_add(allocated_page);
        return DISK_FAIL;
    }

    *disk_idx_storage = disk_idx;
    *result_page_storage = allocated_page;

    return SUCCESS;
}


/**
 * Finds an available page from either the free or standby list and returns it
 * 
 * Returns NULL if there were no pages available at the time
 */
static PAGE* find_available_page() {

    PAGE* allocated_page;

    // If we succeed on the free list, we don't have to do anything else
    if ((allocated_page = allocate_free_frame(free_frames)) != NULL) {
        
        InterlockedDecrement64(&total_available_pages);
        return allocated_page;
    }

    /**
     * Now we have to try to get a page from the standby list
     */


    /**
     * By checking if the length is 0 first, we can avoid contending for the standby list lock and hopefully
     * have a page more quickly. A page could be added to the standby list in between the checks, but that would
     * be rare - reducing the standby list contention is going to be more impactful
     */
    // if ((standby_list->list_length == 0) ||
    //     (allocated_page = standby_pop_page(standby_list)) == NULL) {
    
    if ((allocated_page = standby_pop_page(standby_list)) == NULL) {
        ResetEvent(waiting_for_pages_event);

        SetEvent(trimming_event);

        InterlockedIncrement64(&wait_count);
        WaitForSingleObject(waiting_for_pages_event, INFINITE);

        return NULL;
    }

    InterlockedDecrement64(&total_available_pages);
    return allocated_page;
}


/**
 * For the given PTE, find its associated page in the modified or standby list
 * and return it.
 * 
 * Returns a pointer to the recycled_page page upon success, NULL if it could not be found
 */
static PAGE* rescue_pte(PTE local_pte, PTE* accessed_pte, ULONG64* disk_idx_storage) {

    ULONG64 pfn = local_pte.transition_format.frame_number;

    PAGE* rescue_page = pfn_to_page(pfn);
    
    /**
     * Here, we are exposed to a possible race condition where we could rescue a modified page right
     * when it is being added to standby. Therefore, we must also try the standby list right after
     * sequentially. If we succeeded, the page will not be in the standby format.
     */
    if (page_is_modified(*rescue_page)) {
        if (modified_rescue_page(accessed_pte, rescue_page) == SUCCESS) {
            *disk_idx_storage = DISK_IDX_NOTUSED;
            return rescue_page;
        }
    } 
    

    /**
     * Again, we could have a race condition where we try to get the page from the standby list
     * just as it is given to someone else. So if it fails, we know that this PTE was unlucky
     * and will have to get a new page entirely.
     */
    if (page_is_standby(*rescue_page)){
        if (standby_rescue_page(accessed_pte, rescue_page, disk_idx_storage) == SUCCESS) {
            return rescue_page;
        }
    }


    // if (rescue_page->active_page.status == ACTIVE_STATUS) {
    //     DebugBreak();
    // }

    /**
     * At this point, the page was either rescued by another thread or was recycled and given to someone
     * else. Regardless, the page fault must be re-attempted as this thread's rescue operation failed
     */
    PRINT_F("Rescue failed on pfn %llX\n", local_pte.transition_format.frame_number);
    *disk_idx_storage = DISK_IDX_NOTUSED;
    return NULL;
}


/**
 * Rescues the given page from the modified list, if it can be found
 * 
 * Returns SUCCESS if the rescue page was found and removed, ERROR otherwise
 */
static int modified_rescue_page(PTE* accessed_pte, PAGE* rescue_page) {
    if (rescue_page == NULL) {
        fprintf(stderr, "NULL modified list or rescue_page given to modified_rescue_page\n");
    }
    
    EnterCriticalSection(&modified_list->lock);

    if (rescue_page->modified_page.status != MODIFIED_STATUS) {
        LeaveCriticalSection(&modified_list->lock);
        return ERROR;
    }

    // The trimming thread is in moving this from the modified list to the standby list.
    // The rescue failing here should hopefully only be temporary
    if (rescue_page->modified_page.frame_listnode->listhead_ptr != modified_list->listhead) {
        LeaveCriticalSection(&modified_list->lock);
        return ERROR;
    }

    if (rescue_page->modified_page.pte != accessed_pte) {
        LeaveCriticalSection(&modified_list->lock);
        return ERROR;
    }

    db_remove_from_middle(modified_list->listhead, rescue_page->modified_page.frame_listnode);
    modified_list->list_length--;

    rescue_page->active_page.status = ACTIVE_STATUS;


    LeaveCriticalSection(&modified_list->lock);

    #ifdef DEBUG_CHECKING
    int dbg_result;
    if ((dbg_result = page_is_isolated(rescue_page)) != ISOLATED) {
        DebugBreak();
    }
    #endif

    return SUCCESS;
}


/**
 * Rescues the given page from the standby list, if it can be found
 * 
 * Returns SUCCESS if the rescue page was found and removed, ERROR otherwise
 */
static int standby_rescue_page(PTE* accessed_pte, PAGE* rescue_page, ULONG64* disk_idx_storage) {
    if (standby_list == NULL || rescue_page == NULL) {
        fprintf(stderr, "NULL standby list or rescue page given to standby_rescue_page\n");
    }

    EnterCriticalSection(&standby_list->lock);

    if (rescue_page->standby_page.status != STANDBY_STATUS) {
        LeaveCriticalSection(&standby_list->lock);
        return ERROR;
    }

    // Someone was able to remove the page from the standby list before we reached it
    if (rescue_page->standby_page.frame_listnode->listhead_ptr != standby_list->listhead) {
        // In the current implementation, this should not happen
        DebugBreak();
        LeaveCriticalSection(&standby_list->lock);
        return ERROR;
    }

    /**
     * Take pointer of the PTE and compare it to the rescue page's PTE, if they differ,
     * return ERROR.
     */
    if (rescue_page->standby_page.pte != accessed_pte) {
        LeaveCriticalSection(&standby_list->lock);
        return ERROR;
    }

    db_remove_from_middle(standby_list->listhead, rescue_page->standby_page.frame_listnode);
    standby_list->list_length--;

    // Since we are rescuing the page, we don't need it on disk right now.
    // BW: Later, we can take shortcuts adding the page straight to the standby list if it hasn't been modified

    ULONG64 disk_idx = rescue_page->standby_page.pagefile_idx;

    if (disk->disk_slot_statuses[disk_idx] == DISK_FREESLOT) {
        DebugBreak();
    }

    rescue_page->active_page.status = ACTIVE_STATUS;

    InterlockedDecrement64(&total_available_pages);

    LeaveCriticalSection(&standby_list->lock);

    #ifdef DEBUG_CHECKING
    int dbg_result;
    if ((dbg_result = page_is_isolated(rescue_page)) != ISOLATED) {
        DebugBreak();
    }
    #endif

    // Set the disk idx storage to where the standby page's information used to be stored
    *disk_idx_storage = rescue_page->standby_page.pagefile_idx;

    // EnterCriticalSection(disk_idx_to_lock(disk_idx));
    // release_disk_slot(disk_idx);
    // LeaveCriticalSection(disk_idx_to_lock(disk_idx));

    return SUCCESS;
}


/**
 * Pops and returns a pointer to the oldest page from the standby list and returns it, 
 * and modifies its old PTE to be in the disk format.
 * 
 * Returns NULL upon any error or if the list is empty
 */
static PAGE* standby_pop_page() {

    EnterCriticalSection(&standby_list->lock);

    PAGE* recycled_page = db_pop_from_tail(standby_list->listhead);
    
    // Standby list is empty
    if (recycled_page == NULL) {
        LeaveCriticalSection(&standby_list->lock);
        return NULL;
    }

    standby_list->list_length -= 1;

    // Prepare to update the old PTE to reflect that it is on disk
    PTE* old_pte = recycled_page->standby_page.pte;
    PTE pte_contents;
    pte_contents.complete_format = 0;
    pte_contents.disk_format.always_zero = 0;
    pte_contents.disk_format.pagefile_idx = recycled_page->standby_page.pagefile_idx;
    pte_contents.disk_format.always_zero2 = 0;

    CRITICAL_SECTION* old_pte_lock = &pte_to_locksection(old_pte)->lock;

    // We must modify the other PTE to reflect that it is on the disk
    EnterCriticalSection(old_pte_lock);

    #ifdef DEBUG_CHECKING
    if (pfn_is_single_allocated(page_to_pfn(recycled_page)) == FALSE) {
        DebugBreak();
    }
    #endif

    write_pte_contents(old_pte, pte_contents);

    LeaveCriticalSection(old_pte_lock);

    // This ensures that any PTE attempting to rescue this page when it acquires the standby
    // lock will fail
    recycled_page->active_page.status = ACTIVE_STATUS;

    InterlockedDecrement64(&total_available_pages);

    LeaveCriticalSection(&standby_list->lock);

    #ifdef DEBUG_CHECKING
    int dbg_result;
    if ((dbg_result = page_is_isolated(recycled_page)) != ISOLATED) {
        DebugBreak();
    }
    #endif

    return recycled_page;
}   


/**
 * Adds the given page to its proper slot in the free list
 * 
 * As of now, does NOT zero out the page!
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 */
static void free_frames_add(PAGE* page) {
    // printf("Returning page %p\n", page);
    // Modulo operation based on the pfn to put it alongside other cache-colliding pages
    int listhead_idx = page_to_pfn(page) % NUM_FRAME_LISTS;

    DB_LL_NODE* relevant_listhead = free_frames->listheads[listhead_idx];

    #ifdef DEBUG_CHECKING
    int dbg_result;
    if ((dbg_result = page_is_isolated(page)) != ISOLATED) {
        DebugBreak();
    }
    #endif

    EnterCriticalSection(&free_frames->list_locks[listhead_idx]);

    page->free_page.frame_number = page_to_pfn(page);
    page->free_page.status = FREE_STATUS;
    page->free_page.zeroed_out = 0; // Until we are actually zeroing out frames

    db_insert_node_at_head(relevant_listhead, page->free_page.frame_listnode);
    free_frames->list_lengths[listhead_idx]++;
    
    InterlockedIncrement64(&total_available_pages);

    InterlockedIncrement64(&free_frames->total_available);

    LeaveCriticalSection(&free_frames->list_locks[listhead_idx]);

    SetEvent(waiting_for_pages_event);
}