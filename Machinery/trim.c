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
LPTHREAD_START_ROUTINE thread_aging(void* parameters) {
    WORKER_THREAD_PARAMETERS* thread_params = (WORKER_THREAD_PARAMETERS*) parameters;
    ULONG64 worker_thread_idx = thread_params->thread_idx;

    #if DEBUG_THREAD_STORAGE
    thread_information.thread_local_storages[worker_thread_idx].thread_id = GetCurrentThreadId();
    #endif

    ULONG64 ptes_per_lock = pagetable->num_virtual_pages / pagetable->num_locks;
    ULONG64 section_start;
    PTE_LOCKSECTION* curr_pte_locksection;

    HANDLE events[2];
    ULONG64 signaled_event;

    events[0] = aging_event;
    events[1] = shutdown_event;

    while(TRUE) {

        signaled_event = WaitForMultipleObjects(2, events, FALSE, INFINITE);

        if (signaled_event == 1) {
            return NULL;
        }  

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
 * Updates all of the threads trim signal statuses to the given status
 */
void trim_update_thread_storages(UCHAR trim_signal_status) {
    for (ULONG64 i = 0; i < thread_information.total_thread_count; i++) {
        thread_information.thread_local_storages[i].trim_signaled = trim_signal_status;
    }
}

/**
 * Thread dedicated to trimming PTEs from the pagetable and putting them on the modified list
 */
LPTHREAD_START_ROUTINE thread_trimming(void* parameters) {
    WORKER_THREAD_PARAMETERS* thread_params = (WORKER_THREAD_PARAMETERS*) parameters;
    ULONG64 worker_thread_idx = thread_params->thread_idx;

    #if DEBUG_THREAD_STORAGE
    thread_information.thread_local_storages[worker_thread_idx].thread_id = GetCurrentThreadId();
    #endif


    ULONG64 ptes_per_lock = pagetable->num_virtual_pages / pagetable->num_locks;
    printf("ptes per lock %llX\n", ptes_per_lock);
    ULONG64 section_start;
    PTE* curr_pte;
    PTE_LOCKSECTION* curr_pte_locksection;
    ULONG64 curr_pfn;
    PAGE* curr_page = NULL;

    ULONG64 section_num_ptes_trimmed;
    ULONG64 trim_to_modified;
    ULONG64 trim_to_standby;
    ULONG64 trim_already_being_written;
    BOOL signal_modified;

    PTE* pte_section_trim[TRIM_PER_SECTION];
    PAGE* page_section_trim_to_modified[TRIM_PER_SECTION];
    PAGE* page_section_trim_to_standby[TRIM_PER_SECTION];
    PAGE* page_section_already_being_writen[TRIM_PER_SECTION];
    PULONG_PTR trim_addresses[TRIM_PER_SECTION];

    HANDLE events[2];
    ULONG64 signaled_event;

    events[0] = trimming_event;
    events[1] = shutdown_event;


    while(TRUE) {
        signaled_event = WaitForMultipleObjects(2, events, FALSE, INFINITE);

        if (signaled_event == 1) {
            return NULL;
        }

        trim_update_thread_storages(TRIMMER_NOT_SIGNALLED);

        signal_modified = FALSE;

        // Go through each lock section and increment all valid PTEs
        for (ULONG64 lock_section = 0; lock_section < pagetable->num_locks; lock_section++) {            
            section_start = lock_section * ptes_per_lock;

            curr_pte_locksection = &pagetable->pte_locksections[lock_section];

            // Ignore invalid PTE sections  
            if (curr_pte_locksection->valid_pte_count == 0) continue;

            // if (curr_pte_locksection->valid_pte_count < TRIM_PER_SECTION / 4) continue;


            section_num_ptes_trimmed = 0;
            trim_to_modified = 0;
            trim_to_standby = 0;
            trim_already_being_written = 0;

            EnterCriticalSection(&curr_pte_locksection->lock);

            for (ULONG64 pte_idx = section_start; pte_idx < section_start + ptes_per_lock; pte_idx++) {
        
                // There are no more PTEs left in this section for us to trim
                if (curr_pte_locksection->valid_pte_count == section_num_ptes_trimmed) {
                    break;
                }

                curr_pte = &pagetable->pte_list[pte_idx];

                // Ignore invalid PTEs
                if (is_memory_format(read_pte_contents(curr_pte)) == FALSE) {
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
                    continue;
                }

                // This PTE is now on the chopping block
                pte_section_trim[section_num_ptes_trimmed] = curr_pte;

                custom_spin_assert(curr_page->status == ACTIVE_STATUS);

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

                section_num_ptes_trimmed++;

                if (section_num_ptes_trimmed == TRIM_PER_SECTION) {
                    break;
                }
            }


            // We failed to trim any pages, continue to the next section
            if (section_num_ptes_trimmed == 0) {
                LeaveCriticalSection(&curr_pte_locksection->lock);
                continue;
            }


            PTE transition_pte_contents;
            transition_pte_contents.complete_format = 0;
            transition_pte_contents.transition_format.is_transition = 1;

            for (ULONG64 trim_idx = 0; trim_idx < section_num_ptes_trimmed; trim_idx++) {
                curr_pte = pte_section_trim[trim_idx];
                transition_pte_contents.transition_format.frame_number = curr_pte->memory_format.frame_number;
                write_pte_contents(curr_pte, transition_pte_contents);
                trim_addresses[trim_idx] = pte_to_va(curr_pte);
            }

            // This will be an expensive call to MapUserPhysicalPages, so we want to batch at least a few addresses
            disconnect_va_batch_from_cpu(trim_addresses, section_num_ptes_trimmed);

            InterlockedAdd64(&curr_pte_locksection->valid_pte_count, - section_num_ptes_trimmed);

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
            
            PAGE* beginning_of_section;
            PAGE* end_of_section;

            if (trim_to_modified > 0) {
                create_chain_of_pages(page_section_trim_to_modified, trim_to_modified, MODIFIED_STATUS);
                
                beginning_of_section = page_section_trim_to_modified[0];
                end_of_section = page_section_trim_to_modified[trim_to_modified - 1];
                
                insert_page_section(modified_list, beginning_of_section, end_of_section, trim_to_modified);

                signal_modified = TRUE;
                #if 0
                // If we are desperate, set the event to start mod writing immediately
                if (total_available_pages < physical_page_count / 3) {
                    SetEvent(modified_writer_event);
                }
                #endif
                SetEvent(modified_writer_event);
            }   


            if (trim_to_standby > 0) {
                BOOL signal_waiting_for_pages_event;

                create_chain_of_pages(page_section_trim_to_standby, trim_to_standby, STANDBY_STATUS);

                beginning_of_section = page_section_trim_to_standby[0];
                end_of_section = page_section_trim_to_standby[trim_to_standby - 1];
               
                // We only want to set the event if we desperately need pages, and we check before we modify the global
                signal_waiting_for_pages_event = (total_available_pages == 0);
                
                InterlockedAdd64(&total_available_pages, trim_to_standby);

                insert_page_section(standby_list, beginning_of_section, end_of_section, trim_to_standby);

                if (signal_waiting_for_pages_event == TRUE) {
                    SetEvent(waiting_for_pages_event);
                }
            }


        }

        // If we can, we only set the event here at the very end
        if (signal_modified) {
            SetEvent(modified_writer_event);
        }
        
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

        if (is_memory_format(read_pte_contents(curr_pte)) == FALSE) {
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
        
        if (is_memory_format(read_pte_contents(curr_pte)) == FALSE) {
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
    }

    // A lot changed under us, our heuristic was wrong and we were unable to trim any PTEs
    if (num_ptes_trimmed == 0) {
        LeaveCriticalSection(&curr_pte_locksection->lock);
        return;
    }

    disconnect_va_batch_from_cpu(trimmed_addresses, num_ptes_trimmed);

    InterlockedAdd64(&curr_pte_locksection->valid_pte_count, - num_ptes_trimmed);

    LeaveCriticalSection(&curr_pte_locksection->lock);

    if (trim_already_being_written > 0) {
        for (ULONG64 i = 0; i < trim_already_being_written; i++) {
            release_pagelock(pages_already_being_written[i], 25);
        }
    }

    PAGE* beginning_of_section;
    PAGE* end_of_section;

    // Briefly enter the modified list lock just to add the pages that we have already prepared
    if (trim_to_modified > 0) {

        create_chain_of_pages(pages_trimmed_to_modified, trim_to_modified, MODIFIED_STATUS);

        beginning_of_section = pages_trimmed_to_modified[0];
        end_of_section = pages_trimmed_to_modified[trim_to_modified - 1];
        
        insert_page_section(modified_list, beginning_of_section, end_of_section, trim_to_modified);

        // We don't want to bother setting this event unless the mod-writer will do subtantial work
        if (modified_list->list_length > MAX_PAGES_WRITABLE / 4) {
            SetEvent(modified_writer_event);
        }
        
    }
    
    // Briefly enter the standby list to add our prepared pages - if there are any
    if (trim_to_standby > 0) {
        BOOL signal_waiting_for_pages = (total_available_pages == 0);

        create_chain_of_pages(pages_trimmed_to_standby, trim_to_standby, STANDBY_STATUS);
        
        beginning_of_section = pages_trimmed_to_standby[0];
        end_of_section = pages_trimmed_to_standby[trim_to_standby - 1];

        InterlockedAdd64(&total_available_pages, trim_to_standby);

        insert_page_section(standby_list, beginning_of_section, end_of_section, trim_to_standby);      

        // We only want to set this event if we were out of pages before - otherwise, we can avoid the overhead
        if (signal_waiting_for_pages) {
            SetEvent(waiting_for_pages_event);
        }
    }
}


/**
 * Thread dedicated to writing pages from the modified list to disk, putting finally adding the pages to standby
 */
ULONG64 sb_count = 0;

LPTHREAD_START_ROUTINE thread_modified_writer(void* parameters) {

    WORKER_THREAD_PARAMETERS* thread_params = (WORKER_THREAD_PARAMETERS*) parameters;
    ULONG64 worker_thread_idx = thread_params->thread_idx;

    #if DEBUG_THREAD_STORAGE
    THREAD_LOCAL_STORAGE* storage = &thread_information.thread_local_storages[worker_thread_idx];
    thread_information.thread_local_storages[worker_thread_idx].thread_id = GetCurrentThreadId();
    #endif

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

    DISK_BATCH* disk_batch = malloc(sizeof(DISK_BATCH));

    if (disk_batch == NULL) {
        fprintf(stderr, "Failed to allocate memory for mod writer's disk batch\n");
        return NULL; 
    }    

    PAGE** pages_confirmed_to_standby = (PAGE**) malloc(sizeof(PAGE*) * MOD_WRITER_MINIBATCH_SIZE);

    if (pages_confirmed_to_standby == NULL) {
        fprintf(stderr, "Failed to allocate memory for mod writer's page list\n");
        return NULL;
    }

    BOOL wait_for_signal = TRUE;

    printf("Max pages writable 0x%llx\n", MAX_PAGES_WRITABLE);

    HANDLE events[2];
    ULONG64 signaled_event;

    events[0] = modified_writer_event;
    events[1] = shutdown_event;

    while(TRUE) {
        if (wait_for_signal == TRUE) {
            signaled_event = WaitForMultipleObjects(2, events, FALSE, INFINITE);

            if (signaled_event == 1) {
                free(pages_confirmed_to_standby);
                free(disk_batch);
                return NULL;
            }
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
        ULONG64 section_attempts = 0;
        ULONG64 curr_page_num = 0;

        disk_batch->num_pages = 0;
        disk_batch->write_complete = FALSE;

        /**
         * Sometimes, if there are very few pages available we should hog the modified list lock 
         * and write as many pages to disk as we can
         */
        ULONG64 num_to_get_per_section;
        
        #if 0
        if (standby_list->list_length < physical_page_count / MOD_WRITER_PREFERRED_STANDBY_MINIMUM_PROPORTION) {
            num_to_get_per_section = num_to_write;
        } else {
            num_to_get_per_section = MOD_WRITER_SECTION_SIZE;
        }
        #endif

        while (curr_attempts < num_to_write) {
            section_attempts = 0;

            // We pop from the tail, so we have to acquire the tail pagelock first
            potential_page = modified_list->tail.blink;
            
            // The list is empty
            if (potential_page == &modified_list->head) break;

            // The rare case where the page we want to pop has its pagelock held. We surrender the modified listlock
            if (try_acquire_pagelock(potential_page, 8) == FALSE) {
                
                curr_attempts++;
                continue;
            }

            // By the time we actually got the pagelock, it could have been rescued
            if (potential_page->status != MODIFIED_STATUS) {
                release_pagelock(potential_page, 78);
                curr_attempts++;
                continue;
            }

            custom_spin_assert(potential_page->status == MODIFIED_STATUS);
            custom_spin_assert(potential_page->pagefile_idx == DISK_IDX_NOTUSED);


            // We now remove the page from the modified list
            unlink_page(modified_list, potential_page);
            
            /**
             * Addressing a race condition where a page we had previously popped
             * from this loop was rescued and trimmed again - and is now about to be added again
             * 
             * We work to prevent the double-add to the list while still writing it to disk
             */
            if (potential_page->writing_to_disk == PAGE_NOT_BEING_WRITTEN) {
                disk_batch->pages_being_written[curr_page_num] = potential_page;
                disk_batch->num_pages++;
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

        // We did not get any pages
        if (curr_page_num == 0) continue;

        // We need a list of PFNs for MapUserPhysicalPages
        for (int page_num = 0; page_num < disk_batch->num_pages; page_num++) {
            disk_batch->pfns[page_num] = page_to_pfn(disk_batch->pages_being_written[page_num]);
        }

        ULONG64 num_available_disk_slots = allocate_many_disk_slots(disk_batch->disk_indices, disk_batch->num_pages);

        // custom_spin_assert(num_available_disk_slots == disk_batch->num_pages);

        // Map the physical pages to our large disk write slot
        if (MapUserPhysicalPages(disk->disk_large_write_slot, num_available_disk_slots, disk_batch->pfns) == FALSE) {
            printf ("MapUserPhysPages in thread_modified_writer failed, error %#x\n", GetLastError());
            fprintf(stderr, "Failed to map physical pages to large disk write slot in write_batch_to_disk\n");
            custom_spin_assert(FALSE);
        }

        /**
         * Now, we perform one section of memcpys and adding to the standby list at a time
         * 
         * This ensures that not only did we batch the expensive operation of MapUserPhysicalPages, but we also
         * are able to intermitently populate the standby list without having to wait for all of the memcpys to finish, since with
         * large batch sizes the memory copies become more expensive than the MapUserPhysicalPages.
         * 
         * If we didn't do this - we would often run out of pages early on in our simulation when the mod-writer is struggling to keep up with
         * the huge volume of disk-writes that need to occur. Similarly, in the real world, this would be helpful if we allocated a new large chunk of memory,
         * needed to mod-write it to disk due to a lack of available pages, but didn't want the users to starve in the meantime.
         */
        PULONG_PTR source_addr = disk->disk_large_write_slot;
        BOOL release_slot;
        PULONG_PTR disk_slot_addr;
        ULONG64 num_minibatches = min((disk_batch->num_pages / MOD_WRITER_MINIBATCH_SIZE) + 1, MAX_PAGES_WRITABLE / MOD_WRITER_MINIBATCH_SIZE);
        ULONG64 start_of_minibatch;
        ULONG64 end_of_minibatch;
        PAGE* curr_page;
        ULONG64 num_pages_confirmed_to_standby;
        BOOL set_event_at_end;
        
        /**
         * Rather than doing the entire MOD_WRITER_SECTION_SIZE of memcpys before adding all of the pages to the standby list,
         * we can do smaller batches of them at a time before adding them to the standby list - this can alleviate page starvation more quickly
         * 
         * Previously, before we had a shared lock scheme on the list locks, we needed to do as much work as possible before adding a large
         * number of pages to the standby list quickly to reduce contention. However, we don't have to be as scared of the list lock anymore, and
         * alleviating page starvation became a priority.
         */
        for (ULONG64 minibatch = 0; minibatch < num_minibatches; minibatch++) {
            num_pages_confirmed_to_standby = 0;
            start_of_minibatch = minibatch * MOD_WRITER_MINIBATCH_SIZE;
            end_of_minibatch = min(start_of_minibatch + MOD_WRITER_MINIBATCH_SIZE, disk_batch->num_pages);

            // Perform all of this minibatch's memcpys
            for (ULONG64 i = start_of_minibatch; i < end_of_minibatch; i++) {

                disk_slot_addr = disk_idx_to_addr(disk_batch->disk_indices[i]);

                memcpy(disk_slot_addr, source_addr, PAGE_SIZE);

                // Increment the source address to the next page's data
                source_addr += (PAGE_SIZE / sizeof(PULONG_PTR));
            }

            // Acquire all of the pagelocks, and ensure we can still add them to the standby list
            for (ULONG64 i = start_of_minibatch; i < end_of_minibatch; i++) {
                curr_page = disk_batch->pages_being_written[i];
                release_slot = FALSE;

                acquire_pagelock(curr_page, 9);

                curr_page->writing_to_disk = PAGE_NOT_BEING_WRITTEN;

                // In either case, we need to bail from adding this page to standby
                if (curr_page->status != MODIFIED_STATUS || 
                        curr_page->modified == PAGE_MODIFIED) {

                    custom_spin_assert(curr_page->pagefile_idx == DISK_IDX_NOTUSED);

                    // We can still use the pagefile space even if the page was rescued
                    if (curr_page->modified == PAGE_NOT_MODIFIED) {
                        curr_page->pagefile_idx = disk_batch->disk_indices[i];
                    } else {
                        // We need to discard the pagefile space
                        release_slot = TRUE;
                    }

                    release_pagelock(curr_page, 10);

                    disk_batch->pages_being_written[i] = NULL;

                    // We would rather release the disk slot while not holding this pagelock
                    if (release_slot) {
                        release_single_disk_slot(disk_batch->disk_indices[i]);
                    }
                
                    continue;
                }

                pages_confirmed_to_standby[num_pages_confirmed_to_standby] = curr_page;
                num_pages_confirmed_to_standby++;
            }

            // We have pages to commit to the standby list
            if (num_pages_confirmed_to_standby > 0) {

                set_event_at_end = TRUE;

                // We are able to create an entire chain of pages to the standby list very quickly
                create_chain_of_pages(pages_confirmed_to_standby, num_pages_confirmed_to_standby, STANDBY_STATUS);

                PAGE* beginning_page = pages_confirmed_to_standby[0];
                PAGE* end_page = pages_confirmed_to_standby[num_pages_confirmed_to_standby - 1];
            
                BOOL signal_waiting_for_pages = (total_available_pages == 0);
            
                /**
                 * Now edit all of the pages to add all of the pagefile information
                 */
                for (ULONG64 i = start_of_minibatch; i < end_of_minibatch; i++) {
                    curr_page = disk_batch->pages_being_written[i];

                    if (curr_page == NULL) continue;

                    custom_spin_assert(curr_page->pagefile_idx == DISK_IDX_NOTUSED);
                    curr_page->pagefile_idx = disk_batch->disk_indices[i];
                }

                InterlockedAdd64(&total_available_pages, num_pages_confirmed_to_standby);

                insert_page_section(standby_list, beginning_page, end_page, num_pages_confirmed_to_standby);

                if (signal_waiting_for_pages == TRUE) {
                    SetEvent(waiting_for_pages_event);
                }
            }

            
        }

        // Unmap the physical pages from the large write slot
        if (MapUserPhysicalPages(disk->disk_large_write_slot, num_available_disk_slots, NULL) == FALSE) {
            fprintf(stderr, "MapUserPhysPages in thread_modified_writer failed, error %#x\n", GetLastError());
            DebugBreak();
        }

        /**
         * We have successfully added pages, and even if the total_available_pages was empty, threads could still
         * possibly be waiting for pages. This is rare - so we do not want to constantly set the event in the
         * mod-writer, but we can afford to do it once at the end
         */
        if (set_event_at_end) {
            SetEvent(waiting_for_pages_event);
        }


        curr_mod_list_length = *(volatile ULONG64*) &modified_list->list_length;

        if (curr_mod_list_length > MAX_PAGES_WRITABLE / 4) {
            wait_for_signal = FALSE;
        } else {
            wait_for_signal = TRUE;

            potential_list_refresh(worker_thread_idx);
        }
        
    }
    
    DebugBreak();
    return NULL;
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
    if (access_type == WRITE_ACCESS || is_used_pte(read_pte_contents(pte)) == FALSE) {        
        return PAGE_READWRITE;
    }


    return PAGE_READONLY;
}


/**
 * Determines the appropriate permissions for all of the ptes_in_question, and writes them into the permission_storage
 */
static void determine_batch_address_appropriate_permissions(PTE** ptes_in_question, PTE* original_accessed_pte, ULONG64 access_type, ULONG64 num_ptes, ULONG64* permission_storage) {
    ULONG64 start_index;
    PTE* curr_pte;

    /**
     * If we are doing a disk read, we may have speculated on the ahead PTEs while our original accessed PTE was being read in by someone else
     * or was resolved. In that case, the original access type does not apply
     */
    if (original_accessed_pte != ptes_in_question[0]) {
        start_index = 0;
    } else {
        if (access_type == WRITE_ACCESS || is_used_pte(read_pte_contents(original_accessed_pte)) == FALSE) {
            permission_storage[0] = PAGE_READWRITE;
        } else {
            permission_storage[0] = PAGE_READONLY;
        }
        start_index = 1;
    }

    // Determine the permissions for the other PTEs
    for (ULONG64 i = start_index; i < num_ptes; i++) {
        curr_pte = ptes_in_question[i];

        if (is_used_pte(read_pte_contents(curr_pte)) == FALSE) {
            permission_storage[i] = PAGE_READWRITE;
        } else {
            permission_storage[i] = PAGE_READONLY;
        }

    }
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
    
    InterlockedIncrement64(&pte_locksection->valid_pte_count);

    ULONG64 permissions = determine_address_approprate_permissions(pte, access_type);
    
    // We need to just set the top bit of the pfn to be 1 for MapUserPhysicalPages to set the permissions to readonly without 
    // the need for using VirtualProtect
    if (permissions == PAGE_READONLY) {
        pfn = pfn | PAGE_MAPUSERPHYSCAL_READONLY_MASK;
    }

    // Map the CPU
    if (MapUserPhysicalPages (pte_to_va(pte), 1, &pfn) == FALSE) {

        fprintf (stderr, "connect_pte_to_page : could not map VA %p to pfn %llX\n", pte_to_va(pte), pfn);
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


    return SUCCESS;
}


/**
 * Connects the list of given PTEs to their corresponding pages, and modifies all the PTEs to be
 * in valid format. Assumes all PTEs are in the same PTE locksection
 * 
 * Sets the PTEs permission bits depending on whether or not they have preservable pagefile space and the type of access of the fault
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise 
 */
int connect_batch_ptes_to_pages(PTE** ptes_to_connect, PTE* original_accessed_pte, PAGE** pages, ULONG64 access_type, ULONG64 num_ptes) {

    ULONG64 permissions[MAX_PAGES_READABLE];
    ULONG64 pfns[MAX_PAGES_READABLE];
    PVOID virtual_addresses[MAX_PAGES_READABLE];

    determine_batch_address_appropriate_permissions(ptes_to_connect, original_accessed_pte, access_type, num_ptes, permissions);

    PTE pte_contents;
    pte_contents.complete_format = 0;
    pte_contents.memory_format.valid = VALID;

    PTE* curr_pte;

    for (ULONG64 i = 0; i < num_ptes; i++) {
        curr_pte = ptes_to_connect[i];
        virtual_addresses[i] = pte_to_va(curr_pte);
        pfns[i] = page_to_pfn(pages[i]);        
        
        // For MapUserPhysicalPages, we have to OR the leftmost bit of the pfn to set the actual hardware PTE permissions to readonly
        if (permissions[i] == PAGE_READONLY) {
            // This doesn't affect the frame number stored in the PTE, as that is only 40 bits wide and this is 64
            pfns[i] |= PAGE_MAPUSERPHYSCAL_READONLY_MASK;
        }
    }

    // This is the expensive operation we really want to batch!
    if (MapUserPhysicalPagesScatter(virtual_addresses, num_ptes, pfns) == FALSE) {
        fprintf(stderr, "Failed to map batch of pages in connect_batch_ptes_to_pages\n");
        DebugBreak();
        return ERROR;
    }

    PTE_LOCKSECTION* pte_locksection;
    // = pte_to_locksection(original_accessed_pte);

    for (ULONG64 i = 0; i < num_ptes; i++) {
    
        curr_pte = ptes_to_connect[i];

        pte_locksection = pte_to_locksection(curr_pte);

        if (permissions[i] == PAGE_READONLY) {
            pte_contents.memory_format.protections = PTE_PROTREAD;
        } else {
            pte_contents.memory_format.protections = PTE_PROTREADWRITE;
        }

        pte_contents.memory_format.frame_number = pfns[i];

        InterlockedIncrement64(&pte_locksection->valid_pte_count);

        write_pte_contents(curr_pte, pte_contents);
    }


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

    InterlockedDecrement64(&pte_locksection->valid_pte_count);

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


