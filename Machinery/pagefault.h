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
#define NUM_FAULT_RETURN_VALS 8

// The fault succeeds
#define SUCCESSFUL_FAULT 0
// Represents invalid parameters (such as an invalid PTE) being given to the pagefault handler
#define REJECTION_FAIL 1 
// There were no available pages, so we had to wait for more to become available and retry
#define NO_AVAILABLE_PAGES_FAIL 2
// We were unable to rescue the page from the modified or standby list
#define RESCUE_FAIL 3
// There was a race condition that caused the fault to need to be retried
#define DISK_RACE_CONTIION_FAIL 4
// There was a race condition that caused the unaccessed-PTE fault to fail - 
// likely more than one thread faulted on it and only one resovled it
#define UNACCESSED_RACE_CONDITION_FAIL 6
// There was a race condition that caused the fault to fail on a valid PTE -
// it was likely trimmed or another thread faulted on the same PTE
#define VALID_PTE_RACE_CONTIION_FAIL 7


/**
 * Handles the page fault for the given virtual address, with the given access type (either read or write)
 * 
 * Takes the thread index of the calling thread as parameter so that we can access the thread local storage
 * 
 * Returns SUCCESS if the faulting instruction can be tried again, ERROR otherwise
 * 
 */
int pagefault(PULONG_PTR virtual_address, ULONG64 access_type, ULONG64 thread_idx);