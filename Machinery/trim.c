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
#include "../Datastructures/datastructures.h"
#include "./trim.h"


/**
 * Returns the frame number of the lowest page in the pagetable that is valid, makes the PTE invalid 
 * and communicates with the CPU
 * 
 * Returns either the frame number or ERROR otherwise
 */
ULONG64 steal_lowest_frame() {
    ULONG64 ptes_per_lock = pagetable->num_virtual_pages / pagetable->num_locks;
    ULONG64 section_start;

    // Go through each lock section and increment all valid PTEs
    for (ULONG64 lock_section = 0; lock_section < pagetable->num_locks; lock_section++) {
        section_start = lock_section * ptes_per_lock;

        EnterCriticalSection(&pagetable->pte_locks[lock_section]);
        for (ULONG64 pte_idx = section_start; pte_idx < section_start + ptes_per_lock; pte_idx++) {
            PTE* candidate_pte = &pagetable->pte_list[pte_idx];

            if (! is_memory_format(*candidate_pte)) continue;

            // We have found our victim to steal from
            ULONG64 pfn = candidate_pte->memory_format.frame_number;

            PULONG_PTR pte_va = pte_to_va(candidate_pte);

            ULONG64 disk_idx = 0;

            if (write_to_disk(candidate_pte, &disk_idx) == ERROR) {
                fprintf(stderr, "Unable to get a disk index in steal_lowest_frame\n");

                LeaveCriticalSection(&pagetable->pte_locks[lock_section]);
                return ERROR;
            }

            // Unmap the CPU
            if (MapUserPhysicalPages (pte_va, 1, NULL) == FALSE) {

                fprintf (stderr, "full_virtual_memory_test : could not unmap VA %p\n", pte_va);

                LeaveCriticalSection(&pagetable->pte_locks[lock_section]);
                return ERROR;
            }

            // Disconnect the PTE
            candidate_pte->disk_format.always_zero = 0;
            candidate_pte->disk_format.pagefile_idx = disk_idx;
            candidate_pte->disk_format.on_disk = 1;

            LeaveCriticalSection(&pagetable->pte_locks[lock_section]);
            return pfn;
        }
        LeaveCriticalSection(&pagetable->pte_locks[lock_section]);
    }

    // No frame to steal was ever found
    return ERROR;
}


/**
 * Thread dedicated to trimming PTEs from the pagetable and putting them on the modified list
 */
void thread_trimming() {
    ULONG64 ptes_per_lock = pagetable->num_virtual_pages / pagetable->num_locks;
    ULONG64 section_start;
    PTE* curr_pte;
    PAGE* curr_page;
    ULONG64 final_lock_section;
    ULONG64 curr_lock_section = 0;


    while(TRUE) {
        WaitForSingleObject(trimming_event, INFINITE);

        // Go through each lock section and increment all valid PTEs
        for (ULONG64 lock_section = 0; lock_section < pagetable->num_locks; lock_section++) {            
            section_start = lock_section * ptes_per_lock;

            // Ignore invalid PTE sections
            if (pagetable->valid_pte_counts[lock_section] == 0) continue;

            EnterCriticalSection(&pagetable->pte_locks[lock_section]);
            for (ULONG64 pte_idx = section_start; pte_idx < section_start + ptes_per_lock; pte_idx++) {
                curr_pte = &pagetable->pte_list[pte_idx];
                // Ignore invalid PTEs
                if (! is_memory_format(*curr_pte)) continue;

                curr_page = pfn_to_page(curr_pte->memory_format.frame_number);

                // Unmaps from CPU and decrements its valid_pte_count section
                //BW: Later can have a list of PTEs to pass to this to unmap several at once
                disconnect_pte_from_cpu(curr_pte);

                curr_pte->transition_format.always_zero = 0;
                curr_pte->transition_format.always_zero2 = 0;

                //BW: Can maybe leave pte lock here in the meantime?  
                //      - as of now, no, as we have already set up the transition PTE  
                EnterCriticalSection(&modified_list->lock);
                modified_add_page(curr_page, modified_list);
                LeaveCriticalSection(&modified_list->lock);
                
                break;
            }
            LeaveCriticalSection(&pagetable->pte_locks[lock_section]);
        }

        // Signal that the modified list should be populated
        SetEvent(pagetable_to_modified_event);
    }
}

/**
 * Thread dedicated to aging all of the valid PTEs in the pagetable
 */
