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
#include "./custom_sync.h"

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

    printf("Num physical frames: 0x%llx\n", num_physical_frames);
    
    // This makes it so we can easily shift to find even the lowest physical frame
    page_storage_base -= lowest_pfn;

    // Now, we can actually commit memory for each page
    for (ULONG64 frame_idx = 0; frame_idx < num_physical_frames; frame_idx++) {
        ULONG64 curr_pfn = physical_frame_numbers[frame_idx];
        
        void* page_region = VirtualAlloc(page_storage_base + curr_pfn, 
                                    sizeof(PAGE), MEM_COMMIT, PAGE_READWRITE);
        
        if (page_region == NULL) {
            fprintf(stderr, "Failed to allocate memory for page region in initialize_pages\n");
            return NULL;
        }

        PAGE* new_page = page_storage_base + curr_pfn;
       
        if (new_page == NULL) {
            fprintf(stderr, "Unable to allocate memory for page in initialize_pages\n");
            return NULL;
        }

        new_page->page_lock = PAGE_UNLOCKED;

        #if DEBUG_PAGELOCK
        InitializeCriticalSection(&new_page->dev_page_lock);
        #endif

        #if LIGHT_DEBUG_PAGELOCK
        new_page->origin_code = 0xFFFFFF;
        new_page->prev_code = 0xFFFFFF;
        new_page->two_ago = 0xFFFFFF;
        new_page->three_ago = 0xFFFFFF;
        new_page->four_ago = 0xFFFFFF;
        new_page->five_ago = 0xFFFFFF;
        #endif

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
        
        new_page->frame_listnode = page_listnode;
    }

    return page_storage_base;
}


/**
 * Returns TRUE if the page is in the free status, FALSE otherwise
 */
BOOL page_is_free(PAGE page) {
    return page.status == FREE_STATUS;
}


/**
 * Returns TRUE if the page is in the modified status, FALSE otherwise
 */
BOOL page_is_modified(PAGE page) {
    return page.status == MODIFIED_STATUS;
}


/**
 * Returns TRUE if the page is in the standby status, FALSE otherwise
 */
BOOL page_is_standby(PAGE page) {
    return page.status == STANDBY_STATUS;
}

/**
 * #######################################
 * ZEROED PAGES LIST STRUCTS AND FUNCTIONS
 * #######################################
 */

/**
 * Initializes the zeroed frame lists with all of the initial physical memory in the system
 * 
 * Returns a pointer to the zero lists if successful, NULL otherwise
 */
ZEROED_PAGES_LISTS* initialize_zeroed_lists(PAGE* page_storage_base, PULONG_PTR physical_frame_numbers, ULONG64 num_physical_frames) {
    ZEROED_PAGES_LISTS* zeroed_lists = (ZEROED_PAGES_LISTS*) malloc(sizeof(ZEROED_PAGES_LISTS));

    if (zeroed_lists == NULL) {
        fprintf(stderr, "Unable to allocate memory for zeroed_lists");
        return NULL;
    }

    // Create all the individual listheads
    for (int new_list = 0; new_list < NUM_CACHE_SLOTS; new_list++) {
        DB_LL_NODE* new_listhead = db_create_list();

        if (new_listhead == NULL) {
            fprintf(stderr, "Failed to allocate memory for listhead in initialize_zeroed_lists\n");
            return NULL;
        }

        zeroed_lists->listheads[new_list] = new_listhead;
        zeroed_lists->list_lengths[new_list] = 0;

        initialize_lock(&zeroed_lists->list_locks[new_list]);

        EnterCriticalSection(&zeroed_lists->list_locks[new_list]);
        LeaveCriticalSection(&zeroed_lists->list_locks[new_list]);
    }

    // Add all the physical frames to their respective free lists
    for (int pfn_idx = 0; pfn_idx < num_physical_frames; pfn_idx++) {
        ULONG64 frame_number = physical_frame_numbers[pfn_idx];

        // Modulo operation based on the pfn to put it alongside other cache-colliding pages
        int listhead_idx = frame_number % NUM_CACHE_SLOTS;

        DB_LL_NODE* relevant_listhead = zeroed_lists->listheads[listhead_idx];
        
        PAGE* zero_frame = page_storage_base + frame_number;

        if (zero_frame == NULL) {
            fprintf(stderr, "Unable to find the page associated with the pfn in initialize_zeroed_lists\n");
            return NULL;
        }

        // Add the already allocated frame listnode to the free list
        if (db_insert_node_at_head(relevant_listhead, zero_frame->frame_listnode) == ERROR) {
            fprintf(stderr, "Failed to insert zero_frame in its list\n");
            return NULL;
        }

        zero_frame->status = ZERO_STATUS;

        zeroed_lists->list_lengths[listhead_idx]++;
    }

    zeroed_lists->total_available = num_physical_frames;

    return zeroed_lists;
}


/**
 * ##########################
 * FREE FRAMES LIST FUNCTIONS
 * ##########################
 */


/**
 * Creates the free frames list structure and its associated listheads and locks
 * 
 * Returns a memory allocated pointer to a FREE_FRAMES_LISTS struct, or NULL if an error occurs
 */
FREE_FRAMES_LISTS* initialize_free_frames() {
    FREE_FRAMES_LISTS* free_frames = (FREE_FRAMES_LISTS*) malloc(sizeof(FREE_FRAMES_LISTS));

    if (free_frames == NULL) {
        fprintf(stderr, "Unable to allocate memory for free frames lists");
        return NULL;
    }

    // Create all the individual listheads
    for (int new_list = 0; new_list < NUM_CACHE_SLOTS; new_list++) {
        DB_LL_NODE* new_listhead = db_create_list();

        if (new_listhead == NULL) {
            fprintf(stderr, "Failed to allocate memory for listhead in initialize_zeroed_lists\n");
            return NULL;
        }

        free_frames->listheads[new_list] = new_listhead;
        free_frames->list_lengths[new_list] = 0;

        initialize_lock(&free_frames->list_locks[new_list]);

        EnterCriticalSection(&free_frames->list_locks[new_list]);
        LeaveCriticalSection(&free_frames->list_locks[new_list]);
    }

    free_frames->total_available = 0;

    return free_frames;
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

    initialize_lock(&modified_list->lock);

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

    page->status = MODIFIED_STATUS;
    db_insert_node_at_head(modified_list->listhead, page->frame_listnode);
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

    initialize_lock(&standby_list->lock);

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
    
    page->status = STANDBY_STATUS;
    db_insert_node_at_head(standby_list->listhead, page->frame_listnode);
    standby_list->list_length += 1;

    return SUCCESS;
}