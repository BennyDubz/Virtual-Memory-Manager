/**
 * @author Ben Williams
 * @date June 25th, 2024
 * 
 * All functionality for trimming
 */

#include <stdio.h>
#include <assert.h>
#include <windows.h>
#include "../globals.h"
#include "./conversions.h"
#include "./debug_checks.h"
#include "../Datastructures/datastructures.h"
#include "./disk_operations.h"
#include "./trim.h"


/**
 * Thread dedicated to trimming PTEs from the pagetable and putting them on the modified list
 */
ULONG64 trim_count = 0;
LPTHREAD_START_ROUTINE thread_trimming() {
    ULONG64 ptes_per_lock = pagetable->num_virtual_pages / pagetable->num_locks;
    ULONG64 section_start;
    PTE* curr_pte;
    PTE_LOCKSECTION* curr_pte_locksection;
    ULONG64 curr_pfn;
    PAGE* curr_page = NULL;


    while(TRUE) {
        WaitForSingleObject(trimming_event, INFINITE);

        // Go through each lock section and increment all valid PTEs
        for (ULONG64 lock_section = 0; lock_section < pagetable->num_locks; lock_section++) {            
            section_start = lock_section * ptes_per_lock;

            curr_pte_locksection = &pagetable->pte_locksections[lock_section];

            // Ignore invalid PTE sections  
            if (curr_pte_locksection->valid_pte_count == 0) continue;

            EnterCriticalSection(&curr_pte_locksection->lock);

            if (curr_pte_locksection->valid_pte_count == 0) {
                LeaveCriticalSection(&curr_pte_locksection->lock);
                continue;
            }

            // printf("Start index TT: %llX\n", section_start);
            // printf("Final index TT: %llX\n", section_start + ptes_per_lock);
            #ifdef DEBUG_CHECKING
            if (pte_valid_count_check(&pagetable->pte_list[section_start]) == FALSE) {
                DebugBreak();
            }
            #endif


            for (ULONG64 pte_idx = section_start; pte_idx < section_start + ptes_per_lock; pte_idx++) {
                curr_pte = &pagetable->pte_list[pte_idx];

                assert(curr_pte_locksection == pte_to_locksection(curr_pte));

                // Ignore invalid PTEs
                if (! is_memory_format(*curr_pte)) {
                    if (pte_idx == section_start + ptes_per_lock - 1) {
                        DebugBreak();
                    }
                    continue;
                }

                curr_pfn = curr_pte->memory_format.frame_number;
                curr_page = pfn_to_page(curr_pfn);

                // Unmaps from CPU and decrements its valid_pte_count section
                //BW: Later can have a list of PTEs to pass to this to unmap several at once

                // disconnect_pte_from_cpu(curr_pte);


                //BW: Can maybe leave pte lock here in the meantime?  
                //      - as of now, no, as we have already set up the transition PTE  
                PTE transition_pte_contents;
                transition_pte_contents.complete_format = 0;
                transition_pte_contents.transition_format.always_zero = 0;
                transition_pte_contents.transition_format.frame_number = curr_pfn;
                transition_pte_contents.transition_format.is_transition = 1;
                
                disconnect_pte_from_cpu(curr_pte);
                write_pte_contents(curr_pte, transition_pte_contents);

                if (pte_valid_count_check(curr_pte) == FALSE) {
                    DebugBreak();
                }

                LeaveCriticalSection(&curr_pte_locksection->lock);

                #ifdef DEBUG_CHECKING
                int dbg_result;
                if ((dbg_result = page_is_isolated(curr_page)) != ISOLATED) {
                    DebugBreak();
                }
                #endif

                EnterCriticalSection(&modified_list->lock);  
      

                #ifdef DEBUG_CHECKING
                if (pfn_is_single_allocated(page_to_pfn(curr_page)) == FALSE) {
                    DebugBreak();
                }
                #endif

                modified_add_page(curr_page, modified_list);
                LeaveCriticalSection(&modified_list->lock);
                
                break;
            }
            
        }

        // Signal that the modified list should be populated
        trim_count++;
        SetEvent(modified_to_standby_event);
    }
}


/**
 * Thread dedicated to aging all of the valid PTEs in the pagetable
 */
