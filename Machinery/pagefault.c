/**
 * @author Ben Williams
 * @date June 25th, 2024
 * 
 * All functions for handling the page fault at the highest level
 */

#include <stdio.h>
#include "../globals.h"
#include "../macros.h"
#include "./trim.h"
#include "./conversions.h"
#include "../Datastructures/datastructures.h"

/**
 * GLOBALS FOR PAGE FAULTING
 */


ULONG64 fault_count = 0;

/**
 * Function declarations
 */
static PAGE* rescue_pte(PTE pte);

static PAGE* handle_unaccessed_pte_fault(PTE local_pte);

static PAGE* handle_transition_pte_fault(PTE local_pte);

static PAGE* handle_disk_pte_fault(PTE local_pte);

static PAGE* find_available_page();

static int standby_rescue_page(PAGE* rescue_page);

static int modified_rescue_page(PAGE* rescue_page);

static PAGE* standby_pop_page();

static void free_frames_add(PAGE* page);

# if 0
/**
 * Handles the page fault for the given virtual address
 * 
 * Returns SUCCESS if the faulting instruction can be tried again, ERROR otherwise
 * 
 */
int pagefault(PULONG_PTR virtual_address) {
    fault_count++;
    if (fault_count % 4096 == 0) {
        printf("Curr fault count %llX\n", fault_count);
    }
    // printf("Curr fault count %lld\n", fault_count);

    /**
     * Temporary ways to induce trimming and aging
     */
    if ((total_available_pages < (physical_page_count / 2)) && (fault_count % 16) == 0) {
        SetEvent(aging_event);
    }
    
    if ((total_available_pages < (physical_page_count / 3) && (fault_count % 4) == 0)) {
        SetEvent(trimming_event);
    }


    PTE* accessed_pte = va_to_pte(virtual_address);

    if (accessed_pte == NULL) {
        fprintf(stderr, "Unable to find the accessed PTE\n");
        return ERROR;
    }

    CRITICAL_SECTION* pte_lock = pte_to_lock(accessed_pte);
    BOOL disk_flag = FALSE;
    BOOL rescue_flag = FALSE;
    BOOL resolved_format_status = FALSE;
    PAGE* rescue_page;

    // Much shorter lock hold than using the accessed PTE for all of the other if statements
    EnterCriticalSection(pte_lock);
    PTE local_pte = *accessed_pte;

    if (is_used_pte(local_pte)) {

        PTE original_accessed_pte;
        if (! is_memory_format(local_pte)) {
            while (resolved_format_status == FALSE) {
                rescue_flag = FALSE;
                disk_flag = FALSE;

                local_pte = *accessed_pte;

                LeaveCriticalSection(pte_lock);
                // Check the PTE to decide what our course of action is

                // Memory format - we don't do anything (we could be in this scenario if )
                if (is_disk_format(local_pte)) {
                    disk_flag = TRUE;
                } else if (is_transition_format(local_pte)) {
                    if ((rescue_page = rescue_pte(local_pte)) != NULL) {
                        rescue_flag = TRUE;
                        total_available_pages--;
                    }       
                    // The standby or modified page was stolen or rescued from another thread from beneath us

                }

                EnterCriticalSection(pte_lock);

                // If the accessed PTE hasn't changed - we might not need to look again.
                if (ptes_are_equal(local_pte, *accessed_pte)) {
                    resolved_format_status = TRUE;

                    if (rescue_flag) {
                        local_pte.memory_format.valid = VALID;
                        local_pte.memory_format.age = 0;
                        local_pte.memory_format.frame_number = page_to_pfn(rescue_page);

                        *accessed_pte = local_pte;

                        connect_pte_to_pfn(accessed_pte, page_to_pfn(rescue_page));

                        // printf("Successful rescue\n");
                        LeaveCriticalSection(pte_lock);
                        
                        return SUCCESS;
                    } else if (disk_flag == FALSE) {
                        // What to do if the other thread rescued the page before this?
                        LeaveCriticalSection(pte_lock);
                        return ERROR;
                    }

                } else {
                    // If the accessed PTE has changed from under us - we want to redo the page fault

                    // Note that it is not possible for the page to have been rescued and for the PTE to change -
                    // if that is the case then we have had some race conditions on the standby list where it was given to someone else
                    if (rescue_flag) {
                        DebugBreak();
                    }

                    LeaveCriticalSection(pte_lock);
                    return ERROR;
                }
                
            }

        } 
        
        if (is_memory_format(local_pte)) {
            // We are in the memory format - we can return SUCCESS
            LeaveCriticalSection(pte_lock);
            printf("Memory format pte seen\n");
            return SUCCESS;
        }
    }

    #if 0 
    if (is_used_pte(local_pte)) {

        // Check the PTE to decide what our course of action is
        if (is_memory_format(local_pte)) {
            LeaveCriticalSection(pte_lock);
            // Skip to the next random access, another thread has already validated this address
            return SUCCESS;
        } else if (is_disk_format(local_pte)) {
            disk_flag = TRUE;
        } else if (is_transition_format(local_pte)) {
            PAGE* rescue_page;
            printf("rescuing pte\n");
            if (rescue_pte(&local_pte) == ERROR) {
                LeaveCriticalSection(pte_lock);
                return ERROR;
            } else {
                *accessed_pte = local_pte;
                LeaveCriticalSection(pte_lock);
                total_available_pages--;
                return SUCCESS;
            }              
        }

        }
    }
    #endif

    /**
     * By the time we get here, the PTE has never been accessed before,
     * so we just need to find a frame to allocate to it
     */            
    
    //BW: Might have deadlock if we have a lock for free frames list
    PAGE* new_page = allocate_free_frame(free_frames);

    // Then, check standby
    ULONG64 pfn;
    if (new_page == NULL) {
        
        // BW: Note that a process in this spot will never be able to access pages that get on free list!
        PAGE* standby_page;
        EnterCriticalSection(&standby_list->lock);
        
        if ((standby_page = standby_pop_page(standby_list)) == NULL) {
            LeaveCriticalSection(&standby_list->lock);

            LeaveCriticalSection(pte_lock);

            SetEvent(trimming_event);

            // printf("Waiting for pages\n");
            WaitForSingleObject(waiting_for_pages_event, INFINITE);

            return ERROR;
        }

        // Right now, we fix the other PTE here - but this should be temporary
        PTE* standby_page_old_pte = standby_page->standby_page.pte;

        /**
         * Introduces a race condition if another thread is trying to rescue the
         * a PTE in the same lock section at the same time
         * 
         * Also introduces the case of two PTEs faulting and waiting here
         * 
         */
        if (accessed_pte < standby_page_old_pte) {
            BOOL pte_lock_result;
            pte_lock_result = TryEnterCriticalSection(pte_to_lock(standby_page_old_pte));

            if (pte_lock_result == FALSE) {
                standby_add_page(standby_page, standby_list);
                LeaveCriticalSection(&standby_list->lock);
                LeaveCriticalSection(pte_lock);
                return ERROR;
            }
        } else {
            EnterCriticalSection(pte_to_lock(standby_page_old_pte));
        }

        standby_page_old_pte->disk_format.on_disk = 1;
        standby_page_old_pte->disk_format.pagefile_idx = standby_page->standby_page.pagefile_idx;
        LeaveCriticalSection(pte_to_lock(standby_page_old_pte));

        LeaveCriticalSection(&standby_list->lock);

        pfn = page_to_pfn(standby_page);

    } else {
        pfn = new_page->free_page.frame_number;
        if (pfn == 0) {
            fprintf(stderr, "Invalid pfn from free page\n");
            LeaveCriticalSection(pte_lock);
            return ERROR;
        }
    }

    total_available_pages--;

    // // Allocate the physical frame to the virtual address
    // if (MapUserPhysicalPages (virtual_address, 1, &pfn) == FALSE) {

    //     printf ("full_virtual_memory_test : could not map VA %p to page %llX\n", virtual_address, pfn);
    //     LeaveCriticalSection(pte_lock);
    //     return ERROR;
    // }

    if (disk_flag) {
        // We need to use the accessed pte to have the correct address for pte_to_va
        if (read_from_disk(pfn, local_pte.disk_format.pagefile_idx) == ERROR) {
            fprintf(stderr, "Failed to read from disk into pte\n");
            LeaveCriticalSection(pte_lock);
            return ERROR;
        }
    }

    local_pte.memory_format.age = 0;
    local_pte.memory_format.frame_number = pfn;
    local_pte.memory_format.valid = VALID;

    *accessed_pte = local_pte;

    connect_pte_to_pfn(accessed_pte, pfn);

    LeaveCriticalSection(pte_lock);

    return SUCCESS;


    #if 0
    //
    // Unmap the virtual address translation we installed above
    // now that we're done writing our value into it.
    //

    // 
    if (MapUserPhysicalPages (arbitrary_va, 1, NULL) == FALSE) {

        printf ("full_virtual_memory_test : could not unmap VA %p\n", arbitrary_va);

        return;
    }
    #endif
}
#endif


