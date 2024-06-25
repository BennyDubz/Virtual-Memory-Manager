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
#include "../Datastructures/datastructures.h"


/**
 * Handles the page fault for the given virtual address
 * 
 * Returns SUCCESS if the faulting instruction can be tried again, ERROR otherwise
 * 
 */
int pagefault(PULONG_PTR virtual_address) {
    //
            // Connect the virtual address now - if that succeeds then
            // we'll be able to access it from now on.
            //
            // THIS IS JUST REUSING THE SAME PHYSICAL PAGE OVER AND OVER !
            //
            // IT NEEDS TO BE REPLACED WITH A TRUE MEMORY MANAGEMENT
            // STATE MACHINE !
            //

            // TODO: Look at the PTE for this va to make sure we are doing the right thing
            // Might be trimmed, might be first access, etc...


            // Adjust PTE to reflect its allocated page
            PTE* accessed_pte = va_to_pte(*pagetable, virtual_address);

            if (accessed_pte == NULL) {
                fprintf(stderr, "Unable to find the accessed PTE\n");
                return ERROR;
            }

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
                    printf("Seeing disk format pte\n");
                    // Get the frame
                    // Bring back the page from the pagefile using the PTE
                    // printf("Fetch from disk\n");
                } else if (is_transition_format(local_pte)) {
                    printf("Seeing transition format pte\n");
                    // Rescue the frame from the modified or standby list
                    
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

            local_pte.memory_format.age = 0;
            local_pte.memory_format.frame_number = pfn;
            local_pte.memory_format.valid = VALID;

            *accessed_pte = local_pte;

            // Allocate the physical frame to the virtual address
            if (MapUserPhysicalPages (virtual_address, 1, &pfn) == FALSE) {

                printf ("full_virtual_memory_test : could not map VA %p to page %llX\n", virtual_address, pfn);
                LeaveCriticalSection(&pagetable->pte_lock);
                return ERROR;
            }

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