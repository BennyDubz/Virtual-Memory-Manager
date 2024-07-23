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
#include "./pagelist_operations.h"
#include "../Datastructures/datastructures.h"
#include "./disk_operations.h"
#include "./trim.h"


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
 * Thread dedicated to trimming PTEs from the pagetable and putting them on the modified list
 */
#define TRIM_PER_SECTION 4
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
    ULONG64 trimmed_in_section;
    ULONG64 total_trimmed = 0;

    PTE* pte_section_trim[TRIM_PER_SECTION];
    PAGE* page_section_trim[TRIM_PER_SECTION];
    PULONG_PTR trim_addresses[TRIM_PER_SECTION];


    while(TRUE) {
        WaitForSingleObject(trimming_event, INFINITE);

        // Go through each lock section and increment all valid PTEs
        for (ULONG64 lock_section = 0; lock_section < pagetable->num_locks; lock_section++) {            
            section_start = lock_section * ptes_per_lock;

            curr_pte_locksection = &pagetable->pte_locksections[lock_section];

            // Ignore invalid PTE sections  
            if (curr_pte_locksection->valid_pte_count == 0) continue;

            BOOL acquire_pte_lock = TRUE;

            trimmed_in_section = 0;


            for (ULONG64 pte_idx = section_start; pte_idx < section_start + ptes_per_lock; pte_idx++) {
                /**
                 * We need to acquire the lock at the beginning, and occasionally reacquire it when we
                 * conflict with someone holding the page lock
                 */
                if (acquire_pte_lock) {
                    EnterCriticalSection(&curr_pte_locksection->lock);

                    if (curr_pte_locksection->valid_pte_count == trimmed_in_section) {
                        LeaveCriticalSection(&curr_pte_locksection->lock);
                        break;
                    }

                    acquire_pte_lock = FALSE;
                }

                // There are no more PTEs left in this section for us to trim
                if (curr_pte_locksection->valid_pte_count == trimmed_in_section) {
                    break;
                }

                curr_pte = &pagetable->pte_list[pte_idx];


                assert(curr_pte_locksection == pte_to_locksection(curr_pte));

                // Ignore invalid PTEs
                if (! is_memory_format(*curr_pte)) {
                    continue;
                }

                curr_page = pfn_to_page(curr_pte->memory_format.frame_number);

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

                // This PTE is now on the chopping block
                pte_section_trim[trimmed_in_section] = curr_pte;
                page_section_trim[trimmed_in_section] = curr_page;

                #ifdef DEBUG_CHECKING
                int dbg_result;
                if ((dbg_result = page_is_isolated(curr_page)) != ISOLATED) {
                    DebugBreak();
                }
                #endif

                trimmed_in_section++;
                total_trimmed++;

                if (trimmed_in_section == TRIM_PER_SECTION) {
                    break;
                }
            }

            // We failed to trim any pages
            if (trimmed_in_section == 0) {
                if (acquire_pte_lock == FALSE) {
                    LeaveCriticalSection(&curr_pte_locksection->lock);
                }

                continue;
            };

            /**
             * We separate the disconnecting MapUserPhysicalPagesScatter call from the insertion from the modified list
             * to reduce the time/contention inside of the modified list lock. Hence, the two nearly identical loops
             */
            PTE transition_pte_contents;
            transition_pte_contents.complete_format = 0;
            transition_pte_contents.transition_format.always_zero = 0;
            transition_pte_contents.transition_format.is_transition = 1;

            if (acquire_pte_lock == TRUE) {
                EnterCriticalSection(&curr_pte_locksection->lock);
            }

            for (ULONG64 trim_idx = 0; trim_idx < trimmed_in_section; trim_idx++) {
                curr_pte = pte_section_trim[trim_idx];
                transition_pte_contents.transition_format.frame_number = curr_pte->memory_format.frame_number;
                write_pte_contents(curr_pte, transition_pte_contents);
                trim_addresses[trim_idx] = pte_to_va(curr_pte);
            }

            // This will be an expensive call to MapUserPhysicalPages, so we want to break it up
            disconnect_va_batch_from_cpu(trim_addresses, trimmed_in_section);

            LeaveCriticalSection(&curr_pte_locksection->lock);

            EnterCriticalSection(&modified_list->lock); 

            for (ULONG64 trim_idx = 0; trim_idx < trimmed_in_section; trim_idx++) {
                curr_pte = pte_section_trim[trim_idx];
                curr_page = pfn_to_page(curr_pte->memory_format.frame_number);
                modified_add_page(curr_page, modified_list);
                release_pagelock(curr_page);
            }

            LeaveCriticalSection(&modified_list->lock); 

            trimmed_in_section = 0;
        }

        // Signal that the modified list should be populated
        trim_count++;

        // printf("trimmed %llx\n", total_trimmed);
        total_trimmed = 0;

        SetEvent(modified_to_standby_event);
    }
}


/**
 * Trims the PTEs behind the faulting thread if there are at least FAULTER_TRIM_BEHIND_MIN of them active,
 * but will only enter one PTE locksection to ensure low wait-times for the faulting thread
 * 
 * Will only trim a max of FAULTER_TRIM_BEHIND_MAX pages
 */
