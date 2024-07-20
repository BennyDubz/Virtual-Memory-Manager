/**
 * @author Ben Williams
 * @date July 19th, 2024
 * 
 * All operations required to zeroing out pages
 */

#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <windows.h>
#include "../Datastructures/datastructures.h"
#include "./zero_operations.h"
#include "./conversions.h"
#include "../globals.h"
#include "../macros.h"

#define ZERO_REFRESH_BOUNDARY  NUM_ZERO_SLOTS / 2
long zero_slot_statuses[NUM_ZERO_SLOTS];
ULONG64 total_available_zero_slots;
PULONG_PTR zero_base_addr;


/**
 * Used to initialize the virtual addresses, lists, and variables used for zeroing out pages
 */
int initialize_page_zeroing(MEM_EXTENDED_PARAMETER* vmem_parameters) {
    if (vmem_parameters == NULL) {
        zero_base_addr = VirtualAlloc(NULL, PAGE_SIZE * NUM_ZERO_SLOTS, 
                        MEM_PHYSICAL | MEM_RESERVE, PAGE_READWRITE);
    } else {
        zero_base_addr = VirtualAlloc2(NULL, NULL, PAGE_SIZE * NUM_ZERO_SLOTS,
                        MEM_PHYSICAL | MEM_RESERVE, PAGE_READWRITE, vmem_parameters, 1);
    }

    if (zero_base_addr == NULL) {
        fprintf(stderr, "Unable to allocate memory for zero_base_addr in initialize_page_zeroing\n");
        return ERROR;
    }

    // All slots start as open
    for (ULONG64 i = 0; i < NUM_ZERO_SLOTS; i++) {
        zero_slot_statuses[i] = ZERO_SLOT_OPEN;
    }

    total_available_zero_slots = NUM_ZERO_SLOTS;

    return SUCCESS;
}


/**
 * Finds num_slots open slots and writes them into zero_slot_indices_storage while setting
 * them to be ZERO_SLOT_USED
 * 
 * Returns the number of slots successfully allocated
 */
static ULONG64 acquire_batch_zero_slots(ULONG64* zero_slot_indices_storage, ULONG64 num_slots) {
    long read_old_val;
    volatile long* zero_slot;
    ULONG64 num_allocated = 0;

    for (ULONG64 slot_idx = 0; slot_idx < NUM_ZERO_SLOTS; slot_idx++) {
        zero_slot = &zero_slot_statuses[slot_idx];
        
        if (*zero_slot == ZERO_SLOT_OPEN) {
            read_old_val = InterlockedCompareExchange(zero_slot, ZERO_SLOT_USED, ZERO_SLOT_OPEN);
            
            // We successfully claimed the zero slot
            if (read_old_val == ZERO_SLOT_OPEN) {
                zero_slot_indices_storage[num_allocated] = slot_idx;
                InterlockedDecrement64(&total_available_zero_slots);
                num_allocated++;

                if (num_allocated == num_slots) {
                    return num_allocated;
                }
            }
        }
    }

    return num_allocated;
}


/**
 * Finds an open zero-slot and sets it to used. Writes the index of the zero slot into the storage
 * 
 * Returns SUCCESS if a slot is allocated and written into the storage, ERROR otherwise
 */
static int acquire_zero_slot(ULONG64* zero_slot_idx_storage) {
    long read_old_val;
    volatile long* zero_slot;

    // BW: This can be made much more efficient with a bitmap implementation
    for (ULONG64 slot_idx = 0; slot_idx < NUM_ZERO_SLOTS; slot_idx++) {
        zero_slot = &zero_slot_statuses[slot_idx];

        if (*zero_slot == ZERO_SLOT_OPEN) {
            read_old_val = InterlockedCompareExchange(zero_slot, ZERO_SLOT_USED, ZERO_SLOT_OPEN);
            
            // We successfully claimed the zero slot
            if (read_old_val == ZERO_SLOT_OPEN) {
                assert(*zero_slot == ZERO_SLOT_USED);
                *zero_slot_idx_storage = slot_idx;
                InterlockedDecrement64(&total_available_zero_slots);
                return SUCCESS;
            }
        }
    }

    // There were no slots available, we will need to refresh the list and try again
    return ERROR;
}


/**
 * Marks the zero slot to be refreshed, releasing it for the current thread
 */
static void release_batch_zero_slots(ULONG64* slot_indices, ULONG64 num_slots) {
    
    for (ULONG64 release_idx = 0; release_idx < num_slots; release_idx++) {
        ULONG64 slot_index = slot_indices[release_idx];
        assert(zero_slot_statuses[slot_index] == ZERO_SLOT_USED);

        assert(InterlockedIncrement(&zero_slot_statuses[slot_index]) == ZERO_SLOT_NEEDS_FLUSH);
    }
}