/**
 * Handles the page fault for the given virtual address
 * 
 * Returns SUCCESS if the access can be attempted again, ERROR if
 * the fault must either be re-attempted or if the virtual address's corresponding
 * PTE was modified by another thread in the meantime
= * 
 */
ULONG64 wait_count = 0;
int pagefault(PULONG_PTR virtual_address) {
    fault_count++;
    if (fault_count % 4096 == 0) {
        printf("Curr fault count %llX\n", fault_count);
    }

    /**
     * Temporary ways to induce trimming and aging
     */
    // if ((total_available_pages < (physical_page_count / 2)) && (fault_count % 16) == 0) {
    //     SetEvent(aging_event);
    // }
    
    // if ((total_available_pages < (physical_page_count / 3) && (fault_count % 4) == 0)) {
    //     SetEvent(trimming_event);
    // }


    PTE* accessed_pte = va_to_pte(virtual_address);

    if (accessed_pte == NULL) {
        fprintf(stderr, "Unable to find the accessed PTE\n");
        return ERROR;
    }

    // Make a copy of the PTE in order to perform the fault
    PTE local_pte = *accessed_pte;
    PAGE* allocated_page;

    if (is_memory_format(local_pte)) {
        return SUCCESS;
    } else if (is_used_pte(local_pte) == FALSE) {
        allocated_page = handle_unaccessed_pte_fault(local_pte);
    } else if (is_transition_format(local_pte)) {
        allocated_page = handle_transition_pte_fault(local_pte);
    } else if (is_disk_format(local_pte)) {
        allocated_page = handle_disk_pte_fault(local_pte);
    }

    // For whatever reason the relevant handler failed, so we should retry the fault
    if (allocated_page == NULL) {
        return ERROR;
    }

    CRITICAL_SECTION* pte_lock = pte_to_lock(accessed_pte);

    EnterCriticalSection(pte_lock);
    if (ptes_are_equal(local_pte, *accessed_pte) == FALSE) {

        /**
         * Return the page to the free-list and return ERROR
         * 
         * BW: Later, we need to zero-out pages that were acquired in the failure case
         *  in another thread and re-add them to the free-list
         */ 

        free_frames_add(allocated_page);
        LeaveCriticalSection(pte_lock);
        return ERROR;
    }

    // Connect the PTE to the pfn through the CPU 
    connect_pte_to_pfn(accessed_pte, page_to_pfn(allocated_page));

    LeaveCriticalSection(pte_lock);

    return SUCCESS;
}   


