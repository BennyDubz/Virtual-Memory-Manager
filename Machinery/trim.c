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
 * 
 * As of right now this thread sleeps forever as we do not take advantage of any aging in the trimmer. This is primarily due to the extra overhead
 * it would introduce into the simulation that would not be present in the real world. In the simulation, even when a user thread successfully accessed an address,
 * they would still need to acquire their respective PTE lock in order to set the age back to zero, which would greatly increase contention on the lock. However, in the real world,
 * the age bit is set to zero by the CPU when the address is accessed with no need for the PTE lock. Since I am unable to do it without the lock without creating race conditions,
 * I have not yet implemented a sophisticated aging scheme. We do still make some efforts to trim pages that are unlikely to be accessed again via faulters trimming behind,
 * but that is all.
 * 
 * We are able to do at least some rudimentary aging by using an access bit in the PTE
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

        // Outdated, commenting out for now but leaving for reference
        #if 0
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
        #endif
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
 * Handles the organization and trimming for a single PTE, whose copy has already been
 * verified to be in **memory format** and whose "being changed" bit is **not set**. When this is the case,
 * the probability is very high that we will be able to mark this PTE for trimming.
 * 
 * If the "trim_accessed_ptes" boolean is FALSE, then we will only unset the access bit. Otherwise,
 * we will mark the PTE for trimming. 
 * 
 * Returns TRUE if we marked the PTE for trimming, FALSE otherwise
 */
static BOOL handle_trim_single_pte(PTE* pte, PTE pte_copy, TRIM_INFORMATION* trim_information) {
    PTE pte_contents;
    PAGE* curr_page;

    pte_contents = pte_copy;
    pte_contents.memory_format.being_changed = VALID_PTE_BEING_CHANGED;

    /**
     * We have to set the "being_changed" bit in the valid PTE. If someone else (such as the faulter trying to set WRITE permissions)
     * sets it first, then we ignore it.
     */
    if (InterlockedCompareExchange64((ULONG64*) pte, pte_contents.complete_format, pte_copy.complete_format) != pte_copy.complete_format) {
        return FALSE;
    }

    // If we are NOT trimming accessed PTEs, then we should remove the access bit.
    // Otherwise, we do not care as we are going to trim it anyway
    if (trim_information->trim_accessed_ptes == FALSE && pte_copy.memory_format.access_bit == PTE_ACCESSED) {
        // Since the InterlockedCompareExchange earlier succeeded, this must be set
        pte_copy.memory_format.being_changed = VALID_PTE_BEING_CHANGED;

        pte_contents.memory_format.being_changed = VALID_PTE_NOT_BEING_CHANGED;
        pte_contents.memory_format.access_bit = PTE_NOT_ACCESSED;

        /**
         * We need to force our write here due to potential race conditions with other threads trying to set the access bit and
         * accidentally leaving the "being_changed" bit set.
         * 
         * BW: This might not be 100% necessary and we might be able to get away with a simple write_pte_contents...
         */
        while (InterlockedCompareExchange64((ULONG64*) pte, pte_contents.complete_format, pte_copy.complete_format) != pte_copy.complete_format) {
            pte_copy = read_pte_contents(pte);
        }

        return FALSE;
    }
    
    // At this point, the PTE will be trimmed
    trim_information->pte_storage[trim_information->total_num_ptes] = pte;
    trim_information->trim_addresses[trim_information->total_num_ptes] = pte_to_va(pte);

    curr_page = pfn_to_page(pte_copy.memory_format.frame_number);

    custom_spin_assert(curr_page->status == ACTIVE_STATUS);

    // If the page still has a pagefile index stored within it, then we can trim it straight to standby
    if (curr_page->pagefile_idx == DISK_IDX_NOTUSED) {
        if (curr_page->writing_to_disk == PAGE_BEING_WRITTEN && curr_page->modified == PAGE_NOT_MODIFIED) {

            /**
             * 
             * I want to avoid holding the pagelock at all. However, without the pagelock, I cannot coordinate
             * with the modified writer for pages that are currently being written to disk.
             * 
             * However, if we grab the pagelock and check again, we can be certain that we put the page in the correct list
             * 
             * Fortunately, this will happen rarely and the collisions with the modified writer will be even rarer,
             * but this still needs to be addressed.
             * 
             */
            acquire_pagelock(curr_page, 90);

            if (curr_page->writing_to_disk == PAGE_BEING_WRITTEN && curr_page->modified == PAGE_NOT_MODIFIED) {
                curr_page->status = MODIFIED_STATUS;
            } else if (curr_page->pagefile_idx != DISK_IDX_NOTUSED) {
                trim_information->pages_to_standby[trim_information->num_to_standby] = curr_page;
                trim_information->num_to_standby++;
            } else {
                trim_information->pages_to_modified[trim_information->num_to_modified] = curr_page;
                trim_information->num_to_modified++;
            }

            release_pagelock(curr_page, 91);

        } else {
            trim_information->pages_to_modified[trim_information->num_to_modified] = curr_page;
            trim_information->num_to_modified++;
        }
        
    } else {
        trim_information->pages_to_standby[trim_information->num_to_standby] = curr_page;
        trim_information->num_to_standby++;
    }

    trim_information->total_num_ptes++;

    return TRUE;
}


