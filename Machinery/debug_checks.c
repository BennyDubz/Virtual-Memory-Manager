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



/**
 * Returns TRUE if the page is not in the free, modfied, or standby lists, FALSE otherwise
 */
int page_is_isolated(PAGE* page) {
    if (page == NULL) {
        fprintf(stderr, "NULL page given to page_is_isolated\n");
        return OTHER_ERROR;
    }
    DB_LL_NODE* curr_node;

    // Check the free lists
    for (ULONG64 free_list = 0; free_list < NUM_FRAME_LISTS; free_list++) {
        EnterCriticalSection(&free_frames->list_locks[free_list]);

        curr_node = free_frames->listheads[free_list]->flink;

        while (curr_node != free_frames->listheads[free_list]) {
            if (curr_node->item == page) {
                LeaveCriticalSection(&free_frames->list_locks[free_list]);
                return IN_FREE;
            }

            curr_node = curr_node->flink;
        }
        
        LeaveCriticalSection(&free_frames->list_locks[free_list]);
    }

    // Check the modified list
    EnterCriticalSection(&modified_list->lock);
    curr_node = modified_list->listhead->flink;
    while (curr_node != modified_list->listhead) {
        if (curr_node->item == page) {
            LeaveCriticalSection(&modified_list->lock);
            return IN_MODIIFED;
        }

        curr_node = curr_node->flink;
    }

    LeaveCriticalSection(&modified_list->lock);


    // Check the standby list
    EnterCriticalSection(&standby_list->lock);
    curr_node = standby_list->listhead->flink;
    while (curr_node != standby_list->listhead) {
        if (curr_node->item == page) {
            LeaveCriticalSection(&standby_list->lock);
            return IN_STANDBY;
        }

        curr_node = curr_node->flink;
    }

    LeaveCriticalSection(&standby_list->lock);

    return ISOLATED;
}



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

    if (relevant_page->pagefile_idx != 0) {
        last_used_disk_slot = disk_idx_to_addr(relevant_page->pagefile_idx);
    }

    DebugBreak();
}