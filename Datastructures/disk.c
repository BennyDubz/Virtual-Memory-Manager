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

    PULONG_PTR disk_base = VirtualAlloc(NULL, DISK_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    // PULONG_PTR disk_base = (PULONG_PTR) malloc(DISK_SIZE);

    // printf("Size is %llX\n, num slots %llX", DISK_SIZE, DISK_SLOTS);

    if (disk_base == NULL) {
        fprintf(stderr, "Unable to virtual alloc memory for the disk base in initialize_disk\n");
        free(disk);
        return NULL;
    }
    
    for (ULONG64 disk_idx = 0; disk_idx < DISK_SLOTS; disk_idx++) {
        disk->disk_slot_statuses[disk_idx] = DISK_FREESLOT;
    }

    
    // Add each slot of the disk to the disk listhead, so that each can be popped and treated as a page of storage
    // for (PULONG_PTR disk_slot = disk_base; disk_slot < disk_end; disk_slot += (PAGE_SIZE / sizeof(ULONG_PTR))) {

    //     PULONG_PTR slot = VirtualAlloc(disk_slot, PAGE_SIZE, MEM_COMMIT, PAGE_READWRITE);

        
    // }
    disk->num_open_slots = DISK_SLOTS;
    disk->base_address = disk_base;

    return disk;
}
