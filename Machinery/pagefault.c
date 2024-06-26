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
#include "./conversions.h"
#include "../Datastructures/datastructures.h"


/**
 * Handles the page fault for the given virtual address
 * 
 * Returns SUCCESS if the faulting instruction can be tried again, ERROR otherwise
 * 
 */
int pagefault(PULONG_PTR virtual_address) {
            PTE* accessed_pte = va_to_pte(virtual_address);

            if (accessed_pte == NULL) {
                fprintf(stderr, "Unable to find the accessed PTE\n");
                return ERROR;
            }

            BOOL disk_flag = FALSE;

            // Much shorter lock hold than using the accessed PTE for all of the other if statements
            EnterCriticalSection(&pagetable->pte_lock);
            PTE local_pte = *accessed_pte;

            if (is_used_pte(local_pte)) {
                // Check the PTE to decide what our course of action is
                if (is_memory_format(local_pte)) {
                    LeaveCriticalSection(&pagetable->pte_lock);
                    // Skip to the next random access, another thread has already validated this address
                    return SUCCESS;
                } else if (is_disk_format(local_pte)) {
                    disk_flag = TRUE;
                } else if (is_transition_format(local_pte)) {
                    printf("Seeing transition format pte\n");

                    if (rescue_pte(&local_pte) == ERROR) {
                        LeaveCriticalSection(&pagetable->pte_lock);
                        return ERROR;
                    } else {
                        *accessed_pte = local_pte;
                        LeaveCriticalSection(&pagetable->pte_lock);
                        return SUCCESS;
                    }              
                }
            }

            /**
             * By the time we get here, the PTE has never been accessed before,
             * so we just need to find a frame to allocate to it
             */            
            
            //BW: Might have deadlock if we have a lock for free frames list
            PAGE* new_page = allocate_free_frame(free_frames);

            // Then, check standby
            ULONG64 pfn;
            if (new_page == NULL) {
                
                //BW: Will be replaced by taking from the standby
                // if the standby fails, then we need to release our lock and wait to be signalled by the standby
                // thread to try this page fault again
                pfn = steal_lowest_frame(pagetable);

                if (pfn == ERROR) {
                    fprintf(stderr, "Unable to find valid frame to steal\n");
                }

                // printf("Stolen pfn: %llX\n", pfn);
            } else {
                pfn = new_page->free_page.frame_number;
                if (pfn == 0) {
                    fprintf(stderr, "Invalid pfn from free page\n");
                    LeaveCriticalSection(&pagetable->pte_lock);
                    return ERROR;
                }
            }

            // Allocate the physical frame to the virtual address
            if (MapUserPhysicalPages (virtual_address, 1, &pfn) == FALSE) {

                printf ("full_virtual_memory_test : could not map VA %p to page %llX\n", virtual_address, pfn);
                LeaveCriticalSection(&pagetable->pte_lock);
                return ERROR;
            }

            if (disk_flag) {
                // We need to use the accessed pte to have the correct address for pte_to_va
                if (get_from_disk(accessed_pte) == ERROR) {
                    fprintf(stderr, "Failed to read from disk into pte\n");
                    LeaveCriticalSection(&pagetable->pte_lock);
                    return ERROR;
                }
            }

            local_pte.memory_format.age = 0;
            local_pte.memory_format.frame_number = pfn;
            local_pte.memory_format.valid = VALID;

            *accessed_pte = local_pte;


            LeaveCriticalSection(&pagetable->pte_lock);
            //
            // No exception handler needed now since we have connected
            // the virtual address above to one of our physical pages
            // so no subsequent fault can occur.
            //

            // BW: Will remove this so that the only try/except is at 271,
            // and that that block can be considered user code.

            return SUCCESS;


            #if 0
            //
            // Unmap the virtual address translation we installed above
            // now that we're done writing our value into it.
            //

            // 
            if (MapUserPhysicalPages (arbitrary_va, 1, NULL) == FALSE) {

                printf ("full_virtual_memory_test : could not unmap VA %p\n", arbitrary_va);

                return;
            }
            #endif
}

/**
 * For the given PTE, find its associated page in the standby list or modified list
 * and reconnect its frame back to the pte and remove it from the list
 * 
 * ### The pagetable critical section must be acquired before this! ###
 * 
 * Returns SUCCESS if the pte is succssfully reconnected, ERROR otherwise
 */
int rescue_pte(PTE* pte) {
    if (pte == NULL || !is_transition_format(*pte)) {
        fprintf(stderr, "NULL or invalid pte given to rescue_pte\n");
        return ERROR;
    }

    ULONG64 pfn = pte->transition_format.frame_number;

    PAGE* relevant_page = page_from_pfn(pfn);
    
    int result;

    if (page_is_modified(*relevant_page)) {
        if (modified_rescue_page(modified_list, pte) == NULL) {
            return ERROR;
        }
    } else if (page_is_standby(*relevant_page)){
        if (standby_rescue_page(standby_list, pte) == NULL) {
            return ERROR;
        }
    }

    pte->memory_format.age = 0;
    pte->memory_format.valid = VALID;

    // Reconnect PTE with the CPU
    if (MapUserPhysicalPages (pte_to_va(pte), 1, &pfn) == FALSE) {

        printf ("full_virtual_memory_test : could not map VA %p to page %llX\n", pte_to_va(pte), pfn);
        LeaveCriticalSection(&pagetable->pte_lock);
        return ERROR;
    }

    return SUCCESS;
}