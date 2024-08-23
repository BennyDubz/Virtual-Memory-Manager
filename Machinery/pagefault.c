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

#define SEQUENTIAL_SPECULATIVE_DISK_READS 1
// #define SPECULATIVE_DISK_READ_AVAILABLE_PAGE_PROPORTION  50


/**
 * GLOBALS FOR PAGE FAULTING
 */


ULONG64 fault_count = 0;

/**
 * Function declarations
 */
static int handle_valid_pte_fault(PTE local_pte, PTE* accessed_pte, ULONG64 access_type);

static int handle_unaccessed_pte_fault(PTE* accessed_pte, PAGE** result_page_storage, PTE** ptes_to_connect_storage, ULONG64* num_ptes_to_connect_storage);

static int handle_transition_pte_fault(PTE local_pte, PTE* accessed_pte, PAGE** result_page_storage, PTE** ptes_to_connect_storage, ULONG64* num_ptes_to_connect_storage);

static BOOL acquire_disk_pte_read_rights(PTE* accessed_pte, PTE local_pte);

static int handle_disk_pte_fault(PTE local_pte, PTE* accessed_pte, PAGE** result_page_storage, PTE** ptes_to_connect_storage, ULONG64* num_ptes_to_connect_storage);

static ULONG64 get_trailing_valid_pte_count(PTE* accessed_pte);

static void end_of_fault_work();


/**
 * Handles the page fault for the given virtual address
 * 
 * Returns SUCCESS if the access can be attempted again, ERROR if
 * the fault must either be re-attempted or if the virtual address's corresponding
 * PTE was modified by another thread in the meantime
 * 
 */
