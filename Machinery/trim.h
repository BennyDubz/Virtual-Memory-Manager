/**
 * @author Ben Williams
 * @date June 25th, 2024
 * 
 * All functionality for trimming
 */

#include "../Datastructures/datastructures.h"


/**
 * Returns the frame number of the lowest page in the pagetable that is valid, makes the PTE invalid 
 * and communicates with the CPU
 * 
 * Returns either the frame number or ERROR otherwise
 */
ULONG64 steal_lowest_frame();