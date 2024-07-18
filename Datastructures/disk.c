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

    PULONG_PTR disk_base = VirtualAlloc(NULL, DISK_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (disk_base == NULL) {
        fprintf(stderr, "Unable to virtual alloc memory for the disk base in initialize_disk\n");
        return NULL;
    }

    disk->base_address = disk_base;
    
    
    UCHAR* disk_slot_statuses = (UCHAR*) malloc(sizeof(UCHAR) * DISK_STORAGE_SLOTS);

    if (disk_slot_statuses == NULL) {
        fprintf(stderr, "Unable to allocate memory for disk slots in initialize_disks\n");
        return NULL;
    }
    printf("Making %d disk slots\n", DISK_STORAGE_SLOTS);

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
    disk->disk_slot_statuses = disk_slot_statuses;

    /**
     * Initialize disk slot locks
     */
    ULONG64 num_locks = max(DISK_STORAGE_SLOTS >> 4, 1);
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

    ULONG64 slots_per_lock = DISK_STORAGE_SLOTS / num_locks;

    for (ULONG64 lock_num = 0; lock_num < num_locks; lock_num++) {
        initialize_lock(&disk_slot_locks[lock_num]);
        if (lock_num == 0) {
            disk_open_slots[lock_num] = slots_per_lock - 1;
        } else {
            disk_open_slots[lock_num] = slots_per_lock;
        }
    }

    disk->num_locks = num_locks;
    disk->disk_slot_locks = disk_slot_locks;
    disk->open_slot_counts = disk_open_slots;
    disk->total_available_slots = DISK_STORAGE_SLOTS;

    /**
     * Create special virtual addresses and PTEs for writing to the disk
     * and reading from the disk. These should each have their own linked list that
     * we can pop from. This way, multiple threads can read and write from the disk
     * concurrently. These two lists would need their own locks, but
     * the hold time on them should be very brief.
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
    // We want the virtual memory to be separate from the user address space
    PULONG_PTR disk_write_base;
    if (vmem_parameters == NULL) {
        disk_write_base = VirtualAlloc(NULL, PAGE_SIZE * DISK_WRITE_SLOTS, 
                        MEM_PHYSICAL | MEM_RESERVE, PAGE_READWRITE);
    } else {
        disk_write_base = VirtualAlloc2(NULL, NULL, PAGE_SIZE * DISK_WRITE_SLOTS,
                        MEM_PHYSICAL | MEM_RESERVE, PAGE_READWRITE, vmem_parameters, 1);
    }
    
    if (disk_write_base == NULL) {
        fprintf(stderr, "Unable to virtual alloc memory for disk_write_base in initialize_disk\n");
        return ERROR;
    }

    DB_LL_NODE* disk_write_listhead = db_create_list();

    if (disk_write_listhead == NULL) {
        fprintf(stderr, "Failed to allocate memory for disk write listhead\n");
        return ERROR;
    }

    // Allocate a slot and listnode for each address we virtual alloc'd
    for (ULONG64 slot_num = 0; slot_num < DISK_WRITE_SLOTS; slot_num++) {
        DISK_RW_SLOT* disk_w_slot = (DISK_RW_SLOT*) malloc(sizeof(DISK_RW_SLOT));

        if (disk_w_slot == NULL) {
            fprintf(stderr, "Failed to allocate memory for disk_w_slot in initialize_disk_write\n");
            return ERROR;
        }

        PULONG_PTR disk_write_addr = disk_write_base + (slot_num * PAGE_SIZE / sizeof(PULONG_PTR));
        DB_LL_NODE* disk_w_node = db_create_node(disk_write_addr);

        if (disk_w_node == NULL) {
            fprintf(stderr, "Failed to allocate memory for disk_w_node in initialize_disk_write\n");
            return ERROR;
        }

        disk_w_slot->listnode = disk_w_node;
        disk_w_slot->rw_address = disk_write_addr;
        disk_w_node->item = disk_w_slot;

        db_insert_node_at_head(disk_write_listhead, disk_w_node);
    }

    initialize_lock(&disk->disk_write_list_lock);
    disk->disk_write_listhead = disk_write_listhead;

    /**
     * We also virtual alloc a large virtual address space to support large, batched writes
     */
    PULONG_PTR disk_large_write_slot;
    
    if (vmem_parameters == NULL) {
        disk_large_write_slot = VirtualAlloc(NULL, PAGE_SIZE * MAX_PAGES_WRITABLE, 
                        MEM_PHYSICAL | MEM_RESERVE, PAGE_READWRITE); 
    } else {
        disk_large_write_slot = VirtualAlloc2(NULL, NULL, PAGE_SIZE * MAX_PAGES_WRITABLE,
                        MEM_PHYSICAL | MEM_RESERVE, PAGE_READWRITE, vmem_parameters, 1);
    }
    
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
    
    /**
     * We want the disk read/write slots to have separate virtual address ranges from the user
     * in our simulation. If vmem_parameters is NULL, then we can simply do a normal VirtualAlloc.
     * 
     * Otherwise, we need to use VirtualAlloc2 in order to adjust to the requirements of
     * SUPPORT_MULTIPLE_VA_TO_PAGE being on
     */
    PULONG_PTR disk_read_base;
    if (vmem_parameters == NULL) {
        disk_read_base = VirtualAlloc(NULL, PAGE_SIZE * DISK_READ_SLOTS, 
                        MEM_PHYSICAL | MEM_RESERVE, PAGE_READWRITE);
    } else {
        disk_read_base = VirtualAlloc2(NULL, NULL, PAGE_SIZE * DISK_READ_SLOTS,
                        MEM_PHYSICAL | MEM_RESERVE, PAGE_READWRITE, vmem_parameters, 1);
    }

    
    if (disk_read_base == NULL) {
        fprintf(stderr, "Unable to virtual alloc memory for disk_read_base in initialize_disk\n");
        return ERROR;
    }

    disk->disk_read_base_addr = disk_read_base;
    for (ULONG64 slot_num = 0; slot_num < DISK_READ_SLOTS; slot_num++) {
        disk->disk_read_slot_statues[slot_num] = DISK_READ_OPEN;
    }

    disk->num_available_read_slots = DISK_READ_SLOTS;

    #if 0
    DB_LL_NODE* disk_read_listhead = db_create_list();

    if (disk_read_listhead == NULL) {
        fprintf(stderr, "Failed to allocate memory for disk read listhead\n");
        return ERROR;
    }

    // Allocate a slot and listnode for each address we virtual alloc'd
    for (ULONG64 slot_num = 0; slot_num < DISK_READ_SLOTS; slot_num++) {
        DISK_RW_SLOT* disk_r_slot = (DISK_RW_SLOT*) malloc(sizeof(DISK_RW_SLOT));

        if (disk_r_slot == NULL) {
            fprintf(stderr, "Failed to allocate memory for disk_r_slot in initialize_disk_read\n");
            return ERROR;
        }

        PULONG_PTR disk_read_addr = disk_read_base + (slot_num * PAGE_SIZE / sizeof(PULONG_PTR));
        DB_LL_NODE* disk_r_node = db_create_node(disk_read_addr);

        if (disk_r_node == NULL) {
            fprintf(stderr, "Failed to allocate memory for disk_r_node in initialize_disk_read\n");
            return ERROR;
        }

        disk_r_slot->listnode = disk_r_node;
        disk_r_slot->rw_address = disk_read_addr;
        disk_r_node->item = disk_r_slot;

        db_insert_node_at_head(disk_read_listhead, disk_r_node);
    }

    initialize_lock(&disk->disk_read_list_lock);
    disk->disk_read_listhead = disk_read_listhead;
    #endif

    return SUCCESS;
}
