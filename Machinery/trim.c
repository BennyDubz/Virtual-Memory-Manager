/**
 * @author Ben Williams
 * @date June 25th, 2024
 * 
 * All functionality for trimming
 */

#include <stdio.h>
#include <assert.h>
#include <windows.h>
#include "../globals.h"
#include "./conversions.h"
#include "./debug_checks.h"
#include "../Datastructures/datastructures.h"
#include "./disk_operations.h"
#include "./trim.h"


/**
 * Thread dedicated to trimming PTEs from the pagetable and putting them on the modified list
 */
ULONG64 trim_count = 0;
LPTHREAD_START_ROUTINE thread_trimming() {
    #if 0
    PAGE* pages_to_trim = (PAGE*) malloc(sizeof(PAGE) * pagetable->num_locks);

    if (pages_to_trim == NULL) {
        fprintf(stderr, "Failed to allocate memory for pages_to_trim in thread_trimming\n");
        return;
    }

    void** pte_vas_to_trim = malloc(sizeof(void*) * pagetable->num_locks);

    if (pte_vas_to_trim == NULL) {
        fprintf(stderr, "Failed to allocate memory for pte_vas_to_trim in thread_trimming\n");
        return;
    }

    ULONG64 num_pages;
    #endif

    ULONG64 ptes_per_lock = pagetable->num_virtual_pages / pagetable->num_locks;
    ULONG64 section_start;
    PTE* curr_pte;
    PTE_LOCKSECTION* curr_pte_locksection;
    ULONG64 curr_pfn;
    PAGE* curr_page = NULL;


    while(TRUE) {
        WaitForSingleObject(trimming_event, INFINITE);
        // Go through each lock section and increment all valid PTEs
        for (ULONG64 lock_section = 0; lock_section < pagetable->num_locks; lock_section++) {            
            section_start = lock_section * ptes_per_lock;

            curr_pte_locksection = &pagetable->pte_locksections[lock_section];

            // Ignore invalid PTE sections  
            if (curr_pte_locksection->valid_pte_count == 0) continue;

            BOOL acquire_pte_lock = TRUE;

            for (ULONG64 pte_idx = section_start; pte_idx < section_start + ptes_per_lock; pte_idx++) {
                /**
                 * We need to acquire the lock at the beginning, and occasionally reacquire it when we
                 * conflict with someone holding the page lock
                 */
                if (acquire_pte_lock) {
                    EnterCriticalSection(&curr_pte_locksection->lock);

                    if (curr_pte_locksection->valid_pte_count == 0) {
                        LeaveCriticalSection(&curr_pte_locksection->lock);
                        break;
                    }

                    acquire_pte_lock = FALSE;
                }


                curr_pte = &pagetable->pte_list[pte_idx];

                assert(curr_pte_locksection == pte_to_locksection(curr_pte));

                // Ignore invalid PTEs
                if (! is_memory_format(*curr_pte)) {
                    continue;
                }

                curr_pfn = curr_pte->memory_format.frame_number;
                curr_page = pfn_to_page(curr_pfn);

                /**
                 * If someone else has the pagelock, then we need to back up - they are
                 * likely at the final stage of resolving a fault and we need to give up the lock
                 * so that they can finish. We may still be able to trim the page after, however,
                 * so we decrement the pte_index again.
                 * 
                 * Later, once the age of a PTE is a factor in the trimming, this will be less of a concern
                 * (and may actually save some hot pages from being trimmed last-minute) 
                 * 
                 */
                if (try_acquire_pagelock(curr_page) == FALSE) {
                    LeaveCriticalSection(&curr_pte_locksection->lock);
                    acquire_pte_lock = TRUE;
                    pte_idx--;
                    continue;
                }

                #ifdef DEBUG_CHECKING
                if (pte_valid_count_check(&pagetable->pte_list[section_start]) == FALSE) {
                    DebugBreak();
                }
                #endif

                // Unmaps from CPU and decrements its valid_pte_count section
                //BW: Later can have a list of PTEs to pass to this to unmap several at once

                // disconnect_pte_from_cpu(curr_pte);


                //BW: Can maybe leave pte lock here in the meantime?  
                //      - as of now, no, as we have already set up the transition PTE  
                PTE transition_pte_contents;
                transition_pte_contents.complete_format = 0;
                transition_pte_contents.transition_format.always_zero = 0;
                transition_pte_contents.transition_format.frame_number = curr_pfn;
                transition_pte_contents.transition_format.is_transition = 1;


                write_pte_contents(curr_pte, transition_pte_contents);
                disconnect_pte_from_cpu(curr_pte);

                LeaveCriticalSection(&curr_pte_locksection->lock);

                #ifdef DEBUG_CHECKING
                int dbg_result;
                if ((dbg_result = page_is_isolated(curr_page)) != ISOLATED) {
                    DebugBreak();
                }
                #endif

                
                /**
                 * Somehow, we have a race condition where we held the modified listlock?
                 */
                EnterCriticalSection(&modified_list->lock);  
      

                #ifdef DEBUG_CHECKING
                if (pfn_is_single_allocated(page_to_pfn(curr_page)) == FALSE) {
                    DebugBreak();
                }
                #endif

                modified_add_page(curr_page, modified_list);
                release_pagelock(curr_page);

                LeaveCriticalSection(&modified_list->lock);
                
                break;
            }
            
        }

        // Signal that the modified list should be populated
        trim_count++;
        SetEvent(modified_to_standby_event);
    }
}