void faulter_trim_behind(PTE* accessed_pte) {
    ULONG64 accessed_pte_index = ((ULONG64) accessed_pte - (ULONG64) pagetable->pte_list) / sizeof(PTE);
    PTE_LOCKSECTION* curr_pte_locksection = pte_to_locksection(accessed_pte);
    PTE* curr_pte;

    if (accessed_pte_index < FAULTER_TRIM_BEHIND_MIN) return;

    /**
     * Quick check to see if the PTEs behind are valid before entering any lock
     * 
     * Note that these PTEs could be trimmed before we acquire the lock and look, but if there are enough here,
     * we will try to acquire the lock and trim behind. If we fail to acquire the lock, then we will do nothing to
     * reduce contention
     */
    ULONG64 num_valid_ptes_behind = 0;
    for (ULONG64 pte_idx = accessed_pte_index - 1; pte_idx > 0; pte_idx--) {
        curr_pte = &pagetable->pte_list[pte_idx];

        if (is_memory_format(*curr_pte) == FALSE) {
            break;
        }

        if (pte_to_locksection(&pagetable->pte_list[pte_idx]) != curr_pte_locksection) {
            break;
        }

        num_valid_ptes_behind++;

        if (num_valid_ptes_behind == FAULTER_TRIM_BEHIND_MAX) {
            break;
        }
    }

    // There were not enough valid PTEs behind us to bother trying
    if (num_valid_ptes_behind < FAULTER_TRIM_BEHIND_MIN) {
        return;
    }

    PTE pte_contents;
    pte_contents.complete_format = 0;
    pte_contents.transition_format.is_transition = 1;

    ULONG64 num_ptes_trimmed = 0;
    PULONG_PTR trimmed_addresses[FAULTER_TRIM_BEHIND_MAX];
    PAGE* pages_trimmed[FAULTER_TRIM_BEHIND_MAX];
    ULONG64 curr_pfn;
    PAGE* curr_page;

    // We will not try to contend for the lock at all, and will cut our losses at the loop we already performed
    if (TryEnterCriticalSection(&curr_pte_locksection->lock) == FALSE) {
        return;
    }

    // Now the PTEs can no longer change from under us - so we are free to change anything
    for (ULONG64 pte_idx = accessed_pte_index - 1; pte_idx > accessed_pte_index - 1 - num_valid_ptes_behind; pte_idx--) {
        curr_pte = &pagetable->pte_list[pte_idx];
        
        if (is_memory_format(*curr_pte) == FALSE) {
            continue;
        }

        curr_pfn = curr_pte->memory_format.frame_number;
        curr_page = pfn_to_page(curr_pfn);

        // No contention on held pagelocks either!
        if (try_acquire_pagelock(curr_page) == FALSE) {
            continue;
        }

        // Edit the PTE to be on transition format
        pte_contents.transition_format.frame_number = curr_pfn;
        write_pte_contents(curr_pte, pte_contents);
        
        // Store the virtual addresses so that they can be unmapped easily
        trimmed_addresses[num_ptes_trimmed] = pte_to_va(curr_pte);

        // Store the pages to add to the modified list
        pages_trimmed[num_ptes_trimmed] = curr_page;

        num_ptes_trimmed++;
        curr_pte_locksection->valid_pte_count--;
    }

    // A lot changed under us, our heuristic was wrong and we were unable to trim any PTEs
    if (num_ptes_trimmed == 0) {
        LeaveCriticalSection(&curr_pte_locksection->lock);
        return;
    }

    disconnect_va_batch_from_cpu(trimmed_addresses, num_ptes_trimmed);

    // Briefly enter the modified list lock just to add the pages that we have already prepared
    EnterCriticalSection(&modified_list->lock);
    for (ULONG64 i = 0; i < num_ptes_trimmed; i++) {
        modified_add_page(pages_trimmed[i], modified_list);
        release_pagelock(pages_trimmed[i]);
    }    
    LeaveCriticalSection(&modified_list->lock);

    LeaveCriticalSection(&curr_pte_locksection->lock);

    if (modified_list->list_length > MAX_PAGES_WRITABLE) {
        SetEvent(modified_to_standby_event);
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
        ULONG64 num_pages_added = 0;

        EnterCriticalSection(&standby_list->lock);

        for (int i = 0; i < disk_batch.num_pages; i++) {
            PAGE* curr_page = disk_batch.pages_being_written[i];
            // If someone else has the pagelock, they must be trying to rescue it right now!
            if (curr_page == NULL) continue;

            num_pages_added++;

            // Now, the page can be modified and added to the standby list
            curr_page->pagefile_idx = disk_batch.disk_indices[i];
            curr_page->writing_to_disk = PAGE_NOT_BEING_WRITTEN;
            curr_page->in_disk_batch = PAGE_NOT_IN_DISK_BATCH;

            if (standby_add_page(curr_page, standby_list) == ERROR) {
                DebugBreak();
            }

        }

        InterlockedAdd64(&total_available_pages, num_pages_added);

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


