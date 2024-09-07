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

/**
 * We divide the disk into sections with their own locks to increase lock granularity
 * 
 * Note that we might have one more lock section to account for the additional two disk slots that we have that make
 * the total no longer divisible by two
 */
#define DISK_STORAGE_NUM_SECTIONS   max(DISK_STORAGE_SLOTS >> 4, 1);

// The maximum number of pages readable from a single thread at a time
#define MAX_PAGES_READABLE      16

#define DISK_READSECTION_SIZE  MAX_PAGES_READABLE
#define DISK_READSECTIONS       max(DISK_STORAGE_SLOTS >> 8, 64)

// The total number of page-sized virtual addresses we need to reser4ve for the disk
#define DISK_READ_SLOTS     (DISK_READSECTIONS * DISK_READSECTION_SIZE)

//
#define DISK_REFRESH_BOUNDARY (DISK_READSECTIONS / 4)


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
#define MAX_PAGES_WRITABLE      max(DISK_STORAGE_SLOTS >> 8, 512)

// For storage slots on the disk
#define DISK_USEDSLOT 0
#define DISK_FREESLOT 1

#define DISK_IDX_NOTUSED 0


#ifndef DISK_T
#define DISK_T

/**
 * We want the statuses to be far apart to reduce cache collisions
 */
typedef struct {
    volatile long status;
    long buffer[15];
} DISK_READSECTION_STATUS;


typedef struct {
    /**
     *  Since we are not actually accessing the disk, we need to use some of the virtual memory of the process
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

    DECLSPEC_ALIGN(64)
    volatile ULONG64 total_available_slots;

    /**
     * This supports the batched writing/reading of many pages to the disk from the mod-writer/faulter
     */
    PULONG_PTR disk_large_write_slot;
    PULONG_PTR disk_read_base_addr;

    /**
     * We maintain the disk read slots in their own array that we can use
     * interlocked operations on to claim and mark them.
     * 
     * This allows us to only have to clear/unmap disk read slots infrequently,
     * saving constant calls to MapUserPhysicalPagesScatter
     */
    DECLSPEC_ALIGN(64)
    DISK_READSECTION_STATUS disk_readsection_statuses[DISK_READSECTIONS];

    DECLSPEC_ALIGN(64)
    volatile ULONG64 num_available_readsections;

    // We use interlocked operations to help reduce the amount of linear searching that threads will have to do
    DECLSPEC_ALIGN(64)
    volatile ULONG64 disk_read_curr_idx; 
} DISK;


typedef struct {
    PULONG_PTR thread_disk_read_base;
    ULONG64 num_allocated_readsections;
    ULONG64 min_readsection_idx;
    ULONG64 curr_readsection_idx;
    ULONG64 max_readsection_idx;
} THREAD_DISK_READ_RESOURCES;

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
