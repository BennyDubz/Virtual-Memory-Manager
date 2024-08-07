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
#define TRIM_PER_SECTION 16
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
    printf("ptes per lock %llx\n", ptes_per_lock);
    ULONG64 section_start;
    PTE* curr_pte;
    PTE_LOCKSECTION* curr_pte_locksection;
    ULONG64 curr_pfn;
    PAGE* curr_page = NULL;

    ULONG64 section_num_ptes_trimmed;
    ULONG64 trim_to_modified;
    ULONG64 trim_to_standby;
    ULONG64 trim_already_being_written;
    ULONG64 total_trimmed = 0;

    PTE* pte_section_trim[TRIM_PER_SECTION];
    PAGE* page_section_trim_to_modified[TRIM_PER_SECTION];
    PAGE* page_section_trim_to_standby[TRIM_PER_SECTION];
    PAGE* page_section_already_being_writen[TRIM_PER_SECTION];
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

            section_num_ptes_trimmed = 0;
            trim_to_modified = 0;
            trim_to_standby = 0;
            trim_already_being_written = 0;


            for (ULONG64 pte_idx = section_start; pte_idx < section_start + ptes_per_lock; pte_idx++) {
                /**
                 * We need to acquire the lock at the beginning, and occasionally reacquire it when we
                 * conflict with someone holding the page lock
                 */
                if (acquire_pte_lock) {
                    EnterCriticalSection(&curr_pte_locksection->lock);

                    if (curr_pte_locksection->valid_pte_count == trim_to_modified) {
                        LeaveCriticalSection(&curr_pte_locksection->lock);
                        break;
                    }

                    acquire_pte_lock = FALSE;
                }

                // There are no more PTEs left in this section for us to trim
                if (curr_pte_locksection->valid_pte_count == trim_to_modified + trim_to_standby) {
                    break;
                }

                curr_pte = &pagetable->pte_list[pte_idx];

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
                if (try_acquire_pagelock(curr_page, 6) == FALSE) {
                    LeaveCriticalSection(&curr_pte_locksection->lock);
                    acquire_pte_lock = TRUE;
                    pte_idx--;
                    continue;
                }

                // This PTE is now on the chopping block
                pte_section_trim[section_num_ptes_trimmed] = curr_pte;

                // If the page still has a pagefile index stored within it, then we can trim it straight to standby
                if (curr_page->pagefile_idx == DISK_IDX_NOTUSED) {
                    if (curr_page->writing_to_disk == PAGE_BEING_WRITTEN && curr_page->modified == PAGE_NOT_MODIFIED) {
                        curr_page->status = MODIFIED_STATUS;
                        page_section_already_being_writen[trim_already_being_written] = curr_page;
                        trim_already_being_written++;
                    } else {
                        page_section_trim_to_modified[trim_to_modified] = curr_page;
                        trim_to_modified++;
                    }
                   
                } else {
                    page_section_trim_to_standby[trim_to_standby] = curr_page;
                    trim_to_standby++;
                }

                #ifdef DEBUG_CHECKING
                int dbg_result;
                if ((dbg_result = page_is_isolated(curr_page)) != ISOLATED) {
                    DebugBreak();
                }
                #endif

                total_trimmed++;
                section_num_ptes_trimmed++;

                if (section_num_ptes_trimmed == TRIM_PER_SECTION) {
                    break;
                }
            }


            // We failed to trim any pages, continue to the next section
            if (section_num_ptes_trimmed == 0) {
                if (acquire_pte_lock == FALSE) {
                    LeaveCriticalSection(&curr_pte_locksection->lock);
                }
                continue;
            }

            /**
             * We separate the disconnecting MapUserPhysicalPagesScatter call from the insertion from the modified list
             * to reduce the time/contention inside of the modified list lock. Hence, the two nearly identical loops
             */
            PTE transition_pte_contents;
            transition_pte_contents.complete_format = 0;
            transition_pte_contents.transition_format.is_transition = 1;

            if (acquire_pte_lock == TRUE) {
                EnterCriticalSection(&curr_pte_locksection->lock);
            }

            for (ULONG64 trim_idx = 0; trim_idx < section_num_ptes_trimmed; trim_idx++) {
                curr_pte = pte_section_trim[trim_idx];
                transition_pte_contents.transition_format.frame_number = curr_pte->memory_format.frame_number;
                write_pte_contents(curr_pte, transition_pte_contents);
                trim_addresses[trim_idx] = pte_to_va(curr_pte);
            }

            // This will be an expensive call to MapUserPhysicalPages, so we want to break it up
            disconnect_va_batch_from_cpu(trim_addresses, section_num_ptes_trimmed);

            LeaveCriticalSection(&curr_pte_locksection->lock);


            

            /**
             * Now, we add all of our pages to the modified list or standby list depending on whether
             * or not they have a disk index already
             */

            
            // These pages will be added to the standby list once the mod writer is finished writing
            if (trim_already_being_written > 0) {
                for (ULONG64 trim_idx = 0; trim_idx < trim_already_being_written; trim_idx++) {
                    release_pagelock(page_section_already_being_writen[trim_idx], 26);
                }
            }

            if (trim_to_modified > 0) {
                EnterCriticalSection(&modified_list->lock); 

                for (ULONG64 trim_idx = 0; trim_idx < trim_to_modified; trim_idx++) {
                    curr_page = page_section_trim_to_modified[trim_idx];
                    custom_spin_assert(curr_page->status == ACTIVE_STATUS);
                    modified_add_page(curr_page, modified_list);
                    release_pagelock(curr_page, 18);
                }

                LeaveCriticalSection(&modified_list->lock); 
                SetEvent(modified_to_standby_event);
            }   


            if (trim_to_standby > 0) {
                EnterCriticalSection(&standby_list->lock);

                for (ULONG64 trim_idx = 0; trim_idx < trim_to_standby; trim_idx++) {
                    curr_page = page_section_trim_to_standby[trim_idx];
                    custom_spin_assert(curr_page->status == ACTIVE_STATUS);
                    standby_add_page(curr_page, standby_list);
                    release_pagelock(curr_page, 23);
                }
                InterlockedAdd64(&total_available_pages, trim_to_standby);
                LeaveCriticalSection(&standby_list->lock);

                // BW: Do we need to always set the event?
                SetEvent(waiting_for_pages_event);
            }


        }

        // Signal that the modified list should be populated
        trim_count++;

        
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
    ULONG64 trim_to_modified = 0;
    ULONG64 trim_to_standby = 0;
    ULONG64 trim_already_being_written = 0;

    PULONG_PTR trimmed_addresses[FAULTER_TRIM_BEHIND_MAX];
    PAGE* pages_trimmed_to_modified[FAULTER_TRIM_BEHIND_MAX];
    PAGE* pages_trimmed_to_standby[FAULTER_TRIM_BEHIND_MAX];
    PAGE* pages_already_being_written[FAULTER_TRIM_BEHIND_MAX];

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
        if (try_acquire_pagelock(curr_page, 7) == FALSE) {
            continue;
        }


        // We might have to write it to disk
        if (curr_page->pagefile_idx == DISK_IDX_NOTUSED) {
            /**
             * There is a rare race condition where we are in the process of mod-writing a page, and we rescue it with a read access.
             * 
             * This means the page is not modified - so we continue with the write to disk as normal so we can at least still make use of the
             * not-stale pagefile spot we just filled. However, if we add the page back to the modified list (and set its status to MODIFIED_STATUS)
             * then the mod-writer will try to add it to the standby list. This is fine - we can set it to be back in the MODIFIED_STATUS
             * and the mod writer will put it on the standby list when it is finished. We just need to avoid the case where it is in both the modified
             * and the standby list at the same time - which will corrupt the list structures
             * 
             * Even more rare, a page could be being written, rescued and written to, and trimmed before the write finished.
             * In that case, the pagefile space is again stale so even though the write is in progress, we want to re-add it to the modified list
             */
            if (curr_page->writing_to_disk == PAGE_BEING_WRITTEN && curr_page->modified == PAGE_NOT_MODIFIED) {
                curr_page->status = MODIFIED_STATUS;
                pages_already_being_written[trim_already_being_written] = curr_page;
                trim_already_being_written++;
            } else {
                pages_trimmed_to_modified[trim_to_modified] = curr_page;
                trim_to_modified++;
            }
        // If there is a saved pagefile index in the page, then we can trim it straight to standby
        } else {
            pages_trimmed_to_standby[trim_to_standby] = curr_page;
            trim_to_standby++;
        }

        custom_spin_assert(curr_page->pte == curr_pte);

        // Edit the PTE to be on transition format
        pte_contents.transition_format.frame_number = curr_pfn;
        write_pte_contents(curr_pte, pte_contents);
        
        // Store the virtual addresses so that they can be unmapped easily
        trimmed_addresses[num_ptes_trimmed] = pte_to_va(curr_pte);

        num_ptes_trimmed++;
        curr_pte_locksection->valid_pte_count--;
    }

    // A lot changed under us, our heuristic was wrong and we were unable to trim any PTEs
    if (num_ptes_trimmed == 0) {
        LeaveCriticalSection(&curr_pte_locksection->lock);
        return;
    }

    disconnect_va_batch_from_cpu(trimmed_addresses, num_ptes_trimmed);

    LeaveCriticalSection(&curr_pte_locksection->lock);

    if (trim_already_being_written > 0) {
        for (ULONG64 i = 0; i < trim_already_being_written; i++) {
            release_pagelock(pages_already_being_written[i], 25);
        }
    }


    // Briefly enter the modified list lock just to add the pages that we have already prepared
    if (trim_to_modified > 0) {

        EnterCriticalSection(&modified_list->lock);
        for (ULONG64 i = 0; i < trim_to_modified; i++) {
            curr_page = pages_trimmed_to_modified[i];
            custom_spin_assert(curr_page->status == ACTIVE_STATUS);
            modified_add_page(curr_page, modified_list);
            release_pagelock(curr_page, 19);
        }    
        LeaveCriticalSection(&modified_list->lock);

        SetEvent(modified_to_standby_event);
    }
    
    // Briefly enter the standby list to add our prepared pages - if there are any
    if (trim_to_standby > 0) {

        EnterCriticalSection(&standby_list->lock);
        for (ULONG64 i = 0; i < trim_to_standby; i++) {
            curr_page = pages_trimmed_to_standby[i];
            custom_spin_assert(curr_page->status == ACTIVE_STATUS);
            standby_add_page(curr_page, standby_list);
            release_pagelock(curr_page, 24);
        }
        InterlockedAdd64(&total_available_pages, trim_to_standby);
        LeaveCriticalSection(&standby_list->lock);

        SetEvent(waiting_for_pages_event);
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
    PAGE* curr_page;
    // PAGE** pages_currently_writing[MAX_PAGES_WRITABLE];
    PTE* relevant_PTE;
    ULONG64 disk_storage_idx;

    DISK_BATCH disk_batch;    
    BOOL wait_for_signal = TRUE;


    while(TRUE) {
        if (wait_for_signal == TRUE) {
            WaitForSingleObject(modified_to_standby_event, INFINITE);
        }

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

            custom_spin_assert(potential_page->status == MODIFIED_STATUS);

            // The rare case where the page we want to pop has its pagelock held. We surrender the modified listlock
            if (try_acquire_pagelock(potential_page, 8) == FALSE) {
                LeaveCriticalSection(&modified_list->lock);
                acquire_mod_lock = TRUE;
                curr_attempts++;
                continue;
            }

            custom_spin_assert(potential_page->pagefile_idx == DISK_IDX_NOTUSED);


            // We now remove the page from the modified list
            modified_pop_page(modified_list);

            /**
             * Addressing a race condition where a page we had previously popped
             * from this loop was rescued and trimmed again - and is now about to be added again
             * 
             * We work to prevent the double-add to the list while still writing it to disk
             */
            if (potential_page->writing_to_disk == PAGE_NOT_BEING_WRITTEN) {
                disk_batch.pages_being_written[curr_page_num] = potential_page;
                disk_batch.num_pages++;
                curr_page_num++;
            }

            // We still have an up-to-date reference to the page and haven't written it yet
            if (potential_page->modified == PAGE_MODIFIED) {
                potential_page->modified = PAGE_NOT_MODIFIED;
            }

            potential_page->writing_to_disk = PAGE_BEING_WRITTEN;

            release_pagelock(potential_page, 20);

            curr_attempts++;
        }  

        if (acquire_mod_lock == FALSE) {
            LeaveCriticalSection(&modified_list->lock);
        }

        // We did not get any pages
        if (curr_page_num == 0) continue;

        ULONG64 num_pages_written = write_batch_to_disk(&disk_batch);

        // First check to see if we wrote any pages at all - if we didn't we need to wait for available disk slots
        if (num_pages_written == 0) {
            printf("out of disk slots for mod writer\n");
            for (ULONG64 i = 0; i < disk_batch.num_pages; i++) {
                curr_page = disk_batch.pages_being_written[i];
                acquire_pagelock(curr_page, 27);

                curr_page->writing_to_disk = PAGE_NOT_BEING_WRITTEN;

                // Ignore pages that have been rescued and/or modified
                if (curr_page->status != MODIFIED_STATUS || 
                    curr_page->modified == PAGE_MODIFIED) {
                    
                    disk_batch.pages_being_written[i] = NULL;
                    release_pagelock(curr_page, 28);
                }
            }
            
            EnterCriticalSection(&modified_list->lock);

            for (ULONG64 i = 0; i < disk_batch.num_pages; i++) {
                curr_page = disk_batch.pages_being_written[i];

                if (curr_page == NULL) continue;

                // We would normally insert them at the head - but we want these to be first-in-line to be written again
                db_insert_node_at_tail(modified_list->listhead, curr_page->frame_listnode);
                modified_list->list_length++;
                release_pagelock(curr_page, 29);
            }

            LeaveCriticalSection(&modified_list->lock);

            WaitForSingleObject(disk_open_slots_event, INFINITE);

            // Try again with the whole mod-write scheme
            wait_for_signal = FALSE;
            continue;
        }


        /**
         * We might not always get enough disk slots to write all of the pages. Instead of waiting,
         * we just add the remainder back to the modified list
         */
        if (num_pages_written != disk_batch.num_pages) {
            custom_spin_assert(FALSE);
            ULONG64 start_index = disk_batch.num_pages - num_pages_written;

            for (ULONG64 i = start_index; i < disk_batch.num_pages; i++) {
                curr_page = disk_batch.pages_being_written[i];
                acquire_pagelock(curr_page, 30);

                curr_page->writing_to_disk = PAGE_NOT_BEING_WRITTEN;

                // Ignore pages that have been rescued and/or modified
                if (curr_page->status != MODIFIED_STATUS || 
                    curr_page->modified == PAGE_MODIFIED) {
                    
                    disk_batch.pages_being_written[i] = NULL;
                    release_pagelock(curr_page, 31);
                }
            }
            
            EnterCriticalSection(&modified_list->lock);

            for (ULONG64 i = start_index; i < disk_batch.num_pages; i++) {
                curr_page = disk_batch.pages_being_written[i];

                if (curr_page == NULL) continue;

                // We would normally insert them at the head - but we want these to be first-in-line to be written again
                db_insert_node_at_tail(modified_list->listhead, curr_page->frame_listnode);
                modified_list->list_length++;
                release_pagelock(curr_page, 32);
            }

            LeaveCriticalSection(&modified_list->lock);
        }

        ULONG64 end_index = num_pages_written;
        BOOL release_slot;

        /**
         * Acquire pagelocks ahead of time, then get the standby list lock
         */
        for (int i = 0; i < num_pages_written; i++) {
            
            curr_page = disk_batch.pages_being_written[i];
            release_slot = FALSE;

            acquire_pagelock(curr_page, 9);

            curr_page->writing_to_disk = PAGE_NOT_BEING_WRITTEN;

            // In either case, we need to bail from adding this page to standby
            if (curr_page->status != MODIFIED_STATUS || 
                    curr_page->modified == PAGE_MODIFIED) {

                custom_spin_assert(curr_page->pagefile_idx == DISK_IDX_NOTUSED);

                // We can still use the pagefile space even if the page was rescued
                if (curr_page->modified == PAGE_NOT_MODIFIED) {
                    curr_page->pagefile_idx = disk_batch.disk_indices[i];
                } else {
                    // We need to discard the pagefile space
                    release_slot = TRUE;
                }

                release_pagelock(curr_page, 10);

                disk_batch.pages_being_written[i] = NULL;

                // We would rather release the disk slot while not holding this pagelock
                if (release_slot) {
                    release_single_disk_slot(disk_batch.disk_indices[i]);
                }
            
                continue;
            }

        }

        ULONG64 num_pages_added = 0;

        EnterCriticalSection(&standby_list->lock);

        for (int i = 0; i < num_pages_written; i++) {
            PAGE* curr_page = disk_batch.pages_being_written[i];
            if (curr_page == NULL) continue;

            num_pages_added++;

            custom_spin_assert(curr_page->pagefile_idx == DISK_IDX_NOTUSED);

            // Now, the page can be added to the standby list
            curr_page->pagefile_idx = disk_batch.disk_indices[i];

            if (standby_add_page(curr_page, standby_list) == ERROR) {
                DebugBreak();
            }

            # if 0
            // InterlockedIncrement64(&total_available_pages);

            // release_pagelock(curr_page, 21);
            #endif

        }

        #if 1
        InterlockedAdd64(&total_available_pages, num_pages_added);
        #endif

        LeaveCriticalSection(&standby_list->lock);

        #if 1
        /**
         * Release all of the pagelocks that we hold
         */
        for (int i = 0; i < num_pages_written; i++) {
            PAGE* curr_page = disk_batch.pages_being_written[i];
            if (curr_page != NULL) {
                release_pagelock(curr_page, 21);
            }
        }
        #endif
    
        SetEvent(waiting_for_pages_event);

        sb_count++;

        curr_mod_list_length = *(volatile ULONG64*) &modified_list->list_length;

        if (curr_mod_list_length > MAX_PAGES_WRITABLE / 4) {
            wait_for_signal = FALSE;
        } else {
            wait_for_signal = TRUE;
        }
    }
    
}


