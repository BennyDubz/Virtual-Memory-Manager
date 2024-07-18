/**
 * @author Ben Williams
 * @date July 3rd, 2024
 * 
 * Debugging sanity checking functions
 */

#include <windows.h>
#include "../globals.h"
#include "../Datastructures/datastructures.h"

#define ISOLATED 0
#define IN_FREE 1
#define IN_MODIIFED 2
#define IN_STANDBY 3
#define OTHER_ERROR 4

/**
 * Returns TRUE if the page is not in the free, modfied, or standby lists, FALSE otherwise
 */
int page_is_isolated(PAGE* page);



/**
 * Returns TRUE if the pfn is associated with a single PTE, FALSE if it is associated with zero
 * or more than one
 */
BOOL pfn_is_single_allocated(ULONG64 pfn);


/**
 * Ensures that the number of valid PTEs matches the counts in the given PTE's
 * lock section
 * 
 * To be called whenever we make a PTE valid or invalid
 */
BOOL pte_valid_count_check(PTE* accessed_pte);


/**
 * Logs the page's information into the global circular log structure
 */
void log_page_status(PAGE* page);


/**
 * Nicely collects all the VA's info - its PTE, page, etc, when we fail on a VA
 * 
 * Debugbreaks the program with all values as local variables for the debugger
 */
void debug_break_all_va_info(PULONG_PTR arbitrary_va);