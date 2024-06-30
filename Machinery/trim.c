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

#if 0
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
#endif

/**
 * Thread dedicated to trimming PTEs from the pagetable and putting them on the modified list
 */
LPTHREAD_START_ROUTINE thread_trimming() {
    ULONG64 ptes_per_lock = pagetable->num_virtual_pages / pagetable->num_locks;
    ULONG64 section_start;
    PTE* curr_pte;
    PAGE* curr_page;
    ULONG64 final_lock_section;
    ULONG64 curr_lock_section = 0;

    while(TRUE) {
        WaitForSingleObject(trimming_event, INFINITE);
        // printf("Signalled to trim\n");

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

                // disconnect_pte_from_cpu(curr_pte);

                curr_page->modified_page.pte = curr_pte;

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
        SetEvent(modified_to_standby_event);
    }
}


/**
 * Thread dedicated to aging all of the valid PTEs in the pagetable
 */
LPTHREAD_START_ROUTINE thread_aging() {
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
        // printf("Successfully aged\n");
    }
}


/**
 * Thread dedicated to writing pages from the modified list to disk, putting finally adding the pages to standby
 */
#define MAX_PAGES_TO_WRITE 16
LPTHREAD_START_ROUTINE thread_modified_to_standby() {
    // Variables dedicated to finding and extracting pages to write from the modified list 
    ULONG64 section_start;
    ULONG64 num_to_write;
    CRITICAL_SECTION* pte_lock;
    BOOL pte_lock_status;
    PAGE* potential_page;
    // PAGE** pages_currently_writing[MAX_PAGES_TO_WRITE];
    PTE* relevant_PTE;
    DB_LL_NODE* curr_modified_node;
    ULONG64 disk_storage_idx;

    #if 0
    // Variables dedicated to handling splitting off threads to write pages disk
    ULONG64 num_writing_threads;
    ULONG64 disk_idx_storage_space[MAX_PAGES_TO_WRITE];
    HANDLE worker_writing_threads[MAX_PAGES_TO_WRITE];
    ULONG64 worker_thread_exit_codes[MAX_PAGES_TO_WRITE];
    #endif

    while(TRUE) {
        WaitForSingleObject(modified_to_standby_event, INFINITE);

        // num_writing_threads = 0;

        EnterCriticalSection(&modified_list->lock);

        num_to_write = min(MAX_PAGES_TO_WRITE, modified_list->list_length);
        
        
        curr_modified_node = modified_list->listhead->flink;

        for (ULONG64 curr_page = 0; curr_page < num_to_write; curr_page++) {
            
            /**
             * Note that another thread may be trying to access this exact PTE and rescue it from the modified list,
             * so if we fail to acquire this PTE lock then we should throw this page at the back of the modified list
             * and continue (the loop might end here, too)
             */
            
            // We CANNOT pop pages directly since they could be rescued.
            potential_page = (PAGE*) modified_pop_page(modified_list);

            // Since we release the modified list lock at the end, someone else could come in and rescue pages from the
            // modified list, emptying it, meaning the work here is done until we are signaled again
            if (potential_page == NULL) break;


            if (write_to_disk(potential_page, &disk_storage_idx) == ERROR) {
                // fprintf(stderr, "Failed to write pte to disk in thread_modified_to_standby\n");
                modified_add_page(potential_page, modified_list);
                
                // We will need to wait for more disk slots to be open in order to continue writing to the disk
                LeaveCriticalSection(&modified_list->lock);
                WaitForSingleObject(disk_open_slots_event, INFINITE);
                EnterCriticalSection(&modified_list->lock);

                // We will still want to 
                curr_page--;

                continue;      
            }

            // Wait times for standby list should be brief, but we do not need the modified list lock in the meantime
            // so someone else can try to add to or rescue pages from it
            LeaveCriticalSection(&modified_list->lock);

            EnterCriticalSection(&standby_list->lock);

            //BW: Note the gap where this will be in none of the lists - but the PTE
            // is still in transition format. This means that the rescue could fail.
            potential_page->standby_page.status = STANDBY_STATUS;
            potential_page->standby_page.pagefile_idx = disk_storage_idx;

            standby_add_page(potential_page, standby_list);
            total_available_pages++;
            SetEvent(waiting_for_pages_event);

            LeaveCriticalSection(&standby_list->lock);

            EnterCriticalSection(&modified_list->lock);
        }

        // printf("Successfully added to standby\n");
        LeaveCriticalSection(&modified_list->lock);
    }
}


