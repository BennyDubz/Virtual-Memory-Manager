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
static int acquire_disk_readsection(ULONG64* disk_readsection_idx_storage, THREAD_DISK_READ_RESOURCES* thread_disk_resources) {
    long old_val;
    ULONG64 curr_idx;
    volatile long* disk_readsection;

    ULONG64 num_attempts = 0;

    // curr_idx = InterlockedIncrement64(&disk->disk_read_curr_idx) % DISK_READSECTIONS;
    if (thread_disk_resources->curr_readsection_idx == thread_disk_resources->max_readsection_idx) {
        thread_disk_resources->curr_readsection_idx = thread_disk_resources->min_readsection_idx;
    } else {
        thread_disk_resources->curr_readsection_idx++;
    }

    curr_idx = thread_disk_resources->curr_readsection_idx;

    old_val = InterlockedCompareExchange(&disk->disk_readsection_statuses[curr_idx].status, DISK_READSECTION_USED, DISK_READSECTION_OPEN);
    
    // We successfully claimed the disk slot
    if (old_val == DISK_READSECTION_OPEN) {
        *disk_readsection_idx_storage = curr_idx;
        // InterlockedDecrement64(&disk->num_available_readsections);
        return SUCCESS;
    }

    // There were no slots available, we will need to refresh the list and try again
    return ERROR;
}


/**
 * Releases the given disk readslot and sets its status to DISK_READSECTION_NEEDS_FLUSH
 */
static void release_disk_readsection(ULONG64 disk_readslot_idx) {
    custom_spin_assert(disk->disk_readsection_statuses[disk_readslot_idx].status == DISK_READSECTION_USED);
    custom_spin_assert(InterlockedIncrement(&disk->disk_readsection_statuses[disk_readslot_idx].status) == DISK_READSECTION_NEEDS_FLUSH);
}


/**
 * Unmaps all of these readslot virtual addresses from the CPU,
 * and in the process needs to edit the TLB. By doing this all at once, we reduce the amount
 * of time spent in MapUserPhysicalPagesScatter
 * 
 * If there are no disk read slots available, alerts any waiting threads
 */
#define THREADS_REFRESH_THEMSELVES_ONLY 1

#if THREADS_REFRESH_THEMSELVES_ONLY
#else
long disk_refresh_ongoing = FALSE; // We use the long for interlocked operation parameters
#endif

static void refresh_disk_readslots(THREAD_DISK_READ_RESOURCES* thread_disk_resources) {
    
    // Synchronize whether we or someone else is refreshing the diskslots
    #if THREADS_REFRESH_THEMSELVES_ONLY
    #else
    PULONG_PTR refresh_read_addresses[DISK_READ_SLOTS];
    volatile long* refresh_read_status_slots[DISK_READSECTIONS];

    long old_val = InterlockedOr(&disk_refresh_ongoing, TRUE);

    if (old_val == TRUE) {
        return;
    }

    ULONG64 num_sections_refreshed = 0;
    ULONG64 num_slots_refreshed = 0;
    volatile long* disk_readsection;
    PULONG_PTR disk_read_addr;
    #endif
    
    
    #if THREADS_REFRESH_THEMSELVES_ONLY

    // for (ULONG64 i = thread_disk_resources->min_readsection_idx; i < thread_disk_resources->max_readsection_idx + 1; i++) {
    if (MapUserPhysicalPages(thread_disk_resources->thread_disk_read_base, thread_disk_resources->num_allocated_readsections * DISK_READSECTION_SIZE, NULL) == FALSE) {
        fprintf(stderr, "Error unmapping disk read VAs in refresh_disk_readslots\n");
        DebugBreak();
        return;
    }

    // Finally clear all of the slots
    for (ULONG64 disk_status_refresh = thread_disk_resources->min_readsection_idx; disk_status_refresh < thread_disk_resources->max_readsection_idx + 1; disk_status_refresh++) {
        // InterlockedIncrement64(&disk->num_available_readsections);
        // InterlockedAnd(refresh_read_status_slots[disk_status_refresh], DISK_READSECTION_OPEN);
        disk->disk_readsection_statuses[disk_status_refresh].status = DISK_READSECTION_OPEN;
    }
    #else
    // Find all of the slots to clear
    for (ULONG64 i = 0; i < DISK_READSECTIONS; i++) {
        // printf("min %llx, max %llx, refresh %llx\n", thread_disk_resources->min_readsection_idx, thread_disk_resources->max_readsection_idx, i);
        disk_readsection = &disk->disk_readsection_statuses[i].status;



        if (*disk_readsection == DISK_READSECTION_NEEDS_FLUSH) {
            refresh_read_status_slots[num_sections_refreshed] = disk_readsection;

            PULONG_PTR readsection_addr_base = disk->disk_read_base_addr + (i * DISK_READSECTION_SIZE * PAGE_SIZE / sizeof(PULONG_PTR));

            for (ULONG64 j = 0; j < DISK_READSECTION_SIZE; j++) {
                refresh_read_addresses[num_slots_refreshed] = readsection_addr_base + (j * PAGE_SIZE / sizeof(PULONG_PTR));
                num_slots_refreshed++;
            }
            
            num_sections_refreshed++;
        }
    }
    
    if (MapUserPhysicalPagesScatter(refresh_read_addresses, num_slots_refreshed, NULL) == FALSE) {
        fprintf(stderr, "Error unmapping disk read VAs in refresh_disk_readslots\n");
        DebugBreak();
        return;
    }
    
    // Finally clear all of the slots
    for (ULONG64 disk_status_refresh = 0; disk_status_refresh < num_sections_refreshed; disk_status_refresh++) {
        // InterlockedIncrement64(&disk->num_available_readsections);
        InterlockedAnd(refresh_read_status_slots[disk_status_refresh], DISK_READSECTION_OPEN);
    }
    
    #endif

    #if THREADS_REFRESH_THEMSELVES_ONLY
    #else
    InterlockedAnd(&disk_refresh_ongoing, FALSE);
    #endif

}