/**
 * For a given PTE and the faulter's access type, determines whether we should map the PTE
 * to be READONLY or READWRITE
 * 
 * Modifies the VirtualAlloc'd user address space to represent the modified permissions. Typically,
 * just modifying the PTEs would be enough - but we have to do this for our simulation.
 * 
 * Returns the permissions set (either PAGE_READWRITE or PAGE_READONLY)
 */
static ULONG64 determine_address_approprate_permissions(PTE* pte, ULONG64 access_type) {
    PULONG_PTR pte_va = pte_to_va(pte);
    DWORD old_protection_storage_notused;

    /**
     * Unaccessed PTEs should always get readwrite permissions, as they are likely to 
     * write to an address that they have never used before now
     */
    if (access_type == WRITE_ACCESS || is_used_pte(*pte) == FALSE) {
        #if 0
        if (VirtualProtect(pte_va, PAGE_SIZE, PAGE_READWRITE, &old_protection_storage_notused) == ERROR) {
            fprintf(stderr, "Failed to change permissions in set_address_approprate_permissions %d 0\n", GetLastError());
            DebugBreak();
        }
        #endif
        
        return PAGE_READWRITE;
    }
    
    #if 0
    // We have already reserved this memory, so this should never fail. We are merely changing the permissions
    if (VirtualProtect(pte_va, PAGE_SIZE, PAGE_READONLY, &old_protection_storage_notused) == ERROR) {
        fprintf(stderr, "Failed to change permissions in set_address_approprate_permissions %d 1\n", GetLastError());
        DebugBreak();
    }
    #endif

    return PAGE_READONLY;
}