LPTHREAD_START_ROUTINE thread_aging() {
    ULONG64 ptes_per_lock = pagetable->num_virtual_pages / pagetable->num_locks;
    ULONG64 section_start;
    PTE_LOCKSECTION* curr_pte_locksection;

    while(TRUE) {
        WaitForSingleObject(aging_event, INFINITE);
        
        // Go through each lock section and increment all valid PTEs
        for (ULONG64 lock_section = 0; lock_section < pagetable->num_locks; lock_section++) {
            section_start = lock_section * ptes_per_lock;

            curr_pte_locksection = &pagetable->pte_locksections[lock_section];

            EnterCriticalSection(&curr_pte_locksection->lock);
            for (ULONG64 pte_idx = section_start; pte_idx < section_start + ptes_per_lock; pte_idx++) {

                // Ignore invalid PTEs
                if (! is_memory_format(pagetable->pte_list[pte_idx])) continue;

                // Don't allow the age to wrap
                if (pagetable->pte_list[pte_idx].memory_format.age == MAX_AGE) {
                    continue;
                } else {
                    pagetable->pte_list[pte_idx].memory_format.age++;
                }
            }
            LeaveCriticalSection(&curr_pte_locksection->lock);
        }
        // printf("Successfully aged\n");
    }
}


/**
 * Thread dedicated to writing pages from the modified list to disk, putting finally adding the pages to standby
 */
ULONG64 sb_count = 0;
#define MAX_PAGES_TO_WRITE 256
LPTHREAD_START_ROUTINE thread_modified_to_standby() {
    // Variables dedicated to finding and extracting pages to write from the modified list 
    ULONG64 section_start;
    ULONG64 num_to_write;
    CRITICAL_SECTION* pte_lock;
    BOOL pte_lock_status;
    PAGE* potential_page;
    // PAGE** pages_currently_writing[MAX_PAGES_TO_WRITE];
    PTE* relevant_PTE;
    ULONG64 disk_storage_idx;

    #if 0
    // Variables  dedicated to handling splitting off threads to write pages disk
    ULONG64 num_writing_threads;
    ULONG64 disk_idx_storage_space[MAX_PAGES_TO_WRITE];
    HANDLE worker_writing_threads[MAX_PAGES_TO_WRITE];
    ULONG64 worker_thread_exit_codes[MAX_PAGES_TO_WRITE];
    #endif

    while(TRUE) {
        WaitForSingleObject(modified_to_standby_event, INFINITE);

        num_to_write = min(MAX_PAGES_TO_WRITE, modified_list->list_length);
        ULONG64 curr_page = 0;        
        while (curr_page < num_to_write) {
            EnterCriticalSection(&modified_list->lock);

            // From this line onward until it is added to the standby list, this page cannot be rescued
            potential_page = (PAGE*) modified_pop_page(modified_list);

            // Since we release the modified list lock during this, someone else could come in and rescue pages from the
            // modified list, emptying it, meaning the work here is done until we are signaled again
            if (potential_page == NULL) {
                LeaveCriticalSection(&modified_list->lock);
                break;
            }

            /**
             * Not currently feasible as there must be some way to tell whether the page has been rescued in an atomic
             * operation once we have acquired the standby lock. In the meantime, we will still hold onto the modified lock
             * and be forced to make rescues fail while their pages are between the modified and standby lists
             */
            #if 0 
            // Now that we have popped the page from the modified list, we no longer need the lock as we are not
            // modifying the datastructure. That makes
            LeaveCriticalSection(&modified_list->lock);

           
            if (write_to_disk(potential_page, &disk_storage_idx) == ERROR) {
                EnterCriticalSection(&modified_list->lock);

                /**
                 * We must check that the page is still in modified status. If it is active (or anything else),
                 * that means that it has been rescued while we tried to write it to disk. If that is the case,
                 * we cannot re-add it to the modified list.
                 */
                if (potential_page->modified_page.status == MODIFIED_STATUS) {
                    /**
                     * We must also protect ourselves against the scenario where the page was rescued, trimmed, and
                     * added back to the modified list before we were able to acquire the lock. In that case,
                     * we do not want to add it to the list again as we would have a duplicate.
                     */
                    if (potential_page->modified_page.frame_listnode == NULL) {
                        modified_add_page(potential_page, modified_list);
                    }
                }
                
                // We will need to wait for more disk slots to be open in order to continue writing to the disk
                LeaveCriticalSection(&modified_list->lock);
                WaitForSingleObject(disk_open_slots_event, INFINITE);

                // We will still want to keep looking
                curr_page--;

                continue;      
            }
            #endif

            if (write_to_disk(potential_page, &disk_storage_idx) == ERROR) {
                
                // We will want to try again with this page later
                modified_add_page(potential_page, modified_list);
                
                // We will need to wait for more disk slots to be open in order to continue writing to the disk
                LeaveCriticalSection(&modified_list->lock);
                WaitForSingleObject(disk_open_slots_event, INFINITE);

                // We purposefully do not increment curr_page here so that we still try to write pages to disk
                continue;      
            }

            /**
             * re-acquire page lock
             * Ensure that it is still inactive, if it isn't, then
             */
            LeaveCriticalSection(&modified_list->lock);

            #ifdef DEBUG_CHECKING
            int dbg_result;
            if ((dbg_result = page_is_isolated(pfn_to_page(curr_page))) != ISOLATED) {
                DebugBreak();
            }
            #endif

            // Remember, as of now the page CANNOT be rescued in this interim
            EnterCriticalSection(&standby_list->lock);

            potential_page->standby_page.pagefile_idx = disk_storage_idx;

            standby_add_page(potential_page, standby_list);
            InterlockedIncrement64(&total_available_pages);

            // SetEvent(waiting_for_pages_event);

            LeaveCriticalSection(&standby_list->lock);

            curr_page++;
        }
        SetEvent(waiting_for_pages_event);

        sb_count++;
    }
    
}