/**
 * Thread dedicated to aging all of the valid PTEs in the pagetable
 */
LPTHREAD_START_ROUTINE thread_aging() {
    ULONG64 ptes_per_lock = pagetable->num_virtual_pages / pagetable->num_locks;
    ULONG64 section_start;
    PTE_LOCKSECTION* curr_pte_locksection;

    while(TRUE) {
        WaitForSingleObject(aging_event, INFINITE);
        
        // Go through each lock section and increment all valid PTEs
        for (ULONG64 lock_section = 0; lock_section < pagetable->num_locks; lock_section++) {
            section_start = lock_section * ptes_per_lock;

            curr_pte_locksection = &pagetable->pte_locksections[lock_section];

            EnterCriticalSection(&curr_pte_locksection->lock);
            for (ULONG64 pte_idx = section_start; pte_idx < section_start + ptes_per_lock; pte_idx++) {

                // Ignore invalid PTEs
                if (! is_memory_format(pagetable->pte_list[pte_idx])) continue;

                // Don't allow the age to wrap
                if (pagetable->pte_list[pte_idx].memory_format.age == MAX_AGE) {
                    continue;
                } else {
                    pagetable->pte_list[pte_idx].memory_format.age++;
                }
            }
            LeaveCriticalSection(&curr_pte_locksection->lock);
        }
        // printf("Successfully aged\n");
    }
}


/**
 * Thread dedicated to writing pages from the modified list to disk, putting finally adding the pages to standby
 */
ULONG64 sb_count = 0;

