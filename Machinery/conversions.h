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
 * Returns the index of the PTE in the pagetable based off its address
 */
ULONG64 pte_to_pagetable_idx(PTE* pte);


/**
 * Returns the base virtual address associated with the given PTE, or NULL otherwise
 * 
 */
PULONG_PTR pte_to_va(PTE* pte);


/**
 * Returns a pointer to the pagetable's lock governing the given PTE
 */
PTE_LOCKSECTION* pte_to_locksection(PTE* pte);


/**
 * Given the frame number, returns a pointer to the relevant PAGE struct associated with the frame number
 * using the page storage base
 * 
 * Returns NULL given any error
 */
PAGE* pfn_to_page(ULONG64 frame_number);


/**
 * Given a pointer to the page, returns the pfn associated with it
 */
ULONG64 page_to_pfn(PAGE* page);


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


/**
 * From the index of the disk slot, return a pointer to its governing lock
 */
CRITICAL_SECTION* disk_idx_to_lock(ULONG64 disk_idx);