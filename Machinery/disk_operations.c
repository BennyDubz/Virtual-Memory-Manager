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
#include "./disk_operations.h"
#include "./conversions.h"
#include "./debug_checks.h"
#include "../globals.h"
#include "../macros.h"
#include "../hardware.h"


static void disk_spin();


/**
 * Writes the entire batch to disk, and stores the disk indices into the page
 * 
 * Returns the number of pages successfully written to disk, as if there are not enough slots 
 * we may not write all of them
 */
ULONG64 write_batch_to_disk(DISK_BATCH* disk_batch) {
    ULONG_PTR pfn_list[MAX_PAGES_WRITABLE];

    ULONG64 num_available_disk_slots = allocate_many_disk_slots(disk_batch->disk_indices, disk_batch->num_pages);

    // We were unable to get any disk slots
    if (num_available_disk_slots == 0) return 0;

    // We need a list of PFNs for MapUserPhysicalPages
    for (int page_num = 0; page_num < disk_batch->num_pages; page_num++) {
        pfn_list[page_num] = page_to_pfn(disk_batch->pages_being_written[page_num]);
    }


    // Map the physical pages to our large disk write slot
    if (MapUserPhysicalPages(disk->disk_large_write_slot, num_available_disk_slots, pfn_list) == FALSE) {
        printf ("MapUserPhysPages in write_batch_to_disk failed, error %#x\n", GetLastError());
        fprintf(stderr, "Failed to map physical pages to large disk write slot in write_batch_to_disk\n");
        DebugBreak();
    }

    // Memcpy from the virtual address to each disk slot
    PULONG_PTR source_addr = disk->disk_large_write_slot;
    for (int i = 0; i < num_available_disk_slots; i++) {
        PULONG_PTR disk_slot_addr = disk_idx_to_addr(disk_batch->disk_indices[i]);

        memcpy(disk_slot_addr, source_addr, PAGE_SIZE);

        // Increment the source address to the next page's data
        source_addr += (PAGE_SIZE / sizeof(PULONG_PTR));
    } 

    // Disk penalty
    disk_spin();

    // Unmap the physical pages from the large write slot
    if (MapUserPhysicalPages(disk->disk_large_write_slot, num_available_disk_slots, NULL) == FALSE) {
        printf ("MapUserPhysPages in write_batch_to_disk failed, error %#x\n", GetLastError());
        fprintf(stderr, "Failed to unmap physical pages to large disk write slot in write_batch_to_disk\n");
        DebugBreak();
    }

    disk_batch->write_complete = TRUE;

    return num_available_disk_slots;
}


/**
 * Finds an available disk readslot, or waits for one to become available,
 * and sets its status to DISK_USED and writes the index into disk_readidx_storage
 * 
 * Returns SUCCESS if a slot was allocated, ERROR otherwise
 */
static int acquire_disk_readslot(ULONG64* disk_read_idx_storage) {
    long read_old_val;
    ULONG64 curr_idx;
    volatile long* disk_readslot;

    ULONG64 num_attempts = 0;

    while (num_attempts < DISK_READ_SLOTS) {   
        /**
         * Using the interlocked operations here with the read indices makes it almost certain that threads will not share the same
         * read index, and if we are able to refresh the slots frequently enough, then we should be able to find a disk slot in
         * O(1) time rather than the worst case O(n) time if we performed a typical linear walk. The worst case scenario though is still the
         * linear walk that fails to find a disk slot - in which case a thread will refresh the read slots in the parent
         */
        curr_idx = InterlockedIncrement64(&disk->disk_read_curr_idx) % DISK_READ_SLOTS;

        disk_readslot = &disk->disk_read_slot_statues[curr_idx];

        if (*disk_readslot == DISK_READ_OPEN) {
            read_old_val = InterlockedCompareExchange(disk_readslot, DISK_READ_USED, DISK_READ_OPEN);
            
            // We successfully claimed the disk slot
            if (read_old_val == DISK_READ_OPEN) {
                *disk_read_idx_storage = curr_idx;
                InterlockedDecrement64(&disk->num_available_read_slots);
                return SUCCESS;
            }
        }

        num_attempts++;
    }

    // There were no slots available, we will need to refresh the list and try again
    return ERROR;
}


/**
 * Releases the given disk readslot and sets its status to DISK_READ_NEEDS_FLUSH
 */
static void release_disk_readslot(ULONG64 disk_read_idx) {
    if (disk->disk_read_slot_statues[disk_read_idx] != DISK_READ_USED) {
        DebugBreak();
    }

    custom_spin_assert(InterlockedIncrement(&disk->disk_read_slot_statues[disk_read_idx]) == DISK_READ_NEEDS_FLUSH);
}


/**
 * Unmaps all of these readslot virtual addresses from the CPU,
 * and in the process needs to edit the TLB. By doing this all at once, we reduce the amount
 * of time spent in MapUserPhysicalPagesScatter
 * 
 * If there are no disk read slots available, alerts any waiting threads
 */