/**
 * Reads all of the data from the given disk indices on the PTEs into the given pages
 * 
 * Assumes all of the PTEs are in disk format and have their disk read rights acquired by the calling thread
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 */
int read_pages_from_disk(PAGE** open_pages, PTE** ptes_to_read, ULONG64 num_to_read, THREAD_DISK_READ_RESOURCES* thread_disk_resources) {
    if (num_to_read > MAX_PAGES_READABLE) {
        fprintf(stderr, "Trying to read too many pages from the disk\n");
        DebugBreak();
    }

    
    ULONG64 disk_readsection;
    PVOID disk_read_addresses[MAX_PAGES_READABLE];
    ULONG64 pfns[MAX_PAGES_READABLE];
    ULONG64 num_readslots_acquired = 0;
    ULONG64 new_readslots_acquired;

    for (ULONG64 i = 0; i < num_to_read; i++) {
        pfns[i] = page_to_pfn(open_pages[i]);
    }

    // We if this fails, we need to try to begin the refresh process if it hasn't begun already
    while (acquire_disk_readsection(&disk_readsection, thread_disk_resources) == ERROR) {
        
        #if THREADS_REFRESH_THEMSELVES_ONLY
        refresh_disk_readslots(thread_disk_resources);
        #else
         /**
         * Right now, we will try immediately again if the refresh is ongoing - since it may have refreshed pages behind us
         * otherwise, if a refresh is not ongoing, we will do it ourselves
         */
        if (disk_refresh_ongoing == FALSE) {
            // If another thread beat us to it, this will return almost immediately
            refresh_disk_readslots(thread_disk_resources);
        }
        #endif
    }

    custom_spin_assert(disk->disk_readsection_statuses[disk_readsection].status == DISK_READSECTION_USED);

    // Convert our readslot indices into addresses that we can use
    PULONG_PTR disk_readsection_base = disk->disk_read_base_addr + (disk_readsection * DISK_READSECTION_SIZE * PAGE_SIZE / sizeof(PULONG_PTR));
    ULONG64 base_offset;
    for (ULONG64 i = 0; i < num_to_read; i++) {
        disk_read_addresses[i] = disk_readsection_base + (i * PAGE_SIZE / sizeof(PULONG_PTR));
    }

    // ULONG64 sequential_count_storage[MAX_PAGES_READABLE];
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

    release_disk_readsection(disk_readsection);

    // If we are simulating a penalty for the disk operations, we spin here
    disk_spin();

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
    ULONG64 slots_in_this_section;
    ULONG64 num_slots_allocated = 0;

    for (ULONG64 section = 0; section < disk->num_disk_sections; section++) {
        section_start = section * disk->slots_per_section;

        // Skip over empty disk sections
        if (disk->open_slot_counts[section] == 0) {
            continue;
        }

        for (ULONG64 disk_idx = section_start; disk_idx < section_start + disk->slots_per_section; disk_idx++) {
            if (disk->disk_slot_statuses[disk_idx] == DISK_USEDSLOT) continue;

            // Currently, we do not have threads racing to acquire disk storage slots. This would protect us if that changed
            if (InterlockedExchange8(&disk->disk_slot_statuses[disk_idx], DISK_USEDSLOT) == DISK_USEDSLOT) {
                continue;
            }

            // Now we have a disk slot to use
            open_disk_idx = disk_idx;
            disk->disk_slot_statuses[disk_idx] = DISK_USEDSLOT;

            InterlockedDecrement64(&disk->open_slot_counts[section]);
            InterlockedDecrement64(&disk->total_available_slots);

            result_storage[num_slots_allocated] = disk_idx;
            num_slots_allocated++;

            // If we have found enough slots, or there are none left in this section, break out
            if (num_slots_allocated == num_disk_slots || disk->open_slot_counts[section] == 0) break;
        }

        if (num_slots_allocated == num_disk_slots) return num_disk_slots;
    }

    if (num_slots_allocated < num_disk_slots) DebugBreak();

    return num_slots_allocated;
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

    // EnterCriticalSection(disk_idx_to_lock(disk_idx));

    InterlockedIncrement64(&disk->open_slot_counts[disk_idx / disk->slots_per_section]);

    InterlockedExchange8(&disk->disk_slot_statuses[disk_idx], DISK_FREESLOT);

    if (InterlockedIncrement64(&disk->total_available_slots) == 0) {
        SetEvent(disk_open_slots_event);
    }

    // LeaveCriticalSection(disk_idx_to_lock(disk_idx));
    

    return SUCCESS;
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

            // These represent unaccessed PTEs whose "being_changed" bits are set - they have yet to be mapped
            if (is_memory_format(read_pte_contents(original_pte_accessed))) {
                allocated_pages[0]->pagefile_idx = DISK_IDX_NOTUSED;
            }

            // These represent unaccessed PTEs whose "being_changed" bits are set - they have yet to be mapped
            if (is_memory_format(read_pte_contents(original_pte_accessed))) {
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


        // These represent unaccessed PTEs whose "being_changed" bits are set - they have yet to be mapped
        if (is_memory_format(read_pte_contents(original_pte_accessed))) {
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
 * 
 * This
 */
static void disk_spin() {
    #ifdef HARSH_DISK
    for (int i = 0; i < MB(3); i++) {}
    #endif

    return;
}


