/**
 * @author Ben Williams
 * @date June 26th, 2024
 * 
 * Conversion functions that require reading global datastructures and values
 */


#include "../Datastructures/datastructures.h"



/**
 * Given a virtual address, return the relevant PTE from the pagetable
 * 
 * Returns NULL upon error
 */
PTE* va_to_pte(PULONG_PTR virtual_address);


/**
 * Returns the base virtual address associated with the given PTE, or NULL otherwise
 * 
 */
PULONG_PTR pte_to_va(PTE* pte);


/**
 * Returns a pointer to the pagetable's lock governing the given PTE
 */
CRITICAL_SECTION* pte_to_lock(PTE* pte);


/**
 * Given the frame number, returns a pointer to the relevant PAGE struct associated with the frame number
 * using the page storage base
 * 
 * Returns NULL given any error
 */
PAGE* page_from_pfn(ULONG64 frame_number);


/**
 * From a disk index, get the actual virtual address that the slot represents
 * 
 * Returns a pointer to the relevant slot, or NULL upon error
 */
PULONG_PTR disk_idx_to_addr(ULONG64 disk_idx);


/**
 * From the address of a disk slot, get its corresponding disk index
 * 
 * Returns the disk index upon success, or ERROR otherwise
 */
ULONG64 disk_addr_to_idx(PULONG_PTR disk_slot_addr);