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


/**
 * Thread dedicated to aging all of the valid PTEs in the pagetable
 */
void thread_aging();


/**
 * Writes the given PTE to the disk, and stores the resulting disk_idx in disk_idx_storage
 * 
 * Returns the disk index if there are no issues, ERROR otherwise
 */
int write_to_disk(PTE* pte, ULONG64* disk_idx_storage);


/**
 * Fetches the memory for the given PTE on the disk, 
 * assuming the PTE's virtual address has already been mapped to a valid physical frame
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 */
int get_from_disk(PTE* pte);


/**
 * Writes an open disk idx into the result storage pointer
 * 
 * Returns SUCCESS if we successfully wrote a disk idx, ERROR otherwise (may be empty)
 * 
 */
int get_free_disk_idx(ULONG64* result_storage);


/**
 * Modifies the bitmap on the disk to indicate the given disk slot is free
 * 
 * Returns SUCCESS upon no issues, ERROR otherwise
 */
int return_disk_slot(ULONG64 disk_idx);