/**
 * Handles a pagefault for a PTE that has never been accessed before
 * 
 * Returns a pointer to the page that is allocated to the PTE, or NULL if it fails
 */
static PAGE* handle_unaccessed_pte_fault(PTE local_pte) {

    /**
     * In this case, all we need to do is get a valid page and return it - nothing else
     */
    return find_available_page();
}


/**
 * Handles a pagefault for a PTE that is in transition format - it is either
 * in the modified or standby list
 * 
 * Returns a pointer to the rescued page, or NULL if it could not be rescued
 */
static PAGE* handle_transition_pte_fault(PTE local_pte) {
    return rescue_pte(local_pte);
}


/**
 * Handles a pagefault for a PTE that is in the disk format - it's contents
 * must be fetched from the disk
 * 
 * Returns a pointer to the page with the restored contents on it
 */
static PAGE* handle_disk_pte_fault(PTE local_pte) {
    PAGE* allocated_page = find_available_page();

    if (allocated_page == NULL) {
        return NULL;
    }

    ULONG64 disk_idx = local_pte.disk_format.pagefile_idx;
    
    // Another thread could have rescued this frame ahead of us
    if (read_from_disk(allocated_page, disk_idx) == ERROR) {
        // BW: Here, we would want to add it to the zero-out thread!
        free_frames_add(allocated_page);
        return NULL;
    }

    return allocated_page;
}


