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
    for (ULONG64 virtual_page = 0; virtual_page < num_virtual_pages; virtual_page++) {
        PTE new_pte;
        
        // This leaves the entire PTE empty - signaling it has never been used before
        new_pte.complete_format = 0;

        pte_list[virtual_page] = new_pte;
    }

    ULONG64 num_locks = max(num_virtual_pages >> 6, 1);

    PTE_LOCKSECTION* pte_locksections = (PTE_LOCKSECTION*) malloc(sizeof(PTE_LOCKSECTION) * num_locks);

    if (pte_locksections == NULL) {
        fprintf(stderr, "Unable to allocate memory for pte locksections\n");
        return NULL;
    }

    for (ULONG64 curr_lock = 0; curr_lock < num_locks; curr_lock++) {
        PTE_LOCKSECTION* curr_section = &pte_locksections[curr_lock];
        InitializeCriticalSection(&curr_section->lock);
        curr_section->valid_pte_count = 0;
    } 

    pagetable->num_locks = num_locks;
    pagetable->pte_locksections = pte_locksections;

    return pagetable;
}


/**
 * Returns the contents of the given PTE in one operation indivisibly
 */
PTE read_pte_contents(PTE* pte_to_read) {
    return *(volatile PTE*) pte_to_read;
}


/**
 * Writes the PTE contents as a single indivisble write to the given PTE pointer
 */
void write_pte_contents(PTE* pte_to_write, PTE pte_contents) {
    *(volatile PTE*) pte_to_write = pte_contents; 
}


/**
 * Returns TRUE if the PTE is in the memory format, FALSE otherwise
 */
BOOL is_memory_format(PTE pte) {
    return pte.memory_format.valid == VALID;
}


/**
 * Returns TRUE if the PTE is in the transition format, FALSE otherwise
 */
BOOL is_disk_format(PTE pte) {    
    return (pte.disk_format.always_zero == 0 && \
        pte.disk_format.always_zero2 == 0 && pte.disk_format.pagefile_idx != 0);
}


/**
 * Returns TRUE if the PTE is in the disc format, FALSE otherwise
 */
BOOL is_transition_format(PTE pte) {
    return (pte.transition_format.always_zero == 0 && \
        pte.transition_format.is_transition == 1);
}


/**
 * Returns TRUE if the PTE has ever been accessed, FALSE otherwise
 */
BOOL is_used_pte(PTE pte) {
    return pte.complete_format != 0;
}


/**
 * Returns TRUE if both PTEs are equivalent, FALSE otherwise
 */
BOOL ptes_are_equal(PTE pte1, PTE pte2) {
    return (pte1.complete_format == pte2.complete_format);
}
