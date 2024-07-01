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
 * Initializes all of the pages, and organizes them in memory such that they are reachable using the pfn_to_page
 * function in O(1) time. Returns the address of page_storage_base representing the base address of where all the pages
 * can be found from, minus the lowest pagenumber for simpler arithmetic in the other functions
 * 
 * Returns NULL given any error
 */
PAGE* initialize_pages(PULONG_PTR physical_frame_numbers, ULONG64 num_physical_frames) {

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
        
        void* page_region = VirtualAlloc(page_storage_base + curr_pfn, 
                                    sizeof(PAGE), MEM_COMMIT, PAGE_READWRITE);
        

        PAGE* new_page = page_storage_base + curr_pfn;
       
        if (new_page == NULL) {
            fprintf(stderr, "Unable to allocate memory for page in initialize_pages\n");
            return NULL;
        }

        /**
         * By creating the listnodes here up front as well, we ensure we do not have to malloc them 
         * later on down the road. If we were unable to allocate memory to a node to add
         * it to a standby list, for example, then we would have serious issues.
         */
        DB_LL_NODE* page_listnode = db_create_node(new_page);

        if (page_listnode == NULL) {
            fprintf(stderr, "Unable to allocate memory for frame listnode in initialize_pages\n");
            return NULL;
        }
        
        new_page->free_page.frame_listnode = page_listnode;
        new_page->free_page.frame_number = curr_pfn;
    }


    return page_storage_base;
}


/**
 * Returns TRUE if the page is in the free status, FALSE otherwise
 */
BOOL page_is_free(PAGE page) {
    return page.free_page.status == FREE_STATUS;
}


/**
 * Returns TRUE if the page is in the modified status, FALSE otherwise
 */
BOOL page_is_modified(PAGE page) {
    return page.modified_page.status == MODIFIED_STATUS;
}


/**
 * Returns TRUE if the page is in the standby status, FALSE otherwise
 */
BOOL page_is_standby(PAGE page) {
    return page.standby_page.status == STANDBY_STATUS;
}


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
FREE_FRAMES_LISTS* initialize_free_frames(PAGE* page_storage_base, ULONG64* physical_frame_numbers, ULONG64 num_physical_frames) {
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

        InitializeCriticalSection(&free_frames->list_locks[new_list]);
    }



    // Add all the physical frames to their respective free lists
    for (int pfn_idx = 0; pfn_idx < num_physical_frames; pfn_idx++) {
        ULONG64 frame_number = physical_frame_numbers[pfn_idx];

        // Modulo operation based on the pfn to put it alongside other cache-colliding pages
        int listhead_idx = frame_number % NUM_FRAME_LISTS;

        DB_LL_NODE* relevant_listhead = free_frames->listheads[listhead_idx];

        PAGE* free_frame = page_storage_base + frame_number;

        if (free_frame == NULL) {
            fprintf(stderr, "Unable to find the page associated with the pfn in initialize_free_frames\n");
            return NULL;
        }

        // Add the already allocated frame listnode to the free list
        if (db_insert_node_at_head(relevant_listhead, free_frame->free_page.frame_listnode) == ERROR) {
            fprintf(stderr, "Failed to insert free frame in its list\n");
            return NULL;
        }

        free_frame->free_page.status = FREE_STATUS;
        free_frame->free_page.zeroed_out = 1;

        free_frames->list_lengths[listhead_idx] += 1;
    }

    return free_frames;
}


/**
 * Returns a page off the free list, if there are any. Otherwise, returns NULL
 */
PAGE* allocate_free_frame(FREE_FRAMES_LISTS* free_frames) {
    int curr_attempts = 0;
    
    ULONG64 local_index = free_frames->curr_list_idx;

    PAGE* page = NULL;
    while (curr_attempts < NUM_FRAME_LISTS) {
        // Check for empty list - we can quickly check here before acquiring the lock
        if (free_frames->list_lengths[free_frames->curr_list_idx] == 0) {
            curr_attempts += 1;

            local_index++;

            /**
             * We expect multiple threads to be incrementing this, new starting threads don't start
             * on empty slots
             */
            free_frames->curr_list_idx = (free_frames->curr_list_idx + 1) % NUM_FRAME_LISTS;
            continue;
        }

        // By here, the **odds are better** that we will get a free frame, but not guaranteed
        EnterCriticalSection(&free_frames->list_locks[local_index]);
        DB_LL_NODE* frame_listhead = free_frames->listheads[free_frames->curr_list_idx];

        page = (PAGE*) db_pop_from_head(frame_listhead);

        // We lost the race condition, release lock and try again with the next one
        if (page == NULL) {
            free_frames->curr_list_idx = (free_frames->curr_list_idx + 1) % NUM_FRAME_LISTS;
            /**
             * We purposefully do NOT increment curr_attempts, as we want the thread to be able to
             * try again (especially early on if the standby list is still empty)
             */
            local_index++;
            LeaveCriticalSection(&free_frames->list_locks[local_index]);
            continue;
        }

        free_frames->list_lengths[free_frames->curr_list_idx] -= 1;

        page->active_page.status = ACTIVE_STATUS;

        LeaveCriticalSection(&free_frames->list_locks[local_index]);

        break;
    }

    return page;
}