PULONG_PTR refresh_read_addresses[DISK_READ_SLOTS];
volatile long* refresh_read_status_slots[DISK_READ_SLOTS];
long disk_refresh_ongoing = FALSE; // We use the long for interlocked operation parameters
static void refresh_disk_readslots() {
    // Synchronize whether we or someone else is refreshing the diskslots
    long old_val = InterlockedOr(&disk_refresh_ongoing, TRUE);

    if (old_val == TRUE) {
        return;
    }

    ULONG64 num_slots_refreshed = 0;
    volatile long* disk_readslot;
    PULONG_PTR disk_read_addr;

    // Find all of the slots to clear
    for (ULONG64 disk_read_idx = 0; disk_read_idx < DISK_READ_SLOTS; disk_read_idx++) {
        disk_readslot = &disk->disk_read_slot_statues[disk_read_idx];

        if (*disk_readslot == DISK_READ_NEEDS_FLUSH) {
            refresh_read_status_slots[num_slots_refreshed] = disk_readslot;

            disk_read_addr = disk->disk_read_base_addr + (disk_read_idx * PAGE_SIZE / sizeof(PULONG_PTR));
            refresh_read_addresses[num_slots_refreshed] = disk_read_addr;
            num_slots_refreshed++;
        }
    }

    if (MapUserPhysicalPagesScatter(refresh_read_addresses, num_slots_refreshed, NULL) == FALSE) {
        fprintf(stderr, "Error unmapping disk read VAs in refresh_disk_readslots\n");
        DebugBreak();
        return;
    }

    InterlockedAdd64(&disk->num_available_read_slots, num_slots_refreshed);

    // Finally clear all of the slots
    for (ULONG64 disk_status_refresh = 0; disk_status_refresh < num_slots_refreshed; disk_status_refresh++) {
        // InterlockedIncrement64(&disk->num_available_read_slots);
        InterlockedAnd(refresh_read_status_slots[disk_status_refresh], DISK_READ_OPEN);
    }

    InterlockedAnd(&disk_refresh_ongoing, FALSE);
}


/**
 * Fetches the memory from the disk index and puts it onto the open page
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 */
int read_page_from_disk(PAGE* open_page, ULONG64 disk_idx) {
    if (disk_idx > DISK_STORAGE_SLOTS) {
        fprintf(stderr, "Disk idx too large in read_page_from_disk\n");
        DebugBreak();
        return ERROR;
    }

    ULONG64 pfn = page_to_pfn(open_page);

    PULONG_PTR disk_slot_addr = disk_idx_to_addr(disk_idx);

    ULONG64 disk_read_idx;

    DISK_RW_SLOT* read_slot;

    // Someone else likely beat us to freeing this disk slot
    if (disk->disk_slot_statuses[disk_idx] == DISK_FREESLOT) { 
        DebugBreak();
    }


    // We if this fails, we need to try to begin the refresh process if it hasn't begun already
    while (acquire_disk_readslot(&disk_read_idx) == ERROR) {
        
        /**
         * Right now, we will try immediately again if the refresh is ongoing - since it may have refreshed pages behind us
         * otherwise, if a refresh is not ongoing, we will do it ourselves
         */
        if (disk_refresh_ongoing == FALSE) {
            // If another thread beat us to it, this will return almost immediately
            refresh_disk_readslots();
        }
    }

    PULONG_PTR disk_read_addr = disk->disk_read_base_addr + (disk_read_idx * PAGE_SIZE / sizeof(PULONG_PTR));


    // Map the CPU
    if (MapUserPhysicalPages (disk_read_addr, 1, &pfn) == FALSE) {
        fprintf (stderr, "read_page_from_disk : could not map VA %p\n", disk_read_addr);
        DebugBreak();
        return ERROR;
    }

    memcpy(disk_read_addr, disk_slot_addr, (size_t) PAGE_SIZE);

    // To simulate that real disks are slow, we spin a bit here
    disk_spin();

    release_disk_readslot(disk_read_idx);
    
    // If we are running low on disk read slots, attempt to refresh the disk slots
    if (disk->num_available_read_slots < DISK_REFRESH_BOUNDARY && disk_refresh_ongoing == FALSE) {
        // If someone else is already doing it (race condition) - we will return almost immediately
        refresh_disk_readslots();
    }

    return SUCCESS;
}


/**
 * Writes a list of num_disk_slots into the result storage pointer.
 * 
 * Returns the number of disk slots written into the storage, as there may not be enough available.
 */