/**
 * Finds an available page from either the free or standby list and returns it
 * 
 * Returns NULL if there were no pages available at the time
 */
static PAGE* find_available_page() {

    PAGE* allocated_page;

    // If we succeed on the free list, we don't have to do anything else
    if ((allocated_page = allocate_free_frame(free_frames)) != NULL) {
        total_available_pages--;
        return allocated_page;
    }

    // Now we have to try on the standby list (locks are acquired inside standby_pop_page)
    if ((allocated_page = standby_pop_page(standby_list)) == NULL) {

        SetEvent(trimming_event);
        
        wait_count++;
        WaitForSingleObject(waiting_for_pages_event, INFINITE);

        return NULL;
    }

    total_available_pages--;
    return allocated_page;
}


/**
 * For the given PTE, find its associated page in the modified or standby list
 * and return it.
 * 
 * Returns a pointer to the popped page upon success, NULL if it could not be found
 */
static PAGE* rescue_pte(PTE pte) {

    ULONG64 pfn = pte.transition_format.frame_number;

    PAGE* rescue_page = pfn_to_page(pfn);
    
    /**
     * Here, we are exposed to a possible race condition where we could rescue a modified page right
     * when it is being added to standby. Therefore, we must also try the standby list right after
     * sequentially. If we succeeded, the page will not be in the standby format.
     */
    if (page_is_modified(*rescue_page)) {
        if (modified_rescue_page(rescue_page) == SUCCESS) {
            return rescue_page;
        }
    } 
    
    /**
     * Again, we could have a race condition where we try to get the page from the standby list
     * just as it is given to someone else. So if it fails, we know that this PTE was unlucky
     * and will have to get a new page entirely.
     */
    if (page_is_standby(*rescue_page)){
        if (standby_rescue_page(rescue_page) == SUCCESS) {
            return rescue_page;
        }
    }

    /**
     * At this point, the page was either rescued by another thread or was recycled and given to someone
     * else. Regardless, the page fault must be re-attempted as this thread's rescue operation failed
     */
    return NULL;
}


/**
 * Rescues the given page from the modified list, if it can be found
 * 
 * Returns SUCCESS if the rescue page was found and removed, ERROR otherwise
 */
static int modified_rescue_page(PAGE* rescue_page) {
    if (rescue_page == NULL) {
        fprintf(stderr, "NULL modified list or rescue_page given to modified_rescue_page\n");
    }
    
    EnterCriticalSection(&modified_list->lock);

    if (rescue_page->modified_page.status != MODIFIED_STATUS) {
        LeaveCriticalSection(&modified_list->lock);
        return ERROR;
    }

    if (db_remove_from_middle(rescue_page->modified_page.frame_listnode) == ERROR) {
        LeaveCriticalSection(&modified_list->lock);
        return ERROR;
    }

    rescue_page->active_page.status = ACTIVE_STATUS;

    // DB_LL_NODE* curr_node = modified_list->listhead->flink;
    // while (curr_node != modified_list->listhead) {
        
    //     if (((PAGE*) curr_node->item) == rescue_page) {
    //         db_remove_from_middle(curr_node);
    //         rescue_page->modified_page.modified_again = 1;

    //         modified_list->list_length--;
    //         LeaveCriticalSection(&modified_list->lock);
    //         return SUCCESS;
    //     }
    //     curr_node = curr_node->flink;
    // }

    LeaveCriticalSection(&modified_list->lock);

    return SUCCESS;
}