/**
 * Marks the zero slot to be refreshed, releasing it for the current thread
 */
static void release_zero_slot(ULONG64 slot_idx) {
    if (zero_slot_statuses[slot_idx] != ZERO_SLOT_USED) {
        DebugBreak();
    }

    assert(InterlockedIncrement(&zero_slot_statuses[slot_idx]) == ZERO_SLOT_NEEDS_FLUSH);
}


/**
 * Takes all of the zero slots in the list that need to be refreshed and handle them in a single
 * batch - reducing the amount of load we need on MapUserPhysicalPagesScatter
 */
PULONG_PTR refresh_zero_addresses[NUM_ZERO_SLOTS];
volatile long* refresh_zero_status_slots[NUM_ZERO_SLOTS];
long zero_refresh_ongoing = FALSE; // We use the long for interlocked operation parameters
static void refresh_zero_slots() {
    // Synchronize whether we or someone else is refreshing the zero slots
    long old_val = InterlockedOr(&zero_refresh_ongoing, TRUE);

    if (old_val == TRUE) {
        return;
    }

    ULONG64 num_slots_refreshed = 0;
    volatile long* zero_slot;
    PULONG_PTR zero_slot_addr;

    // Find all of the slots to clear
    for (ULONG64 slot_idx = 0; slot_idx < NUM_ZERO_SLOTS; slot_idx++) {
        zero_slot = &zero_slot_statuses[slot_idx];

        if (*zero_slot == ZERO_SLOT_NEEDS_FLUSH) {
            refresh_zero_status_slots[num_slots_refreshed] = zero_slot;

            zero_slot_addr = zero_base_addr + (slot_idx * PAGE_SIZE / sizeof(PULONG_PTR));
            refresh_zero_addresses[num_slots_refreshed] = zero_slot_addr;
            num_slots_refreshed++;
        }
    }

    if (MapUserPhysicalPagesScatter(refresh_zero_addresses, num_slots_refreshed, NULL) == FALSE) {
        fprintf(stderr, "Error unmapping zeroing VAs in refreshzero_slots\n");
        DebugBreak();
        return;
    }

    InterlockedAdd64(&total_available_zero_slots, num_slots_refreshed);

    // Finally clear all of the slots
    for (ULONG64 zero_status_refresh = 0; zero_status_refresh < num_slots_refreshed; zero_status_refresh++) {
        InterlockedAnd(refresh_zero_status_slots[zero_status_refresh], ZERO_SLOT_OPEN);
    }

    InterlockedAnd(&zero_refresh_ongoing, FALSE);
}


/**
 * Memsets all of the virtual addresses to zero - each for a size of PAGE_SIZE
 */
static void zero_out_addresses(PULONG_PTR* virtual_addresses, ULONG64 num_addresses) {
    for (ULONG64 i = 0; i < num_addresses; i++) {
        memset(virtual_addresses[i], 0, PAGE_SIZE);
    }
}


/**
 * Given the pages that need to be cleaned, zeroes out all of the contents on their physical pages
 */
void zero_out_pages(PAGE** pages, ULONG64 num_pages) {
    if (pages == NULL || num_pages == 0 || num_pages > MAX_ZEROABLE || pages[0] == NULL ) {
        fprintf(stderr, "NULL or invalid arguments given to zero_out_pages\n");
        DebugBreak();
    }


    PULONG_PTR zero_slot_addresses[MAX_ZEROABLE];
    ULONG64 zero_slot_indices[MAX_ZEROABLE];
    ULONG64 pfns[MAX_ZEROABLE];
    ULONG64 slots_allocated_this_loop = 0;
    ULONG64 total_slots_allocated = 0;

    while ((total_slots_allocated += 
            acquire_batch_zero_slots(&zero_slot_indices[total_slots_allocated], num_pages - total_slots_allocated)) != num_pages) {

        if (zero_refresh_ongoing == FALSE) {
            // If we have a race condition here, only one thread will refresh and the other will return almost immediately
            refresh_zero_slots();
        }

    }

    // Collect all of the addresses and the PFNs
    for (ULONG64 i = 0; i < total_slots_allocated; i++) {
        zero_slot_addresses[i] = zero_base_addr + (zero_slot_indices[i] * PAGE_SIZE / sizeof(PULONG_PTR));
        pfns[i] = page_to_pfn(pages[i]);
    }


    if (MapUserPhysicalPagesScatter(zero_slot_addresses, num_pages, pfns) == FALSE) {
        DebugBreak();
    }  

    zero_out_addresses(zero_slot_addresses, num_pages);

    release_batch_zero_slots(zero_slot_indices, total_slots_allocated);

    if (total_available_zero_slots < ZERO_REFRESH_BOUNDARY && zero_refresh_ongoing == FALSE) {
        refresh_zero_slots();
    }
    
}

