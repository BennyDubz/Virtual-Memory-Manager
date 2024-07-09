/**
 * @author Ben Williams
 * @date July 8th, 2024
 * 
 * Condensed file for all disk operations and threads
 */

#include <windows.h>
#include <stdio.h>
#include <assert.h>
#include "../Datastructures/datastructures.h"
#include "./conversions.h"
#include "../globals.h"
#include "../macros.h"
#include "../hardware.h"


static void disk_spin();

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

    // DISK_RW_SLOT* write_slot = db_pop_from_tail(disk->disk_write_listhead);

    /**
     * We need to find an open disk write slot VA, followed by mapping the pfn from this
     * page to it. This allows us to map the contents of the pfn from the temporary slot
     * to the disk without allowing the user to access the PTE and modify its contents.
     */
    
    ULONG64 pfn = page_to_pfn(transition_page);

    // We need to now get an open disk slot...
    ULONG64 disk_storage_slot;

    if (allocate_disk_slot(&disk_storage_slot) == ERROR) {
        // fprintf(stderr, "Failed to get disk slot in write_to_disk\n");
        // DebugBreak();
        return ERROR;
    }

    PULONG_PTR disk_storage_addr = disk_idx_to_addr(disk_storage_slot);

    DISK_RW_SLOT* write_slot;
    EnterCriticalSection(&disk->disk_write_list_lock);

    // We need to try until we get a slot available
    while ((write_slot = db_pop_from_tail(disk->disk_write_listhead)) == NULL) {
        LeaveCriticalSection(&disk->disk_write_list_lock);

        WaitForSingleObject(disk_write_available_event, INFINITE);

        EnterCriticalSection(&disk->disk_write_list_lock);
    }
    LeaveCriticalSection(&disk->disk_write_list_lock);


    // At this point, we know that we have a slot to write to
    // Map the CPU
    if (MapUserPhysicalPages (write_slot->rw_address, 1, &pfn) == FALSE) {
        fprintf (stderr, "write_to_disk : could not map VA %p to pfn %llX\n", write_slot->rw_address, pfn);
        DebugBreak();
        return ERROR;
    }

    // Copy from the temporary slot to the disk
    memcpy(disk_storage_addr, write_slot->rw_address, PAGE_SIZE);

    // To simulate that real disks are slow, we spin a bit here
    disk_spin();

    // Unmap the temporary write slot from our pfn
    if (MapUserPhysicalPages (write_slot->rw_address, 1, NULL) == FALSE) {
        fprintf (stderr, "write_to_disk : could not unmap VA %p\n", write_slot->rw_address);
        DebugBreak();
        return ERROR;
    }

    // db_insert_node_at_head(disk->disk_write_listhead, write_slot->listnode);

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
 * Fetches the memory from the disk index and puts it onto the open page
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 */
int read_from_disk(PAGE* open_page, ULONG64 disk_idx) {
    if (disk_idx > DISK_STORAGE_SLOTS) {
        fprintf(stderr, "Disk idx too large in read_from_disk\n");
        DebugBreak();
        return ERROR;
    }

    ULONG64 pfn = page_to_pfn(open_page);

    PULONG_PTR disk_slot_addr = disk_idx_to_addr(disk_idx);

    DISK_RW_SLOT* read_slot;

    // Someone else likely beat us to freeing this disk slot

    /**
     *  We are here with no locks at all, so we are opportunistically checking
     */ 
    if (disk->disk_slot_statuses[disk_idx] == DISK_FREESLOT) return ERROR;

    EnterCriticalSection(&disk->disk_read_list_lock);


    // We need to try until we get a slot available
    while ((read_slot = db_pop_from_tail(disk->disk_read_listhead)) == NULL) {
        LeaveCriticalSection(&disk->disk_read_list_lock);

        WaitForSingleObject(disk_read_available_event, INFINITE);

        EnterCriticalSection(&disk->disk_read_list_lock);
    }
    LeaveCriticalSection(&disk->disk_read_list_lock);


    // // Someone else likely beat us to freeing this disk slot while waiting for a read slot
    // if (disk->disk_slot_statuses[disk_idx] == DISK_FREESLOT) {
    //     LeaveCriticalSection(disk_idx_to_lock(disk_idx));
    //     // Add the read slot back to the list, and signal anyone waiting
    //     EnterCriticalSection(&disk->disk_read_list_lock);

    //     db_insert_node_at_head(disk->disk_read_listhead, read_slot->listnode);
    //     SetEvent(disk_read_available_event);

    //     LeaveCriticalSection(&disk->disk_read_list_lock);

    //     return ERROR;
    // }


    EnterCriticalSection(disk_idx_to_lock(disk_idx));

    // // Someone else likely beat us to freeing this disk slot while waiting for a disk lock
    // if (disk->disk_slot_statuses[disk_idx] == DISK_FREESLOT) {
    //     LeaveCriticalSection(disk_idx_to_lock(disk_idx));
    //     // Add the read slot back to the list, and signal anyone waiting
    //     EnterCriticalSection(&disk->disk_read_list_lock);

    //     db_insert_node_at_head(disk->disk_read_listhead, read_slot->listnode);
    //     SetEvent(disk_read_available_event);

    //     LeaveCriticalSection(&disk->disk_read_list_lock);

    //     return ERROR;
    // }

    /**
     * Need page lock while mapping the CPU
     */

    // Map the CPU
    if (MapUserPhysicalPages (read_slot->rw_address, 1, &pfn) == FALSE) {
        fprintf (stderr, "read_from_disk : could not map VA %p\n", read_slot->rw_address);
        DebugBreak();
        LeaveCriticalSection(disk_idx_to_lock(disk_idx));
        return ERROR;
    }

    memcpy(read_slot->rw_address, disk_slot_addr, (size_t) PAGE_SIZE);

    // To simulate that real disks are slow, we spin a bit here
    disk_spin();

    // Unmap the CPU
    if (MapUserPhysicalPages (read_slot->rw_address, 1, NULL) == FALSE) {
        fprintf (stderr, "read_from_disk : could not unmap VA %p\n", read_slot->rw_address);
        DebugBreak();
        LeaveCriticalSection(disk_idx_to_lock(disk_idx));
        return ERROR;
    }

    // Someone may need the slot on the disk
    SetEvent(disk_open_slots_event);

    LeaveCriticalSection(disk_idx_to_lock(disk_idx));

    // Add the read slot back to the list, and signal anyone waiting
    EnterCriticalSection(&disk->disk_read_list_lock);

    db_insert_node_at_head(disk->disk_read_listhead, read_slot->listnode);
    SetEvent(disk_read_available_event);

    LeaveCriticalSection(&disk->disk_read_list_lock);

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
        assert(open_disk_idx != 0);
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
int release_disk_slot(ULONG64 disk_idx) {
    if (disk_idx > DISK_STORAGE_SLOTS) {
        fprintf(stderr, "Invalid disk_idx given to release_disk_slot\n");
        DebugBreak();
        return ERROR;
    }

    if (disk->disk_slot_statuses[disk_idx] == DISK_FREESLOT) {
        fprintf(stderr, "Already free disk idx given to release_disk_slot\n");
        DebugBreak();
        return ERROR;
    }

    EnterCriticalSection(disk_idx_to_lock(disk_idx));

    disk->disk_slot_statuses[disk_idx] = DISK_FREESLOT;
    disk->open_slot_counts[disk_idx / (DISK_STORAGE_SLOTS / disk->num_locks)] += 1;
    
    LeaveCriticalSection(disk_idx_to_lock(disk_idx));

    SetEvent(disk_open_slots_event);

    return SUCCESS;
}


/**
 * Real disks take a long time to perform their operations. We simulate this
 * by forcing the caller to spin, so that we can better represent optimizations
 * on disk reads
 */
static void disk_spin() {
    // If we want to test with a lenient disk (so that we are not slowed down),
    #ifndef LENIENT_DISK
    for (int i = 0; i < MB(3); i++) {}
    #endif

    return;
}