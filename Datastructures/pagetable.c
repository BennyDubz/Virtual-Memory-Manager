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
PAGETABLE* initialize_pagetable(ULONG64 num_virtual_pages) {
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

    // Create empty, invalid PTEs for entire virtual address space
    for (int virtual_page = 0; virtual_page <= num_virtual_pages; virtual_page++) {
        PTE new_pte;

        new_pte.memory_format.age = 0;
        new_pte.memory_format.valid = INVALID;

        pte_list[virtual_page] = new_pte;
    }

    return pagetable;
}


/**
 * Given a virtual address, return the relevant PTE from the pagetable
 * 
 * Returns NULL upon error
 */
PTE* va_to_pte(PAGETABLE* pagetable, PULONG_PTR virtual_address, PULONG_PTR vmem_base) {
    ULONG64 pte_index = DOWN_TO_PAGE_NUM(virtual_address - vmem_base);

    // printf("accessed va %llX\n", (ULONG64) (virtual_address - vmem_base));
    // printf("accessed pte_index %lld\n", pte_index);

    if (pte_index > pagetable->num_virtual_pages) {
        fprintf(stderr, "Illegal virtual address given to va_to_pte %llX\n", (ULONG64) virtual_address);
        return NULL;
    }

    return &pagetable->frame_list[pte_index];
}
