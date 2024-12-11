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

static int handle_disk_pte_fault(ULONG64 thread_idx, PTE local_pte, PTE* accessed_pte, PAGE** result_page_storage, PTE** ptes_to_connect_storage, ULONG64* num_ptes_to_connect_storage, ULONG64 access_type);

static ULONG64 get_trailing_valid_pte_count(PTE* accessed_pte);

static ULONG64 get_trailing_valid_and_being_read_pte_count(PTE* accessed_pte);

static void end_of_fault_work();

static void commit_pages(PAGE** pages_to_commit, PTE** ptes, PTE* accessed_pte, ULONG64 num_pages, ULONG64 access_type);



/**
 * Handles the page fault for the given virtual address
 * 
 * Returns SUCCESS if the access can be attempted again, ERROR if
 * the fault must either be re-attempted or if the virtual address's corresponding
 * PTE was modified by another thread in the meantime
 * 
 */
int pagefault(PULONG_PTR virtual_address, ULONG64 access_type, ULONG64 thread_idx) {    
    
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

    } else if (is_disk_format(local_pte)) {

        if ((handler_result = handle_disk_pte_fault(thread_idx, local_pte, accessed_pte, allocated_pages, ptes_to_connect, &num_ptes_to_connect, access_type)) != SUCCESS) {
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

        // For unaccessed PTEs that we might have speculated on incorrectly, we might need to return pages that we never used
        if (num_unneeded_pages > 0) {
            release_batch_unneeded_pages(unneeded_pages, num_unneeded_pages);
        }   

        num_ptes_to_connect = num_ptes_not_changed;

        /**
         * Set all of the PTEs that we are connecting to the valid format, but being changed
         */
        PTE* curr_pte;
        PTE pte_contents;
        pte_contents.memory_format.valid = VALID;
        pte_contents.memory_format.being_changed = VALID_PTE_BEING_CHANGED;
        for (ULONG64 i = 0; i < num_ptes_to_connect; i++) {
            curr_pte = ptes_to_connect[i];
            pte_contents.memory_format.frame_number = page_to_pfn(allocated_pages[i]);

            // The protections are determined later on when we are connecting them, so this is a placeholder since we will fault anyway
            pte_contents.memory_format.protections = PTE_PROTNONE;
            write_pte_contents(curr_pte, pte_contents);
        }
        
        LeaveCriticalSection(pte_lock);
    }

    

    if (is_disk_format(local_pte) == FALSE) {
        commit_pages(allocated_pages, ptes_to_connect, accessed_pte, num_ptes_to_connect, access_type);
    }

    if (connect_batch_ptes_to_pages(ptes_to_connect, accessed_pte, allocated_pages, access_type, num_ptes_to_connect) == ERROR) {
        DebugBreak();
    }
    
    // if (is_used_pte(local_pte) == FALSE) {
    //     LeaveCriticalSection(pte_lock);
    // }

    end_of_fault_work(accessed_pte, thread_idx);

    return SUCCESSFUL_FAULT;
}   



/**
 * Spins until the accessed PTE is no longer equal to the local PTE. Yields the processor between checks
 */
static void spin_wait_for_pte_change(PTE local_pte, PTE* accessed_pte) {
    while (ptes_are_equal(local_pte, read_pte_contents(accessed_pte))) {
        YieldProcessor();
    }
}


/**
 * Handles the fault for a fault on a valid PTE
 * 
 * This means that we likely need to adjust the permissions of the PTE, potentially throw out pagefile space,
 * and we can take the opportunity to reset the age to zero
 */
