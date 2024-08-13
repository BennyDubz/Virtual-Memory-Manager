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
 * Used to compare two virtual addresses for sorting
 */
static int va_compare(const void* va_1, const void* va_2) {
    if ((ULONG_PTR) va_1 < (ULONG_PTR) va_2) {
        return -1;
    } else if ((ULONG_PTR) va_1 == (ULONG_PTR) va_2) {
        return 0;
    } else {
        return 1;
    }
}


/**
 * Sorts the virtual addresses representing a page, and then loops through them to determine how many of them are adjacent 
 * to eachother. We write into the sequential_va_count_storage how many sequential VAs there are in a row.
 * 
 * For example, if we have virtual addresses 0x1000, 0x2000, and 0x4000 - the first two are next to eachother.
 * We would write in the values [2, 1] into the storage array. If we had to memcpy (such as for reading from the disk), we would
 * be able to make fewer, but larger, memcpy calls in order to do this
 */
void pre_prepare_page_memcpys(PULONG_PTR* virtual_addresses, ULONG64 num_addresses, ULONG64* sequential_va_count_storage) {
    qsort(virtual_addresses, num_addresses, sizeof(PULONG_PTR), va_compare);

    ULONG64 curr_section = 0;
    ULONG64 num_sequential = 1;

    ULONG_PTR prev_addr_num = (ULONG_PTR) virtual_addresses[0];
    ULONG_PTR curr_addr_num;
    for (ULONG64 i = 1; i < num_addresses; i++) {
        curr_addr_num = (ULONG_PTR) virtual_addresses[i];

        if (curr_addr_num - PAGE_SIZE == prev_addr_num) {
            num_sequential++;
        } else {
            sequential_va_count_storage[curr_section] = num_sequential;
            num_sequential = 1;
            curr_section++;
        }

        prev_addr_num = curr_addr_num;
    }

    sequential_va_count_storage[curr_section] = num_sequential;
}



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
 * Releases all of the given readslots and sets their statuses to DISK_READ_NEEDS_FLUSH
 */
static void release_batch_disk_readslots(ULONG64* disk_read_indices, ULONG64 num_to_release) {
    for (ULONG64 i = 0; i < num_to_release; i++) {
        custom_spin_assert(InterlockedIncrement(&disk->disk_read_slot_statues[disk_read_indices[i]]) == DISK_READ_NEEDS_FLUSH);
    }
}


/**
 * Tries to find num_to_acquire disk readslots and writes them into the storage
 * 
 * Returns the number of disk readslots found
 */
