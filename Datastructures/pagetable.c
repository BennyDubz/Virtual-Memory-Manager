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

    ULONG64 num_locks = max(num_virtual_pages >> 8, 1);

    CRITICAL_SECTION* pte_locks = (CRITICAL_SECTION*) malloc(sizeof(CRITICAL_SECTION) * num_locks);

    if (pte_locks == NULL) {
        fprintf(stderr, "Unable to allocate memory for pagetable locks\n");
        return NULL;
    }

    ULONG64* valid_pte_counts = (ULONG64*) malloc(sizeof(ULONG64));

    if (valid_pte_counts == NULL) {
        fprintf(stderr, "Unable to allocate memory for valid_pte_counts\n");
        return NULL;
    }

    for (ULONG64 curr_lock = 0; curr_lock < num_locks; curr_lock++) {
        InitializeCriticalSection(&pte_locks[curr_lock]);
        valid_pte_counts[curr_lock] = 0;
    } 

    pagetable->num_locks = num_locks;
    pagetable->pte_locks = pte_locks;
    pagetable->valid_pte_counts = valid_pte_counts;

    return pagetable;
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
        pte.disk_format.on_disk == 1);
}


/**
 * Returns TRUE if the PTE is in the disc format, FALSE otherwise
 */
BOOL is_transition_format(PTE pte) {
    return (pte.transition_format.always_zero == 0 && \
        pte.transition_format.always_zero2 == 0);
}


/**
 * Returns TRUE if the PTE has ever been accessed, FALE otherwise
 */
BOOL is_used_pte(PTE pte) {
    return pte.memory_format.frame_number != 0;
}


/**
 * Returns TRUE if both PTEs are equivalent, FALSE otherwise
 */
BOOL ptes_are_equal(PTE pte1, PTE pte2) {
    if (is_memory_format(pte1) && is_memory_format(pte2)) {
        return (pte1.memory_format.frame_number == pte2.memory_format.frame_number
            && pte1.memory_format.age == pte2.memory_format.age);
    }

    if (is_transition_format(pte1) && is_transition_format(pte2)) {
        return (pte1.transition_format.frame_number == pte2.transition_format.frame_number);
    }

    if (is_disk_format(pte1) && is_disk_format(pte2)) {
        return (pte1.disk_format.pagefile_idx == pte2.disk_format.pagefile_idx);
    }

    return FALSE;
}