void thread_aging() {
    ULONG64 ptes_per_lock = pagetable->num_virtual_pages / pagetable->num_locks;
    ULONG64 section_start;

    while(TRUE) {
        WaitForSingleObject(aging_event, INFINITE);
        
        // Go through each lock section and increment all valid PTEs
        for (ULONG64 lock_section = 0; lock_section < pagetable->num_locks; lock_section++) {
            section_start = lock_section * ptes_per_lock;

            EnterCriticalSection(&pagetable->pte_locks[lock_section]);
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
            LeaveCriticalSection(&pagetable->pte_locks[lock_section]);
        }
    }
}


/**
 * Thread dedicated to going through the pagetable and putting high-age PTEs on the modified list
 */
void thread_pagetable_to_modified() {
    ULONG64 ptes_per_lock = pagetable->num_virtual_pages / pagetable->num_locks;
    ULONG64 section_start;

    while(TRUE) {
        WaitForSingleObject(pagetable_to_modified_event, INFINITE);

        /**
         * FOR EACH PAGETABLE LOCK SECTION:
            * 1. Enter each pagetable lock
            * 
            * 2. Enter modified lock (note difference here - we don't always hold mod lock)
            * 
            * 3. Get high age PTEs, find their pages, turn them into modified status, add to modified list
            * 
            * 4. Leave modified lock
            * 
            * 5. Leave pagetable lock
            * 
         *  - Consider stopping early if we get enough pages to the modified list (maybe a global?)
         *              - in which case, we should start at the following pte lock section
         */

    }
}


/**
 * Thread dedicated to writing pages from the modified list to disk, putting finally adding the pages to standby
 */
#define MAX_PAGES_TO_WRITE 8
void thread_modified_to_standby() {
    // ULONG64 ptes_per_lock = pagetable->num_virtual_pages / pagetable->num_locks;
    ULONG64 section_start;
    ULONG64 num_to_write;
    CRITICAL_SECTION* pte_lock;
    BOOL pte_lock_status;
    PAGE* potential_page;
    PTE* relevant_PTE;

    // PAGE* pages_to_move = (PAGE*) malloc(sizeof(PAGE) * MAX_PAGES_TO_WRITE);
    // ULONG64 curr_successful_page;

    // Ask about if I need this initialized to zero to write to it
    ULONG64 disk_storage_idx = 0;

    while(TRUE) {
        WaitForSingleObject(modified_to_standby_event, INFINITE);

        EnterCriticalSection(&modified_list->lock);

        // num_to_write = min(MAX_PAGES_TO_WRITE, modified_list->list_length);
        num_to_write = modified_list->list_length;
        // curr_successful_page = 0;

        for (ULONG64 curr_page = 0; curr_page < num_to_write; num_to_write++) {
            /**
             * Note that another thread may be trying to access this exact PTE and rescue it from the modified list,
             * so if we fail to acquire this PTE lock then we should throw this page at the back of the modified list
             * and continue (the loop might end here, too)
             */

            potential_page = (PAGE*) modified_pop_page(modified_list);

            // Since we release the modified list lock at the end, someone else could come in and rescue pages from the
            // modified list, emptying it, meaning the work here is done until we are signaled again
            if (potential_page == NULL) break;

            relevant_PTE = potential_page->modified_page.pte;  

            pte_lock = pte_to_lock(relevant_PTE);

            pte_lock_status = TryEnterCriticalSection(pte_lock);

            // Someone else is trying to use this PTE, or one in its section, so back off and put it at the back
            if (pte_lock_status == FALSE) {
                modified_add_page(potential_page, modified_list);
                continue;
            }  

            // Write the PTE to the disk [disk locks for idx sections needed]
            if (write_to_disk(relevant_PTE, disk_storage_idx) == ERROR) {
                fprintf(stderr, "Failed to write pte to disk in thread_modified_to_standby\n");
                modified_add_page(potential_page, modified_list);
                LeaveCriticalSection(pte_lock);
                continue;
            }

            potential_page->standby_page.status = STANDBY_STATUS;
            potential_page->standby_page.pagefile_idx = disk_storage_idx;

            // Wait times for standby list should be brief, but we do not need the modified list lock in the meantime
            // so someone else can try to add to or rescue pages from it
            LeaveCriticalSection(&modified_list->lock);

            EnterCriticalSection(&standby_list->lock);
            standby_add_page(potential_page, standby_list);
            LeaveCriticalSection(&standby_list->lock);

            // BW: Edit PTE?
            LeaveCriticalSection(pte_lock);

            EnterCriticalSection(&modified_list->lock);
        }

        LeaveCriticalSection(&modified_list->lock);
    }
}