/**
 * Connects the given PTE to the open page's physical frame and alerts the CPU
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 */
int connect_pte_to_pfn(PTE* pte, ULONG64 pfn) {
    if (pte == NULL) {
        fprintf(stderr, "NULL PTE given to connect_pte_to_pfn\n");
        return ERROR;
    }
    
    ULONG64 base_address_pte_list = (ULONG64) pagetable->pte_list;
    ULONG64 pte_address = (ULONG64) pte;
    ULONG64 pte_index = (pte_address - base_address_pte_list) / sizeof(PTE);
    LONG64 lock_index = pte_index / (pagetable->num_virtual_pages / pagetable->num_locks);

    pagetable->valid_pte_counts[lock_index] += 1;
    
    pte->memory_format.age = 0;
    pte->memory_format.frame_number = pfn;
    pte->memory_format.valid = VALID;

    // Map the CPU
    if (MapUserPhysicalPages (pte_to_va(pte), 1, &pfn) == FALSE) {

        fprintf (stderr, "connect_pte_to_pfn : could not map VA %p\n", pte_to_va(pte));
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
 * A thread dedicated to writing the given page to the disk. Writes the resulting
 * disk storage index into the given pointer disk_idx_storage.
 * 
 * Returns SUCCESS if we write the page to disk, ERROR otherwise
 */
int write_to_disk(PAGE* transition_page, ULONG64* disk_idx_storage) {
    if (transition_page == NULL || disk_idx_storage == NULL) {
        fprintf(stderr, "NULL transition page or disk_idx_storage given to thread_worker_write_to_disk\n");
        return ERROR;
    }

    DISK_RW_SLOT* write_slot = db_pop_from_tail(disk->disk_write_listhead);

    #if 0 // Not necessary unless we are writing more than one page at a time
    /**
     * We need to find an open disk write slot VA, followed by mapping the pfn from this
     * page to it. This allows us to map the contents of the pfn from the temporary slot
     * to the disk without allowing the user to access the PTE and modify its contents.
     */

    // EnterCriticalSection(&disk->disk_write_list_lock);

    // // We need to try until we get a slot available
    // while ((write_slot = db_pop_from_tail(disk->disk_write_listhead)) == NULL) {
    //     LeaveCriticalSection(&disk->disk_write_list_lock);

    //     WaitForSingleObject(disk_write_available_event, INFINITE);

    //     EnterCriticalSection(&disk->disk_write_list_lock);
    // }
    // LeaveCriticalSection(&disk->disk_write_list_lock);
    #endif

    ULONG64 pfn = page_to_pfn(transition_page);

    // We need to now get an open disk slot...
    ULONG64 disk_storage_slot;

    if (allocate_disk_slot(&disk_storage_slot) == ERROR) {
        // fprintf(stderr, "Failed to get disk slot in write_to_disk\n");
        // DebugBreak();
        return ERROR;
    }

    PULONG_PTR disk_storage_addr = disk_idx_to_addr(disk_storage_slot);

    // At this point, we know that we have a slot to write to
    // Map the CPU
    if (MapUserPhysicalPages (write_slot->rw_address, 1, &pfn) == FALSE) {
        fprintf (stderr, "write_to_disk : could not map VA %p to pfn %llX\n", write_slot->rw_address, pfn);
        DebugBreak();
        return ERROR;
    }


    // Copy from the temporary slot to the disk
    memcpy(disk_storage_addr, write_slot->rw_address, PAGE_SIZE);

    // Unmap the temporary write slot from our pfn
    if (MapUserPhysicalPages (write_slot->rw_address, 1, NULL) == FALSE) {
        fprintf (stderr, "write_to_disk : could not unmap VA %p\n", write_slot->rw_address);
        DebugBreak();
        return ERROR;
    }

    db_insert_node_at_head(disk->disk_write_listhead, write_slot->listnode);

    #if 0 // Not necessary unless we are writing more than one page at a time
    // Add the write slot back to the list, and signal anyone waiting
    EnterCriticalSection(&disk->disk_write_list_lock);
    db_insert_node_at_head(disk->disk_write_listhead, write_slot->listnode);
    SetEvent(disk_write_available_event);
    LeaveCriticalSection(&disk->disk_write_list_lock);
    #endif

    // Store the disk storage slot into the given idx storage
    *disk_idx_storage = disk_storage_slot;

    return SUCCESS;
}


/**
 * Fetches the memory from the disk index and puts it onto the open page
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 */
int read_from_disk(ULONG64 pfn, ULONG64 disk_idx) {
    if (disk_idx > DISK_STORAGE_SLOTS) {
        fprintf(stderr, "Disk idx too large in read_from_disk\n");
        return ERROR;
    }

    PULONG_PTR disk_slot_addr = disk_idx_to_addr(disk_idx);

    DISK_RW_SLOT* read_slot = db_pop_from_tail(disk->disk_read_listhead);

    // Map the CPU
    if (MapUserPhysicalPages (read_slot->rw_address, 1, &pfn) == FALSE) {
        fprintf (stderr, "read_from_disk : could not map VA %p\n", read_slot->rw_address);
        DebugBreak();
        return ERROR;
    }

    memcpy(read_slot->rw_address, disk_slot_addr, (size_t) PAGE_SIZE);

    // Map the CPU
    if (MapUserPhysicalPages (read_slot->rw_address, 1, NULL) == FALSE) {
        fprintf (stderr, "read_from_disk : could not unmap VA %p\n", read_slot->rw_address);
        DebugBreak();
        return ERROR;
    }

    db_insert_node_at_head(disk->disk_read_listhead, read_slot->listnode);

    if (return_disk_slot(disk_idx) == ERROR) {
        fprintf(stderr, "Failed to return disk slot to the disk\n");
        return ERROR;
    }

    SetEvent(disk_open_slots_event);

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

            break;
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

            break;
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