/**
 * Zeroes out the memory on the physical frame so that it can be reallocated without privacy loss
 * 
 * Returns SUCCESS if no issues, ERROR otherwise
 */
int zero_out_page(PAGE* page) {
    if (page == NULL) {
        fprintf(stderr, "NULL page given to zero out page\n");
        return ERROR;
    }

    /**
     * Have a few addresses to pop from that we use temporarily to map the pfn to
     * so that we can memset() them to zero.
     */

    return SUCCESS;
}


/**
 * #######################
 * MODIFIED LIST FUNCTIONS
 * #######################
 */


/**
 * Allocates memory for and initializes a modified list struct
 * 
 * Returns a pointer to the modified list or NULL upon error
 */
MODIFIED_LIST* initialize_modified_list() {
    MODIFIED_LIST* modified_list = (MODIFIED_LIST*) malloc(sizeof(MODIFIED_LIST));

    if (modified_list == NULL) {
        fprintf(stderr, "Unable to allocate memory for modified list in initialize_modified_list\n");
        return NULL;
    }

    DB_LL_NODE* mod_listhead = db_create_list();

    if (mod_listhead == NULL) {
        fprintf(stderr, "Unable to create listhead in initialize_modified_list\n");
        return NULL;
    }

    modified_list->listhead = mod_listhead;
    modified_list->list_length = 0;

    InitializeCriticalSection(&modified_list->lock);

    return modified_list;
}


/**
 * Adds the given page to the modified list (at the head)
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 */
int modified_add_page(PAGE* page, MODIFIED_LIST* modified_list) {
    if (page == NULL || modified_list == NULL) {
        fprintf(stderr, "NULL page or modified list given to modified_add_page\n");
        return ERROR;
    }

    page->modified_page.status = MODIFIED_STATUS;
    db_insert_node_at_head(modified_list->listhead, page->modified_page.frame_listnode);
    modified_list->list_length += 1;

    return SUCCESS;
}


/**
 * Pops the oldest page (tail) from the modified list and returns it
 * 
 * Returns NULL upon any error or if the list is empty
 */
PAGE* modified_pop_page(MODIFIED_LIST* modified_list) {
    if (modified_list == NULL) {
        fprintf(stderr, "NULL standby list given to modified_pop_page\n");
        return NULL;
    }

    PAGE* popped = db_pop_from_tail(modified_list->listhead);
    if (popped != NULL) {
        modified_list->list_length -= 1;
    }

    return popped;
}   


/**
 * ######################
 * STANDBY LIST FUNCTIONS
 * ######################
 */


/**
 * Allocates memory for and initializes a standby list struct
 * 
 * Returns a pointer to the standby list or NULL upon error
 */
STANDBY_LIST* initialize_standby_list() {
    STANDBY_LIST* standby_list = (STANDBY_LIST*) malloc(sizeof(STANDBY_LIST));

    if (standby_list == NULL) {
        fprintf(stderr, "Unable to allocate memory for modified list in initialize_modified_list\n");
        return NULL;
    }

    DB_LL_NODE* mod_listhead = db_create_list();

    if (mod_listhead == NULL) {
        fprintf(stderr, "Unable to create listhead in initialize_modified_list\n");
        return NULL;
    }

    standby_list->listhead = mod_listhead;
    standby_list->list_length = 0;

    InitializeCriticalSection(&standby_list->lock);

    return standby_list;
}


/**
 * Adds the given page to the standby list
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 */
int standby_add_page(PAGE* page, STANDBY_LIST* standby_list) {
    if (page == NULL || standby_list == NULL) {
        fprintf(stderr, "NULL page or standby list given to standby_add_page\n");
        return ERROR;
    }
    
    page->standby_page.status = STANDBY_STATUS;
    db_insert_node_at_head(standby_list->listhead, page->modified_page.frame_listnode);
    standby_list->list_length += 1;

    return SUCCESS;
}