int pagefault(PULONG_PTR virtual_address, ULONG64 access_type, ULONG64 thread_idx) {    
    
    #if 0
    if (fault_count % KB(128) == 0) {
        // ULONG64 manual_count_disk_slots = 0;

        // for (ULONG64 i = 0; i < DISK_STORAGE_SLOTS; i++) {
        //     if (disk->disk_slot_statuses[i] == DISK_FREESLOT) manual_count_disk_slots++;
        // }

        printf("Curr fault count 0x%llX\n", fault_count);
        printf("\tPhys page standby ratio: %f Zeroed: 0x%llX Free: 0x%llX Standby: 0x%llX Mod 0x%llX Num disk slots %llX\n", (double)  standby_list->list_length / physical_page_count, zero_lists->total_available, free_frames->total_available, 
                                            standby_list->list_length, modified_list->list_length, disk->total_available_slots);
    }
    #endif

    #ifdef DEBUG_CHECKING
    custom_spin_assert(modified_list->list_length <= disk->total_available_slots);
    custom_spin_assert(disk->disk_slot_statuses[DISK_IDX_NOTUSED] == DISK_USEDSLOT);
    #endif

    PTE* accessed_pte = va_to_pte(virtual_address);

    if (accessed_pte == NULL) {
        fprintf(stderr, "Unable to find the accessed PTE in pagefault handler\n");
        return REJECTION_FAIL;
    }

    if (access_type != READ_ACCESS && access_type != WRITE_ACCESS) {
        fprintf(stderr, "Invalid access type given to pagefault handler\n");
        return REJECTION_FAIL;
    }

    if (thread_idx > thread_information.total_thread_count) {
        fprintf(stderr, "Invalid thread index given to pagefault handler\n");
        return REJECTION_FAIL;
    }

    // Make a copy of the PTE in order to perform the fault
    PTE local_pte = read_pte_contents(accessed_pte);
    PAGE* unneeded_pages[MAX_PAGES_READABLE];
    ULONG64 num_unneeded_pages = 0;

    PAGE* allocated_pages[MAX_PAGES_READABLE];
    PTE* ptes_to_connect[MAX_PAGES_READABLE];
    ULONG64 num_ptes_to_connect;

    CRITICAL_SECTION* pte_lock = &pte_to_locksection(accessed_pte)->lock;
    int handler_result;

    if (is_memory_format(local_pte)) {

        if ((handler_result = handle_valid_pte_fault(local_pte, accessed_pte, access_type)) != SUCCESS) {
            return handler_result;
        }

        return SUCCESSFUL_FAULT;

    } else if (is_used_pte(local_pte) == FALSE) {

        if ((handler_result = handle_unaccessed_pte_fault(accessed_pte, allocated_pages, ptes_to_connect, &num_ptes_to_connect)) != SUCCESS) {
            return handler_result;
        }

    } else if (is_transition_format(local_pte)) {

        if ((handler_result = handle_transition_pte_fault(local_pte, accessed_pte, allocated_pages, ptes_to_connect, &num_ptes_to_connect)) != SUCCESS) {
            return handler_result;
        }

        custom_spin_assert(is_transition_format(*ptes_to_connect[0]));

        #if 0
        ptes_to_connect[0] = accessed_pte;
        num_ptes_to_connect = 1;
        #endif

    } else if (is_disk_format(local_pte)) {

        if ((handler_result = handle_disk_pte_fault(local_pte, accessed_pte, allocated_pages, ptes_to_connect, &num_ptes_to_connect)) != SUCCESS) {
            return handler_result;
        }

    }

    // At this point, we have our pages and PTEs that we need to conenct
    

    /**
     * Only unaccessed PTEs need to acquire the lock and check for race conditions here, due to the following policies:
     * 
     * Transition PTEs can be edited by holding their corresponding pagelock
     * 
     * Disk PTEs are only edited at the end of faults by the threads who set their being_read_from_disk field in the PTE. This means that we have to access
     * the PTE lock earlier on in the fault in order to set this field, but we do not need to re-acquire it when we are finishing the fault
     * 
     * Unaccessed PTEs, however, get their pages acquired BEFORE we acquire the PTE lock. This reduces the time that we hold the PTE lock
     * but comes with the drawback that we might acquire pages that we do not need in the end and need to return them.
     */
    if (is_used_pte(local_pte) == FALSE) {
        EnterCriticalSection(pte_lock);
        PTE* ptes_not_changed[MAX_PAGES_READABLE];
        ULONG64 num_ptes_not_changed = 0;

        for (ULONG64 i = 0; i < num_ptes_to_connect; i++) {
            // If the PTE is still unaccessed, we can still connect it
            if (is_used_pte(read_pte_contents(ptes_to_connect[i])) == FALSE) {
                ptes_not_changed[num_ptes_not_changed] = ptes_to_connect[i];
                num_ptes_not_changed++;
            }
        }

        // All of our PTEs have changed, there is nothing for us to do
        if (num_ptes_not_changed == 0) {
            LeaveCriticalSection(pte_lock);
            release_batch_unneeded_pages(allocated_pages, num_ptes_to_connect);
            return UNACCESSED_RACE_CONDITION_FAIL;
        }

        for (ULONG64 i = 0; i < num_ptes_not_changed; i++) {
            ptes_to_connect[i] = ptes_not_changed[i];
        }

        // We need to ensure that we return the pages that we do not need anymore back to their lists
        if (num_ptes_not_changed < num_ptes_to_connect) {
            for (ULONG64 unneeded_page_idx = num_ptes_not_changed; unneeded_page_idx < num_ptes_to_connect; unneeded_page_idx++) {
                unneeded_pages[num_unneeded_pages] = allocated_pages[unneeded_page_idx];
                num_unneeded_pages++;
            }
        }

        num_ptes_to_connect = num_ptes_not_changed;

        #if 0
        /**
         * More than one thread was trying to access this PTE at once, meaning that only one of them succeeded and mapped the page
         */
        if (ptes_are_equal(local_pte, *accessed_pte) == FALSE) {
            LeaveCriticalSection(pte_lock);
            release_unneeded_page(allocated_pages[0]);
            return UNACCESSED_RACE_CONDITION_FAIL;
        }
        #endif
    }

    // We may need to edit the old PTE, in which case, we want a copy so we can still find it
    PAGE allocated_pages_copies[MAX_PAGES_READABLE];

    if (is_transition_format(local_pte) == FALSE) {
        for (ULONG64 i = 0; i < num_ptes_to_connect; i++) {
            allocated_pages_copies[i] = *allocated_pages[i];
        }
    }

    // We may need to release stale pagefile slots and/or modify the page's pagefile information
    if (num_ptes_to_connect == 1) {
        handle_end_of_fault_disk_slot(read_pte_contents(ptes_to_connect[0]), allocated_pages[0], access_type);
    } else {
        handle_batch_end_of_fault_disk_slot(ptes_to_connect, accessed_pte, allocated_pages, access_type, num_ptes_to_connect);
    }

    /**
     * For unaccessed PTEs, we need to ensure they start off with a clean page
     * 
     * Note - this is treating the individual threads to be similar to different processes accessing the same
     * address space, which would not be the case normally. However, this allows us to demonstrate the infrastructure
     * required to zero out pages when needed. Typically, different threads within a process would not need to have zeroed out pages
     * if their previous owner was withn the same process - since the threads are a part of the same process, we do not have the
     * security concern of sharing data between processes. 
     */

    PAGE* pages_to_zero[MAX_PAGES_READABLE];
    ULONG64 num_pages_to_zero = 0;

    for (ULONG64 i = 0; i < num_ptes_to_connect; i++) {
        if (is_used_pte(read_pte_contents(ptes_to_connect[i])) == FALSE) {
            if (allocated_pages[i]->status != ZERO_STATUS) {
                pages_to_zero[num_pages_to_zero] = allocated_pages[i];
                num_pages_to_zero++;
            }
        }

        allocated_pages[i]->status = ACTIVE_STATUS;
        allocated_pages[i]->pte = ptes_to_connect[i];

        #if LIGHT_DEBUG_PAGELOCK
        allocated_pages[i]->acquiring_pte_copy = *ptes_to_connect[i];
        #endif
    }

    if (num_pages_to_zero > 0) {
        zero_out_pages(pages_to_zero, num_pages_to_zero);
    }

    // We are writing to the page - this may communicate to the modified writer that they need to return pagefile space
    if (access_type == WRITE_ACCESS) {
        allocated_pages[0]->modified = PAGE_MODIFIED;
    }

    if (num_ptes_to_connect == 1) {
        if (connect_pte_to_page(ptes_to_connect[0], allocated_pages[0], access_type) == ERROR) {
            DebugBreak();
        }
    } else {
        if (connect_batch_ptes_to_pages(ptes_to_connect, accessed_pte, allocated_pages, access_type, num_ptes_to_connect) == ERROR) {
            DebugBreak();
        }
    }

    
    if (is_used_pte(local_pte) == FALSE) {
        LeaveCriticalSection(pte_lock);
    }
    

    /**
     * We need to modify the other PTEs associated with the standby pages now that we are committing
     * 
     * The worst case is that other threads are spinning on the pagelock in transition format waiting for this to happen,
     * and they will need to retry the fault as their PTE will be in disk format
     */

    //BW: We will need to change this if we speculate on more than just disk PTEs!
    if (is_transition_format(local_pte) == FALSE) {
        PAGE curr_page_copy;
        PTE pte_contents;
        pte_contents.complete_format = 0;

        for (ULONG64 i = 0; i < num_ptes_to_connect; i++) {
            curr_page_copy = allocated_pages_copies[i];

            if (curr_page_copy.status == STANDBY_STATUS) {
                PTE* old_pte = curr_page_copy.pte;
                pte_contents.disk_format.pagefile_idx = curr_page_copy.pagefile_idx;

                write_pte_contents(old_pte, pte_contents);
            }
        }
    }

    for (ULONG64 i = 0; i < num_ptes_to_connect; i++) {
        release_pagelock(allocated_pages[i], 11);
    }
    
    // For unaccessed PTEs that we might have speculated on incorrectly, we might need to return pages that we never used
    if (num_unneeded_pages > 0) {
        release_batch_unneeded_pages(unneeded_pages, num_unneeded_pages);
    }

    end_of_fault_work(accessed_pte, thread_idx);

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

    /**
     * Here - if the PTE that we read is still in memory format, then we can guarantee that we are the only ones who can access
     * it and that no one else can trim it. This allows us to edit the PTE without the PTE lock and 
     */

    PTE updated_pte_copy = read_pte_contents(accessed_pte);

    if (ptes_are_equal(local_pte, updated_pte_copy) == FALSE) {
        release_pagelock(curr_page, 36);
        return VALID_PTE_RACE_CONTIION_FAIL;
    }

    #if 0
    EnterCriticalSection(pte_lock);

    if (ptes_are_equal(local_pte, *accessed_pte) == FALSE) {
        LeaveCriticalSection(pte_lock);
        release_pagelock(curr_page, 35);
        return VALID_PTE_RACE_CONTIION_FAIL;
    }
    #endif

    // PAGE* curr_page = pfn_to_page(local_pte.memory_format.frame_number);

    // See if we need to change the permissions to PAGE_READWRITE
    if (access_type == WRITE_ACCESS && local_pte.memory_format.protections == PTE_PROTREAD) {
        ULONG64 pfn = page_to_pfn(curr_page);

        // All we want to do is change the permissions on the page - using MapUserPhysicalPages for this happens to be faster than
        // using VirtualProtect
        if (MapUserPhysicalPages (pte_to_va(accessed_pte), 1, &pfn) == FALSE) {

            fprintf (stderr, "handle_valid_pte_fault : could not map VA %p to pfn %llX\n", pte_to_va(accessed_pte), pfn);
            DebugBreak();
        }

        if (curr_page->pagefile_idx != DISK_IDX_NOTUSED) {
            release_single_disk_slot(curr_page->pagefile_idx);
            curr_page->pagefile_idx = DISK_IDX_NOTUSED;
        }

        curr_page->modified = PAGE_MODIFIED;

    } else if (access_type == WRITE_ACCESS) {
        custom_spin_assert(curr_page->pagefile_idx == DISK_IDX_NOTUSED);
    }


    custom_spin_assert(pfn_to_page(accessed_pte->memory_format.frame_number)->status == ACTIVE_STATUS);

    custom_spin_assert(is_memory_format(read_pte_contents(accessed_pte)));

    write_pte_contents(accessed_pte, pte_contents);

    release_pagelock(curr_page, 34);

    #if 0
    LeaveCriticalSection(pte_lock);
    #endif

    return SUCCESS;
}