/**
 * Connects the given PTE to the open page's physical frame and modifies the PTE
 * 
 * Sets the permission bits of the PTE in accordance with its status as well as the type of access
 * (read / write) that occurred. We try to get away with PAGE_READONLY permissions when it would allow us
 * to potentially conserve pagefile space - and therefore unncessary modified writes.
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 */
int connect_pte_to_page(PTE* pte, PAGE* open_page, ULONG64 access_type) {
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

    ULONG64 permissions = determine_address_approprate_permissions(pte, access_type);
    
    // We need to just set the top bit of the pfn to be 1 for MapUserPhysicalPages to set the permissions to readonly without 
    // the need for using VirtualProtect
    if (permissions == PAGE_READONLY) {
        pfn = pfn | PAGE_MAPUSERPHYSCAL_READONLY_MASK;
    }

    // Map the CPU
    if (MapUserPhysicalPages (pte_to_va(pte), 1, &pfn) == FALSE) {

        fprintf (stderr, "connect_pte_to_page : could not map VA %p to pfn %llx\n", pte_to_va(pte), pfn);
        DebugBreak();

        return ERROR;
    }


    if (is_memory_format(*pte)) {
        DebugBreak();
    }

    PTE pte_contents;
    pte_contents.complete_format = 0;
    pte_contents.memory_format.age = 0;
    pte_contents.memory_format.frame_number = pfn;
    pte_contents.memory_format.valid = VALID;

    if (permissions = PAGE_READONLY) {
        pte_contents.memory_format.protections = PTE_PROTREAD;
    } else {
        pte_contents.memory_format.protections = PTE_PROTREADWRITE;
    }

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

    // This allows us to unmap all of the virtual addresses in a single function call
    if (MapUserPhysicalPagesScatter(virtual_addresses, num_ptes, NULL) == FALSE) {
        fprintf(stderr, "Failed to unmap PTE virtual addresses in disconnect_va_batch_from_cpu\n");
        DebugBreak();
        return ERROR;
    }

    return SUCCESS;
}


