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

//TODO: Think about various types of structs we would have in here
// Each would have a status, but then they would have their own 

#ifndef PTE_T
#define PTE_T
/**
 * For frames that are either currently accessible or that are on the free list
 */
typedef struct {
    ULONG64 valid:1;
    ULONG64 frame_number:40;
    ULONG64 age:4;
} VALID_PTE;

/**
 * For frames that are currently invalid, and is on the disk. It is already being used by another process 
 * and is not rescuable. We need to fetch it from the disk at the pagefile address in order to access this VA again
 */
typedef struct {
    ULONG64 always_zero:1;
    ULONG64 pagefile_idx:40;
    // Will always be one for this structure
    ULONG64 on_disk:1;
} DISK_PTE;


/**
 * For frames that are modified but not accessible- they are either:
 * 1. Currently being written to disk
 * 2. Already written to disk, but on the standby list and is rescuable by the VA currently using it
 */
typedef struct {
    ULONG64 always_zero:1;
    ULONG64 frame_number:40;
    // It is not on the disk yet, so this is zero
    ULONG64 always_zero2:1;
} TRANSITION_PTE;


// Generic PTE struct that can handle all cases
typedef struct {
    union {
        VALID_PTE memory_format;
        DISK_PTE disk_format;
        TRANSITION_PTE transition_format;
    };
} PTE;

#endif

#ifndef PAGETABLE_T
#define PAGETABLE_T

// #define PAGETABLE_NUMLOCKS ((VIRTUAL_ADDRESS_SIZE / PAGE_SIZE) >> 8)

typedef struct {
    PTE* pte_list;
    ULONG64 num_virtual_pages;
    // To allow calculations from PTEs to virtual addresses and vice verca
    ULONG64 vmem_base;

    ULONG64 num_locks;
    CRITICAL_SECTION* pte_locks;
    // LOCK
} PAGETABLE;

#define MAX_AGE (1 << 5) - 1
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
