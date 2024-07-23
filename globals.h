/**
 * @author Ben Williams
 * @date June 25th, 2024
 * 
 * All global variables used across the state machine as well as inside the datastructures
 */

#include <windows.h>
#include "Datastructures/disk.h"
#include "Datastructures/pagetable.h"
#include "Datastructures/pagelists.h"
#include "./Machinery/pagelist_operations.h"

/**
 * GLOBAL VALUES OR POINTERS
 */

extern PAGE* page_storage_base;

extern volatile ULONG64 total_available_pages;

extern ULONG64 physical_page_count;

/**
 * GLOBAL DATASTRUCTURES
 */
extern PAGETABLE* pagetable;

extern DISK* disk;

extern ZEROED_PAGES_LISTS* zero_lists;

extern FREE_FRAMES_LISTS* free_frames;

extern STANDBY_LIST* standby_list;

extern MODIFIED_LIST* modified_list;

extern PAGE_ZEROING_STRUCT* page_zeroing;

#define LOG_SIZE 512
#if DEBUG_PAGELOCK
extern PAGE_LOGSTRUCT page_log[LOG_SIZE];

extern volatile ULONG64 log_idx;
#endif




/**
 * GLOBAL SYNCHRONIZATION
 */

extern HANDLE waiting_for_pages_event;

extern HANDLE aging_event;

extern HANDLE trimming_event;

extern HANDLE pagetable_to_modified_event;

extern HANDLE modified_to_standby_event;

extern HANDLE zero_pages_event;

extern HANDLE disk_write_available_event;

extern HANDLE disk_read_available_event;

extern HANDLE disk_open_slots_event;

extern ULONG64 num_child_threads;

extern HANDLE* threads;

