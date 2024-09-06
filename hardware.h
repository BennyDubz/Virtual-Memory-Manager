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
#define VIRTUAL_ADDRESS_SIZE        MB(8)

#define VIRTUAL_ADDRESS_SIZE_IN_UNSIGNED_CHUNKS        (VIRTUAL_ADDRESS_SIZE / sizeof (ULONG_PTR))

#define CACHE_SIZE      KB(16)

/**
 * Right now, we need the + 2 as we do not use the 0th disk slot, and we need an extra for mod-writing when all other slots are taken.
 * 
 * Currently, we use excess disk space to support us being able to preserve disk slots - keeping a note of them on the physical pages when we **read** from 
 * a virtual address that had a corresponding copy on the disk (whether it be a rescue, or a disk-read). This allows us to trim pages straight to the standby
 * list if they still have a disk-index referenced. 
 * 
 * Typically, the total commit limit would be:
 * 
 * disk space + physical memory - 2 (bc we don't use the first slot, and we need 1 in reserve)
 * 
 * But this means we cannot double-store content in pages and on the disk. Normally, we would start to disallow the double-storage of data on physical memory
 * and on disk if we are running low on pagefile space - but I haven't implemented this yet (but might in the future).
 * 
 */
#define DISK_SIZE       (VIRTUAL_ADDRESS_SIZE + (PAGE_SIZE * 2))

#define NUMBER_OF_PHYSICAL_PAGES   ((VIRTUAL_ADDRESS_SIZE / PAGE_SIZE) / 3)


#else

/**
 * LARGE SIMULATION
 */
#define VIRTUAL_ADDRESS_SIZE        GB(1)

#define VIRTUAL_ADDRESS_SIZE_IN_UNSIGNED_CHUNKS        (VIRTUAL_ADDRESS_SIZE / sizeof (ULONG_PTR))

#define CACHE_SIZE      MB(1)

/**
 * See above comment for the reasoning behind this number
 */
#define DISK_SIZE      (VIRTUAL_ADDRESS_SIZE + (PAGE_SIZE * 2))

#define NUMBER_OF_PHYSICAL_PAGES   ((VIRTUAL_ADDRESS_SIZE / PAGE_SIZE) / 3)
#endif