ULONG64 allocate_many_disk_slots(ULONG64* result_storage, ULONG64 num_disk_slots) {
    ULONG64 section_start;
    ULONG64 open_disk_idx;
    BOOL lock_result; 
    ULONG64 disk_slots_per_lock = DISK_STORAGE_SLOTS / disk->num_locks;

    ULONG64 num_slots_allocated = 0;

    // Go through each lock section and **try** to enter the critical sections
    for (ULONG64 lock_section = 0; lock_section < disk->num_locks; lock_section++) {
        section_start = lock_section * disk_slots_per_lock;

        /**
         * First, we only TRY to enter the critical sections - but then we wait the second time around
         * 
         * This ensures faster response times in ideal cases, and could spread out the disk slot allocation across
         * multiple lock sections, hopefully reducing contension for locks 
         */

        // Skip over empty disk sections
        if (disk->open_slot_counts[lock_section] == 0) {
            continue;
        }
        
        lock_result = TryEnterCriticalSection(&disk->disk_slot_locks[lock_section]);

        if (lock_result == FALSE) {
            continue;
        }

        // Skip over empty disk sections
        if (disk->open_slot_counts[lock_section] == 0) {
            LeaveCriticalSection(&disk->disk_slot_locks[lock_section]);
            continue;
        }

        // Now we are guaranteed to find a disk slot somewhere in this section - take as many as possible
        for (ULONG64 disk_idx = section_start; disk_idx < section_start + disk_slots_per_lock; disk_idx++) {
            if (disk->disk_slot_statuses[disk_idx] == DISK_USEDSLOT) continue;

            // Now we have a disk slot to use
            open_disk_idx = disk_idx;
            disk->disk_slot_statuses[disk_idx] = DISK_USEDSLOT;
            disk->open_slot_counts[lock_section] -= 1;
            InterlockedDecrement64(&disk->total_available_slots);

            result_storage[num_slots_allocated] = disk_idx;
            num_slots_allocated++;

            // If we have found enough slots, or there are none left in this section, break out
            if (num_slots_allocated == num_disk_slots || disk->open_slot_counts[lock_section] == 0) break;
        }

        LeaveCriticalSection(&disk->disk_slot_locks[lock_section]);

        if (num_slots_allocated == num_disk_slots) return num_disk_slots;
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

            result_storage[num_slots_allocated] = disk_idx;
            num_slots_allocated++;

            // If we have found enough slots, or there are none left in this section, break out
            if (num_slots_allocated == num_disk_slots || disk->open_slot_counts[lock_section] == 0) break;
        }

        LeaveCriticalSection(&disk->disk_slot_locks[lock_section]);

        if (num_slots_allocated == num_disk_slots) return num_disk_slots;
    }

    return num_slots_allocated;
}

/**
 * Writes an open disk idx into the result storage pointer and sets the disk slot to DISK_USEDSLOT
 * 
 * Returns SUCCESS if we successfully wrote a disk idx, ERROR otherwise (may be empty)
 */
int allocate_single_disk_slot(ULONG64* result_storage) {
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
            InterlockedDecrement64(&disk->total_available_slots);

            break;
        }

        LeaveCriticalSection(&disk->disk_slot_locks[lock_section]);
        custom_spin_assert(open_disk_idx != 0);
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
int release_single_disk_slot(ULONG64 disk_idx) {
    if (disk_idx > DISK_STORAGE_SLOTS) {
        fprintf(stderr, "Invalid disk_idx given to release_single_disk_slot\n");
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

    if (disk->total_available_slots == 0) {
        SetEvent(disk_open_slots_event);
    }
    
    InterlockedIncrement64(&disk->total_available_slots);

    return SUCCESS;
}


/**
 * Called at the end of a pagefault to determine whether or not a pagefile slot needs to be
 * released. Modifies the page if necessary to remove the reference to the pagefile slot if it is released,
 * and releases the disk slot if appropriate
 * 
 * Assumes the caller holds the given page's pagelock.
 */
void handle_end_of_fault_disk_slot(PTE local_pte, PAGE* allocated_page, ULONG64 access_type) {
        
    if (access_type == READ_ACCESS) {

        // We want to store the disk index in the page so we don't lose it
        if (is_disk_format(local_pte)) {
            allocated_page->pagefile_idx = local_pte.disk_format.pagefile_idx;
            return;
        }

        if (is_used_pte(local_pte) == FALSE) {
            allocated_page->pagefile_idx = DISK_IDX_NOTUSED;
            return;
        }

        return;
    }

    if (access_type == WRITE_ACCESS) {

        // We have to throw out the pagefile space stored in the PTE
        if (is_disk_format(local_pte)) {
            release_single_disk_slot(local_pte.disk_format.pagefile_idx);
        } else if (is_transition_format(local_pte) && allocated_page->status == STANDBY_STATUS) {
            release_single_disk_slot(allocated_page->pagefile_idx);
        } else {
            custom_spin_assert(allocated_page->pagefile_idx == DISK_IDX_NOTUSED);
        }

        // In all cases, we cannot store any pagefile information in the page
        allocated_page->pagefile_idx = DISK_IDX_NOTUSED;
        return;
    }
    
    DebugBreak();
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