LPTHREAD_START_ROUTINE thread_modified_to_standby() {
    // Variables dedicated to finding and extracting pages to write from the modified list 
    ULONG64 section_start;
    ULONG64 num_to_write;
    CRITICAL_SECTION* pte_lock;
    BOOL pte_lock_status;
    PAGE* potential_page;
    // PAGE** pages_currently_writing[MAX_PAGES_WRITABLE];
    PTE* relevant_PTE;
    ULONG64 disk_storage_idx;

    DISK_BATCH disk_batch;    
    

    while(TRUE) {
        WaitForSingleObject(modified_to_standby_event, INFINITE);

        /**
         * We need to have a volatile read here as even using the min() function or the
         * new if statement below can be inaccurate if the modified list length changes from a different thread
         * 
         * This can lead to a race condition where we have a TOCTOU issue with the modified list length, where it increases
         * and we end up writing more than MAX_PAGES_WRITABLE pages into the diskbatch - overrunning and corrupting the stack
         */
        ULONG64 curr_mod_list_length = *(volatile ULONG64*) &modified_list->list_length;

        if (MAX_PAGES_WRITABLE < curr_mod_list_length) {
            num_to_write = MAX_PAGES_WRITABLE;
        } else {
            num_to_write = curr_mod_list_length;
        }

        ULONG64 curr_attempts = 0;
        ULONG64 curr_page_num = 0;
        BOOL acquire_mod_lock = TRUE;
        disk_batch.num_pages = 0;
        disk_batch.write_complete = FALSE;

        while (curr_attempts < num_to_write) {
            if (acquire_mod_lock) EnterCriticalSection(&modified_list->lock);

            acquire_mod_lock = FALSE;

            // We pop from the tail, so we have to acquire the tail pagelock first
            potential_page = (PAGE*) modified_list->listhead->blink->item;

            // The list is empty
            if (potential_page == NULL) break;

            // The rare case where the page we want to pop has its pagelock held. We surrender the modified listlock
            if (try_acquire_pagelock(potential_page) == FALSE) {
                LeaveCriticalSection(&modified_list->lock);
                acquire_mod_lock = TRUE;
                curr_attempts++;
                continue;
            }

            // We now remove the page from the modified list
            modified_pop_page(modified_list);

            /**
             * Addressing a race condition where a page we had previously popped
             * from this loop was rescued and trimmed again - and is now about to be added again
             * 
             * We work to prevent the double-add to the list while still writing it to disk
             */
            if (potential_page->in_disk_batch == PAGE_NOT_IN_DISK_BATCH) {
                disk_batch.pages_being_written[curr_page_num] = potential_page;
                disk_batch.num_pages++;
                curr_page_num++;
            }

            potential_page->writing_to_disk = PAGE_BEING_WRITTEN;
            potential_page->in_disk_batch = PAGE_IN_DISK_BATCH;

            release_pagelock(potential_page);

            curr_attempts++;
        }  

        if (acquire_mod_lock == FALSE) {
            LeaveCriticalSection(&modified_list->lock);
        }

        // We did not get any pages
        if (curr_page_num == 0) continue;

        ULONG64 num_pages_written = write_batch_to_disk(&disk_batch);

        /**
         * We should expect to be able to write all pages, but we may want to be able to handle cases
         * where we fail to acquire enough disk slots for all of these pages in the future
         * 
         * As of now, the bad scenario should not be possible - so we leave a DebugBreak()
         */
        if (num_pages_written != disk_batch.num_pages) {
            DebugBreak();
        }

        /**
         * Acquire pagelocks ahead of time, then get the standby list lock
         */
        for (int i = 0; i < disk_batch.num_pages; i++) {
            PAGE* curr_page = disk_batch.pages_being_written[i];

            acquire_pagelock(curr_page);


            if (curr_page->status != MODIFIED_STATUS || 
                    curr_page->writing_to_disk != PAGE_BEING_WRITTEN) {
                
                curr_page->in_disk_batch = PAGE_NOT_IN_DISK_BATCH;
                release_pagelock(curr_page);

                disk_batch.pages_being_written[i] = NULL;

                //BW: We can consider batching these up later
                release_single_disk_slot(disk_batch.disk_indices[i]);
                continue;
            }

        }

        EnterCriticalSection(&standby_list->lock);

        for (int i = 0; i < disk_batch.num_pages; i++) {
            PAGE* curr_page = disk_batch.pages_being_written[i];
            // If someone else has the pagelock, they must be trying to rescue it right now!
            if (curr_page == NULL) continue;

            // Now, the page can be modified and added to the standby list
            curr_page->pagefile_idx = disk_batch.disk_indices[i];
            curr_page->writing_to_disk = PAGE_NOT_BEING_WRITTEN;
            curr_page->in_disk_batch = PAGE_NOT_IN_DISK_BATCH;

            if (standby_add_page(curr_page, standby_list) == ERROR) {
                DebugBreak();
            }

            InterlockedIncrement64(&total_available_pages);
        }

        LeaveCriticalSection(&standby_list->lock);

        /**
         * Release all of the pagelocks that we hold
         */
        for (int i = 0; i < disk_batch.num_pages; i++) {
            PAGE* curr_page = disk_batch.pages_being_written[i];
            if (curr_page != NULL) {
                release_pagelock(curr_page);
            }
        }


        #if 0
        ULONG64 curr_page = 0;        
        while (curr_page < num_to_write) {
            EnterCriticalSection(&modified_list->lock);

            // From this line onward until it is added to the standby list, this page cannot be rescued
            potential_page = (PAGE*) modified_pop_page(modified_list);

            // Since we release the modified list lock during this, someone else could come in and rescue pages from the
            // modified list, emptying it, meaning the work here is done until we are signaled again
            if (potential_page == NULL) {
                LeaveCriticalSection(&modified_list->lock);
                break;
            }

            /**
             * Not currently feasible as there must be some way to tell whether the page has been rescued in an atomic
             * operation once we have acquired the standby lock. In the meantime, we will still hold onto the modified lock
             * and be forced to make rescues fail while their pages are between the modified and standby lists
             */
            #if 0 
            // Now that we have popped the page from the modified list, we no longer need the lock as we are not
            // modifying the datastructure. That makes
            LeaveCriticalSection(&modified_list->lock);

           
            if (write_page_to_disk(potential_page, &disk_storage_idx) == ERROR) {
                EnterCriticalSection(&modified_list->lock);

                /**
                 * We must check that the page is still in modified status. If it is active (or anything else),
                 * that means that it has been rescued while we tried to write it to disk. If that is the case,
                 * we cannot re-add it to the modified list.
                 */
                if (potential_page->status == MODIFIED_STATUS) {
                    /**
                     * We must also protect ourselves against the scenario where the page was rescued, trimmed, and
                     * added back to the modified list before we were able to acquire the lock. In that case,
                     * we do not want to add it to the list again as we would have a duplicate.
                     */
                    if (potential_page->frame_listnode == NULL) {
                        modified_add_page(potential_page, modified_list);
                    }
                }
                
                // We will need to wait for more disk slots to be open in order to continue writing to the disk
                LeaveCriticalSection(&modified_list->lock);
                WaitForSingleObject(disk_open_slots_event, INFINITE);

                // We will still want to keep looking
                curr_page--;

                continue;      
            }
            #endif

            if (write_page_to_disk(potential_page, &disk_storage_idx) == ERROR) {
                
                // We will want to try again with this page later
                modified_add_page(potential_page, modified_list);
                
                // We will need to wait for more disk slots to be open in order to continue writing to the disk
                LeaveCriticalSection(&modified_list->lock);
                WaitForSingleObject(disk_open_slots_event, INFINITE);

                // We purposefully do not increment curr_page here so that we still try to write pages to disk
                continue;      
            }

            /**
             * re-acquire page lock
             * Ensure that it is still inactive, if it isn't, then
             */
            LeaveCriticalSection(&modified_list->lock);

            #ifdef DEBUG_CHECKING
            int dbg_result;
            if ((dbg_result = page_is_isolated(pfn_to_page(curr_page))) != ISOLATED) {
                DebugBreak();
            }
            #endif

            // Remember, as of now the page CANNOT be rescued in this interim
            EnterCriticalSection(&standby_list->lock);

            potential_page->pagefile_idx = disk_storage_idx;

            standby_add_page(potential_page, standby_list);
            InterlockedIncrement64(&total_available_pages);

            // SetEvent(waiting_for_pages_event);

            LeaveCriticalSection(&standby_list->lock);

            curr_page++;
        }
        #endif
        SetEvent(waiting_for_pages_event);

        sb_count++;
    }
    
}




