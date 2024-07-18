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
#define DISK_READ_SLOTS 256
#define DISK_REFRESH_BOUNDARY (DISK_READ_SLOTS / 2)

#define DISK_READ_OPEN 0
#define DISK_READ_USED 1
#define DISK_READ_NEEDS_FLUSH 2 // We will clear all of these slots to 0 simultaneously

// For the large disk write slot
#define MAX_PAGES_WRITABLE 256


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
    UCHAR* disk_slot_statuses;

    CRITICAL_SECTION* disk_slot_locks;
    ULONG64 num_locks;
    ULONG64* open_slot_counts;
    ULONG64 total_available_slots;

    /**
     * To support batched writing, we will have a special virtual address that can map in many physical pages
     */
    PULONG_PTR disk_large_write_slot;

    /**
     * We will have unique virtual addresses dedicated to reading and writing from the disk
     * that are stored in these listheads so that they can be accessed in O(1) time, rather than
     * the bitmap.
     */
    DB_LL_NODE* disk_write_listhead;
    CRITICAL_SECTION disk_write_list_lock;

    /**
     * We maintain the disk read slots in their own array that we can use
     * interlocked operations on to claim and mark them.
     * 
     * This allows us to only have to clear/unmap disk read slots infrequently,
     * saving constant calls to MapUserPhysicalPagesScatter
     */
    volatile long disk_read_slot_statues[DISK_READ_SLOTS];
    PULONG_PTR disk_read_base_addr;
    volatile ULONG64 num_available_read_slots;

    // DB_LL_NODE* disk_read_listhead;
    // CRITICAL_SECTION disk_read_list_lock;

} DISK;

#endif


/**
 * Initializes the disk and commits the memory for it in the simulating process's virtual address space
 * 
 * Takes in the MEM_EXTENDED_PARAMETER so that the disk reading/writing operations can
 * work in the simulation using MapUserPhysicalPages alongside shared pages.
 * 
 * Returns NULL upon any error
 */
DISK* initialize_disk(MEM_EXTENDED_PARAMETER* vmem_parameters);