/**
 * Takes all of the PTEs from the thread local storages from trimming behind
 * and stores them in the given buffer (should be the malloc'd buffer for the trimmer)
 * 
 * Returns the number of PTEs written into the storage. Assumes that the malloc'd buffer has enough room
 * to account for **all** of the threads' buffers being full
 */
static void get_ptes_from_trim_behind(TRIM_INFORMATION* trim_information) {
    THREAD_TRIM_RESOURCES* user_thread_resources;
    PTE** thread_pte_storage;
    ULONG64 num_ptes_found_total = 0;
    ULONG64 num_ptes_found_thread;
    ULONG64 num_candidates_invalidated_thread;
    PTE* curr_pte;
    PTE pte_copy;
    BOOL trimmed_pte;
    PTE_LOCKSECTION* pte_locksection;

    for (ULONG64 thread_idx = 0; thread_idx < thread_information.num_usermode_threads; thread_idx++) {
        user_thread_resources = &thread_information.thread_local_storages[thread_idx].trim_resources;
        thread_pte_storage = user_thread_resources->pte_storage;
        num_ptes_found_thread = 0;
        num_candidates_invalidated_thread = 0;

        // Not worth our time
        if (user_thread_resources->num_ptes_in_buffer < THREAD_TRIM_STORAGE_SIZE / 4) {
            continue;
        }

        for (ULONG64 i = 0; i < THREAD_TRIM_STORAGE_SIZE; i++) {
            curr_pte = thread_pte_storage[i];

            if (curr_pte == NULL) {
                continue;
            }

            pte_copy = read_pte_contents(curr_pte);

            if (is_memory_format(pte_copy) == FALSE) {
                thread_pte_storage[i] = NULL;

                num_candidates_invalidated_thread++;

                continue;
            }

            // If the PTE is being changed, we should ignore it as it is having its permissions changed by a faulter
            if (pte_copy.memory_format.being_changed == VALID_PTE_BEING_CHANGED) {
                thread_pte_storage[i] = NULL;

                num_candidates_invalidated_thread++;

                continue;
            }
            
            // Try to trim the PTE
            if (handle_trim_single_pte(curr_pte, pte_copy, trim_information)) {
                num_ptes_found_thread++;
                pte_locksection = pte_to_locksection(curr_pte);

                InterlockedDecrement64(&pte_locksection->valid_pte_count);
                thread_pte_storage[i] = NULL;   
            }

        }

        InterlockedAdd64(&user_thread_resources->num_ptes_in_buffer, - (num_ptes_found_thread + num_candidates_invalidated_thread));
    }  

}


/**
 * Tries to mark the PTEs in a single PTE section for trimming.
 * 
 * Assumes that there is enough room in the trim_information's buffer to store all of the PTEs and pages
 * that can be obtained from a single section. We also subtract the number of PTEs that are successfully marked
 * from the locksection's valid_pte_count.
 */
