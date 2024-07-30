/**
 * @author Ben Williams
 * @date July 8th, 2024
 * 
 * Condensed file for all disk operations and threads
 */

#include <windows.h>
#include "../Datastructures/datastructures.h"

typedef struct {
    ULONG_PTR num_pages;
    BOOL write_complete;
    PAGE* pages_being_written[MAX_PAGES_WRITABLE];
    ULONG_PTR disk_indices[MAX_PAGES_WRITABLE];
} DISK_BATCH;


/**
 * Writes the entire batch to disk, and stores the disk indices into the page
 * 
 * Returns the number of pages successfully written to disk, as if there are not enough slots 
 * we may not write all of them
 */
ULONG64 write_batch_to_disk(DISK_BATCH* disk_batch);


/**
 * Fetches the memory from the disk index and puts it onto the open page
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 */
int read_page_from_disk(PAGE* open_page, ULONG64 disk_idx);


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