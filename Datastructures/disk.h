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

#define DISK_STORAGE_SLOTS (DISK_SIZE / PAGE_SIZE)
#define DISK_WRITE_SLOTS 8
#define DISK_READ_SLOTS 8

#define DISK_USEDSLOT 0
#define DISK_FREESLOT 1

#define DISK_IDX_NOTUSED 0

#ifndef DISK_T
#define DISK_T

/**
 * We have these bundled as we must never have to reallocate memory for the listnode
 * after we first create it. The disk must work even if we later run out of memory and can
 * no longer malloc.
 */
typedef struct {
    PULONG_PTR rw_address;
    DB_LL_NODE* listnode;
} DISK_RW_SLOT;


typedef struct {
    /**
     *  Since we cannot actually access the disk, we need to use some of the virtual memory of the process
     *  that is running the simulation. We will actually commit the memory so that the disk should always
     *  be able to be initialized at the beginning of the simulation
     */
    PULONG_PTR base_address;
    UCHAR disk_slot_statuses[DISK_STORAGE_SLOTS];

    CRITICAL_SECTION* disk_slot_locks;
    ULONG64 num_locks;
    ULONG64* open_slot_counts;

    /**
     * We will have unique virtual addresses dedicated to reading and writing from the disk
     * that are stored in these listheads so that they can be accessed in O(1) time, rather than
     * the bitmap.
     */
    DB_LL_NODE* disk_write_listhead;
    CRITICAL_SECTION disk_write_list_lock;

    DB_LL_NODE* disk_read_listhead;
    CRITICAL_SECTION disk_read_list_lock;
} DISK;

#endif


/**
 * Initializes the disk and commits the memory for it in the simulating process's virtual address space
 * 
 * Returns NULL upon any error
 */
DISK* initialize_disk();
