/**
 * @author Ben Williams
 * @date July 3rd, 2024
 * 
 * Debugging sanity checking functions
 */

#include <windows.h>
#include <stdio.h>
#include "../globals.h"
#include "../Datastructures/datastructures.h"
#include "./conversions.h"
#include "./debug_checks.h"

void custom_spin_assert(BOOL expression);


/**
 * Returns TRUE if the pfn is associated with a single PTE, FALSE if it is associated with zero
 * or more than one.
 */
BOOL pfn_is_single_allocated(ULONG64 pfn) {
    BOOL found = FALSE;
    ULONG64 section_start;
    ULONG64 ptes_per_lock = pagetable->num_virtual_pages / pagetable->num_locks;
    // May be useful when in the debugger
    PTE* first_pte_found;
    PTE* curr_pte;

    PAGE* curr_page = pfn_to_page(pfn);

    if (curr_page->status == FREE_STATUS) {
        DebugBreak();
    }

    // Go through each lock section and check their PTEs
    for (ULONG64 lock_section = 0; lock_section < pagetable->num_locks; lock_section++) {            
        section_start = lock_section * ptes_per_lock;

        for (ULONG64 pte_idx = section_start; pte_idx < section_start + ptes_per_lock; pte_idx++) {
            curr_pte = &pagetable->pte_list[pte_idx];

            // These are not PFNs, but rather disk addresses
            if (is_disk_format(*curr_pte)) {
                continue;
            }

            // Frame number is in the same place for memory and transition format
            if (curr_pte->memory_format.frame_number == pfn) {
                // If the pfn had already been found - we have a fatal error
                if (found) {
                    DebugBreak();                
                    return FALSE;
                }
                first_pte_found = curr_pte;
                found = TRUE;
            }

            
        }
    }

    return found;
}


/**
 * Ensures that the number of valid PTEs matches the counts in the given PTE's
 * lock section
 * 
 * To be called whenever we make a PTE valid or invalid
 */
BOOL pte_valid_count_check(PTE* accessed_pte) {
    ULONG64 base_address_pte_list = (ULONG64) pagetable->pte_list;
    ULONG64 pte_address = (ULONG64) accessed_pte;

    ULONG64 pte_index = (pte_address - base_address_pte_list) / sizeof(PTE);

    ULONG64 ptes_per_lock = pagetable->num_virtual_pages / pagetable->num_locks;

    ULONG64 lock_idx = pte_index / ptes_per_lock;

    ULONG64 start_idx = lock_idx * ptes_per_lock;

    ULONG64 stored_count = pagetable->pte_locksections[lock_idx].valid_pte_count;

    PTE curr_pte;
    ULONG64 valid_count = 0;

    for (ULONG64 pte_idx = start_idx; pte_idx < start_idx + ptes_per_lock; pte_idx++) {
        curr_pte = read_pte_contents(&pagetable->pte_list[pte_idx]);

        if (pte_to_locksection(&pagetable->pte_list[pte_idx]) != &pagetable->pte_locksections[lock_idx]) {
            DebugBreak();
        }

        if (is_memory_format(curr_pte)) valid_count++;
    }

    if (valid_count != stored_count) {
        DebugBreak();
        return FALSE;
    }


    return TRUE;
}


/**
 * Logs the page's information into the global circular log structure
 */
void log_page_status(PAGE* page) {

    #if DEBUG_PAGELOCK
    ULONG64 curr_log_idx = InterlockedIncrement64(&log_idx) % LOG_SIZE;

    PAGE_LOGSTRUCT log_struct;
    log_struct.actual_page = page;
    log_struct.page_copy = *page;
    CaptureStackBackTrace(0, 8, log_struct.stack_trace, NULL);
    log_struct.pfn = page_to_pfn(page);
    log_struct.linked_pte = page->pte;
    if (page->pte != NULL) {
        log_struct.linked_pte_copy = *page->pte;
    }
    log_struct.thread_id = GetCurrentThreadId();

    page_log[curr_log_idx] = log_struct;
    #endif
}


/**
 * Nicely collects all the VA's info - its PTE, page, etc, when we fail on a VA
 * 
 * Debugbreaks the program with all values as local variables for the debugger
 * 
 */
