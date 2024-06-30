
/**
 * @author Ben Williams
 * @date June 26th, 2024
 * 
 * Conversion functions that require reading global datastructures and values
 */

#include <stdio.h>
#include "./conversions.h"
#include "../globals.h"
#include "../macros.h"
#include "../hardware.h"


/**
 * Given a virtual address, return the relevant PTE from the pagetable
 * 
 * Returns NULL upon error
 */
PTE* va_to_pte(PULONG_PTR virtual_address) {
    if (virtual_address == NULL) {
        fprintf(stderr, "NULL pagetable or virtual address given to va_to_pta");
        return NULL;
    }

    ULONG64 pte_index = DOWN_TO_PAGE_NUM((ULONG64) virtual_address - pagetable->vmem_base);

    if (pte_index > pagetable->num_virtual_pages) {
        fprintf(stderr, "Illegal virtual address given to va_to_pte %llX\n", (ULONG64) virtual_address);
        return NULL;
    }

    return &pagetable->pte_list[pte_index];
}


/**
 * Returns the base virtual address associated with the given PTE, or NULL otherwise
 * 
 */
PULONG_PTR pte_to_va(PTE* pte) {
    if (pte == NULL) {
        fprintf(stderr, "NULL PTE given to pte_to_va");
        return NULL;
    }

    //BW: Implement additional safety checks
    ULONG64 base_address_pte_list = (ULONG64) pagetable->pte_list;
    ULONG64 pte_address = (ULONG64) pte;

    ULONG64 pte_index = (pte_address - base_address_pte_list) / sizeof(PTE);

    PULONG_PTR virtual_address = (PULONG_PTR) (pagetable->vmem_base + (pte_index * PAGE_SIZE));

    return virtual_address;
}


/**
 * Returns a pointer to the pagetable's lock governing the given PTE
 */
CRITICAL_SECTION* pte_to_lock(PTE* pte) {
    ULONG64 base_address_pte_list = (ULONG64) pagetable->pte_list;
    ULONG64 pte_address = (ULONG64) pte;

    ULONG64 pte_index = (pte_address - base_address_pte_list) / sizeof(PTE);

    LONG64 lock_index = pte_index / (pagetable->num_virtual_pages / pagetable->num_locks);

    return &pagetable->pte_locks[lock_index];
}


/**
 * ### Initialize pages must be called before this function! ###
 * 
 * Given the frame number, returns a pointer to the relevant PAGE struct associated with the frame number
 * using the page storage base
 * 
 * Returns NULL given any error
 */
PAGE* pfn_to_page(ULONG64 frame_number) {
    return page_storage_base + frame_number;
}


/**
 * Given a pointer to the page, returns the pfn associated with it
 */
ULONG64 page_to_pfn(PAGE* page) {
    // page* = psb + (framenumber * sizeof(page))
    //page* - psb = (framenumber * sizeof(page)
    // return (ULONG64) (page - page_storage_base) / sizeof(PAGE);
    return (ULONG64) (page - page_storage_base);

}


/**
 * From a disk index, get the actual virtual address that the slot represents
 * 
 * Returns a pointer to the relevant slot, or NULL upon error
 */
PULONG_PTR disk_idx_to_addr(ULONG64 disk_idx) {
    if (disk_idx > DISK_STORAGE_SLOTS) {
        fprintf(stderr, "NULL diskbase or invalid disk_idx given to disk_idx_to_addr\n");
        return NULL;
    }

    return disk->base_address + ((disk_idx / sizeof(PULONG_PTR)) * PAGE_SIZE);
}


/**
 * From the address of a disk slot, get its corresponding disk index
 * 
 * Returns the disk index upon success, or ERROR otherwise
 */
ULONG64 disk_addr_to_idx(PULONG_PTR disk_slot_addr) {

    if (disk_slot_addr == NULL || disk_slot_addr > (disk->base_address + (DISK_SIZE / sizeof(ULONG_PTR)))) {
        
        fprintf(stderr, "Invalid disk base or disk slot addr given to disk_addr_to_idx\n");
        return ERROR;
    }

    ULONG64 difference = (ULONG64) (disk_slot_addr - disk->base_address);

    return difference / PAGE_SIZE;
}


/**
 * From the index of the disk slot, return a pointer to its governing lock
 * 
 * Returns NULL upon error
 */
CRITICAL_SECTION* disk_idx_to_lock(ULONG64 disk_idx) {
    if (disk_idx > DISK_STORAGE_SLOTS) {
        fprintf(stderr, "Disk idx above number of available slots in disk_idx_to_lock\n");
        return NULL;
    }

    ULONG64 lock_index = disk_idx / (DISK_STORAGE_SLOTS / disk->num_locks);

    return &disk->disk_slot_locks[lock_index];
}