/**
 * Connects the given PTE to the open page's physical frame and alerts the CPU
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 */
int connect_pte_to_page(PTE* pte, PAGE* open_page) {
    if (pte == NULL) {
        fprintf(stderr, "NULL PTE given to connect_pte_to_page\n");
        return ERROR;
    }
    
    PTE_LOCKSECTION* pte_locksection = pte_to_locksection(pte);

    ULONG64 pfn = page_to_pfn(open_page);
    
    #ifdef DEBUG_CHECKING
    if (pte_valid_count_check(pte) == FALSE) {
        DebugBreak();
    }
    #endif

    pte_locksection->valid_pte_count++;

    // Map the CPU
    if (MapUserPhysicalPages (pte_to_va(pte), 1, &pfn) == FALSE) {

        fprintf (stderr, "connect_pte_to_page : could not map VA %p to pfn %llx\n", pte_to_va(pte), pfn);
        DebugBreak();

        return ERROR;
    }

    if (is_memory_format(*pte)) {
        printf("MEM FORMAT PTE\n");
    }

    PTE pte_contents;
    pte_contents.complete_format = 0;
    pte_contents.memory_format.age = 0;
    pte_contents.memory_format.frame_number = pfn;
    pte_contents.memory_format.valid = VALID;

    write_pte_contents(pte, pte_contents);

    #ifdef DEBUG_CHECKING
    if (pte_valid_count_check(pte) == FALSE) {
        DebugBreak();
    }
    #endif

    return SUCCESS;
}