static void get_ptes_from_pte_section(TRIM_INFORMATION* trim_information, ULONG64 pte_section) {
    PTE_LOCKSECTION* pte_locksection = &pagetable->pte_locksections[pte_section];
    ULONG64 num_marked_to_trim = 0;
    PTE* curr_pte;
    PTE pte_copy;
    
    // Section num * ptes_per_section = index of starting PTE in given section
    ULONG64 section_start = pte_section * trim_information->ptes_per_locksection;

    for (ULONG64 pte_idx = section_start; pte_idx < section_start + trim_information->ptes_per_locksection; pte_idx++) {

        // There are no more PTEs left in this section for us to trim
        if (pte_locksection->valid_pte_count == num_marked_to_trim) {
            break;
        }

        curr_pte = &pagetable->pte_list[pte_idx];
        pte_copy = read_pte_contents(curr_pte);

        // Ignore invalid PTEs
        if (is_memory_format(pte_copy) == FALSE) {
            continue;
        }

        // PTE has already been marked to be trimmed or we are colliding with a faulting thread
        if (pte_copy.memory_format.being_changed == VALID_PTE_BEING_CHANGED) {
            continue;
        }

        // Try to trim the single PTE
        if (handle_trim_single_pte(curr_pte, pte_copy, trim_information)) {
            num_marked_to_trim++;
        }

        // The maximum to trim in a section
        if (num_marked_to_trim == TRIM_PER_SECTION) {
            break;
        }
    }

    if (num_marked_to_trim > 0) {
        InterlockedAdd64(&pte_locksection->valid_pte_count, - num_marked_to_trim);
    }
}


/**
 * Initializes all of the trim information datastructure. Returns it upon success, or NULL upon failure
 * 
 * Failure here is fatal for the simulation.
 */
static TRIM_INFORMATION* init_trim_information() {
    // We want all of the values to start off as 0
    TRIM_INFORMATION* trim_information = (TRIM_INFORMATION*) calloc(1, sizeof(TRIM_INFORMATION));

    ULONG64 buffer_size = (THREAD_TRIM_STORAGE_SIZE * thread_information.num_usermode_threads) + TRIM_PER_SECTION;

    if (trim_information == NULL) {
        return NULL;
    }

    trim_information->pages_to_modified = (PAGE**) malloc(sizeof(PAGE*) * buffer_size);

    if (trim_information->pages_to_modified == NULL) {
        return NULL;
    }

    trim_information->pages_to_standby = (PAGE**) malloc(sizeof(PAGE*) * buffer_size);

    if (trim_information->pages_to_standby == NULL) {
        return NULL;
    }

    trim_information->pte_storage = (PTE**) malloc(sizeof(PTE*) * buffer_size);

    if (trim_information->pte_storage == NULL) {
        return NULL;
    }

    trim_information->trim_addresses = (PULONG_PTR*) malloc(sizeof(PULONG_PTR) * buffer_size);

    if (trim_information->trim_addresses == NULL) {
        return NULL;
    }

    trim_information->ptes_per_locksection = pagetable->num_virtual_pages / pagetable->num_locks;

    return trim_information;
}


/**
 * Thread dedicated to everything trimming related.
 * 
 * Takes valid PTEs off of buffers from threads trimming behind themselves, as well as PTEs straight off
 * the pagetable to unmap and put their pages on the modified/standby list depending on whether or not
 * they have been modified.
 * 
 * Has a limited aging implementation, where we use the access bit to sometimes avoid trimming PTEs and unset this bit
 * when we come across it.
 */
