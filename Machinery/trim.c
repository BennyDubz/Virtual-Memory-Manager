/**
 * @author Ben Williams
 * @date June 25th, 2024
 * 
 * All functionality for trimming
 */

#include <stdio.h>
#include <assert.h>
#include "../globals.h"
#include "../Datastructures/datastructures.h"
#include "./trim.h"

/**
 * Returns the frame number of the lowest page in the pagetable that is valid, makes the PTE invalid 
 * and communicates with the CPU
 * 
 * Returns either the frame number or ERROR otherwise
 */
ULONG64 steal_lowest_frame() {
    for (ULONG64 pte_idx = 0; pte_idx < pagetable->num_virtual_pages; pte_idx++) {
        PTE* candidate_pte = &pagetable->pte_list[pte_idx];

        // See if the pte is currently linked to a physical frame that we can take
        if (is_memory_format(*candidate_pte)) {

            // We have found our victim to steal from
            ULONG64 pfn = candidate_pte->memory_format.frame_number;

            PULONG_PTR pte_va = pte_to_va(*pagetable, candidate_pte);

            // Unmap the CPU
            if (MapUserPhysicalPages (pte_va, 1, NULL) == FALSE) {

                fprintf (stderr, "full_virtual_memory_test : could not unmap VA %p\n", pte_va);

                return ERROR;
            }

            // Disconnect the PTE
            candidate_pte->memory_format.valid = INVALID;

            return pfn;
        }
    }

    // No frame to steal was ever found
    return ERROR;
}


/**
 * Returns the frame number of the lowest page in the pagetable that is valid, makes the PTE invalid 
 * and communicates with the CPU
 * 
 * Returns either the frame number or ERROR otherwise
 */
ULONG64 trimming_thread() {
    while (TRUE) {
        //BW: Wait for event signal...
        PAGETABLE* pagetable;

        EnterCriticalSection(&pagetable->pte_lock);
        for (ULONG64 pte_idx = 0; pte_idx < pagetable->num_virtual_pages; pte_idx++) {
            PTE* candidate_pte = &pagetable->pte_list[pte_idx];

            // See if the pte is currently linked to a physical frame that we can take
            if (is_memory_format(*candidate_pte)) {

                // We have found our victim to steal from
                ULONG64 pfn = candidate_pte->memory_format.frame_number;

                PULONG_PTR pte_va = pte_to_va(*pagetable, candidate_pte);

                // Unmap the CPU
                if (MapUserPhysicalPages (pte_va, 1, NULL) == FALSE) {

                    fprintf (stderr, "full_virtual_memory_test : could not unmap VA %p\n", pte_va);
                    assert(FALSE);
                    return ERROR;
                }

                // Disconnect the PTE
                candidate_pte->memory_format.valid = INVALID;

                //BW: Insert page into the modified list instead
                // Also will want to check if we have enough pfns added to the modified list 
                // to see if we continue or break
            }
        }

        LeaveCriticalSection(&pagetable->pte_lock);
    }
}