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

/**
 * Initializes the pagetable with all VALID_PTE entries, but all have the valid bit set to 0
 * 
 * Returns a pointer to a list of PTEs with num_physical_frames entries, or NULL upon an error
 */
PTE* initialize_pagetable(ULONG64 num_physical_frames, ULONG64* physical_frame_numbers) {
    PTE* page_table = (PTE*) malloc(sizeof(PTE) * num_physical_frames);

    if (page_table == NULL) {
        fprintf(stderr, "Unable to allocate memory for pagetable\n");
        return NULL;
    }

    for(int physical_frame = 0; physical_frame <= num_physical_frames; physical_frame++) {
        PTE new_pte;

        new_pte.memory_format.frame_number = physical_frame_numbers[physical_frame];
        new_pte.memory_format.age = 0;
        new_pte.memory_format.valid = INVALID;

        page_table[physical_frame] = new_pte;
    }

    return page_table;
}

