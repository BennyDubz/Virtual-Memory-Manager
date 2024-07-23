/**
 * @author Ben Williams
 * @date June 25th, 2024
 * 
 * All functionality for trimming
 */

#include "../Datastructures/datastructures.h"

#define FAULTER_TRIM_BEHIND_MIN 4
#define FAULTER_TRIM_BEHIND_MAX 64

/**
 * Thread dedicated to aging all of the valid PTEs in the pagetable
 */
LPTHREAD_START_ROUTINE thread_aging();


/**
 * Thread dedicated to trimming PTEs from the pagetable and putting them on the modified list
 */
LPTHREAD_START_ROUTINE thread_trimming();


/**
 * Trims the PTEs behind the faulting thread if there are at least FAULTER_TRIM_BEHIND_BOUNDARY of them active,
 * but will only enter one PTE locksection to ensure low wait-times for the faulting thread
 */
void faulter_trim_behind();


/**
 * Thread dedicated to writing pages from the modified list to disk, putting finally adding the pages to standby
 */
LPTHREAD_START_ROUTINE thread_modified_to_standby();


/**
 * Connects the given PTE to the open page's physical frame and alerts the CPU
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 */
int connect_pte_to_page(PTE* pte, PAGE* open_page);


/**
 * Disconnects the PTE from the CPU, but **does not** change the PTE structure
 * as this may be used for both disk and transition format PTEs
 */
int disconnect_pte_from_cpu(PTE* pte);