/**
 * Handles a pagefault for a PTE that has never been accessed before
 * 
 * Writes the p, or NULL if it fails
 */
static int handle_unaccessed_pte_fault(PTE* accessed_pte, PAGE** result_page_storage, PTE** ptes_to_connect_storage, ULONG64* num_ptes_to_connect_storage) {

    ptes_to_connect_storage[0] = accessed_pte;
    ULONG64 num_pages_to_acquire = 1;
    PTE_LOCKSECTION* accessed_pte_locksection = pte_to_locksection(accessed_pte);

    // See if there are enough pages to justify speculative mapping
    if (total_available_pages > physical_page_count / SPECULATIVE_PAGE_MINIMUM_PROPORTION) {
        ULONG64 trailing_count = get_trailing_valid_pte_count(accessed_pte);

        ULONG64 accessed_pte_idx = pte_to_pagetable_idx(accessed_pte);

        // If this is not true, then we should not speculate as there is not enough evidence of sequential accesses or we are at the end
        // of the pagetable
        if (trailing_count > 0 && accessed_pte_idx < pagetable->num_virtual_pages - 1) {
            ULONG64 end_index = min(accessed_pte_idx + trailing_count, pagetable->num_virtual_pages - 1);
            PTE_LOCKSECTION* curr_ptes_locksection;
            PTE* curr_pte;
            PTE pte_copy;

            for (ULONG64 speculative_pte_idx = accessed_pte_idx + 1; speculative_pte_idx < end_index; speculative_pte_idx++) {
                curr_pte = &pagetable->pte_list[speculative_pte_idx];
                pte_copy = read_pte_contents(curr_pte);

                if (is_used_pte(pte_copy)) {
                    continue;
                }

                // We do not want to have to edit more than one PTE locksection and have to acquire more locks later
                if (pte_to_locksection(curr_pte) != accessed_pte_locksection) {
                    break;
                }

                /**
                 * Now, we can speculate on this PTE. We cannot guarantee that another faulter will not resolve it, but we will try
                 */
                ptes_to_connect_storage[num_pages_to_acquire] = curr_pte;
                num_pages_to_acquire++;
            }
        }
    }

    if (num_pages_to_acquire > MAX_PAGES_READABLE) {
        custom_spin_assert(FALSE);
    }

    ULONG64 num_pages_acquired = find_batch_available_pages(TRUE, result_page_storage, num_pages_to_acquire);

    if (num_pages_acquired == 0) {
        wait_for_pages_signalling();

        /**
         * As we do not hold any locks, we are okay to bail after this. But after waiting for available pages
         * it is likely enough that things have changed that it is better to redo the fault
         */
        return NO_AVAILABLE_PAGES_FAIL;
    }

    // If we get fewer pages than there are PTEs that we speculated on, they will be ignored
    *num_ptes_to_connect_storage = num_pages_acquired;

    return SUCCESS;


    #if 0
    /**
     * In this case, all we need to do is get a valid page and return it - nothing else
     */
    PAGE* allocated_page = find_available_page(TRUE);

    if (allocated_page == NULL) {

        // The thread will sleep until pages are available
        wait_for_pages_signalling();

        return NO_AVAILABLE_PAGES_FAIL;
    }

    *ptes_to_connect_storage = accessed_pte;
    *result_page_storage = allocated_page;
    return SUCCESS;
    #endif
    
}