static int handle_valid_pte_fault(PTE local_pte, PTE* accessed_pte, ULONG64 access_type) {

    // The PTE is being trimmed, we just have to wait until it is in transition format. This collision with the trimmer should be unlikely
    if (local_pte.memory_format.being_changed == VALID_PTE_BEING_CHANGED) {
        spin_wait_for_pte_change(local_pte, accessed_pte);
        return VALID_PTE_RACE_CONTIION_FAIL;
    }

    custom_spin_assert(local_pte.memory_format.protections != PTE_PROTNONE);

    CRITICAL_SECTION* pte_lock = &pte_to_locksection(accessed_pte)->lock;
    PULONG_PTR pte_va = pte_to_va(accessed_pte);
    
    // Prepare the contents ahead of time depending on whether or not we are changing the permissions
    PTE pte_contents;

    pte_contents = local_pte;
    pte_contents.memory_format.being_changed = VALID_PTE_BEING_CHANGED;

    /**
     * Effectively acquire the rights to edit this PTE by setting its "being_changed" bit
     */
    if (InterlockedCompareExchange64((ULONG64*) accessed_pte, pte_contents.complete_format, local_pte.complete_format) != local_pte.complete_format) {
        return VALID_PTE_RACE_CONTIION_FAIL;
    }


    pte_contents.complete_format = 0;
    pte_contents.memory_format.valid = VALID;
    pte_contents.memory_format.frame_number = local_pte.memory_format.frame_number;
    pte_contents.memory_format.access_bit = PTE_ACCESSED;

    if (access_type == READ_ACCESS) {
        pte_contents.memory_format.protections = PTE_PROTREAD;
    } else {
        pte_contents.memory_format.protections = PTE_PROTREADWRITE;
    }

    PAGE* curr_page = pfn_to_page(local_pte.memory_format.frame_number);

    PTE updated_pte_copy = read_pte_contents(accessed_pte);

    // See if we need to change the permissions to PAGE_READWRITE
    if (access_type == WRITE_ACCESS && local_pte.memory_format.protections == PTE_PROTREAD) {
        ULONG64 pfn = page_to_pfn(curr_page);

        // All we want to do is change the permissions on the page - using MapUserPhysicalPages for this happens to be faster than
        // using VirtualProtect
        if (MapUserPhysicalPages(pte_to_va(accessed_pte), 1, &pfn) == FALSE) {

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


    custom_spin_assert(curr_page->status == ACTIVE_STATUS);

    custom_spin_assert(is_memory_format(read_pte_contents(accessed_pte)));

    /**
     * While we have the rights to change this PTE's permission bits, we must ensure that this write is sustained
     * across any other thread setting the access bit
     */
    while (InterlockedCompareExchange64((ULONG64*) accessed_pte, pte_contents.complete_format, updated_pte_copy.complete_format) != updated_pte_copy.complete_format) {
        updated_pte_copy = read_pte_contents(accessed_pte);
    }

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
}


/**
 * Handles a pagefault for a PTE in transition format
 * 
 * Writes the rescued page into result_page_storage, and the disk index into the disk_idx_storage if applicable,
 * otherwise, DISK_IDX_NOTUSED is written into it
 * 
 * Returns SUCCESS if there are no issues, or RESCUE_FAIL otherwise
 */
#define ONLY_SAME_STATUS 0
static int handle_transition_pte_fault(PTE local_pte, PTE* accessed_pte, PAGE** result_page_storage, PTE** ptes_to_connect_storage, ULONG64* num_ptes_to_connect_storage) {
    ULONG64 pfn = local_pte.transition_format.frame_number;

    PAGE* page_to_rescue = pfn_to_page(pfn);

    // After we acquire the pagelock, if the page is rescuable then we should always be able to succeed
    acquire_pagelock(page_to_rescue, 1);

    // We lost the race to rescue this PTE - whether it was stolen from under us or someone else saved it
    // or rarely, if the page is moved from standby to the free/zeroed list
    if (page_to_rescue->pte != accessed_pte || 
        (page_to_rescue->status != STANDBY_STATUS && page_to_rescue->status != MODIFIED_STATUS)) {

        release_pagelock(page_to_rescue, 2);
        return RESCUE_FAIL;
    }

    ULONG64 num_to_speculate;

    if (total_available_pages < physical_page_count / SPECULATIVE_PAGE_MINIMUM_PROPORTION) {
        num_to_speculate = 0;
    } else {
        num_to_speculate = min(get_trailing_valid_pte_count(accessed_pte), MAX_PAGES_RESCUABLE - 1);
    }

    ULONG64 accessed_pte_idx = pte_to_pagetable_idx(accessed_pte);
    ULONG64 total_pages_acquired = 1;

    PAGE* modified_rescues[MAX_PAGES_RESCUABLE];
    ULONG64 num_modifed_rescues = 0;
    PAGE* standby_rescues[MAX_PAGES_RESCUABLE];
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
                num_modifed_rescues++;
            } else {
                standby_rescues[num_standby_rescues] = possible_page;
                num_standby_rescues++;
            }
        }
    }

    rescue_batch_pages(modified_rescues, num_modifed_rescues, standby_rescues, num_standby_rescues);

    *num_ptes_to_connect_storage = num_modifed_rescues + num_standby_rescues;

    return SUCCESS;
}


