/**
 * @author Ben Williams
 * @date June 25th, 2024
 * 
 * All functionality for trimming
 */

#include "../Datastructures/datastructures.h"

// The maximum number of PTEs that we can trim per PTE locksection
#define TRIM_PER_SECTION 96

// The minimum and maximum amount of PTEs that can be trimmed by the faulting threads
#define FAULTER_TRIM_BEHIND_MIN 16
#define FAULTER_TRIM_BEHIND_MAX 64

// We need to OR the final bit of a pfn in order to have read-only permissions
#define PAGE_MAPUSERPHYSCAL_READONLY_MASK 0x8000000000000000

#define MOD_WRITER_MAX_NUM_SECTIONS 16
#define MOD_WRITER_SECTION_SIZE     (MAX_PAGES_WRITABLE / MOD_WRITER_MAX_NUM_SECTIONS)

// How many memcpys will we perform in each section before we re-acquire the pagelocks, chain the pages together, and add them to standby
#define MOD_WRITER_MINIBATCH_SIZE 32

/**
 * If the standby list is smaller than the (total physical memory / proportion) then it will hog the modified list lock
 */
#define MOD_WRITER_PREFERRED_STANDBY_MINIMUM_PROPORTION  3


/**
 * Thread dedicated to aging all of the valid PTEs in the pagetable
 */
LPTHREAD_START_ROUTINE thread_aging(void* parameters);


/**
 * Updates all of the threads trim signal statuses to the given status
 */
void trim_update_thread_storages(UCHAR trim_signal_status);


/**
 * Thread dedicated to trimming PTEs from the pagetable and putting them on the modified list
 */
LPTHREAD_START_ROUTINE thread_trimming(void* parameters);


/**
 * Trims the PTEs behind the faulting thread if there are at least FAULTER_TRIM_BEHIND_BOUNDARY of them active,
 * but will only enter one PTE locksection to ensure low wait-times for the faulting thread
 */
void faulter_trim_behind();


/**
 * Thread dedicated to writing pages from the modified list to disk, adding the pages to standby
 */
LPTHREAD_START_ROUTINE thread_modified_writer(void* parameters);


/**
 * Connects the given PTE to the open page's physical frame and modifies the PTE
 * 
 * Sets the permission bits of the PTE in accordance with its status as well as the type of access
 * (read / write) that occurred. We try to get away with PAGE_READONLY permissions when it would allow us
 * to potentially conserve pagefile space - and therefore unncessary modified writes.
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 */
int connect_pte_to_page(PTE* pte, PAGE* open_page, ULONG64 access_type);


/**
 * Connects the list of given PTEs to their corresponding pages, and modifies all the PTEs to be
 * in valid format. Assumes all PTEs are in the same PTE locksection
 * 
 * Sets the PTEs permission bits depending on whether or not they have preservable pagefile space and the type of access of the fault
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise 
 */
int connect_batch_ptes_to_pages(PTE** ptes_to_connect, PTE* original_accessed_pte, PAGE** pages, ULONG64 access_type, ULONG64 num_ptes);


/**
 * Disconnects the PTE from the CPU, but **does not** change the PTE structure
 * as this may be used for both disk and transition format PTEs
 */
int disconnect_pte_from_cpu(PTE* pte);
