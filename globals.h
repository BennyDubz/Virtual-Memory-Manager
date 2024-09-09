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
 * ADDITIONAL STRUCTS FOR GLOBAL LISTS
 */


/**
 * If we can have local storage for each individual thread, then we can
 * sometimes avoid threads colliding on hot cache lines
 * 
 * For instance, we have an InterlockedOr to decide if a thread is going to refresh the
 * free and zero lists - if all of the threads collide on this variable then
 * we slow all of them down even with a seemingly light operation like an InterlockedOr.
 * 
 * If we do the interlocked operation and then set all of the list_refresh_ongoing values in each of the 
 * threads, and threads check this value before attempting to refresh, then we can avoid most of these collisions
 * and excess work.
 * 
 * We could also have used the thread local storage in the win32 library.
 */

#define DEBUG_THREAD_STORAGE 1

#define LIST_REFRESH_NOT_ONGOING 0
#define LIST_REFRESH_ONGOING 1

#define TRIMMER_NOT_SIGNALLED 0
#define TRIMMER_SIGNALED 1

#ifndef THREAD_LOCAL_STORAGE_T
#define THREAD_LOCAL_STORAGE_T

typedef struct {
    #ifdef DEBUG_THREAD_STORAGE
    ULONG64 thread_id;
    ULONG64 buffer1[7];
    #endif

    UCHAR list_refresh_status;
    UCHAR buffer2[63];

    UCHAR trim_signaled;
    UCHAR buffer3[63];

    THREAD_DISK_READ_RESOURCES disk_resources;
} THREAD_LOCAL_STORAGE;


typedef struct {
    // This value might be accessed frequently in loops,
    ULONG64 total_thread_count;
    ULONG64 buffer[7];
    THREAD_LOCAL_STORAGE* thread_local_storages;
} THREAD_SIMULATION_INFORMATION;

typedef struct {
    ULONG64 thread_idx;
    void* other_parameters;
} WORKER_THREAD_PARAMETERS;

#endif


/**
 * GLOBAL VALUES OR POINTERS
 */

extern PAGE* page_storage_base;

extern volatile ULONG64 total_available_pages;

extern ULONG64 physical_page_count;

extern MEM_EXTENDED_PARAMETER vmem_parameters;

// Since we stamp the virtual addresses, we can keep track of the amount of un-stamped VAs
extern volatile ULONG64 remaining_writable_addresses;


/**
 * GLOBAL DATASTRUCTURES
 */

extern PAGETABLE* pagetable;

extern DISK* disk;

extern ZEROED_PAGES_LISTS* zero_lists;

extern FREE_FRAMES_LISTS* free_frames;

extern PAGE_LIST* standby_list;

extern PAGE_LIST* modified_list;

extern PAGE_ZEROING_STRUCT* page_zeroing;

extern THREAD_SIMULATION_INFORMATION thread_information;

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

extern HANDLE modified_writer_event;

extern HANDLE zero_pages_event;

extern HANDLE disk_write_available_event;

extern HANDLE disk_read_available_event;

extern HANDLE disk_open_slots_event;

extern HANDLE refresh_lists_event;

extern ULONG64 num_worker_threads;

extern HANDLE shutdown_event;

extern HANDLE* threads;

