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
typedef struct pte {
    short int status;
    union information{
        ULONG64 address;
    }
} pte_t;