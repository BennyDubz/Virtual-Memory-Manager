/**
 * @author Ben Williams
 * @date June 19th, 2024
 * 
 * Implementation of freelist functions and creation
 */

#include <stdio.h>
#include "../db_linked_list.h"
#include "./freelist.h"
#include "../../macros.h"


/**
 * Given the new pagetable, create free frames lists that contain all of the physical frames
 * 
 * Returns a memory allocated pointer to a FREE_FRAMES_LISTS struct, or NULL if an error occurs
 */
FREE_FRAMES_LISTS* initialize_free_frames(PTE* page_table, ULONG64 num_physical_frames) {
    FREE_FRAMES_LISTS* free_frames = (FREE_FRAMES_LISTS*) malloc(sizeof(FREE_FRAMES_LISTS));

    if (free_frames == NULL) {
        fprintf(stderr, "Unable to allocate memory for free frames lists");
        return NULL;
    }

    // Create all the individual listheads
    for (int new_list = 0; new_list < NUM_FRAME_LISTS; new_list++) {
        DB_LL_NODE* new_listhead = create_db_list();
        free_frames->frame_listheads[new_list] = new_listhead;
    }


    // Add all the physical frames to their respective free lists
    for (int pte_idx = 0; pte_idx < num_physical_frames; pte_idx++) {
        PTE* curr_pte = &page_table[pte_idx];

        // Modulo-operation based on the frame number, not the pte index
        int listhead_idx = curr_pte->memory_format.frame_number % NUM_FRAME_LISTS;
        DB_LL_NODE* relevant_listhead = free_frames->frame_listheads[listhead_idx];

        FREE_FRAME* free_frame = (FREE_FRAME*) malloc(sizeof(FREE_FRAME));

        if (free_frame == NULL) {
            fprintf(stderr, "Unable to allocate memory for a free frame\n");
            return NULL;
        }

        free_frame->physical_frame_number = curr_pte->memory_format.frame_number;
        // Free frames on boot are clean to begin with
        free_frame->zeroed_out = 1;

        if (db_insert_at_head(relevant_listhead, free_frame) == ERROR) {
            fprintf(stderr, "Failed to insert free frame in its list\n");
            //BW: Might want to free memory for everything we've allocated already
            return NULL;
        }
    }

    return free_frames;
}


/**
 * Zeroes out the memory on the physical frame so that it can be allocated to a new process without privacy loss
 * 
 * Returns SUCCESS if no issues, ERROR otherwise
 */
int zero_out_frame(PTE* page_table, ULONG64 frame_number);