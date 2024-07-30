/**
 * @author Ben Williams
 * @date June 25th, 2024
 * 
 * All functions for handling the page fault at the highest level
 */

#include "../macros.h"
#include "../hardware.h"
#include "../globals.h"


/**
 * This allows us to store the results of various faults
 */

// To make it easier to store results
#define NUM_FAULT_RETURN_VALS 6

// The fault succeeds
#define SUCCESSFUL_FAULT 0
// Represents accessing an invalid PTE that should never be mapped
#define REJECTION_FAIL 1 
// There were no available pages, so we had to wait for more to become available and retry
#define NO_AVAILABLE_PAGES_FAIL 2
// We were unable to rescue the page from the modified or standby list
#define RESCUE_FAIL 3
// We were unable to read from the disk
#define DISK_FAIL 4
// The PTE changed from under the faulting process, and it had to retry
#define RACE_CONDITION_FAIL 5


/**
 * Handles the page fault for the given virtual address
 * 
 * Returns SUCCESS if the faulting instruction can be tried again, ERROR otherwise
 * 
 */
int pagefault(PULONG_PTR virtual_address, ULONG64 access_type);