#define READSLOT_INTERLOCKED_OPERATIONS 1
static ULONG64 acquire_batch_disk_readslots(ULONG64* disk_read_indices_storage, ULONG64 num_to_acquire) {
    long read_old_val;
    volatile long* disk_readslot;

    ULONG64 num_attempts = 0;
    ULONG64 num_acquired = 0;
    #if READSLOT_INTERLOCKED_OPERATIONS
    ULONG64 curr_idx;
    #else
    ULONG64 curr_idx = 0;
    #endif

    while (num_attempts < DISK_READ_SLOTS && num_acquired < num_to_acquire) {   

        #if READSLOT_INTERLOCKED_OPERATIONS
        /**
         * Using the interlocked operations here with the read indices makes it almost certain that threads will not share the same
         * read index, and if we are able to refresh the slots frequently enough, then we should be able to find a disk slot in
         * O(1) time rather than the worst case O(n) time if we performed a typical linear walk. The worst case scenario though is still the
         * linear walk that fails to find a disk slot - in which case a thread will refresh the read slots in the parent
         */
        curr_idx = InterlockedIncrement64(&disk->disk_read_curr_idx) % DISK_READ_SLOTS;
        #endif

        #if 0
        disk_readslot = &disk->disk_read_slot_statues[curr_idx];

        if (*disk_readslot == DISK_READ_OPEN) {
            read_old_val = InterlockedCompareExchange(disk_readslot, DISK_READ_USED, DISK_READ_OPEN);
            
            // We successfully claimed the disk slot
            if (read_old_val == DISK_READ_OPEN) {
                disk_read_indices_storage[num_acquired] = curr_idx;
                InterlockedDecrement64(&disk->num_available_read_slots);
                num_acquired++;
            }
        }
        #endif

        read_old_val = InterlockedCompareExchange(&disk->disk_read_slot_statues[curr_idx], DISK_READ_USED, DISK_READ_OPEN);
            
        // We successfully claimed the disk slot
        if (read_old_val == DISK_READ_OPEN) {
            disk_read_indices_storage[num_acquired] = curr_idx;
            InterlockedDecrement64(&disk->num_available_read_slots);
            num_acquired++;
        }


        #if READSLOT_INTERLOCKED_OPERATIONS
        #else
        curr_idx++;
        #endif
        num_attempts++;
    }

    // There were no slots available, we will need to refresh the list and try again
    return num_acquired;
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

    /**
     * When we begin to clear spots at the end, we want them to be as close to our current index as possible
     * so that anyone acquiring slots can do so rapidly
     */
    ULONG64 start = disk->disk_read_curr_idx;
    ULONG64 curr_idx;

    // Find all of the slots to clear
    for (ULONG64 i = 0; i < DISK_READ_SLOTS; i++) {
        curr_idx = (start + i) % DISK_READ_SLOTS;

        disk_readslot = &disk->disk_read_slot_statues[curr_idx];

        if (*disk_readslot == DISK_READ_NEEDS_FLUSH) {
            refresh_read_status_slots[num_slots_refreshed] = disk_readslot;

            disk_read_addr = disk->disk_read_base_addr + (curr_idx * PAGE_SIZE / sizeof(PULONG_PTR));
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
 * Reads all of the data from the given disk indices on the PTEs into the given pages
 * 
 * Assumes all of the PTEs are in disk format and have their disk read rights acquired by the calling thread
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 */
int read_pages_from_disk(PAGE** open_pages, PTE** ptes_to_read, ULONG64 num_to_read) {
    if (num_to_read > MAX_PAGES_READABLE) {
        fprintf(stderr, "Trying to read too many pages from the disk\n");
        DebugBreak();
    }

    
    ULONG64 disk_readslots[MAX_PAGES_READABLE];
    PVOID disk_read_addresses[MAX_PAGES_READABLE];
    ULONG64 pfns[MAX_PAGES_READABLE];
    ULONG64 num_readslots_acquired = 0;
    ULONG64 new_readslots_acquired;

    for (ULONG64 i = 0; i < num_to_read; i++) {
        pfns[i] = page_to_pfn(open_pages[i]);
    }

    // It may take several attempts to get all of the readslots that we need
    while (num_readslots_acquired < num_to_read) {
        
        new_readslots_acquired = acquire_batch_disk_readslots(&disk_readslots[num_readslots_acquired], num_to_read - num_readslots_acquired);

        // If we failed to acquire enough readslots, we might need to refresh the readslots if noone else is already
        if (new_readslots_acquired != (num_to_read - num_readslots_acquired) && disk_refresh_ongoing == FALSE) {
            refresh_disk_readslots();
        }

        num_readslots_acquired += new_readslots_acquired;
    }

    // Convert our readslot indices into addresses that we can use
    ULONG64 base_offset;
    for (ULONG64 i = 0; i < num_to_read; i++) {
        base_offset = (disk_readslots[i] * PAGE_SIZE) / sizeof(ULONG_PTR);
        disk_read_addresses[i] = disk->disk_read_base_addr + base_offset;
    }

    ULONG64 sequential_count_storage[MAX_PAGES_READABLE];
    // pre_prepare_page_memcpys((PULONG_PTR*) disk_read_addresses, num_to_read, sequential_count_storage);

    // This is the expensive operation that we really benefit from batching
    if (MapUserPhysicalPagesScatter(disk_read_addresses, num_to_read, pfns) == FALSE) {
        fprintf(stderr, "Failed to map a batch of pfns to the disk read addresses in read_pages_from_disk\n");
        DebugBreak();
        return ERROR;
    }
    
    // Copy everything into our pages
    PULONG_PTR disk_storage_address;
    ULONG64 disk_idx;


    for (ULONG64 i = 0; i < num_to_read; i++) {
        PTE pte_copy = read_pte_contents(ptes_to_read[i]);
        custom_spin_assert(is_disk_format(pte_copy));
        custom_spin_assert(pte_copy.disk_format.being_read_from_disk == PTE_BEING_READ_FROM_DISK);

        disk_idx = ptes_to_read[i]->disk_format.pagefile_idx;
        disk_storage_address = disk_idx_to_addr(disk_idx);
        memcpy(disk_read_addresses[i], disk_storage_address, PAGE_SIZE);
    }

    release_batch_disk_readslots(disk_readslots, num_to_read);

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
    ULONG64 slots_in_this_section;
    ULONG64 num_slots_allocated = 0;


    // Go through each lock section and **try** to enter the critical sections
    for (ULONG64 lock_section = 0; lock_section < disk->num_locks; lock_section++) {
        section_start = lock_section * disk->slots_per_lock;

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
        for (ULONG64 disk_idx = section_start; disk_idx < section_start + disk->slots_per_lock; disk_idx++) {
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
     */
    for (ULONG64 lock_section = 0; lock_section < disk->num_locks; lock_section++) {
        section_start = lock_section * disk->slots_per_lock;
        
        // This will block the thread until we get into the critical section, and is the only part
        // that differs from the previous loop
        EnterCriticalSection(&disk->disk_slot_locks[lock_section]);

        // Skip over empty disk sections
        if (disk->open_slot_counts[lock_section] == 0) {
            LeaveCriticalSection(&disk->disk_slot_locks[lock_section]);
            continue;
        }

        // Now we are guaranteed to find a disk slot somewhere in this section
        for (ULONG64 disk_idx = section_start; disk_idx < section_start + disk->slots_per_lock; disk_idx++) {
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
    if (disk_idx == DISK_IDX_NOTUSED || disk_idx > DISK_STORAGE_SLOTS) {
        fprintf(stderr, "Invalid disk_idx given to release_single_disk_slot\n");
        DebugBreak();
        return ERROR;
    }

    if (disk->disk_slot_statuses[disk_idx] == DISK_FREESLOT) {
        fprintf(stderr, "Already free disk idx given to release_disk_slot\n");
        custom_spin_assert(FALSE);
        return ERROR;
    }

    // singly_allocated_disk_idx_check(disk_idx);

    EnterCriticalSection(disk_idx_to_lock(disk_idx));

    disk->disk_slot_statuses[disk_idx] = DISK_FREESLOT;
    disk->open_slot_counts[disk_idx / disk->slots_per_lock] ++;

    if (InterlockedIncrement64(&disk->total_available_slots) == 0) {
        SetEvent(disk_open_slots_event);
    }

    LeaveCriticalSection(disk_idx_to_lock(disk_idx));
    

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
            UCHAR disk_status = disk->disk_slot_statuses[local_pte.disk_format.pagefile_idx];

            allocated_page->pagefile_idx = local_pte.disk_format.pagefile_idx;
            custom_spin_assert(disk_status == DISK_USEDSLOT);
            return;
        }

        if (is_used_pte(local_pte) == FALSE) {
            allocated_page->pagefile_idx = DISK_IDX_NOTUSED;
            return;
        } 
        
        // By enabling this, we no longer preserve disk slots. Trimming straight to standby will now be impossible
        #if DISABLE_PAGEFILE_PRESERVATION
        if (is_transition_format(local_pte) && allocated_page->pagefile_idx != DISK_IDX_NOTUSED) {
            release_single_disk_slot(allocated_page->pagefile_idx);
            allocated_page->pagefile_idx = DISK_IDX_NOTUSED;
        }
        #endif

        return;
    }

    if (access_type == WRITE_ACCESS) {

        // We have to throw out the pagefile space stored in the PTE
        if (is_disk_format(local_pte)) {
            release_single_disk_slot(local_pte.disk_format.pagefile_idx);
        } else if (is_transition_format(local_pte) && allocated_page->status == STANDBY_STATUS) {
            release_single_disk_slot(allocated_page->pagefile_idx);
        }

        // In all cases, we cannot store any pagefile information in the page
        allocated_page->pagefile_idx = DISK_IDX_NOTUSED;
        return;
    }
    
    DebugBreak();
}



/**
 * Called at the end of a pagefault to determine whether or not pagefile slots need to be
 * released. Modifies the pages only if necessary to remove references to pagefile slots when appropriate
 * and release pagefile slots when appropriate
 * 
 * Assumes the caller holds the given page's pagelocks.
 */
void handle_batch_end_of_fault_disk_slot(PTE** ptes, PTE* original_pte_accessed, PAGE** allocated_pages, ULONG64 access_type, ULONG64 num_ptes) {
    
    ULONG64 start_index;

    // See if we are only doing speculative reads from the disk
    if (original_pte_accessed != ptes[0]) {
        start_index = 0;
    } else {
        if (access_type == READ_ACCESS) {
            if (is_disk_format(read_pte_contents(original_pte_accessed))) {
                UCHAR disk_status = disk->disk_slot_statuses[original_pte_accessed->disk_format.pagefile_idx];

                allocated_pages[0]->pagefile_idx = original_pte_accessed->disk_format.pagefile_idx;
                custom_spin_assert(disk_status == DISK_USEDSLOT);
            } 

            if (is_used_pte(read_pte_contents(original_pte_accessed)) == FALSE) {
                allocated_pages[0]->pagefile_idx = DISK_IDX_NOTUSED;
            } 
        }

        if (access_type == WRITE_ACCESS) {
            if (is_transition_format(read_pte_contents(original_pte_accessed)) && allocated_pages[0]->status == STANDBY_STATUS) {
                release_single_disk_slot(allocated_pages[0]->pagefile_idx);
            } else if (is_disk_format(read_pte_contents(original_pte_accessed))) {
                release_single_disk_slot(original_pte_accessed->disk_format.pagefile_idx);
            }

            allocated_pages[0]->pagefile_idx = DISK_IDX_NOTUSED;
        }

        start_index = 1;
    }

    PTE* curr_pte;
    PAGE* curr_page;

    // Now we determine how to handle the PTEs we are bringing in speculatively
    for (ULONG64 i = start_index; i < num_ptes; i++) {
        curr_pte = ptes[i];
        curr_page = allocated_pages[i];

        if (is_disk_format(read_pte_contents(curr_pte))) {
            UCHAR disk_status = disk->disk_slot_statuses[curr_pte->disk_format.pagefile_idx];
            curr_page->pagefile_idx = curr_pte->disk_format.pagefile_idx;
            custom_spin_assert(disk_status == DISK_USEDSLOT);
            continue;
        }

        if (is_used_pte(read_pte_contents(curr_pte)) == FALSE) {
            curr_page->pagefile_idx = DISK_IDX_NOTUSED;
            continue;
        }

        // Transition PTEs will already have the disk index in their respective page
    }

}



/**
 * Real disks take a long time to perform their operations. We simulate this
 * by forcing the caller to spin, so that we can better represent optimizations
 * on disk operations
 */
static void disk_spin() {
    // If we want to test with a lenient disk (so that we are not slowed down),
    #ifndef LENIENT_DISK
    for (int i = 0; i < MB(3); i++) {}
    #endif

    return;
}


