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
#include "../macros.h"
#include <windows.h>

#define DISK_STORAGE_SLOTS (DISK_SIZE / PAGE_SIZE)

// The maximum number of pages readable from a single thread at a time
#define MAX_PAGES_READABLE      16

#define DISK_READSECTION_SIZE  MAX_PAGES_READABLE
#define DISK_READSECTIONS max(DISK_STORAGE_SLOTS >> 10, 64)

// The total number of page-sized virtual addresses we need to reserve for the disk
#define DISK_READ_SLOTS     (DISK_READSECTIONS * DISK_READSECTION_SIZE)

#define DISK_REFRESH_BOUNDARY (DISK_READSECTIONS / 2)

/**
 * Rather than have a single page-sized virtual address for the disk reads, 
 * we have a large one that we break into sections of (DISK_READSECTION_SIZE * PAGE_SIZE).
 * 
 * If the readsection is open, then its corresponding section of the large virtual address is unmapped.
 * 
 * If the readsection is used, then its corresponding section of the large virtual address is potentially mapped -
 * but the using thread is not done with it yet
 * 
 * If the readsection needs to be flushed, the thread that used the address is finished with it. However, the
 * address is still mapped and needs to be unmapped. However, this unmapping is expensive as we need to make
 * another call to MapUserPhysicalPages. Therefore, we wait for many readsections to require being flushed before we
 * clear them all at once - finally setting them to open.
 */
#define DISK_READSECTION_OPEN 0
#define DISK_READSECTION_USED 1
#define DISK_READSECTION_NEEDS_FLUSH 2 

// This determines the size of the large write slot that the mod-writer uses
#define MAX_PAGES_WRITABLE      max(DISK_STORAGE_SLOTS >> 4, 512)

// For storage slots on the disk
#define DISK_USEDSLOT 0
#define DISK_FREESLOT 1

#define DISK_IDX_NOTUSED 0


#ifndef DISK_T
#define DISK_T

typedef struct {
    /**
     *  Since we cannot actually access the disk, we need to use some of the virtual memory of the process
     *  that is running the simulation. We will actually commit the memory so that the disk should always
     *  be able to be initialized at the beginning of the simulation
     * 
     * If we fail to malloc the amount of memory we need for the disk, then we do not run the simulation
     */
    PULONG_PTR base_address;
    UCHAR* disk_slot_statuses;

    CRITICAL_SECTION* disk_slot_locks;
    ULONG64 num_locks;
    ULONG64 slots_per_lock;
    ULONG64* open_slot_counts;
    volatile ULONG64 total_available_slots;

    /**
     * This supports the batched writing of many pages to the disk from the mod-writer
     */
    PULONG_PTR disk_large_write_slot;


    /**
     * We maintain the disk read slots in their own array that we can use
     * interlocked operations on to claim and mark them.
     * 
     * This allows us to only have to clear/unmap disk read slots infrequently,
     * saving constant calls to MapUserPhysicalPagesScatter
     */
    volatile long disk_readsection_statuses[DISK_READ_SLOTS];
    PULONG_PTR disk_read_base_addr;
    volatile ULONG64 num_available_readsections;

    // We use interlocked operations to help reduce the amount of linear searching that threads will have to do
    volatile ULONG64 disk_read_curr_idx; 


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
