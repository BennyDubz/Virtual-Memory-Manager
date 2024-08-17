/**
 * @author Ben Williams
 * @date June 22nd, 2024
 * 
 * Implementation of a simulated disk
 */

#include <stdio.h>
#include <windows.h>
#include "./disk.h"
#include "./custom_sync.h"

static int initialize_disk_write(DISK* disk, MEM_EXTENDED_PARAMETER* vmem_parameters);
static int initialize_disk_read(DISK* disk, MEM_EXTENDED_PARAMETER* vmem_parameters);


/**
 * Initializes the disk and commits the memory for it in the simulating process's virtual address space
 * 
 * Takes in the MEM_EXTENDED_PARAMETER so that the disk reading/writing operations can
 * work in the simulation using MapUserPhysicalPages alongside shared pages.
 * 
 * Returns NULL upon any error
 */
DISK* initialize_disk(MEM_EXTENDED_PARAMETER* vmem_parameters) {
    DISK* disk = (DISK*) malloc(sizeof(DISK));

    if (disk == NULL) {
        fprintf(stderr, "Unable to allocate memory for disk struct in initialize_disk\n");
        return NULL;
    }

    PULONG_PTR disk_base = VirtualAlloc(NULL, DISK_SIZE, 
                        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    

    if (disk_base == NULL) {
        fprintf(stderr, "Unable to virtual alloc memory for the disk base in initialize_disk\n");
        return NULL;
    }


    disk->base_address = disk_base;

    /**
     * Initialize disk slot locks
     */
    ULONG64 num_locks = max(DISK_STORAGE_SLOTS >> 6, 1);
    ULONG64 slots_per_lock = DISK_STORAGE_SLOTS / num_locks;

    ULONG64 remaining_disk_slots = DISK_STORAGE_SLOTS % num_locks;
    BOOL extra_lock;
    // Determine whether we need an extra lock for the last few disk slots
    if (remaining_disk_slots != 0) {
        num_locks++;
        extra_lock = TRUE;
    } else {
        extra_lock = FALSE;
    }

    CRITICAL_SECTION* disk_slot_locks = (CRITICAL_SECTION*) malloc(sizeof(CRITICAL_SECTION) * num_locks);

    if (disk_slot_locks == NULL) {
        fprintf(stderr, "Unable to allocate memory for disk slot locks in initialize_disk\n");
        return NULL;
    }

    ULONG64* disk_open_slots = (ULONG64*) malloc(sizeof(ULONG64) * num_locks);

    if (disk_open_slots == NULL) {
        fprintf(stderr, "Unable to allocate memory for disk_open_slots in initialize_disk\n");
        return NULL;
    }

    // We will set all of the final indices to DISK_USED if we have an extra lock for the last few disk slots
    ULONG64 extra_invalid_slot_count = slots_per_lock - remaining_disk_slots;
    ULONG64 total_char_amount = extra_lock ? DISK_STORAGE_SLOTS + extra_invalid_slot_count : DISK_STORAGE_SLOTS;

    UCHAR* disk_slot_statuses = (UCHAR*) malloc(sizeof(UCHAR) * total_char_amount);

    if (disk_slot_statuses == NULL) {
        fprintf(stderr, "Unable to allocate memory for disk slots in initialize_disks\n");
        return NULL;
    }

    /**
     * We set all of the disk slots to be free initially, EXCEPT disk slot 0
     * as it may cause us to believe a PTE is unaccessed when it is really on disk.
     * 
     * This is because the all other fields of the disk format PTE may be 0, so if the disk_idx
     * stored in the PTE was also 0, we may confuse it.
     */
    disk_slot_statuses[0] = DISK_USEDSLOT;
    for (ULONG64 disk_idx = 1; disk_idx < DISK_STORAGE_SLOTS; disk_idx++) {
        disk_slot_statuses[disk_idx] = DISK_FREESLOT;
    }

    // Check to see if we want to set the last bit of the statuses to USED (or invalid)
    if (extra_lock) {
        for (ULONG64 invalid_disk_idx = DISK_STORAGE_SLOTS; invalid_disk_idx < (DISK_STORAGE_SLOTS) + extra_invalid_slot_count; invalid_disk_idx++) {
            disk_slot_statuses[invalid_disk_idx] = DISK_USEDSLOT;
        }
    }

    disk->disk_slot_statuses = disk_slot_statuses;


    /**
     * Keeping a count of the number of open disk slots in each section can make
     * searching for disk slots much easier
     */
    for (ULONG64 lock_num = 0; lock_num < num_locks; lock_num++) {
        initialize_lock(&disk_slot_locks[lock_num]);
        if (lock_num == 0) {
            disk_open_slots[lock_num] = slots_per_lock - 1;
        } else if (extra_lock && lock_num == num_locks - 1){
            disk_open_slots[lock_num] = remaining_disk_slots;
        } else {
            disk_open_slots[lock_num] = slots_per_lock;
        }
    }

    disk->num_locks = num_locks;
    disk->slots_per_lock = slots_per_lock;
    disk->disk_slot_locks = disk_slot_locks;
    disk->open_slot_counts = disk_open_slots;
    disk->total_available_slots = DISK_STORAGE_SLOTS - 1;

    /**
     * Create special virtual addresses for writing to the disk
     * and reading from the disk.
     */
    if (initialize_disk_write(disk, vmem_parameters) == ERROR) {
        fprintf(stderr, "Failed to initialize disk write in initialize_disk\n");
        return NULL;
    }

    if (initialize_disk_read(disk, vmem_parameters) == ERROR) {
        fprintf(stderr, "Failed to initialize disk write in initialize_disk\n");
        return NULL;
    }

    return disk;
}


/**
 * Initializes all of the disk write slots, their list, and its lock for the new disk struct
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 */
static int initialize_disk_write(DISK* disk, MEM_EXTENDED_PARAMETER* vmem_parameters) {
    /**
     * We virtual alloc a large virtual address space to support large, batched writes
     */
    PULONG_PTR disk_large_write_slot;

    disk_large_write_slot = VirtualAlloc2(NULL, NULL, PAGE_SIZE * MAX_PAGES_WRITABLE,
                        MEM_PHYSICAL | MEM_RESERVE, PAGE_READWRITE, vmem_parameters, 1);
    
    if (disk_large_write_slot == NULL) {
        fprintf(stderr, "Failed to virtual alloc the large disk write slot\n");
        return ERROR;
    }
    disk->disk_large_write_slot = disk_large_write_slot;

    return SUCCESS;
}


/**
 * Initializes all of the disk read slots, their list, and its lock for the new disk struct
 * 
 * Return SUCCESS if there are no issues, ERROR otherwise
 */
static int initialize_disk_read(DISK* disk, MEM_EXTENDED_PARAMETER* vmem_parameters) {
    
    PULONG_PTR disk_read_base;
    disk_read_base = VirtualAlloc2(NULL, NULL, PAGE_SIZE * DISK_READ_SLOTS,
                        MEM_PHYSICAL | MEM_RESERVE, PAGE_READWRITE, vmem_parameters, 1);

    
    if (disk_read_base == NULL) {
        fprintf(stderr, "Unable to virtual alloc memory for disk_read_base in initialize_disk\n");
        return ERROR;
    }

    disk->disk_read_base_addr = disk_read_base;
    for (ULONG64 slot_num = 0; slot_num < DISK_READ_SLOTS; slot_num++) {
        disk->disk_readsection_statuses[slot_num] = DISK_READSECTION_OPEN;
    }

    disk->num_available_readsections = DISK_READSECTIONS;
    disk->disk_read_curr_idx = 0;

    return SUCCESS;
}
