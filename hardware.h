/**
 * @author Ben Williams
 * @date June 18th, 2024
 * 
 * Hardware specs for our simulation
 */

#include "./macros.h"
#include <windows.h>
#include <stdio.h>



// #define DEBUG_CHECKING      0
#ifdef DEBUG_CHECKING
#define PRINT_F printf
#else
#define PRINT_F 
#endif 


// Representing how many powers of 2 large the pages are in bytes
#define PAGE_POWER                  12 

// The size of the page in bytes
#define PAGE_SIZE                   (1 << PAGE_POWER)

#define DOWN_TO_PAGE_ADDR(x) (x & ~(PAGE_SIZE - 1))
#define DOWN_TO_PAGE_NUM(x) (x >> PAGE_POWER)

#ifndef LARGE_SIM

/**
 * SMALL SIMULATION
 */
#define VIRTUAL_ADDRESS_SIZE        MB(16)

#define VIRTUAL_ADDRESS_SIZE_IN_UNSIGNED_CHUNKS        (VIRTUAL_ADDRESS_SIZE / sizeof (ULONG_PTR))

#define CACHE_SIZE      KB(16)

#define DISK_SIZE       VIRTUAL_ADDRESS_SIZE

#define NUMBER_OF_PHYSICAL_PAGES   ((VIRTUAL_ADDRESS_SIZE / PAGE_SIZE) / 3)


#else

/**
 * LARGE SIMULATION
 */
#define VIRTUAL_ADDRESS_SIZE        GB(1)

#define VIRTUAL_ADDRESS_SIZE_IN_UNSIGNED_CHUNKS        (VIRTUAL_ADDRESS_SIZE / sizeof (ULONG_PTR))

#define CACHE_SIZE      MB(1)

#define DISK_SIZE       VIRTUAL_ADDRESS_SIZE

#define NUMBER_OF_PHYSICAL_PAGES   ((VIRTUAL_ADDRESS_SIZE / PAGE_SIZE) / 3)
#endif