/**
 * Handles a pagefault for a PTE in transition format
 * 
 * Writes the rescued page into result_page_storage, and the disk index into the disk_idx_storage if applicable,
 * otherwise, DISK_IDX_NOTUSED is written into it
 * 
 * Returns SUCCESS if there are no issues, or RESCUE_FAIL otherwise
 */
#define ONLY_SAME_STATUS 1
static int handle_transition_pte_fault(PTE local_pte, PTE* accessed_pte, PAGE** result_page_storage, PTE** ptes_to_connect_storage, ULONG64* num_ptes_to_connect_storage) {
    ULONG64 pfn = local_pte.transition_format.frame_number;

    PAGE* page_to_rescue = pfn_to_page(pfn);

    // After we acquire the pagelock, if the page is rescuable then we should always be able to succeed
    acquire_pagelock(page_to_rescue, 1);

    // We lost the race to rescue this PTE - whether it was stolen from under us or someone else saved it
    if (page_to_rescue->pte != accessed_pte || page_to_rescue->status == ACTIVE_STATUS) {
        release_pagelock(page_to_rescue, 2);
        return RESCUE_FAIL;
    }

    ULONG64 num_to_speculate;

    if (total_available_pages < physical_page_count / SPECULATIVE_PAGE_MINIMUM_PROPORTION) {
        num_to_speculate = 0;
    } else {
        num_to_speculate = min(get_trailing_valid_pte_count(accessed_pte), MAX_PAGES_TRANSITION_RESCUE - 1);
    }

    ULONG64 accessed_pte_idx = pte_to_pagetable_idx(accessed_pte);
    ULONG64 total_pages_acquired = 1;

    PAGE* modified_rescues[MAX_PAGES_TRANSITION_RESCUE];
    ULONG64 num_modifed_rescues = 0;
    PAGE* standby_rescues[MAX_PAGES_TRANSITION_RESCUE];
    ULONG64 num_standby_rescues = 0;

    result_page_storage[0] = page_to_rescue;
    ptes_to_connect_storage[0] = accessed_pte;

    /**
     * Find where our first, most important page, needs to go
     */
    if (page_to_rescue->status == MODIFIED_STATUS) {
        modified_rescues[num_modifed_rescues] = page_to_rescue;
        num_modifed_rescues++;
    } else {
        standby_rescues[num_standby_rescues] = page_to_rescue;
        num_standby_rescues++;
    }

    #if ONLY_SAME_STATUS
    ULONG64 accessed_page_status = page_to_rescue->status;
    #endif

    /**
     * If there are valid PTEs behind us and we are not at the end of our pagetable,
     * then we can speculate on the PTEs ahead of us - and rescue other transition format PTEs
     */
    if (num_to_speculate > 0 && accessed_pte_idx < pagetable->num_virtual_pages - 1) {
        PTE pte_copy;
        PTE* curr_pte;
        ULONG64 end_index = min(accessed_pte_idx + num_to_speculate, pagetable->num_virtual_pages - 1);
        PAGE* possible_page;

        for (ULONG64 speculative_pte_idx = accessed_pte_idx + 1; speculative_pte_idx < end_index; speculative_pte_idx++) {
            curr_pte = &pagetable->pte_list[speculative_pte_idx];
            pte_copy = read_pte_contents(curr_pte);

            if (is_transition_format(pte_copy) == FALSE) {
                continue;
            } 

            possible_page = pfn_to_page(pte_copy.transition_format.frame_number);

            /**
             * We don't want the faulting thread to have to wait an excessive amount of time for pagelocks
             */
            if (try_acquire_pagelock(possible_page, 37) == FALSE) {
                continue;
            }

            #if ONLY_SAME_STATUS
            if (possible_page->status != accessed_page_status) {
                release_pagelock(possible_page, 38);
                continue;
            }
            #else
            /**
             * We know the PTE cannot be in transition format anymore if the page is in neither of these statuses
             */
            if (possible_page->status != MODIFIED_STATUS && possible_page->status != STANDBY_STATUS) {
                release_pagelock(possible_page, 38);
                continue;
            }
            #endif
            

            /**
             * Finally, this addresses a race condition where the PTE was re-allocated to someone else, and then
             * they were trimmed again - so this page is in the correct status, but no longer references the correct PTE
             */
            if (possible_page->pte != &pagetable->pte_list[speculative_pte_idx]) {
                release_pagelock(possible_page, 39);
                continue;
            }

            /**
             * At this point, we can be certain that the PTE ahead of us is still in transition format and this is still
             * the page that is connected to it. Now that we have the pagelock, we can now rescue it
             */

            result_page_storage[num_modifed_rescues + num_standby_rescues] = possible_page;
            ptes_to_connect_storage[num_modifed_rescues + num_standby_rescues] = curr_pte;

            if (possible_page->status == MODIFIED_STATUS) {
                modified_rescues[num_modifed_rescues] = possible_page;
                num_modifed_rescues ++;
            } else {
                standby_rescues[num_standby_rescues] = possible_page;
                num_standby_rescues++;
            }
        }
    }

    rescue_batch_pages(modified_rescues, num_modifed_rescues, standby_rescues, num_standby_rescues);

    *num_ptes_to_connect_storage = num_modifed_rescues + num_standby_rescues;

    return SUCCESS;

    #if 0
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
    #endif
}

