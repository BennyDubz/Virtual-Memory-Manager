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
 * Handles the page fault for the given virtual address
 * 
 * Returns SUCCESS if the faulting instruction can be tried again, ERROR otherwise
 * 
 */
int pagefault(PULONG_PTR virtual_address);