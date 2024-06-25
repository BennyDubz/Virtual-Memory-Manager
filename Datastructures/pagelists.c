/**
 * @author Ben Williams
 * @date June 19th, 2024
 * 
 * Implementation of freelist functions and creation
 */

#include <stdio.h>
#include <windows.h>
#include "./db_linked_list.h"
#include "./pagelists.h"
#include "../macros.h"

/**
 * ###########################
 * GENERAL PAGE LIST FUNCTIONS
 * ###########################
 */


/**
 * Initializes all of the pages, and organizes them in memory such that they are reachable using the page_from_pfn
 * function in O(1) time. Returns the address of page_storage_base representing the base address of where all the pages
 * can be found from, minus the lowest pagenumber for simpler arithmetic in the other functions
 * 
 * Returns NULL given any error
 */
PULONG_PTR initialize_pages(PULONG_PTR physical_frame_numbers, ULONG64 num_physical_frames) {

    ULONG64 lowest_pfn = 0xFFFFFFFFFFFFFFFF;
    ULONG64 highest_pfn = 0x0;

    for (ULONG64 frame_idx = 0; frame_idx < num_physical_frames; frame_idx++) {
        ULONG64 curr_pfn = physical_frame_numbers[frame_idx];
        
        lowest_pfn = curr_pfn < lowest_pfn ? curr_pfn : lowest_pfn;

        highest_pfn = curr_pfn > highest_pfn ? curr_pfn : highest_pfn;
    }

    // Now we can reserve the minimum amount of memory required for this scheme
    PAGE* page_storage_base = VirtualAlloc(NULL, (highest_pfn - lowest_pfn) * sizeof(PAGE), 
                                                MEM_RESERVE, PAGE_READWRITE);
    
    if (page_storage_base == NULL) {
        fprintf(stderr, "Unable to reserve memory for the pages\n");
        return NULL;
    }
    
    // This makes it so we can easily shift to find even the lowest physical frame
    page_storage_base -= lowest_pfn;


    // Now, we can actually commit memory for each page
    for (ULONG64 frame_idx = 0; frame_idx < num_physical_frames; frame_idx++) {
        ULONG64 curr_pfn = physical_frame_numbers[frame_idx];
        
        PAGE* new_page = VirtualAlloc(page_storage_base + curr_pfn, 
                                    sizeof(PAGE), MEM_COMMIT, PAGE_READWRITE);
        
        if (new_page == NULL) {
            fprintf(stderr, "Unable to allocate memory for page in initialize_pages\n");
            return NULL;
        }

        /**
         * By creating the listnodes here up front as well, we ensure we do not have to malloc them 
         * later on down the road. If we were unable to allocate memory to a node to add
         * it to a standby list, for example, then we would have serious issues.
         */
        # if 0
        DB_LL_NODE* page_listnode = db_create_node(new_page);

        if (page_listnode == NULL) {
            fprintf(stderr, "Unable to allocate memory for frame listnode in initialize_pages\n");
            return NULL;
        }
        
        new_page->free_page.frame_listnode = page_listnode;
        #endif
        new_page->free_page.frame_number = curr_pfn;
        new_page->free_page.status = INVALID;
        new_page->free_page.zeroed_out = 1;
    }


    return (PULONG_PTR) page_storage_base;
}


/**
 * ### Initialize pages must be called before this function! ###
 * 
 * Given the frame number and the base address of where the pages are stored, returns a pointer to the relevant 
 * PAGE struct associated with the frame number. 
 * 
 * Returns NULL given any error
 */
PAGE* page_from_pfn(ULONG64 frame_number, PULONG_PTR page_storage_base) {
    if (page_storage_base == NULL) {
        fprintf(stderr, "Page storage base is NULL in page_from_pfn\n");
        return NULL;
    }

    return (PAGE*) (page_storage_base) + frame_number;
}


// /**
//  * Connects the given PTE to the open page's physical frame and alerts the CPU
//  * 
//  * Returns SUCCESS if there are no issues, ERROR otherwise
//  */
// int connect_pte_to_page(PTE* pte, PAGE* open_page) {

// }

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
FREE_FRAMES_LISTS* initialize_free_frames(PULONG_PTR page_storage_base, ULONG64* physical_frame_numbers, ULONG64 num_physical_frames) {
    FREE_FRAMES_LISTS* free_frames = (FREE_FRAMES_LISTS*) malloc(sizeof(FREE_FRAMES_LISTS));

    if (free_frames == NULL) {
        fprintf(stderr, "Unable to allocate memory for free frames lists");
        return NULL;
    }

    // Create all the individual listheads
    for (int new_list = 0; new_list < NUM_FRAME_LISTS; new_list++) {
        DB_LL_NODE* new_listhead = db_create_list();
        free_frames->listheads[new_list] = new_listhead;
        free_frames->list_lengths[new_list] = 0;
        free_frames->curr_list_idx = 0;
    }

    // Add all the physical frames to their respective free lists
    for (int pfn_idx = 0; pfn_idx < num_physical_frames; pfn_idx++) {
        ULONG64 frame_number = physical_frame_numbers[pfn_idx];

        // Modulo-operation based on the frame number, not the pte index
        int listhead_idx = frame_number % NUM_FRAME_LISTS;

        DB_LL_NODE* relevant_listhead = free_frames->listheads[listhead_idx];

        PAGE* free_frame = page_from_pfn(frame_number, page_storage_base);

        if (free_frame == NULL) {
            fprintf(stderr, "Unable to find the page associated with the pfn in initialize_free_frames\n");
            return NULL;
        }

        DB_LL_NODE* free_listnode = db_insert_at_head(relevant_listhead, free_frame);

        #if 0
        // Add the already allocated frame listnode to the free list
        if (db_insert_node_at_head(relevant_listhead, free_frame->free_page.frame_listnode) == ERROR) {
            fprintf(stderr, "Failed to insert free frame in its list\n");
            return NULL;
        }
        #endif

        free_frame->free_page.status = FREE_STATUS;
        free_frame->free_page.zeroed_out = 1;
        free_frame->free_page.frame_number = frame_number;

        free_frames->list_lengths[listhead_idx] += 1;
    }

    return free_frames;
}


/**
 * Returns a page off the free list, if there are any. Otherwise, returns NULL
 */
PAGE* allocate_free_frame(FREE_FRAMES_LISTS* free_frames) {
    int curr_attempts = 0;
    
    //BW: SYNC - incrementing curr_list_idx
    PAGE* page = NULL;
    while (curr_attempts < NUM_FRAME_LISTS) {
        // Check for empty list
        if (free_frames->list_lengths[free_frames->curr_list_idx] == 0) {
            curr_attempts += 1;
            free_frames->curr_list_idx = (free_frames->curr_list_idx + 1) % NUM_FRAME_LISTS;
            continue;
        }

        // By here, we know we can get a free frame
        DB_LL_NODE* frame_listhead = free_frames->listheads[free_frames->curr_list_idx];

        page = (PAGE*) db_pop_from_head(frame_listhead);
        free_frames->list_lengths[free_frames->curr_list_idx] -= 1;
        free_frames->curr_list_idx = (free_frames->curr_list_idx + 1) % NUM_FRAME_LISTS;

        //BW: The listnode should also be refreshed 
        page->free_page.frame_listnode->blink = NULL;
        page->free_page.frame_listnode->flink = NULL;


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