/**
 * Connects the given PTE to the open page's physical frame and alerts the CPU
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 */
int connect_pte_to_page(PTE* pte, PAGE* open_page) {
    if (pte == NULL) {
        fprintf(stderr, "NULL PTE given to connect_pte_to_page\n");
        return ERROR;
    }
    
    PTE_LOCKSECTION* pte_locksection = pte_to_locksection(pte);

    ULONG64 pfn = page_to_pfn(open_page);
    
    #ifdef DEBUG_CHECKING
    if (pte_valid_count_check(pte) == FALSE) {
        DebugBreak();
    }
    #endif

    pte_locksection->valid_pte_count++;

    // Map the CPU
    if (MapUserPhysicalPages (pte_to_va(pte), 1, &pfn) == FALSE) {

        fprintf (stderr, "connect_pte_to_page : could not map VA %p to pfn %llx\n", pte_to_va(pte), pfn);
        DebugBreak();

        return ERROR;
    }

    if (is_memory_format(*pte)) {
        printf("MEM FORMAT PTE\n");
    }

    PTE pte_contents;
    pte_contents.complete_format = 0;
    pte_contents.memory_format.age = 0;
    pte_contents.memory_format.frame_number = pfn;
    pte_contents.memory_format.valid = VALID;

    write_pte_contents(pte, pte_contents);

    open_page->active_page.pte = pte;

    #ifdef DEBUG_CHECKING
    if (pte_valid_count_check(pte) == FALSE) {
        DebugBreak();
    }
    #endif

    return SUCCESS;
}


/**
 * Disconnects the PTE from the CPU, but **does not** change the PTE structure
 * as this may be used for both disk and transition format PTEs
 */
int disconnect_pte_from_cpu(PTE* pte) {
    if (pte == NULL) {
        fprintf(stderr, "NULL pte given to disconnect_pte_from_cpu\n");
        return ERROR;
    }

    PTE_LOCKSECTION* pte_locksection = pte_to_locksection(pte);

    pte_locksection->valid_pte_count --;

    #ifdef DEBUG_CHECKING
    if (pte_valid_count_check(pte) == FALSE) {
        DebugBreak();
    }
    #endif

    // Unmap the CPU
    if (MapUserPhysicalPages (pte_to_va(pte), 1, NULL) == FALSE) {

        fprintf (stderr, "full_virtual_memory_test : could not unmap VA %p\n", pte_to_va(pte));

        DebugBreak();
        return ERROR;
    }

    return SUCCESS;
}
