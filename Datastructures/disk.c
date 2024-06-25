/**
 * @author Ben Williams
 * @date June 22nd, 2024
 * 
 * Implementation of a simulated disk
 */

#include <stdio.h>
#include <windows.h>
#include "./disk.h"


/**
 * Initializes the disk and commits the memory for it in the simulating process's virtual address space
 * 
 * Returns NULL upon any error
 */
DISK* initialize_disk() {
    DISK* disk = (DISK*) malloc(sizeof(DISK));

    if (disk == NULL) {
        fprintf(stderr, "Unable to allocate memory for disk struct in initialize_disk\n");
        return NULL;
    }

    PULONG_PTR disk_base = VirtualAlloc(NULL, DISK_SIZE, MEM_RESERVE, PAGE_READWRITE);
    // PULONG_PTR disk_base = (PULONG_PTR) malloc(DISK_SIZE);

    // printf("Size is %llX\n, num slots %llX", DISK_SIZE, DISK_SLOTS);

    if (disk_base == NULL) {
        fprintf(stderr, "Unable to virtual alloc memory for the disk base in initialize_disk\n");
        free(disk);
        return NULL;
    }

    DB_LL_NODE* disk_listhead = db_create_list();

    if (disk_listhead == NULL) {
        fprintf(stderr, "Unable to initialize disk listhead\n");
        return NULL;
    }
    
    int i = 0;

    PULONG_PTR disk_end = disk_base + (DISK_SIZE / sizeof(PULONG_PTR));
    
    // Add each slot of the disk to the disk listhead, so that each can be popped and treated as a page of storage
    for (PULONG_PTR disk_slot = disk_base; disk_slot < disk_end; disk_slot += (PAGE_SIZE / sizeof(ULONG_PTR))) {

        PULONG_PTR slot = VirtualAlloc(disk_slot, PAGE_SIZE, MEM_COMMIT, PAGE_READWRITE);

        // printf("diff is %llX, pagesize is %X\n", disk_slot - prev, PAGE_SIZE);
        // printf("curr slot: %X, prev %X\n", disk_slot, prev);
        
        *disk_slot = 0x12345678;

        if (db_insert_at_head(disk_listhead, slot) == NULL) {
            fprintf(stderr, "Unable to create listnode for disk slot address\n");
            return NULL;
        }
        // prev = disk_slot;
        i++; 
    }


    disk->base_address = disk_base;
    disk->disk_slot_listhead = disk_listhead;

    return disk;
}


/**
 * Returns a pointer to an open disk slot, if there are any
 * 
 * Returns NULL if the disk does not exist or if there are no slots left
 */
PULONG_PTR get_free_disk_slot(DISK* disk) {
    if (disk == NULL) {
        fprintf(stderr, "NULL disk given to get_free_disk_slot\n");
        return NULL;
    }

    PULONG_PTR disk_slot = db_pop_from_head(disk->disk_slot_listhead);

    if (disk_slot == NULL) {
        fprintf(stderr, "No disk slots remaining in get_free_disk_slot\n");
        return NULL;
    }

    return disk_slot;
}


/**
 * Writes the given PTE to the disk, and modifies the PTE to reflect this
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 */
int write_to_disk(PAGETABLE* pagetable, PTE* pte, DISK* disk) {
    if (pte == NULL || disk == NULL) {
        fprintf(stderr, "NULL pte or disk given to write_to_disk\n");
        return ERROR;
    } 

    //BW: We could also have a race condition where two threads are writing to the disk at the same time,
    // that scenario may change this line to checking if the PTE is also already in disk format
    if (is_memory_format(pte) == FALSE) {
        fprintf(stderr, "Incorrect memory format of PTE given in write_to_disk\n");
        return ERROR;
    }
    
    // PTE has already been written to disk
    // if (is_disc_format(pte)) {
    //     return SUCCESS;
    // }

    PULONG_PTR disk_slot = get_free_disk_slot(disk);

    if (disk_slot == NULL) {
        fprintf(stderr, "Unable to write to disk as there are no slots remaining\n");
        return ERROR;
    }

    // *disk_slot = 0x87654321;

    PULONG_PTR pte_va = pte_to_va(pagetable, pte);

    if (pte_va == NULL) {
        fprintf(stderr, "Error getting pte VA in write_to_disk\n");
        return ERROR;
    }

    // *pte_va = 0x111111111;

    // Copy the memory over to the slot
    memcpy(disk_slot, pte_va, PAGE_SIZE);

    // Simulate the fact that disk writes take a while
    // Sleep(10);

    // Modify the PTE to reflect that it is now on the disk
    pte->disk_format.always_zero = 0;
    pte->disk_format.pagefile_address = (ULONG64) disk_slot;
    pte->disk_format.on_disk = 1;

    return SUCCESS;
}


/**
 * Fetches the memory for the given PTE on the disk, puts it on the given open page.
 * It then edits the PTE to reflect that it is now valid and accessible.
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 */
int get_from_disk(PAGETABLE* pagetable, PTE* pte, ULONG64 pfn, DISK* disk) {
    if (pte == NULL || disk == NULL) {
        fprintf(stderr, "NULL pte, page, or disk given to get_from_disk");
        return ERROR;
    }

    if (is_transition_format(pte) == FALSE) {
        fprintf(stderr, "PTE is not in disk format in get_from_disk\n");
        return ERROR;
    }

    PULONG_PTR disk_slot = (PULONG_PTR) pte->disk_format.pagefile_address;

    PULONG_PTR pte_va = pte_to_va(pagetable, pte);

    if (pte_va == NULL) {
        fprintf(stderr, "NULL destination virtual address in get_from_disk\n");
        return ERROR;
    }

    // Allocate the physical frame to the virtual address
    if (MapUserPhysicalPages (pte_va, 1, &pfn) == FALSE) {

        printf ("full_virtual_memory_test : could not map VA %p to page %llX\n", pte_va, pfn);

        return ERROR;
    }

    if (memcpy(pte_va, disk_slot, (size_t) PAGE_SIZE) == NULL) {
        fprintf(stderr, "Unable to copy memory from pte to disk\n");
        return ERROR;
    }

    // Now that the data is copied, we can update the actual PTE
    pte->memory_format.age = 0;
    pte->memory_format.frame_number = pfn;
    pte->memory_format.valid = VALID;

    return SUCCESS;
}