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

// Status possibilities
#define ACCESSIBLE 0
#define TRIMMED 1

// Modified?


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
    ULONG64 pagefile_address:40;
    ULONG64 on_disc:1;
} DISK_PTE;


/**
 * For frames that are modified but not accessible- they are either:
 * 1. Currently being written to disk
 * 2. Already written to disk, but on the standby list and is rescuable by the VA currently using it
 */
typedef struct {
    ULONG64 always_zero:1;
    ULONG64 frame_number:40;
    ULONG64 always_zero2:1;
} TRANSITION_PTE;


// Generic PTE struct that can handle all cases
typedef struct {
    union {
        VALID_PTE memory_format;
        DISK_PTE disc_format;
        TRANSITION_PTE transition_format;
    };
} PTE;

#endif

#ifndef PAGETABLE_T
#define PAGETABLE_T
typedef struct {
    PTE* frame_list;
    // LOCK
} PAGETABLE;
#endif

/**
 * Initializes the pagetable with all VALID_PTE entries, but all have the valid bit set to 0
 * 
 * Returns a pointer to a list of PTEs with num_physical_frames entries, or NULL if there is an error
 */
PTE* initialize_pagetable(ULONG64 num_physical_frames, ULONG64* physical_frame_numbers);