#if 0
/**
 * Returns the frame number of the lowest page in the pagetable that is valid, makes the PTE invalid 
 * and communicates with the CPU
 * 
 * Returns either the frame number or ERROR otherwise
 */
ULONG64 trimming_thread() {
    while (TRUE) {
        //BW: Wait for event signal...
        PAGETABLE* pagetable;

        EnterCriticalSection(&pagetable->pte_lock);
        for (ULONG64 pte_idx = 0; pte_idx < pagetable->num_virtual_pages; pte_idx++) {
            PTE* candidate_pte = &pagetable->pte_list[pte_idx];

            // See if the pte is currently linked to a physical frame that we can take
            if (is_memory_format(*candidate_pte)) {

                // We have found our victim to steal from
                ULONG64 pfn = candidate_pte->memory_format.frame_number;

                PULONG_PTR pte_va = pte_to_va(*pagetable, candidate_pte);

                // Unmap the CPU
                if (MapUserPhysicalPages (pte_va, 1, NULL) == FALSE) {

                    fprintf (stderr, "full_virtual_memory_test : could not unmap VA %p\n", pte_va);
                    assert(FALSE);
                    return ERROR;
                }

                // Disconnect the PTE
                candidate_pte->memory_format.valid = INVALID;

                //BW: Insert page into the modified list instead
                // Also will want to check if we have enough pfns added to the modified list 
                // to see if we continue or break
            }
        }

        LeaveCriticalSection(&pagetable->pte_lock);
    }
}
#endif