/**
 * Rescues the given page from the standby list, if it can be found
 * 
 * Returns SUCCESS if the rescue page was found and removed, ERROR otherwise
 */
static int standby_rescue_page(PAGE* rescue_page) {
    if (standby_list == NULL || rescue_page == NULL) {
        fprintf(stderr, "NULL standby list or rescue page given to standby_rescue_page\n");
    }

    EnterCriticalSection(&standby_list->lock);

    if (rescue_page->standby_page.status != STANDBY_STATUS) {
        LeaveCriticalSection(&standby_list->lock);
        return ERROR;
    }

    db_remove_from_middle(rescue_page->standby_page.frame_listnode);

    rescue_page->active_page.status = ACTIVE_STATUS;

    total_available_pages--;

    // DB_LL_NODE* curr_node = standby_list->listhead->flink;
    // while (curr_node != standby_list->listhead) {
        
    //     if (((PAGE*) curr_node->item) == rescue_page) {
    //         db_remove_from_middle(curr_node);

    //         if (return_disk_slot(rescue_page->standby_page.pagefile_idx) == ERROR) {
    //             fprintf(stderr, "Failed to return disk slot in standby_rescue_page\n");
    //             DebugBreak();                
    //         }

    //         // We have made a disk slot available, and should notify any threads waiting
    //         SetEvent(disk_open_slots_event);

    //         standby_list->list_length--;
    //         LeaveCriticalSection(&standby_list->lock);
    //         return SUCCESS;
    //     }
    //     curr_node = curr_node->flink;
    // }
    LeaveCriticalSection(&standby_list->lock);

    return SUCCESS;
}


/**
 * Pops and returns a pointer to the oldest page from the standby list and returns it, 
 * and modifies its old PTE to be in the disk format.
 * 
 * Returns NULL upon any error or if the list is empty
 */
static PAGE* standby_pop_page() {

    EnterCriticalSection(&standby_list->lock);

    PAGE* popped = db_pop_from_tail(standby_list->listhead);
    if (popped == NULL) {
        LeaveCriticalSection(&standby_list->lock);
        return NULL;
    }

    standby_list->list_length -= 1;
    PTE* old_pte = popped->standby_page.pte;

    // We must modify the other PTE to reflect that it is on the disk
    EnterCriticalSection(pte_to_lock(old_pte));
    old_pte->disk_format.on_disk = 1;
    old_pte->disk_format.pagefile_idx = popped->standby_page.pagefile_idx;

    // This ensures that any PTE attempting to rescue this page when it acquires the standby
    // lock will fail
    popped->active_page.status = ACTIVE_STATUS;

    LeaveCriticalSection(pte_to_lock(old_pte));

    total_available_pages--;

    LeaveCriticalSection(&standby_list->lock);

    return popped;
}   


/**
 * Adds the given page to its proper slot in the free list
 * 
 * As of now, does NOT zero out the page!
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 */
static void free_frames_add(PAGE* page) {
    // Modulo operation based on the pfn to put it alongside other cache-colliding pages
    int listhead_idx = page_to_pfn(page) % NUM_FRAME_LISTS;

    DB_LL_NODE* relevant_listhead = free_frames->listheads[listhead_idx];

    EnterCriticalSection(&free_frames->list_locks[listhead_idx]);

    page->free_page.frame_number = page_to_pfn(page);
    page->free_page.status = FREE_STATUS;
    page->free_page.zeroed_out = 0; // Until we are actually zeroing out frames

    db_insert_node_at_head(relevant_listhead, page->free_page.frame_listnode);
    free_frames->list_lengths[listhead_idx] += 1;
    total_available_pages++;

    LeaveCriticalSection(&free_frames->list_locks[listhead_idx]);

    SetEvent(waiting_for_pages_event);
}