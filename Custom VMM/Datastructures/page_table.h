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

typedef struct {
    ULONG64 valid:1;
    ULONG64 frame_number:40;
    ULONG64 age:4;
} VALID_PTE;

typedef struct {
    ULONG64 always_zero:1;
    ULONG64 frame_number:40;
    ULONG64 on_disc:1;
} INVALID_PTE;

typedef struct {
    ULONG64 always_zero:1;
    ULONG64 frame_number:40;
    ULONG64 always_zero2:1;
} TRANSITION_PTE;

typedef struct {
    union {
        VALID_PTE memory_format;
        INVALID_PTE disc_format;
        TRANSITION_PTE transition_format;
    };
} PTE;