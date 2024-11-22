/**
 * @author Ben Williams
 * @date June 25th, 2024
 * 
 * All functionality for trimming
 */

#include "../Datastructures/datastructures.h"

// The maximum number of PTEs that we can trim per PTE locksection
#define TRIM_PER_SECTION 96

// Once the number of total available pages is less than (total physical memory / proportion), we will trim PTES
// that have the access bit set
#define TRIM_ACCESSED_PTES_PROPORTION 5

// The minimum and maximum amount of PTEs that can be trimmed by the faulting threads
#define FAULTER_TRIM_BEHIND_NUM 64

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


typedef struct {
    ULONG64 total_num_ptes;
    PTE** pte_storage;
    PULONG_PTR* trim_addresses;

    ULONG64 num_to_modified;
    PAGE** pages_to_modified;

    ULONG64 num_to_standby;
    PAGE** pages_to_standby;

    BOOL trim_accessed_ptes;
    ULONG64 ptes_per_locksection;
} TRIM_INFORMATION;

/**
 * Thread dedicated to aging all of the valid PTEs in the pagetable
 */
LPTHREAD_START_ROUTINE thread_aging(void* parameters);


/**
 * Updates all of the threads trim signal statuses to the given status
 */
void trim_update_thread_storages(UCHAR trim_signal_status);


/**
 * Thread dedicated to everything trimming related.
 * 
 * Takes valid PTEs off of buffers from threads trimming behind themselves, as well as PTEs straight off
 * the pagetable to unmap and put their pages on the modified/standby list depending on whether or not
 * they have been modified.
 * 
 * Has a limited aging implementation, where we use the access bit to sometimes avoid trimming PTEs and unset this bit
 * when we come across it.
 */
LPTHREAD_START_ROUTINE thread_trimming(void* parameters);


/**
 * Trims the PTEs behind the faulting thread if there are at least FAULTER_TRIM_BEHIND_MIN of them active,
 * but will only enter one PTE locksection to ensure low wait-times for the faulting thread
 * 
 * Does NOT distinguish between accessed / unaccessed valid PTEs. We are trimming behind ourselves because
 * we are speculating that these PTEs will no longer be accessed anymore (as they were recently sequentially accessed).
 * 
 * We consider only a maximum of FAULTER_TRIM_BEHIND_NUM pages. Furthermore, we do not actually trim the PTEs to completion-
 * instead, we put them on a buffer for the trimming thread to handle. This saves the faulting thread a considerable amount of time.
 */
void faulter_trim_behind(PTE* accessed_pte, ULONG64 thread_idx);


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