#if 0
/**
 * Sets the being_read_from_disk bit in the accessed PTE to 1 if possible, otherwise releases the lock and spins until the read is complete
 * 
 * This is certainly not perfect - as we don't like the other CPU spinning while this one is being read from disk... but it is a temporary
 * way to reduce contention on the entire PTE lock. Ideally, collisons on just this single PTE should be rare so this should be uncommon
 */
static BOOL acquire_disk_pte_read_rights(PTE* accessed_pte, PTE local_pte) {
    PTE_LOCKSECTION* pte_locksection = pte_to_locksection(accessed_pte);
    BOOL wait_until_change = FALSE;

    // If the local PTE is already indicating that it is being read from the disk, we should just spin and not enter the critical section
    if (local_pte.disk_format.being_read_from_disk == PTE_NOT_BEING_READ_FROM_DISK) {
        EnterCriticalSection(&pte_locksection->lock);

        // Someone else is reading this disk PTE right now
        if (is_disk_format(read_pte_contents(accessed_pte))) {
            if (accessed_pte->disk_format.being_read_from_disk == PTE_BEING_READ_FROM_DISK) {
                wait_until_change = TRUE;
            } else {
                PTE pte_contents;
                pte_contents.complete_format = 0;
                pte_contents.disk_format.pagefile_idx = local_pte.disk_format.pagefile_idx;
                pte_contents.disk_format.being_read_from_disk = PTE_BEING_READ_FROM_DISK;

                local_pte = pte_contents;
                write_pte_contents(accessed_pte, pte_contents);
            }
        } else {
            // The disk read is already complete! We can just return now
            LeaveCriticalSection(&pte_locksection->lock);
            return FALSE;
        }

        LeaveCriticalSection(&pte_locksection->lock);
    } else {
        // The local PTE indicates that we should spin until the disk read is finished
        wait_until_change = TRUE;
    }
    
    // We would like some sort of set event here... but this is the interim solution. Collisions here should be very rare
    if (wait_until_change) {
        while (ptes_are_equal(read_pte_contents(accessed_pte), local_pte)) {

        }
        //WaitOnAddress(accessed_pte, &local_pte, sizeof(PTE), INFINITE);
        return FALSE;
    }

    return TRUE;

}
#endif

