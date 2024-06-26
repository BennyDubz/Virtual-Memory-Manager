/**
 * @author Ben Williams
 * @date June 22nd, 2024
 * 
 * Header file for simulated disk
 */

#include "../hardware.h"
#include "./db_linked_list.h"
#include "./pagetable.h"
#include "./pagelists.h"
#include <windows.h>

#define DISK_SLOTS (DISK_SIZE / PAGE_SIZE)

#define DISK_USEDSLOT 0
#define DISK_FREESLOT 1

#ifndef DISK_T
#define DISK_T

typedef struct {
    /**
     *  Since we cannot actually access the disk, we need to use some of the virtual memory of the process
     *  that is running the simulation. We will actually commit the memory so that the disk should always
     *  be able to be initialized at the beginning of the simulation
     */
    PULONG_PTR base_address;
    UCHAR disk_slot_statuses[DISK_SLOTS];
    ULONG64 num_open_slots;
} DISK;

#endif


/**
 * Initializes the disk and commits the memory for it in the simulating process's virtual address space
 * 
 * Returns NULL upon any error
 */
DISK* initialize_disk();