/**
 * Opportunistically (without the PTE lock) looks for disk PTEs that are not being read currently, and writes them into the
 * found pte storage. If the PTE locksection changes at all within the found PTEs, the index (of the found PTEs list) at which it changes will be written
 * into locksection_switch_storage, if it doesn't switch, we write INFINITE into locksection_switch_storage
 * 
 * We assume that the num_to_check is less than or equal to MAX_PAGES_READABLE, and that we will never cross into more than two
 * PTE locksections
 * 
 * Returns the number of unread/not-being-read disk PTEs written into found_pte_storage
 */
static ULONG64 find_unread_sequential_disk_ptes(PTE* accessed_pte, PTE** found_pte_storage, ULONG64* locksection_switch_storage, ULONG64 num_to_check) {
    ULONG64 curr_pte_index = pte_to_pagetable_idx(accessed_pte);
    ULONG64 final_locksection_idx = INFINITE;
    ULONG64 num_ptes_checked = 0;
    ULONG64 num_unread_disk_ptes_found = 0;
    BOOL found_locksection_switch = FALSE;
    PTE* curr_pte;
    PTE pte_copy;

    while (num_ptes_checked < num_to_check) {
        
        // speculatively read in the PTE, see if it is unread-disk-format, and check if our PTE index is greater than our boundary
        curr_pte = &pagetable->pte_list[curr_pte_index];
        pte_copy = read_pte_contents(curr_pte);

        if (is_disk_format(pte_copy) && pte_copy.disk_format.being_read_from_disk == PTE_NOT_BEING_READ_FROM_DISK) {
            /**
             * The goal is to keep track of when we would need to switch PTE locksections when reading and editing PTEs
             * that (we speculate will be later) in the disk/unread format. We want this to start at the index of the PTE that
             * we find first in the correct format - not necessarily in the accessed PTE.
             * 
             * The issue that caused this was that the first PTE that we found that was disk/unread was in a different section than the accessed PTE,
             * so we would accidentlly switch locksections when we were not supposed to later
             */
            if (final_locksection_idx == INFINITE) {
                final_locksection_idx = pte_to_locksection(curr_pte)->final_pte_index;
            }

            if (found_locksection_switch == FALSE && curr_pte_index > final_locksection_idx) {
                *locksection_switch_storage = num_unread_disk_ptes_found;
                found_locksection_switch = TRUE;
            }

            found_pte_storage[num_unread_disk_ptes_found] = curr_pte;
            num_unread_disk_ptes_found++;
        } 

        curr_pte_index++;
        
        // The end of our single level pagetable
        if (curr_pte_index == pagetable->num_virtual_pages) {
            break;
        }

        num_ptes_checked++;
    }

    if (found_locksection_switch == FALSE) {
        *locksection_switch_storage = INFINITE;
    }

    return num_unread_disk_ptes_found;
}


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

    PTE* potential_ptes[MAX_PAGES_READABLE];
    ULONG64 pte_locksection_switch_idx;

    /**
     * This scheme allows us to speculatively collect the PTEs that we will likely be reading in from the disk,
     * this means that we spend less time holding the PTE lock later on as we actually determine which PTEs we are reading in,
     * and do not have to spend additional effort determining which PTE locksection we need to be in
     */
    ULONG64 num_potential_ptes = find_unread_sequential_disk_ptes(accessed_pte, potential_ptes, &pte_locksection_switch_idx, num_ptes);

    if (num_potential_ptes == 0) {
        return 0;
    }

    PTE_LOCKSECTION* curr_pte_locksection = pte_to_locksection(potential_ptes[0]);
    const BOOL locksection_will_switch = (pte_locksection_switch_idx != INFINITE);
    PTE* curr_pte;
    PTE pte_copy;
    PTE pte_contents;
    ULONG64 num_pte_rights_acquired = 0;
    pte_contents.complete_format = 0;
    pte_contents.disk_format.being_read_from_disk = PTE_BEING_READ_FROM_DISK;
    
    EnterCriticalSection(&curr_pte_locksection->lock);
    for (ULONG64 i = 0; i < num_potential_ptes; i++) {
        // Determine if we need to switch critical sections
        if (locksection_will_switch && pte_locksection_switch_idx == i) {
            LeaveCriticalSection(&curr_pte_locksection->lock);

            // The next pte locksection is stored adjacent to this one, so we can increment the pointer
            PTE_LOCKSECTION* old_section = curr_pte_locksection;

            curr_pte_locksection++;
            
            ULONG64 pte_idx = pte_to_pagetable_idx(potential_ptes[i]);
            PTE_LOCKSECTION* ls = pte_to_locksection(potential_ptes[i]);
            custom_spin_assert(curr_pte_locksection == ls);

            EnterCriticalSection(&curr_pte_locksection->lock);
        }

        curr_pte = potential_ptes[i];
        pte_copy = read_pte_contents(curr_pte);

        if (is_disk_format(pte_copy) && pte_copy.disk_format.being_read_from_disk == PTE_NOT_BEING_READ_FROM_DISK) {
            pte_contents.disk_format.pagefile_idx = pte_copy.disk_format.pagefile_idx;

            write_pte_contents(curr_pte, pte_contents);
            acquired_rights_pte_storage[num_pte_rights_acquired] = curr_pte;
            num_pte_rights_acquired++;
        }

    }
    LeaveCriticalSection(&curr_pte_locksection->lock);

    return num_pte_rights_acquired;
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

    // EnterCriticalSection(pte_lock);

    for (ULONG64 i = 0; i < num_ptes; i++) {
        curr_pte = ptes_to_release[i];

        custom_spin_assert(is_disk_format(read_pte_contents(curr_pte)));

        pte_contents.disk_format.pagefile_idx = curr_pte->disk_format.pagefile_idx;

        write_pte_contents(curr_pte, pte_contents);
    }

    // LeaveCriticalSection(pte_lock);

}


