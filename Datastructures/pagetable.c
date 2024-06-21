/**
 * @author Ben Williams
 * @date June 18th, 2024
 * 
 * Header file for the pagetable and its functions
 */

#include <stdlib.h>
#include <stdio.h>
#include <windows.h>
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

    pagetable->frame_list = pte_list;
    pagetable->num_virtual_pages = num_virtual_pages;
    pagetable->vmem_base = (ULONG64) vmem_base;

    // Create empty, invalid PTEs for entire virtual address space
    for (int virtual_page = 0; virtual_page <= num_virtual_pages; virtual_page++) {
        PTE new_pte;
        new_pte.format_id = UNACCESSED_FORMAT; 

        pte_list[virtual_page] = new_pte;
    }

    return pagetable;
}


/**
 * Given a virtual address, return the relevant PTE from the pagetable
 * 
 * Returns NULL upon error
 */
PTE* va_to_pte(PAGETABLE* pagetable, PULONG_PTR virtual_address) {
    ULONG64 pte_index = DOWN_TO_PAGE_NUM((ULONG64) virtual_address - pagetable->vmem_base);

    if (pte_index > pagetable->num_virtual_pages) {
        fprintf(stderr, "Illegal virtual address given to va_to_pte %llX\n", (ULONG64) virtual_address);
        return NULL;
    }

    return &pagetable->frame_list[pte_index];
}

/**
 * Returns the base virtual address associated with the given PTE, or ERROR otherwise
 * 
 */
PULONG_PTR pte_to_va(PAGETABLE* pagetable, PTE* pte) {
    if (pagetable == NULL || pte == NULL) {
        fprintf(stderr, "NULL pagetable or PTE given to pte_to_va");
        return ERROR;
    }

    //BW: Implement additional safety checks
    ULONG64 base_address_pte_list = (ULONG64) pagetable->frame_list;
    ULONG64 pte_address = (ULONG64) pte;

    ULONG64 pte_index = (pte_address - base_address_pte_list) / sizeof(PTE);

    PULONG_PTR virtual_address = (PULONG_PTR) (pagetable->vmem_base + (pte_index * PAGE_SIZE));

    return virtual_address;
}

/**
 * Returns the frame number of the lowest page in the pagetable that is valid, makes the PTE invalid 
 * and communicates with the CPU
 * 
 * Returns either the frame number or ERROR otherwise
 */
ULONG64 steal_lowest_frame(PAGETABLE* pagetable) {

    for (ULONG64 pte_idx = 0; pte_idx < pagetable->num_virtual_pages; pte_idx++) {
        PTE* candidate_pte = &pagetable->frame_list[pte_idx];

        if (candidate_pte->format_id == VALID_FORMAT && candidate_pte->memory_format.valid == VALID) {
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