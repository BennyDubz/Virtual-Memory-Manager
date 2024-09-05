/**
 * @author Ben Williams
 * @date July 8th, 2024
 * 
 * Condensed file for all disk operations and threads
 */


#define DISABLE_PAGEFILE_PRESERVATION 0

#include <windows.h>
#include "../Datastructures/datastructures.h"

typedef struct {
    ULONG_PTR num_pages;
    BOOL write_complete;
    PAGE* pages_being_written[MAX_PAGES_WRITABLE];
    ULONG_PTR disk_indices[MAX_PAGES_WRITABLE];
    ULONG64 pfns[MAX_PAGES_WRITABLE];
} DISK_BATCH;


/**
 * Sorts the virtual addresses representing a page, and then loops through them to determine how many of them are adjacent 
 * to eachother. We write into the sequential_va_count_storage how many sequential VAs there are in a row.
 * 
 * For example, if we have virtual addresses 0x1000, 0x2000, and 0x4000 - the first two are next to eachother.
 * We would write in the values [2, 1] into the storage array. If we had to memcpy (such as for reading from the disk), we would
 * be able to make fewer, but larger, memcpy calls in order to do this
 */
void pre_prepare_page_memcpys(PULONG_PTR* virtual_addresses, ULONG64 num_addresses, ULONG64* sequential_va_count_storage);


/**
 * Writes the entire batch to disk, and stores the disk indices into the page
 * 
 * Returns the number of pages successfully written to disk, as if there are not enough slots 
 * we may not write all of them
 */
ULONG64 write_batch_to_disk(DISK_BATCH* disk_batch);

#if 0
/**
 * Fetches the memory from the disk index and puts it onto the open page
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 */
int read_page_from_disk(PAGE* open_page, ULONG64 disk_idx);
#endif

/**
 * Reads all of the data from the given disk indices on the PTEs into the given pages
 * 
 * Assumes all of the PTEs are in disk format and have their disk read rights acquired by the calling thread
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 */
int read_pages_from_disk(PAGE** open_pages, PTE** ptes_to_read, ULONG64 num_to_read, THREAD_DISK_READ_RESOURCES* thread_disk_resources);


/**
 * Writes a list of num_disk_slots into the result storage pointer.
 * 
 * Returns the number of disk slots written into the storage, as there may not be enough available.
 */
ULONG64 allocate_many_disk_slots(ULONG64* result_storage, ULONG64 num_disk_slots);


/**
 * Writes an open disk idx into the result storage pointer
 * 
 * Returns SUCCESS if we successfully wrote a disk idx, ERROR otherwise (may be empty)
 * 
 */
int allocate_single_disk_slot(ULONG64* result_storage);


/**
 * Modifies the bitmap on the disk to indicate the given disk slot is free
 * 
 * Returns SUCCESS upon no issues, ERROR otherwise
 */
int release_single_disk_slot(ULONG64 disk_idx);


/**
 * Called at the end of a pagefault to determine whether or not a pagefile slot needs to be
 * released. Modifies the page if necessary to remove the reference to the pagefile slot if it is released,
 * and releases the disk slot if appropriate
 * 
 * Assumes the caller holds the given page's pagelock
 */
void handle_end_of_fault_disk_slot(PTE local_pte, PAGE* allocated_page, ULONG64 access_type);


/**
 * Called at the end of a pagefault to determine whether or not pagefile slots need to be
 * released. Modifies the pages only if necessary to remove references to pagefile slots when appropriate
 * and release pagefile slots when appropriate
 * 
 * Assumes the caller holds the given page's pagelocks.
 */
void handle_batch_end_of_fault_disk_slot(PTE** ptes, PTE* original_pte_accessed, PAGE** allocated_pages, ULONG64 access_type, ULONG64 num_ptes);