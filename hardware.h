/**
 * @author Ben Williams
 * @date June 18th, 2024
 * 
 * Hardware specs for our simulation
 */

#include "./macros.h"


#define PAGE_SIZE                   4096

//
// This is intentionally a power of two so we can use masking to stay
// within bounds.
//
#define VIRTUAL_ADDRESS_SIZE        MB(16)

#define VIRTUAL_ADDRESS_SIZE_IN_UNSIGNED_CHUNKS        (VIRTUAL_ADDRESS_SIZE / sizeof (ULONG_PTR))

#define CACHE_SIZE KB(16)

//
// Deliberately use a physical page pool that is approximately 1% of the
// virtual address space !
//
#define NUMBER_OF_PHYSICAL_PAGES   ((VIRTUAL_ADDRESS_SIZE / PAGE_SIZE) / 64)