/**
 * Acquires the right to read sequential PTEs starting at the accessed PTE and stores the pointers in the acquired_rights_pte_storage.
 * This "right" is obtained by setting the being_read_from_disk bit in the accessed PTE. We will only stay within a single PTE locksection,
 * and even if we fail to acquire the accessed_pte's rights, we will still continue with the sequential ones that we do acquire
 * 
 * Takes in the PTE that was originally accessed, a place to store the PTEs, and the number of PTEs we would like to check sequentially
 * 
 * Returns the number of PTEs that we will read from the disk
 */
static ULONG64 acquire_sequential_disk_read_rights(PTE* accessed_pte, PTE** acquired_rights_pte_storage, ULONG64 num_ptes) {
    if (num_ptes == 0 || num_ptes > MAX_PAGES_READABLE) {
        fprintf(stderr, "Invalid number of PTEs to acquire disk read rights to\n");
        DebugBreak();
    }
    
    PTE_LOCKSECTION* prev_pte_locksection = pte_to_locksection(accessed_pte);
    PTE_LOCKSECTION* curr_pte_locksection = prev_pte_locksection;

    ULONG64 num_ptes_checked = 0;
    ULONG64 num_rights_acquired = 0;
    ULONG64 curr_pte_idx = pte_to_pagetable_idx(accessed_pte);
    PTE* curr_pte;
    PTE curr_pte_copy;

    // We can pre-prepare most of the PTE contents, but will need to adjust the pagefile-idx field for each PTE
    PTE pte_contents;
    pte_contents.complete_format = 0;
    pte_contents.disk_format.being_read_from_disk = PTE_BEING_READ_FROM_DISK;

    EnterCriticalSection(&curr_pte_locksection->lock);

    while (num_ptes_checked < num_ptes) {
        curr_pte = &pagetable->pte_list[curr_pte_idx];

        // We opt to stay within just our own current PTE locksection to avoid extra waiting
        curr_pte_locksection = pte_to_locksection(curr_pte);
        
        if (curr_pte_locksection != prev_pte_locksection) {
            LeaveCriticalSection(&prev_pte_locksection->lock);
            EnterCriticalSection(&curr_pte_locksection->lock);
            break;
        }

        /**
         * Importantly, we are only ever able to set the disk pte's reading from disk status to PTE_BEING_READ_FROM_DSIK 
         * while holding the lock, so we can make a decision by looking at the copy of the PTE here.
         * 
         * If the copy shows that it is already being read - or that the read has finished and the status has changed -
         * then we can ignore it. If the copy shows that the disk PTE is NOT being read, then we are the only ones who are able to
         * set it's status to PTE_BEING_READ_FROM_DISK. This scheme is able to reduce contention on the PTE lock at the end of the fault.
         */
        curr_pte_copy = read_pte_contents(curr_pte);

        if (is_disk_format(curr_pte_copy)) {
            if (curr_pte_copy.disk_format.being_read_from_disk == PTE_NOT_BEING_READ_FROM_DISK) {
                pte_contents.disk_format.pagefile_idx = curr_pte_copy.disk_format.pagefile_idx;
                write_pte_contents(curr_pte, pte_contents);

                acquired_rights_pte_storage[num_rights_acquired] = curr_pte;
                num_rights_acquired++;
            }
        }

        num_ptes_checked++;
        curr_pte_idx++;
        prev_pte_locksection = curr_pte_locksection;

        // BW: We would need to adjust this if we move to a multi-level pagetable
        if (curr_pte_idx == pagetable->num_virtual_pages) {
            break;
        }
    }

    LeaveCriticalSection(&curr_pte_locksection->lock);

    return num_rights_acquired;
}