void debug_break_all_va_info(PULONG_PTR arbitrary_va) {
    PTE* relevant_pte = va_to_pte(arbitrary_va);
    PULONG_PTR pte_va = pte_to_va(relevant_pte);

    PAGE* relevant_page = pfn_to_page(relevant_pte->memory_format.frame_number);

    PULONG_PTR last_used_disk_slot;

    ULONG64 disk_idx;

    PULONG_PTR other_va = (PULONG_PTR) *arbitrary_va;

    PTE* other_pte = va_to_pte(other_va);

    if (relevant_page->pagefile_idx != 0) {
        last_used_disk_slot = disk_idx_to_addr(relevant_page->pagefile_idx);
        disk_idx = relevant_page->pagefile_idx;
    }

    acquire_pagelock(relevant_page, 0xFFFFFF);

    custom_spin_assert(FALSE);
}


/**
 * A spinning debugbreak specific to the disk index checking
 */
static void singly_allocated_disk_idx_debugbreak(ULONG64 disk_idx, PTE* found_pte, PTE* curr_pte, PAGE* found_page, PAGE* curr_page) {
    PULONG_PTR disk_slot_addr = disk_idx_to_addr(disk_idx);

    PULONG_PTR found_pte_va = pte_to_va(found_pte);

    PULONG_PTR curr_pte_va = pte_to_va(curr_pte);

    if (curr_page != NULL) {
        acquire_pagelock(curr_page, 0xAAAAAA);
    } 

    if (found_page != NULL) {
        acquire_pagelock(found_page, 0xBBBBBB);
    }

    custom_spin_assert(FALSE);
}


/**
 * Loops through the entire pagetable and ensures there is only one reference to a disk index
 */
void singly_allocated_disk_idx_check(ULONG64 disk_idx) {

    ULONG64 num_ptes = pagetable->num_virtual_pages;
    PTE* curr_pte_ptr;
    PTE curr_pte_contents;
    PAGE* curr_page;
    
    PTE* found_pte = NULL;
    PAGE* found_page = NULL;

    for (ULONG64 i = 0; i < num_ptes; i++) {
        curr_pte_ptr = &pagetable->pte_list[i];
        curr_pte_contents = read_pte_contents(curr_pte_ptr);

        if (is_used_pte(curr_pte_contents) == FALSE) continue;

        if (is_memory_format(curr_pte_contents)) {
            curr_page = pfn_to_page(curr_pte_contents.memory_format.frame_number);

            if (curr_page->pagefile_idx == disk_idx) {
                if (found_pte != NULL)  {
                    singly_allocated_disk_idx_debugbreak(disk_idx, found_pte, curr_pte_ptr, found_page, curr_page);
                }
                found_page = curr_page;
                found_pte = curr_pte_ptr;
            }

            continue;
        }

        if (is_transition_format(curr_pte_contents)) {
            curr_page = pfn_to_page(curr_pte_contents.transition_format.frame_number);

            if (curr_page->pagefile_idx == disk_idx) {
                if (found_pte != NULL)  {
                    singly_allocated_disk_idx_debugbreak(disk_idx, found_pte, curr_pte_ptr, found_page, curr_page);
                }
                found_page = curr_page;
                found_pte = curr_pte_ptr;
            }
            continue;
        }


        if (is_disk_format(curr_pte_contents)) {
            if (curr_pte_contents.disk_format.pagefile_idx == disk_idx) {
                if (found_pte != NULL) {
                    singly_allocated_disk_idx_debugbreak(disk_idx, found_pte, curr_pte_ptr, found_page, NULL);
                }
                found_pte = curr_pte_ptr;
            }
            continue;
        }
    }

    // if (found_pte == NULL) DebugBreak();
}


/**
 * An assert that will print out a message and spin forever, allowing you to debugbreak in when otherwise the
 * debugger is too slow
 */
void custom_spin_assert(BOOL expression) {
    if (expression == TRUE) return;

    fprintf(stderr, "Custom spinning assert triggered! Thread %lx spinning\n", GetCurrentThreadId());
    for (ULONG64 i = 0; i < 0xFFFFFFFFFFFFFFFF; i++) {
        for (ULONG64 j = 0; j < 0xFFFFFFFFFFFFFFFF; j++) {

        }
    }
}