/**
 * Connects the given PTE to the open page's physical frame and alerts the CPU
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 */
int connect_pte_to_page(PTE* pte, PAGE* open_page) {
    if (pte == NULL || open_page == NULL) {
        fprintf(stderr, "NULL PTE or open page givne to connect_pte_to_page\n");
        return ERROR;
    }
    
    ULONG64 base_address_pte_list = (ULONG64) pagetable->pte_list;
    ULONG64 pte_address = (ULONG64) pte;
    ULONG64 pte_index = (pte_address - base_address_pte_list) / sizeof(PTE);
    LONG64 lock_index = pte_index / (pagetable->num_virtual_pages / pagetable->num_locks);

    pagetable->valid_pte_counts[lock_index] += 1;

    ULONG64 pfn = page_to_pfn(open_page);
    
    pte->memory_format.age = 0;
    pte->memory_format.frame_number = pfn;
    pte->memory_format.valid = VALID;

    // Map the CPU
    if (MapUserPhysicalPages (pte_to_va(pte), 1, &pfn) == FALSE) {

        fprintf (stderr, "full_virtual_memory_test : could not unmap VA %p\n", pte_to_va(pte));
        return ERROR;
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

    ULONG64 base_address_pte_list = (ULONG64) pagetable->pte_list;
    ULONG64 pte_address = (ULONG64) pte;
    ULONG64 pte_index = (pte_address - base_address_pte_list) / sizeof(PTE);
    LONG64 lock_index = pte_index / (pagetable->num_virtual_pages / pagetable->num_locks);

    pagetable->valid_pte_counts[lock_index] -= 1;

    // Unmap the CPU
    if (MapUserPhysicalPages (pte_to_va(pte), 1, NULL) == FALSE) {

        fprintf (stderr, "full_virtual_memory_test : could not unmap VA %p\n", pte_to_va(pte));

        DebugBreak();
        return ERROR;
    }

    return SUCCESS;
}


/**
 * Writes the given PTE to the disk, and stores the resulting disk_idx in disk_idx_storage
 * 
 * Returns the disk index if there are no issues, ERROR otherwise
 */
int write_to_disk(PTE* pte, ULONG64* disk_idx_storage) {
    if (pte == NULL) {
        fprintf(stderr, "NULL pte given to write_to_disk\n");
        return ERROR;
    }

    //BW: We could also have a race condition where two threads are writing to the disk at the same time,
    // that scenario may change this line to checking if the PTE is also already in disk format
    if (is_transition_pte(*pte) == FALSE) {
        fprintf(stderr, "Incorrect memory format of PTE given in write_to_disk\n");
        return ERROR;
    }
    
    // PTE has already been written to disk
    // if (is_disc_format(pte)) {
    //     return SUCCESS;
    // }

    ULONG64 disk_idx = 0;

    if (allocate_disk_slot(&disk_idx) == ERROR) {
        fprintf(stderr, "Unable to write to disk as there are no slots remaining\n");
        return ERROR;
    }

    disk->disk_slot_statuses[disk_idx] = DISK_USEDSLOT;

    PULONG_PTR pte_va = pte_to_va(pte);

    *pte_va = (ULONG64) pte_va;

    if (pte_va == NULL) {
        fprintf(stderr, "Error getting pte VA in write_to_disk\n");
        return ERROR;
    }

    PULONG_PTR disk_slot_addr = disk_idx_to_addr(disk_idx);

    // Copy the memory over to the slot
    memcpy(disk_slot_addr, pte_va, PAGE_SIZE);

    *disk_idx_storage = disk_idx;

    return SUCCESS;
}


/**
 * A thread dedicated to writing the given page to the disk. Writes the resulting
 * disk storage index into the given pointer disk_idx_storage.
 * 
 * Returns SUCCESS if we write the page to disk, ERROR otherwise
 */
int thread_worker_write_to_disk(PAGE* transition_page, ULONG64* disk_idx_storage) {
    if (transition_page == NULL || disk_idx_storage == NULL) {
        fprintf(stderr, "NULL transition page or disk_idx_storage given to thread_worker_write_to_disk\n");
        return ERROR;
    }

    /**
     * We need to find an open disk write slot VA, followed by mapping the pfn from this
     * page to it. This allows us to map the contents of the pfn from the temporary slot
     * to the disk without allowing the user to access the PTE and modify its contents.
     */
    DISK_RW_SLOT* write_slot;

    EnterCriticalSection(&disk->disk_write_list_lock);

    // We need to try until we get a slot available
    while ((write_slot = db_pop_from_tail(disk->disk_write_listhead)) == NULL) {
        LeaveCriticalSection(&disk->disk_write_list_lock);

        WaitForSingleObject(disk_write_available_event, INFINITE);

        EnterCriticalSection(&disk->disk_write_list_lock);
    }
    LeaveCriticalSection(&disk->disk_write_list_lock);
    

    ULONG64 pfn = page_to_pfn(transition_page);

    // At this point, we know that we have a slot to write to
    // Map the CPU
    if (MapUserPhysicalPages (write_slot->rw_address, 1, &pfn) == FALSE) {
        fprintf (stderr, "thread_worker_write_to_disk : could not unmap VA %p\n", write_slot->rw_address);
        DebugBreak();
        return ERROR;
    }

    // We need to now get an open disk slot...
    ULONG64 disk_storage_slot;

    if (allocate_disk_slot(&disk_storage_slot) == ERROR) {
        fprintf(stderr, "Failed to get disk slot in thread_worker_write_to_disk\n");
        DebugBreak();
        return ERROR;
    }

    // Copy from the temporary slot to the disk
    memcpy(disk_storage_slot, write_slot->rw_address, PAGE_SIZE);

    // Add the write slot back to the list, and signal anyone waiting
    EnterCriticalSection(&disk->disk_write_list_lock);
    db_insert_node_at_head(disk->disk_write_listhead, write_slot->listnode);
    SetEvent(disk_write_available_event);
    LeaveCriticalSection(&disk->disk_write_list_lock);

    // Store the disk storage slot into the given idx storage
    *disk_idx_storage = disk_storage_slot;

    return SUCCESS;
}


/**
 * Fetches the memory for the given PTE on the disk, 
 * assuming the PTE's virtual address has already been mapped to a valid physical frame
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 */
int get_from_disk(PTE* pte) {
    if (pte == NULL) {
        fprintf(stderr, "NULL pte given to get_from_disk");
        return ERROR;
    }

    if (is_disk_format(*pte) == FALSE) {
        fprintf(stderr, "PTE is not in disk format in get_from_disk\n");
        return ERROR;
    }

    ULONG64 disk_idx = pte->disk_format.pagefile_idx;

    PULONG_PTR disk_slot_addr = disk_idx_to_addr(disk_idx);

    PULONG_PTR pte_va = pte_to_va(pte);

    memcpy(pte_va, disk_slot_addr, (size_t) PAGE_SIZE);

    if (return_disk_slot(disk_idx) == ERROR) {
        fprintf(stderr, "Failed to return disk slot to the disk\n");
        return ERROR;
    }

    // Now that the data is copied, we can update the actual PTE
    // pte->memory_format.age = 0;
    // pte->memory_format.frame_number = pfn;
    // pte->memory_format.valid = VALID;

    return SUCCESS;
}


/**
 * Writes an open disk idx into the result storage pointer and sets the disk slot to DISK_USEDSLOT
 * 
 * Returns SUCCESS if we successfully wrote a disk idx, ERROR otherwise (may be empty)
 */
int allocate_disk_slot(ULONG64* result_storage) {
    //
    // if (disk->num_open_slots == 0) {
    //     return ERROR;
    // }
    ULONG64 section_start;
    ULONG64 open_disk_idx;
    BOOL lock_result; 
    ULONG64 disk_slots_per_lock = DISK_STORAGE_SLOTS / disk->num_locks;

    // Go through each lock section and **try** to enter the critical sections
    for (ULONG64 lock_section = 0; lock_section < disk->num_locks; lock_section++) {
        section_start = lock_section * disk_slots_per_lock;

        /**
         * First, we only TRY to enter the critical sections - but then we wait the second time around
         * 
         * This ensures faster response times in ideal cases, and could spread out the disk slot allocation across
         * multiple lock sections, hopefully reducing contension for locks 
         */
        
        lock_result = TryEnterCriticalSection(&disk->disk_slot_locks[lock_section]);

        if (lock_result == FALSE) {
            continue;
        }

        // Skip over empty disk sections
        if (disk->open_slot_counts[lock_section] == 0) {
            LeaveCriticalSection(&disk->disk_slot_locks[lock_section]);
            continue;
        }

        // Now we are guaranteed to find a disk slot somewhere in this section
        for (ULONG64 disk_idx = section_start; disk_idx < section_start + disk_slots_per_lock; disk_idx++) {
            if (disk->disk_slot_statuses[disk_idx] == DISK_USEDSLOT) continue;

            // Now we have a disk slot to use
            open_disk_idx = disk_idx;
            disk->disk_slot_statuses[disk_idx] = DISK_USEDSLOT;
            disk->open_slot_counts[lock_section] -= 1;
        }

        LeaveCriticalSection(&disk->disk_slot_locks[lock_section]);

        *result_storage = open_disk_idx;
        return SUCCESS;
    }

    /**
     * Now, we are forced to wait for disk lock sections
     * 
     */
    for (ULONG64 lock_section = 0; lock_section < disk->num_locks; lock_section++) {
        section_start = lock_section * disk_slots_per_lock;
        
        // This will block the thread until we get into the critical section, and is the only part
        // that differs from the previous loop
        EnterCriticalSection(&disk->disk_slot_locks[lock_section]);

        // Skip over empty disk sections
        if (disk->open_slot_counts[lock_section] == 0) {
            LeaveCriticalSection(&disk->disk_slot_locks[lock_section]);
            continue;
        }

        // Now we are guaranteed to find a disk slot somewhere in this section
        for (ULONG64 disk_idx = section_start; disk_idx < section_start + disk_slots_per_lock; disk_idx++) {
            if (disk->disk_slot_statuses[disk_idx] == DISK_USEDSLOT) continue;

            // Now we have a disk slot to use
            open_disk_idx = disk_idx;
            disk->disk_slot_statuses[disk_idx] = DISK_USEDSLOT;
            disk->open_slot_counts[lock_section] -= 1;
        }

        LeaveCriticalSection(&disk->disk_slot_locks[lock_section]);

        *result_storage = open_disk_idx;
        return SUCCESS;
    }

    // The disk is empty
    return ERROR;
}


/**
 * Modifies the bitmap on the disk to indicate the given disk slot is free
 * 
 * Returns SUCCESS upon no issues, ERROR otherwise
 */
int return_disk_slot(ULONG64 disk_idx) {
    if (disk_idx > DISK_STORAGE_SLOTS) {
        fprintf(stderr, "Invalid disk_idx given to return_disk_slot\n");
        return ERROR;
    }

    if (disk->disk_slot_statuses[disk_idx] == DISK_FREESLOT) {
        fprintf(stderr, "Already free disk idx given to return_disk_slot\n");
        return ERROR;
    }

    EnterCriticalSection(disk_idx_to_lock(disk_idx));

    disk->disk_slot_statuses[disk_idx] = DISK_FREESLOT;
    disk->open_slot_counts[disk_idx / (DISK_STORAGE_SLOTS / disk->num_locks)] += 1;

    LeaveCriticalSection(disk_idx_to_lock(disk_idx));


    return SUCCESS;
}