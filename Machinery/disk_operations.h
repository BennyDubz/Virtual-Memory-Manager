/**
 * @author Ben Williams
 * @date July 8th, 2024
 * 
 * Condensed file for all disk operations and threads
 */

#include <windows.h>
#include "../Datastructures/datastructures.h"


/**
 * A thread dedicated to writing the given page to the disk. Writes the resulting
 * disk storage index into the given pointer disk_idx_storage.
 * 
 * Returns SUCCESS if we write the page to disk, ERROR otherwise
 */
int write_to_disk(PAGE* transition_page, ULONG64* disk_idx_storage);


/**
 * Fetches the memory from the disk index and puts it onto the open page
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 */
int read_from_disk(PAGE* open_page, ULONG64 disk_idx);


/**
 * Writes an open disk idx into the result storage pointer
 * 
 * Returns SUCCESS if we successfully wrote a disk idx, ERROR otherwise (may be empty)
 * 
 */
int allocate_disk_slot(ULONG64* result_storage);


/**
 * Modifies the bitmap on the disk to indicate the given disk slot is free
 * 
 * Returns SUCCESS upon no issues, ERROR otherwise
 */
int release_disk_slot(ULONG64 disk_idx);