LPTHREAD_START_ROUTINE thread_trimming(void* parameters) {
    WORKER_THREAD_PARAMETERS* thread_params = (WORKER_THREAD_PARAMETERS*) parameters;
    ULONG64 worker_thread_idx = thread_params->thread_idx;

    #if DEBUG_THREAD_STORAGE
    thread_information.thread_local_storages[worker_thread_idx].thread_id = GetCurrentThreadId();
    #endif


    /**
     * We use large buffers for handling trimming behind as well as trimming each section
     */
    TRIM_INFORMATION* trim_information = init_trim_information();

    if (trim_information == NULL) {
        fprintf(stderr, "Failed to allocate memory for trimming datastructures\n");
        return NULL;
    }

    PTE* curr_pte;
    PTE_LOCKSECTION* curr_pte_locksection;
    PAGE* curr_page = NULL;
    PTE pte_contents;
    PTE pte_copy;

    BOOL signal_modified;

    HANDLE events[2];
    ULONG64 signaled_event;

    BOOL trim_accessed_ptes;

    events[0] = trimming_event;
    events[1] = shutdown_event;

    while(TRUE) {
        signaled_event = WaitForMultipleObjects(2, events, FALSE, INFINITE);

        if (signaled_event == 1) {
            return NULL;
        }

        trim_update_thread_storages(TRIMMER_NOT_SIGNALLED);

        signal_modified = FALSE;

        /**
         * Note: we used to need the locks to edit the PTEs here, but these sections are still
         * how all of the PTEs are organized. One day this can be transitioned into a multi-level
         * pagetable (and then this would be the lowest level, rather than a "PTE section")
         */
        for (ULONG64 lock_section = 0; lock_section < pagetable->num_locks; lock_section++) {   

            curr_pte_locksection = &pagetable->pte_locksections[lock_section];

            // Ignore invalid PTE sections  
            if (curr_pte_locksection->valid_pte_count == 0) continue;

            trim_information->num_to_modified = 0;
            trim_information->num_to_standby = 0;
            trim_information->total_num_ptes = 0;

            // If we really need pages, we should trim PTEs that have the access bit set
            if (total_available_pages < physical_page_count / TRIM_ACCESSED_PTES_PROPORTION) {
                trim_information->trim_accessed_ptes = TRUE;
            } else {
                trim_information->trim_accessed_ptes = FALSE;
            }

            get_ptes_from_trim_behind(trim_information);

            get_ptes_from_pte_section(trim_information, lock_section);

            // We failed to mark any PTEs for trimming
            if (trim_information->total_num_ptes == 0) {
                continue;
            }

            disconnect_va_batch_from_cpu(trim_information->trim_addresses, trim_information->total_num_ptes);

            /**
             * Now, we add all of our pages to the modified list or standby list depending on whether
             * or not they have a disk index already
             */
            
            PAGE* beginning_of_section;
            PAGE* end_of_section;

            if (trim_information->num_to_modified > 0) {
                create_chain_of_pages(trim_information->pages_to_modified, trim_information->num_to_modified, MODIFIED_STATUS);
                
                beginning_of_section = trim_information->pages_to_modified[0];
                end_of_section = trim_information->pages_to_modified[trim_information->num_to_modified - 1];
                
                insert_page_section_no_release(modified_list, beginning_of_section, end_of_section, trim_information->num_to_modified);

                signal_modified = TRUE;
               
                SetEvent(modified_writer_event);
            }   


            if (trim_information->num_to_standby > 0) {
                BOOL signal_waiting_for_pages_event;

                create_chain_of_pages(trim_information->pages_to_standby, trim_information->num_to_standby, STANDBY_STATUS);

                beginning_of_section = trim_information->pages_to_standby[0];
                end_of_section = trim_information->pages_to_standby[trim_information->num_to_standby - 1];
               
                // We only want to set the event if we desperately need pages, and we check before we modify the global
                signal_waiting_for_pages_event = (total_available_pages == 0);
                
                InterlockedAdd64(&total_available_pages, trim_information->num_to_standby);

                insert_page_section_no_release(standby_list, beginning_of_section, end_of_section, trim_information->num_to_standby);

                if (signal_waiting_for_pages_event == TRUE) {
                    SetEvent(waiting_for_pages_event);
                }
            }

            // Reset the PTE contents so that we can use it to change the PTEs into transition format
            pte_contents.complete_format = 0;
            pte_contents.transition_format.is_transition = 1;

            /**
             * Only now that the pages are in the correct lists can we change the PTEs to be in transition format.
             * If we did this too early, then transition PTEs would try to grab and remove pages from lists that they aren't in yet
             */
            for (ULONG64 i = 0; i < trim_information->total_num_ptes; i++) {
                curr_pte = trim_information->pte_storage[i];
                pte_copy = read_pte_contents(curr_pte);
                pte_contents.transition_format.frame_number = pte_copy.memory_format.frame_number;

                /**
                 * While we **do** want to force our writes when we conflict with a thread setting the access bit, 
                 * we **do not** want to force writes against anyone setting the PTE to be in disk format.
                 * 
                 * This is possible if we add a page to the standby list and then it is repurposed before we are able to set it
                 * 
                 */
                if (is_disk_format(pte_copy)) {
                    continue;
                }

                while (InterlockedCompareExchange64((ULONG64*) curr_pte, pte_contents.complete_format, pte_copy.complete_format) != pte_copy.complete_format) {
                    pte_copy = read_pte_contents(curr_pte);

                    if (is_disk_format(pte_copy)) {
                        break;
                    }
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
 * Does NOT distinguish between accessed / unaccessed valid PTEs. We are trimming behind ourselves because
 * we are speculating that these PTEs will no longer be accessed anymore (as they were recently sequentially accessed).
 * 
 * We consider only a maximum of FAULTER_TRIM_BEHIND_NUM pages. Furthermore, we do not actually trim the PTEs to completion-
 * instead, we put them on a buffer for the trimming thread to handle. This saves the faulting thread a considerable amount of time.
 */
void faulter_trim_behind(PTE* accessed_pte, ULONG64 thread_idx) {
    ULONG64 accessed_pte_index = ((ULONG64) accessed_pte - (ULONG64) pagetable->pte_list) / sizeof(PTE);
    PTE_LOCKSECTION* curr_pte_locksection = pte_to_locksection(accessed_pte);
    PTE* curr_pte;
    THREAD_TRIM_RESOURCES* trim_resources = &thread_information.thread_local_storages[thread_idx].trim_resources;

    // We do not have the room to store potential candidates
    if (trim_resources->num_ptes_in_buffer == THREAD_TRIM_STORAGE_SIZE) {
        return;
    }

    ULONG64 min_index;
    PTE pte_copy;   
    ULONG64 curr_pfn;

    // Ensure we do not underflow when we are going down the pagetable
    if (accessed_pte_index == 0) {
        return;
    } else if (accessed_pte_index - 1 < FAULTER_TRIM_BEHIND_NUM) {
        min_index = 0;
    } else {
        min_index = accessed_pte_index - 1 - FAULTER_TRIM_BEHIND_NUM;
    }

    // Now the PTEs can no longer change from under us - so we are free to change anything
    for (ULONG64 pte_idx = accessed_pte_index - 1; pte_idx > min_index; pte_idx--) {
        curr_pte = &pagetable->pte_list[pte_idx];
        pte_copy = read_pte_contents(curr_pte);
        
        if (is_memory_format(pte_copy) == FALSE) {
            continue;
        }

        // Addresses the race condition of the trimming thread already modifying this PTE, 
        // or that it was an unaccessed PTE whose first pagefault is being resolved
        if (pte_copy.memory_format.being_changed == VALID_PTE_BEING_CHANGED) {
            continue;
        }

        trim_resources->pte_storage[trim_resources->curr_idx] = curr_pte;

        InterlockedIncrement64(&trim_resources->num_ptes_in_buffer);

        trim_resources->curr_idx = (trim_resources->curr_idx + 1) % THREAD_TRIM_STORAGE_SIZE;

        // We can no longer store any more PTEs
        if (trim_resources->num_ptes_in_buffer == THREAD_TRIM_STORAGE_SIZE) {
            break;
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
        /**
         * If the PTE is in memory format, it has the "being-changed" bit set - and it was a previously
         * an unaccessed PTE whose fault we are resolving. 
         */
        if (access_type == WRITE_ACCESS || is_memory_format(read_pte_contents(original_accessed_pte))) {
            permission_storage[0] = PAGE_READWRITE;
        } else {
            permission_storage[0] = PAGE_READONLY;
        }
        start_index = 1;
    }

    // Determine the permissions for the other PTEs
    for (ULONG64 i = start_index; i < num_ptes; i++) {
        curr_pte = ptes_in_question[i];

        /**
         * If the PTE is in memory format, it has the "being-changed" bit set - and it was a previously
         * an unaccessed PTE who we are speculatively bring in. Since it cannot have any pagefile space preserved,
         * we set its permissions to READWRITE to avoid an extra fault if we write to the PTE.
         */
        if (is_memory_format(read_pte_contents(curr_pte))) {
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
    pte_contents.memory_format.access_bit = PTE_ACCESSED;
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

    // TODO: Decide whether only the first PTE should have the access bit set, or all of them
    pte_contents.memory_format.access_bit = PTE_ACCESSED;


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
    PTE pte_copy;

    for (ULONG64 i = 0; i < num_ptes; i++) {
    
        curr_pte = ptes_to_connect[i];
        pte_copy = read_pte_contents(curr_pte);

        pte_locksection = pte_to_locksection(curr_pte);

        if (permissions[i] == PAGE_READONLY) {
            pte_contents.memory_format.protections = PTE_PROTREAD;
        } else {
            pte_contents.memory_format.protections = PTE_PROTREADWRITE;
        }

        pte_contents.memory_format.frame_number = pfns[i];

        InterlockedIncrement64(&pte_locksection->valid_pte_count);

        /**
         * A faulting thread could have actually set the access bit before we were even able to get here,
         * so we need to ensure that the "being_changed" bit is unset. Otherwise, there is a possibility
         * that the "being_changed" bit would be set forever and the PTE would be locked in valid format.
         */
        while (InterlockedCompareExchange64((ULONG64*) curr_pte, pte_contents.complete_format, pte_copy.complete_format) != pte_copy.complete_format) {
            pte_copy = read_pte_contents(curr_pte);
        }
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