/**
 * Handles a pagefault for a PTE that is in the disk format - it's contents
 * must be fetched from the disk
 * 
 * Returns a pointer to the page with the restored contents on it, and stores the
 * disk index that it was at in disk_idx_storage
 */
static int handle_disk_pte_fault(ULONG64 thread_idx, PTE local_pte, PTE* accessed_pte, PAGE** result_pages_storage, PTE** ptes_to_connect_storage, ULONG64* num_ptes_to_connect_storage, ULONG64 access_type) {
    
    #if SEQUENTIAL_SPECULATIVE_DISK_READS

    ULONG64 num_pte_rights_to_acquire;

    if (total_available_pages < physical_page_count / SPECULATIVE_PAGE_MINIMUM_PROPORTION) {
        num_pte_rights_to_acquire = 1;
    } else {
        // Determine how many PTEs we might try to speculatively read from the disk, if we have available pages
        ULONG64 trailing_valid_pte_count = get_trailing_valid_pte_count(accessed_pte);
        ULONG64 trailing_valid_and_being_read_count = get_trailing_valid_and_being_read_pte_count(accessed_pte);

        if (trailing_valid_pte_count < MAX_PAGES_READABLE) {
            num_pte_rights_to_acquire = max(trailing_valid_and_being_read_count, 1);
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
        spin_wait_for_pte_change(local_pte, accessed_pte);

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

    commit_pages(result_pages_storage, ptes_to_connect_storage, accessed_pte, num_pages_acquired, access_type);

    THREAD_DISK_READ_RESOURCES* thread_disk_resources = &thread_information.thread_local_storages[thread_idx].disk_resources;

    if (read_pages_from_disk(result_pages_storage, ptes_to_connect_storage, num_pages_acquired, thread_disk_resources) == ERROR) {
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
 * Without acquiring any locks, gets a count of the number of valid PTEs and disk PTEs currently being read in trailing
 * the accessed PTE, up to MAX_PAGES_READABLE
 * 
 * We do this for disk-read speculation, as we can take advantage of a CPU in the fault handler whose PTE is being read in from disk already,
 * who might itself be being read in speculatively from another thread
 */
static ULONG64 get_trailing_valid_and_being_read_pte_count(PTE* accessed_pte) {
    ULONG64 total_count = 0;

    ULONG64 pte_idx = pte_to_pagetable_idx(accessed_pte);
    PTE pte_copy;
    PTE* curr_pte;

    // We are at the bottom of the pagetable - there cannot be any valid PTEs behind this one
    if (pte_idx == 0) {
        return 0;
    }

    // We want to start on the PTE before the accessed one
    pte_idx--;

    while (total_count < MAX_PAGES_READABLE && pte_idx > 0) {
        curr_pte = &pagetable->pte_list[pte_idx];
        pte_copy = read_pte_contents(curr_pte);

        if (is_memory_format(pte_copy)) {
            total_count++;
        } else if (is_disk_format(pte_copy) && pte_copy.disk_format.being_read_from_disk == PTE_BEING_READ_FROM_DISK) {
            total_count++;
        } else {
            break;
        }

        pte_idx--;
    }

    return total_count;

}


/**
 * Commits the pages and releases the pagelocks
 * 
 * - All standby pages have their old PTEs set to disk format
 * 
 * - For unaccessed PTEs, their pages are zeroed if necessary
 */
static void commit_pages(PAGE** pages_to_commit, PTE** ptes, PTE* accessed_pte, ULONG64 num_pages, ULONG64 access_type) {
    // We may need to edit the old PTE, in which case, we want a copy so we can still find it
    PAGE allocated_pages_copies[MAX_PAGES_READABLE];

    if (is_transition_format(read_pte_contents(ptes[0])) == FALSE) {
        for (ULONG64 i = 0; i < num_pages; i++) {
            allocated_pages_copies[i] = *pages_to_commit[i];
        }
    }

    // We are writing to the page - this may communicate to the modified writer that they need to return pagefile space
    if (access_type == WRITE_ACCESS) {
        pages_to_commit[0]->modified = PAGE_MODIFIED;
    }

    for (ULONG64 i = 0; i < num_pages; i++) {
        custom_spin_assert(pages_to_commit[i]->status != ACTIVE_STATUS);
    }

    // We may need to release stale pagefile slots and/or modify the page's pagefile information
    handle_batch_end_of_fault_disk_slot(ptes, accessed_pte, pages_to_commit, access_type, num_pages);

    /**
     * For unaccessed PTEs, we need to ensure they start off with a clean page
     * 
     * Note - this is treating the individual threads to be similar to different processes accessing the same
     * address space, which would not be the case normally. However, this allows me to demonstrate the infrastructure
     * required to zero out pages when needed. Typically, different threads within a process would not need to have zeroed out pages
     * if their previous owner was withn the same process - since the threads are a part of the same process, we do not have the
     * security concern of sharing data between processes. 
     */

    PAGE* pages_to_zero[MAX_PAGES_READABLE];
    ULONG64 num_pages_to_zero = 0;

    for (ULONG64 i = 0; i < num_pages; i++) {
        if (is_memory_format(read_pte_contents(ptes[i]))) {
            if (pages_to_commit[i]->status != ZERO_STATUS) {
                pages_to_zero[num_pages_to_zero] = pages_to_commit[i];
                num_pages_to_zero++;
            }
        }

        pages_to_commit[i]->status = ACTIVE_STATUS;
        pages_to_commit[i]->pte = ptes[i];

        #if LIGHT_DEBUG_PAGELOCK
        pages_to_commit[i]->acquiring_pte_copy = read_pte_contents(ptes[i]);
        #endif
    }

    if (num_pages_to_zero > 0) {
        zero_out_pages(pages_to_zero, num_pages_to_zero);
    }

    /**
     * We need to modify the other PTEs associated with the standby pages now that we are committing
     * 
     * The worst case is that other threads are spinning on the pagelock in transition format waiting for this to happen,
     * and they will need to retry the fault as their PTE will be in disk format
     */
    if (is_transition_format(read_pte_contents(ptes[0])) == FALSE) {
        PAGE curr_page_copy;
        PTE old_pte_copy;
        PTE pte_contents;
        pte_contents.complete_format = 0;

        for (ULONG64 i = 0; i < num_pages; i++) {
            curr_page_copy = allocated_pages_copies[i];

            if (curr_page_copy.status == STANDBY_STATUS) {
                PTE* old_pte = curr_page_copy.pte;
                old_pte_copy = read_pte_contents(old_pte);
                pte_contents.disk_format.pagefile_idx = curr_page_copy.pagefile_idx;

                /**
                 * We need to force a write here, as we must not collide with the end of the trimming thread when pages are added
                 * to the standby list. See more info there
                 */
                while (InterlockedCompareExchange64((ULONG64*) old_pte, pte_contents.complete_format, old_pte_copy.complete_format) != old_pte_copy.complete_format) {
                    old_pte_copy = read_pte_contents(old_pte);
                }

            }
        }
    }

    for (ULONG64 i = 0; i < num_pages; i++) {
        release_pagelock(pages_to_commit[i], 11);
    }
}


/**
 * Speculatively sets the access bits on valid PTEs ahead of the accessed_pte.
 * This reduces the probability that they will be trimmed by the time we access them sequentially,
 * and might significantly reduce the amount of work for the accessing thread
 */
static void speculative_set_access_bits(PTE* accessed_pte) {
    ULONG64 num_to_speculate = max(get_trailing_valid_pte_count(accessed_pte), 1);

    ULONG64 accessed_pte_idx = pte_to_pagetable_idx(accessed_pte);
    PTE pte_contents;

    ULONG64 end_index = min(accessed_pte_idx + num_to_speculate + 1, pagetable->num_virtual_pages);

    PTE* curr_pte;
    PTE pte_copy;
    for (ULONG64 speculative_idx = accessed_pte_idx + 1; speculative_idx < end_index; speculative_idx++) {
        curr_pte = &pagetable->pte_list[speculative_idx];
        pte_copy = read_pte_contents(curr_pte);

        // Ignore invalid PTEs, we cannot set their bit
        if (is_memory_format(pte_copy) == FALSE || pte_copy.memory_format.access_bit == PTE_ACCESSED) {
            continue;
        }

        pte_contents = pte_copy;
        pte_contents.memory_format.access_bit = PTE_ACCESSED;
        
        InterlockedCompareExchange64((ULONG64*) curr_pte, pte_contents.complete_format, pte_copy.complete_format);
    }
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

    // We speculate that we will access the PTEs ahead of us, so we set their access bits so that they aren't trimmed
    speculative_set_access_bits(accessed_pte);


    // We look at the PTEs behind us and put valid ones in a buffer to be trimmed
    // We are speculating that we are much less likely to access sequential addresses backwards
    faulter_trim_behind(accessed_pte, thread_idx);


    // If the conditions are right, we will signal a thread to take frames from standby and move
    // them to the free frames list and start the process of moving some to the zeroed pages list
    potential_list_refresh(thread_idx);
}