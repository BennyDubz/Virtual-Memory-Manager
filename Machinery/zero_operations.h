/**
 * @author Ben Williams
 * @date July 19th, 2024
 * 
 * All operations required to zeroing out pages
 */

#define NUM_ZERO_SLOTS 512
#define MAX_ZEROABLE 64
#define ZERO_SLOT_OPEN 0
#define ZERO_SLOT_USED 1
#define ZERO_SLOT_NEEDS_FLUSH 2

#include "../Datastructures/datastructures.h"

/**
 * Used to initialize the virtual addresses, lists, and variables used for zeroing out pages
 * 
 * Returns SUCCESS if we successfully initialized everything, ERROR otherwise
 */
int initialize_page_zeroing();


/**
 * Given the pages that need to be cleaned, zeroes out all of the contents on their physical pages
 */
void zero_out_pages(PAGE** pages, ULONG64 num_pages);

