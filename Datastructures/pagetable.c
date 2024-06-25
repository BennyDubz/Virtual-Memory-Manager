/**
 * @author Ben Williams
 * @date June 18th, 2024
 * 
 * Header file for the pagetable and its functions
 */

#include <stdlib.h>
#include <stdio.h>
#include <windows.h>
#include <assert.h>
#include "./pagetable.h"
#include "../hardware.h"

/**
 * Initializes the pagetable with all VALID_PTE entries, but all have the valid bit set to 0
 * 
 * Returns a pointer to a list of PTEs with num_physical_frames entries, or NULL upon an error
 */
PAGETABLE* initialize_pagetable(ULONG64 num_virtual_pages, PULONG_PTR vmem_base) {
    printf("Number of virtual pages: %lld\n", num_virtual_pages);
    PAGETABLE* pagetable = (PAGETABLE*) malloc(sizeof(PAGETABLE));

    if (pagetable == NULL) {
        fprintf(stderr, "Unable to allocate memory for pagetable\n");
        return NULL;
    }

    PTE* pte_list = (PTE*) malloc(sizeof(PTE) * num_virtual_pages);

    if (pte_list == NULL) {
        fprintf(stderr, "Unable to allocate memory for pte list\n");
        return NULL;
    }

    pagetable->pte_list = pte_list;
    pagetable->num_virtual_pages = num_virtual_pages;
    pagetable->vmem_base = (ULONG64) vmem_base;

    // Create empty, invalid PTEs for entire virtual address space
    for (int virtual_page = 0; virtual_page <= num_virtual_pages; virtual_page++) {
        PTE new_pte;
        
        //BW: Ask why valid could be 1 when uninitialized
        new_pte.memory_format.frame_number = 0;
        new_pte.memory_format.age = 0;
        new_pte.memory_format.valid = INVALID;

        pte_list[virtual_page] = new_pte;
    }

    InitializeCriticalSection(&pagetable->pte_lock);

    return pagetable;
}


/**
 * Given a virtual address, return the relevant PTE from the pagetable
 * 
 * Returns NULL upon error
 */
PTE* va_to_pte(PAGETABLE* pagetable, PULONG_PTR virtual_address) {
    if (pagetable == NULL || virtual_address == NULL) {
        fprintf(stderr, "NULL pagetable or virtual address given to va_to_pta");
        return NULL;
    }

    ULONG64 pte_index = DOWN_TO_PAGE_NUM((ULONG64) virtual_address - pagetable->vmem_base);

    if (pte_index > pagetable->num_virtual_pages) {
        fprintf(stderr, "Illegal virtual address given to va_to_pte %llX\n", (ULONG64) virtual_address);
        return NULL;
    }

    return &pagetable->pte_list[pte_index];
}


/**
 * Returns the base virtual address associated with the given PTE, or NULL otherwise
 * 
 */
PULONG_PTR pte_to_va(PAGETABLE* pagetable, PTE* pte) {
    if (pagetable == NULL || pte == NULL) {
        fprintf(stderr, "NULL pagetable or PTE given to pte_to_va");
        return NULL;
    }

    //BW: Implement additional safety checks
    ULONG64 base_address_pte_list = (ULONG64) pagetable->pte_list;
    ULONG64 pte_address = (ULONG64) pte;

    ULONG64 pte_index = (pte_address - base_address_pte_list) / sizeof(PTE);

    PULONG_PTR virtual_address = (PULONG_PTR) (pagetable->vmem_base + (pte_index * PAGE_SIZE));

    return virtual_address;
}


/**
 * Returns TRUE if the PTE is in the memory format, FALSE otherwise
 * or if the PTE is NULL
 */
BOOL is_memory_format(PTE* pte) {
    if (pte == NULL) return FALSE;

    return pte->memory_format.valid == VALID;
}


/**
 * Returns TRUE if the PTE is in the transition format, FALSE otherwise
 * or if the PTE is NULL
 */
BOOL is_disk_format(PTE* pte) {
    if (pte == NULL) return FALSE;
    
    return (pte->disk_format.always_zero == 0 && \
        pte->disk_format.on_disk == 1);
}


/**
 * Returns TRUE if the PTE is in the disc format, FALSE otherwise
 * or if the PTE is NULL
 */
BOOL is_transition_format(PTE* pte) {
    if (pte == NULL) return FALSE;

    return (pte->transition_format.always_zero == 0 && \
        pte->transition_format.always_zero2 == 0);
}


/**
 * Returns TRUE if the PTE has ever been accessed, FALE otherwise
 * or if the PTE is NULL
 */
BOOL is_used_pte(PTE* pte) {
    if (pte == NULL) return FALSE;

    return pte->memory_format.frame_number != 0;
}


/**
 * Returns the frame number of the lowest page in the pagetable that is valid, makes the PTE invalid 
 * and communicates with the CPU
 * 
 * Returns either the frame number or ERROR otherwise
 */
ULONG64 steal_lowest_frame(PAGETABLE* pagetable) {
    if (pagetable == NULL) {
        fprintf(stderr, "NULL pagetable given to steal_lowest_frame\n");
        return ERROR;
    }


    for (ULONG64 pte_idx = 0; pte_idx < pagetable->num_virtual_pages; pte_idx++) {
        PTE* candidate_pte = &pagetable->pte_list[pte_idx];

        // See if the pte is currently linked to a physical frame that we can take
        if (is_memory_format(candidate_pte)) {

            // We have found our victim to steal from
            ULONG64 pfn = candidate_pte->memory_format.frame_number;

            PULONG_PTR pte_va = pte_to_va(pagetable, candidate_pte);

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
            if (is_memory_format(candidate_pte)) {

                // We have found our victim to steal from
                ULONG64 pfn = candidate_pte->memory_format.frame_number;

                PULONG_PTR pte_va = pte_to_va(pagetable, candidate_pte);

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