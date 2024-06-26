/**
 * @author Ben Williams
 * @date June 25th, 2024
 * 
 * All functionality for trimming
 */

#include <stdio.h>
#include <assert.h>
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
    for (ULONG64 pte_idx = 0; pte_idx < pagetable->num_virtual_pages; pte_idx++) {
        PTE* candidate_pte = &pagetable->pte_list[pte_idx];

        // See if the pte is currently linked to a physical frame that we can take
        if (is_memory_format(*candidate_pte)) {

            // We have found our victim to steal from
            ULONG64 pfn = candidate_pte->memory_format.frame_number;

            PULONG_PTR pte_va = pte_to_va(candidate_pte);

            ULONG64 disk_idx = 0;

            if (write_to_disk(candidate_pte, &disk_idx) == ERROR) {
                fprintf(stderr, "Unable to get a disk index in steal_lowest_frame\n");
                return ERROR;
            }

            // Unmap the CPU
            if (MapUserPhysicalPages (pte_va, 1, NULL) == FALSE) {

                fprintf (stderr, "full_virtual_memory_test : could not unmap VA %p\n", pte_va);

                return ERROR;
            }

            // Disconnect the PTE
            candidate_pte->disk_format.always_zero = 0;
            candidate_pte->disk_format.pagefile_idx = disk_idx;
            candidate_pte->disk_format.on_disk = 1;

            return pfn;
        }
    }

    // No frame to steal was ever found
    return ERROR;
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
    if (is_memory_format(*pte) == FALSE) {
        fprintf(stderr, "Incorrect memory format of PTE given in write_to_disk\n");
        return ERROR;
    }
    
    // PTE has already been written to disk
    // if (is_disc_format(pte)) {
    //     return SUCCESS;
    // }

    ULONG64 disk_idx = 0;

    if (get_free_disk_idx(&disk_idx) == ERROR) {
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

    // *pte_va = 0x111111111;

    // Copy the memory over to the slot
    memcpy(disk_slot_addr, pte_va, PAGE_SIZE);

    // Simulate the fact that disk writes take a while
    // Sleep(10);

    // // Modify the PTE to reflect that it is now on the disk
    // pte->disk_format.always_zero = 0;
    // pte->disk_format.pagefile_idx = (ULONG64) disk_slot;
    // pte->disk_format.on_disk = 1;
    *disk_idx_storage = disk_idx;

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


ULONG64 free_disk_idx_search = 0;
static free_disk_idx_increment() {
    free_disk_idx_search++;
    if (free_disk_idx_search == DISK_SLOTS) {
        free_disk_idx_search = 0;
    }
}

/**
 * Writes an open disk idx into the result storage pointer
 * 
 * Returns SUCCESS if we successfully wrote a disk idx, ERROR otherwise (may be empty)
 * 
 */
int get_free_disk_idx(ULONG64* result_storage) {
    if (disk->num_open_slots == 0) {
        return ERROR;
    }

    ULONG64 result;
    
    if (disk->disk_slot_statuses[free_disk_idx_search] == DISK_FREESLOT) {
        result = free_disk_idx_search;
        free_disk_idx_increment();
        disk->num_open_slots -= 1;
        *result_storage = result;
        return SUCCESS;
    }

    // Iterate all the way through until we wrap
    ULONG64 end = free_disk_idx_search;
    free_disk_idx_increment();

    while(free_disk_idx_search != end) {
        if (disk->disk_slot_statuses[free_disk_idx_search] == DISK_FREESLOT) {
            result = free_disk_idx_search;
            free_disk_idx_increment();
            disk->num_open_slots -=1;

            *result_storage = result;
            return SUCCESS;
        }

        free_disk_idx_increment();
    }

    // We should never reach here
    assert(FALSE);
    return ERROR;
}




/**
 * Modifies the bitmap on the disk to indicate the given disk slot is free
 * 
 * Returns SUCCESS upon no issues, ERROR otherwise
 */
int return_disk_slot(ULONG64 disk_idx) {
    if (disk_idx > DISK_SLOTS) {
        fprintf(stderr, "Invalid disk_idx given to return_disk_slot\n");
        return ERROR;
    }

    if (disk->disk_slot_statuses[disk_idx] == DISK_FREESLOT) {
        fprintf(stderr, "Already free disk idx given to return_disk_slot\n");
        return ERROR;
    }

    disk->disk_slot_statuses[disk_idx] = DISK_FREESLOT;
    disk->num_open_slots++;

    return SUCCESS;
}