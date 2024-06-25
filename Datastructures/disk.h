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
 
#ifndef DISK_T
#define DISK_T

typedef struct {
    /**
     *  Since we cannot actually access the disk, we need to use some of the virtual memory of the process
     *  that is running the simulation. We will actually commit the memory so that the disk should always
     *  be able to be initialized at the beginning of the simulation
     */
    PULONG_PTR base_address;
    DB_LL_NODE* disk_slot_listhead;
    // LOCK
} DISK;

#endif


/**
 * Initializes the disk and commits the memory for it in the simulating process's virtual address space
 * 
 * Returns NULL upon any error
 */
DISK* initialize_disk();


/**
 * Returns a pointer to an open disk slot, if there are any
 * 
 * Returns NULL if the disk does not exist or if there are no slots left
 */
PULONG_PTR get_free_disk_slot(DISK* disk);


/**
 * Writes the given PTE to the disk, and modifies the PTE to reflect this
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 */
int write_to_disk(PAGETABLE* pagetable, PTE* pte, DISK* disk);


/**
 * Fetches the memory for the given PTE on the disk, puts it on the given open page.
 * It then edits the PTE to reflect that it is now valid and accessible.
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 */
int get_from_disk(PAGETABLE* pagetable, PTE* pte, ULONG64 pfn, DISK* disk);