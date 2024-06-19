/**
 * @author Ben Williams
 * @date June 19th, 2024
 * 
 * Implementation of freelist functions and creation
 */

#include <stdio.h>
#include "./db_linked_list.h"
#include "./pagelists.h"
#include "../macros.h"


/**
 * ##########################
 * FREE FRAMES LIST FUNCTIONS
 * ##########################
 */


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
        free_frames->listheads[new_list] = new_listhead;
    }


    // Add all the physical frames to their respective free lists
    for (int pte_idx = 0; pte_idx < num_physical_frames; pte_idx++) {
        PTE* curr_pte = &page_table[pte_idx];

        // Modulo-operation based on the frame number, not the pte index
        int listhead_idx = curr_pte->memory_format.frame_number % NUM_FRAME_LISTS;
        DB_LL_NODE* relevant_listhead = free_frames->listheads[listhead_idx];

        PAGE* free_frame = (PAGE*) malloc(sizeof(PAGE));

        if (free_frame == NULL) {
            fprintf(stderr, "Unable to allocate memory for a free frame\n");
            return NULL;
        }

        if (db_insert_at_head(relevant_listhead, free_frame) == ERROR) {
            fprintf(stderr, "Failed to insert free frame in its list\n");
            //BW: Might want to free memory for everything we've allocated already
            return NULL;
        }

        free_frame->free_page.status = FREE_STATUS;
        free_frame->free_page.pte = curr_pte;
        free_frame->free_page.zeroed_out = 1;
        
    }

    return free_frames;
}


/**
 * Returns a page off the free list, if there are any. Otherwise, returns NULL
 */
PAGE* allocate_free_frame(FREE_FRAMES_LISTS* free_frames) {
    int curr_attempts = 0;
    
    // SYNC - incrementing curr_list_idx
    PAGE* page = NULL;
    while (curr_attempts < NUM_FRAME_LISTS) {
        // Check for empty list
        if (free_frames->list_lengths[free_frames->curr_list_idx] == 0) {
            curr_attempts += 1;
            free_frames->curr_list_idx += 1;
            continue;
        }

        // By here, we know we can get a free frame
        DB_LL_NODE* frame_listhead = free_frames->listheads[free_frames->curr_list_idx];

        page = (PAGE*) db_pop_from_head(frame_listhead);
        free_frames->list_lengths[free_frames->curr_list_idx] -= 1;
        break;
    }

    return page;
}

/**
 * Zeroes out the memory on the physical frame so that it can be allocated to a new process without privacy loss
 * 
 * Returns SUCCESS if no issues, ERROR otherwise
 */
int zero_out_frame(PTE* page_table, ULONG64 frame_number);