/**
 * @author Ben Williams
 * @date June 18th, 2024
 * 
 * Header file for the pagetable and its functions
 */

#include <stdlib.h>
#include <windows.h>

// Bits for invalid/valid
#define INVALID 0
#define VALID 1

// PTE protection statuses
#define PTE_PROTNONE 0
#define PTE_PROTREAD 1
#define PTE_PROTWRITE 2
#define PTE_PROTREADWRITE  PTE_PROTREAD | PTE_PROTWRITE


/**
 * Influences disk reads by allowing us to "lock" a disk PTE until we are finished reading it from the disk 
 * Prevents unnecessesary disk reads and reduces PTE lock contention
 */
#define PTE_NOT_BEING_READ_FROM_DISK 0
#define PTE_BEING_READ_FROM_DISK 1


#ifndef PTE_T
#define PTE_T
/**
 * For frames that are either currently accessible or that are on the free list
 */
typedef struct {
    ULONG64 valid:1;
    ULONG64 frame_number:40;
    ULONG64 age:4;
    ULONG64 protections:2;
} VALID_PTE;


/**
 * For frames that are currently invalid, and is on the disk. It is already being used by another process 
 * and is not rescuable. We need to fetch it from the disk at the pagefile address in order to access this VA again
 */
typedef struct {
    ULONG64 always_zero:1;
    ULONG64 pagefile_idx:40;
    ULONG64 always_zero2:1;
    ULONG64 being_read_from_disk:1;
} DISK_PTE;


typedef struct {
    ULONG64 always_zero:1;
    ULONG64 frame_number:40;
    // It is not on the disk yet, so this is always 1
    ULONG64 is_transition:1;
} TRANSITION_PTE;


// Generic PTE struct that can handle all cases
typedef struct {
    union {
        VALID_PTE memory_format;
        DISK_PTE disk_format;
        TRANSITION_PTE transition_format;

        // We can check if this is zero to see if the PTE have been used before
        ULONG64 complete_format;
    };
} PTE;

#endif

#ifndef PAGETABLE_T
#define PAGETABLE_T

// #define PAGETABLE_NUMLOCKS ((VIRTUAL_ADDRESS_SIZE / PAGE_SIZE) >> 8)

typedef struct {
    CRITICAL_SECTION lock;
    volatile ULONG64 valid_pte_count;
    ULONG64 locksection_idx;
} PTE_LOCKSECTION;

typedef struct {
    PTE* pte_list;
    ULONG64 num_virtual_pages;
    // To allow calculations from PTEs to virtual addresses and vice verca
    ULONG64 vmem_base;

    ULONG64 num_locks;
    PTE_LOCKSECTION* pte_locksections;
    // CRITICAL_SECTION* pte_locks;
    // volatile ULONG64* valid_pte_counts;
    // LOCK
} PAGETABLE;

#define MAX_AGE (1 << 4) - 1
// typedef struct {
    
// } AGELISTS;
#endif

/**
 * Initializes the pagetable with all VALID_PTE entries, but all have the valid bit set to 0
 * 
 * Returns a pointer to a pagetable containing all invalid PTEs ready for assignment
 */
PAGETABLE* initialize_pagetable(ULONG64 num_virtual_pages, PULONG_PTR vmem_base);


/**
 * Returns the contents of the given PTE in one operation indivisibly
 */
PTE read_pte_contents(PTE* pte_to_read);


/**
 * Writes the PTE contents as a single indivisble write to the given PTE pointer
 */
void write_pte_contents(PTE* pte_to_write, PTE pte_contents);

/**
 * Returns TRUE if the PTE is in the memory format, FALSE otherwise
 * or if the PTE is NULL
 */
BOOL is_memory_format(PTE pte);


/**
 * Returns TRUE if the PTE is in the transition format, FALSE otherwise
 * or if the PTE is NULL
 */
BOOL is_disk_format(PTE pte);


/**
 * Returns TRUE if the PTE is in the disc format, FALSE otherwise
 * or if the PTE is NULL
 */
BOOL is_transition_format(PTE pte);


/**
 * Returns TRUE if the PTE has ever been accessed, FALSE otherwise
 * or if the PTE is NULL
 */
BOOL is_used_pte(PTE pte);


/**
 * Returns TRUE if both PTEs are equivalent, FALSE otherwise
 */
BOOL ptes_are_equal(PTE pte1, PTE pte2);