/**
 * Disconnects the PTE from the CPU, but **does not** change the PTE structure
 * as this may be used for both disk and transition format PTEs
 */
int disconnect_pte_from_cpu(PTE* pte) {
    if (pte == NULL) {
        fprintf(stderr, "NULL pte given to disconnect_pte_from_cpu\n");
        return ERROR;
    }

    PTE_LOCKSECTION* pte_locksection = pte_to_locksection(pte);

    pte_locksection->valid_pte_count --;

    // if (pte_valid_count_check(pte) == FALSE) {
    //     DebugBreak();
    // }

    #ifdef DEBUG_CHECKING
    if (pte_valid_count_check(pte) == FALSE) {
        DebugBreak();
    }
    #endif

    // Unmap the CPU
    if (MapUserPhysicalPages (pte_to_va(pte), 1, NULL) == FALSE) {

        fprintf (stderr, "full_virtual_memory_test : could not unmap VA %p\n", pte_to_va(pte));

        DebugBreak();
        return ERROR;
    }

    return SUCCESS;
}


/**
 * Disconnects all of the given ptes from the CPU.
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 */
int disconnect_va_batch_from_cpu(void** virtual_addresses, ULONG64 num_ptes) {
    if (virtual_addresses == NULL || num_ptes == 0) {
        fprintf(stderr, "Bad virtual addresses or 0 num_pte's given to disconnect_va_batch_from_cpu\n");
        return ERROR;
    }

    // This allows us to unmap the
    if (MapUserPhysicalPagesScatter(virtual_addresses, num_ptes, NULL) == FALSE) {
        fprintf(stderr, "Failed to unmap PTE virtual addresses in disconnect_va_batch_from_cpu\n");
        DebugBreak();
        return ERROR;
    }

    return SUCCESS;
}


/**
 * Spins until the pagelock for the given page can be acquired and returns
 */
void acquire_pagelock(PAGE* page) {

    #if DEBUG_PAGELOCK
    EnterCriticalSection(&page->dev_page_lock);
    log_page_status(page);
    page->page_lock = PAGE_LOCKED;
    page->holding_threadid = GetCurrentThreadId();
    return;
    #endif

    unsigned old_lock_status;
    while(TRUE) {
        old_lock_status = InterlockedCompareExchange(&page->page_lock, PAGE_LOCKED, PAGE_UNLOCKED);
        if (old_lock_status == PAGE_UNLOCKED) break;
    }
}


/**
 * Releases the pagelock for other threads to use
 */
void release_pagelock(PAGE* page) {
    
    #if DEBUG_PAGELOCK
    log_page_status(page);
    if (page->holding_threadid != GetCurrentThreadId()) {
        DebugBreak();
    }
    page->page_lock = PAGE_UNLOCKED;
    page->holding_threadid = 0;
    LeaveCriticalSection(&page->dev_page_lock);
    return;
    #endif

    if (InterlockedCompareExchange(&page->page_lock, PAGE_UNLOCKED, PAGE_LOCKED) != PAGE_LOCKED) {
        DebugBreak();
    };
}


/**
 * Tries to acquire the pagelock without any spinning. 
 * 
 * Returns TRUE if successful, FALSE otherwise
 */
BOOL try_acquire_pagelock(PAGE* page) {
    #if DEBUG_PAGELOCK
    if (TryEnterCriticalSection(&page->dev_page_lock)) {
        log_page_status(page);
        page->page_lock = PAGE_LOCKED;
        page->holding_threadid = GetCurrentThreadId();
        return TRUE;
    } else {
        return FALSE;
    }
    #endif
    return InterlockedCompareExchange(&page->page_lock, PAGE_LOCKED, PAGE_UNLOCKED) == PAGE_UNLOCKED;

}