/**
 * Releases the disk read rights for several PTEs at once, but leaves them in disk format. This may be
 * used if we do not acquire enough pages for all of the PTEs
 * 
 * Assumes they are in the same PTE locksection!
 */
static void release_sequential_disk_read_rights(PTE** ptes_to_release, ULONG64 num_ptes) {
    if (num_ptes == 0 || num_ptes > MAX_PAGES_READABLE) {
        fprintf(stderr, "Invalid number of PTEs to release disk read rights to\n");
        DebugBreak();
    }

    PTE* curr_pte;
    CRITICAL_SECTION* pte_lock = &pte_to_locksection(ptes_to_release[0])->lock;
    PTE pte_contents;
    // All fields except the pagefile index will be zero - wiping out the PTE_BEING_READ_FROM_DISK bit
    pte_contents.complete_format = 0;

    EnterCriticalSection(pte_lock);

    for (ULONG64 i = 0; i < num_ptes; i++) {
        curr_pte = ptes_to_release[i];

        custom_spin_assert(is_disk_format(read_pte_contents(curr_pte)));

        pte_contents.disk_format.pagefile_idx = curr_pte->disk_format.pagefile_idx;

        write_pte_contents(curr_pte, pte_contents);
    }

    LeaveCriticalSection(pte_lock);

}


/**
 * Handles a pagefault for a PTE that is in the disk format - it's contents
 * must be fetched from the disk
 * 
 * Returns a pointer to the page with the restored contents on it, and stores the
 * disk index that it was at in disk_idx_storage
 */
static int handle_disk_pte_fault(PTE local_pte, PTE* accessed_pte, PAGE** result_pages_storage, PTE** ptes_to_connect_storage, ULONG64* num_ptes_to_connect_storage) {
    
    #if SEQUENTIAL_SPECULATIVE_DISK_READS

    ULONG64 num_pte_rights_to_acquire;

    if (total_available_pages < physical_page_count / SPECULATIVE_PAGE_MINIMUM_PROPORTION) {
        num_pte_rights_to_acquire = 1;
    } else {
        // Determine how many PTEs we might try to speculatively read from the disk, if we have available pages
        ULONG64 trailing_valid_pte_count = get_trailing_valid_pte_count(accessed_pte);

        if (trailing_valid_pte_count < MAX_PAGES_READABLE) {
            num_pte_rights_to_acquire = max(trailing_valid_pte_count, 1);
        } else {
            num_pte_rights_to_acquire = MAX_PAGES_READABLE;
        }
    }
    #else
    ULONG64 num_pte_rights_to_acquire = 1;
    #endif

    /**
     * This is the actual number of PTEs that we are going to read from the disk
     * 
     * Note that this might not actually include the accessed PTE if it has already changed or another thread is reading it from
     * the disk already - but we will so the speculative reads anyway
    */    
    ULONG64 num_to_read = acquire_sequential_disk_read_rights(accessed_pte, ptes_to_connect_storage, num_pte_rights_to_acquire);
    
    // We failed to get any rights on any PTEs, including our accessed PTE
    if (num_to_read == 0) {
        // We may want a better solution to this... spin for the accessed PTE to be resolved?
        return DISK_RACE_CONTIION_FAIL;
    }

    ULONG64 num_pages_acquired = find_batch_available_pages(FALSE, result_pages_storage, num_to_read);

    if (num_pages_acquired == 0) {
        release_sequential_disk_read_rights(ptes_to_connect_storage, num_to_read);
        
        // The thread will sleep until pages are available
        wait_for_pages_signalling();

        return NO_AVAILABLE_PAGES_FAIL;
    } else if (num_pages_acquired < num_to_read) {
        // We failed to acquire enough pages - but we will continue
        release_sequential_disk_read_rights(&ptes_to_connect_storage[num_pages_acquired], num_to_read - num_pages_acquired);
    }

    if (read_pages_from_disk(result_pages_storage, ptes_to_connect_storage, num_pages_acquired) == ERROR) {
        DebugBreak();
    }

    *num_ptes_to_connect_storage = num_pages_acquired;

    return SUCCESS;
}


/**
 * Without acquiring any locks, gets a count of the number of valid PTEs trailing the accessed PTE,
 * up to MAX_PAGES_READABLE
 * 
 * Note that since we do not acquire any locks this count may not be accurate after we take it, but it can
 * serve as a heuristic for the number of PTEs we might read in from the disk ahead of the accessed PTE
 */
static ULONG64 get_trailing_valid_pte_count(PTE* accessed_pte) {
    ULONG64 valid_count = 0;
    ULONG64 pte_idx = pte_to_pagetable_idx(accessed_pte);
    PTE* curr_pte;

    // We are at the bottom of the pagetable - there cannot be any valid PTEs behind this one
    if (pte_idx == 0) {
        return 0;
    }

    // We want to start on the PTE before the accessed one
    pte_idx--;


    while (valid_count < MAX_PAGES_READABLE && pte_idx > 0) {
        curr_pte = &pagetable->pte_list[pte_idx];

        if (is_memory_format(read_pte_contents(curr_pte))) {
            valid_count++;
        } else {
            break;
        }

        pte_idx--;
    }

    return valid_count;
}


/**
 * Determines what kind of work or signaling the faulting thread should perform after they have resolved the pagefault
 */
static void end_of_fault_work(PTE* accessed_pte, ULONG64 thread_idx) {
    /**
     * Temporary ways to induce trimming until we have a better heuristic
     */    
    if (thread_information.thread_local_storages[thread_idx].trim_signaled == TRIMMER_NOT_SIGNALLED) {
        if ((total_available_pages < (physical_page_count / 2)) && modified_list->list_length < physical_page_count / 2) {
            trim_update_thread_storages(TRIMMER_SIGNALED);
            SetEvent(trimming_event);
        }
    }
    

    // We try to trim behind us... currently using placeholder heuristics
    if (total_available_pages < (physical_page_count / 4) && modified_list->list_length < physical_page_count / 4) {
        faulter_trim_behind(accessed_pte);
    }
    //faulter_trim_behind(accessed_pte);

    // The faulter might take pages from the standby list to replenish the free and zero lists
    potential_list_refresh(thread_idx);
}

