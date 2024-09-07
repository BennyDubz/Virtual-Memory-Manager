/**
 * @author Ben Williams 
 * @date June 28th, 2024
 * 
 * Contains everything related to initializing all of the global variables, datastructures, and
 * the simulation
 */


#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <excpt.h>
#include <stdbool.h>
#include <assert.h>

#include "./Datastructures/datastructures.h"
#include "./Machinery/pagefault.h"
#include "./Machinery/trim.h"
#include "./Machinery/pagelist_operations.h"
#include "./init.h"
#include "./globals.h"
#include "./hardware.h"

// Allows us to access the windows permission libraries so that we can actually get physical frames
#pragma comment(lib, "advapi32.lib")

#pragma comment(lib, "onecore.lib")

// ########## DEFINED GLOBALS ##########

/**
 * GLOBAL VALUES OR POINTERS
 */

PAGE* page_storage_base;

DECLSPEC_ALIGN(64) volatile ULONG64 total_available_pages;

DECLSPEC_ALIGN(64) ULONG64 physical_page_count;

MEM_EXTENDED_PARAMETER vmem_parameters;

volatile ULONG64 remaining_writable_addresses;


/**
 * GLOBAL DATASTRUCTURES
 */
PAGETABLE* pagetable;

DISK* disk;

ZEROED_PAGES_LISTS* zero_lists;

FREE_FRAMES_LISTS* free_frames;

PAGE_LIST* standby_list;

PAGE_LIST* modified_list;

PAGE_ZEROING_STRUCT* page_zeroing;

THREAD_SIMULATION_INFORMATION thread_information;

#if DEBUG_PAGELOCK
PAGE_LOGSTRUCT page_log[LOG_SIZE];

volatile ULONG64 log_idx;
#endif

/**
 * GLOBAL SYNCHRONIZATION
 */

HANDLE waiting_for_pages_event;

HANDLE aging_event;

HANDLE trimming_event;

HANDLE modified_writing_event;

HANDLE disk_write_available_event;

HANDLE disk_read_available_event;

HANDLE disk_open_slots_event;

HANDLE pagetable_to_modified_event;

HANDLE modified_writer_event;

HANDLE zero_pages_event;

ULONG64 num_worker_threads;

HANDLE* threads;


// #####################################

/**
 * INITIALIZATION GLOBALS
 */

PULONG_PTR vmem_base;
BOOL obtained_pages;
PULONG_PTR physical_page_numbers;
ULONG_PTR virtual_address_size;
ULONG_PTR virtual_address_size_in_unsigned_chunks;



BOOL
GetPrivilege  (
    VOID
    )
{
    struct {
        DWORD Count;
        LUID_AND_ATTRIBUTES Privilege [1];
    } Info;

    //
    // This is Windows-specific code to acquire a privilege.
    // Understanding each line of it is not so important for
    // our efforts.
    //

    HANDLE hProcess;
    HANDLE Token;
    BOOL Result;

    //
    // Open the token.
    //

    hProcess = GetCurrentProcess ();

    Result = OpenProcessToken (hProcess,
                               TOKEN_ADJUST_PRIVILEGES,
                               &Token);

    if (Result == FALSE) {
        printf ("Cannot open process token.\n");
        return FALSE;
    }

    //
    // Enable the privilege. 
    //

    Info.Count = 1;
    Info.Privilege[0].Attributes = SE_PRIVILEGE_ENABLED;

    //
    // Get the LUID.
    //

    Result = LookupPrivilegeValue (NULL,
                                   SE_LOCK_MEMORY_NAME,
                                   &(Info.Privilege[0].Luid));

    if (Result == FALSE) {
        printf ("Cannot get privilege\n");
        return FALSE;
    }

    //
    // Adjust the privilege.
    //

    Result = AdjustTokenPrivileges (Token,
                                    FALSE,
                                    (PTOKEN_PRIVILEGES) &Info,
                                    0,
                                    NULL,
                                    NULL);

    //
    // Check the result.
    //

    if (Result == FALSE) {
        printf ("Cannot adjust token privileges %u\n", GetLastError ());
        return FALSE;
    } 

    if (GetLastError () != ERROR_SUCCESS) {
        printf ("Cannot enable the SE_LOCK_MEMORY_NAME privilege - check local policy\n");
        return FALSE;
    }

    CloseHandle (Token);

    return TRUE;
}


HANDLE
CreateSharedMemorySection (
    VOID
    )
{
    HANDLE section;
    MEM_EXTENDED_PARAMETER parameter = { 0 };

    //
    // Create an AWE section.  Later we deposit pages into it and/or
    // return them.
    //

    parameter.Type = MemSectionExtendedParameterUserPhysicalFlags;
    parameter.ULong = 0;

    section = CreateFileMapping2 (INVALID_HANDLE_VALUE,
                                  NULL,
                                  SECTION_MAP_READ | SECTION_MAP_WRITE,
                                  PAGE_READWRITE,
                                  SEC_RESERVE,
                                  0,
                                  NULL,
                                  &parameter,
                                  1);

    return section;
}



static int init_simulation(PULONG_PTR* vmem_base_storage, ULONG64* virtual_memory_size_storage);
static int init_datastructures();
static int init_multithreading(ULONG64 num_usermode_threads);


/**
 * Initializes the simulation, all memory management datastructures, and initializes all threads
 * 
 * Stores the base address of virtual memory and the total amount of usable virtual memory vmem_base_storage
 * and virtual_memory_size_storage respectively.
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 */
int init_all(PULONG_PTR* vmem_base_storage, ULONG64* virtual_memory_size_storage, ULONG64 num_usermode_threads) {
    int return_code;

    return_code = init_simulation(vmem_base_storage, virtual_memory_size_storage);
    printf("Initialized simulation\n");
    if (return_code == ERROR) return ERROR;

    return_code = init_datastructures();
    printf("Initialized datastrucures\n");

    if (return_code == ERROR) return ERROR;

    return_code = init_multithreading(num_usermode_threads);
    printf("Initialized threads\n");

    if (return_code == ERROR) return ERROR;

    return SUCCESS;
}


/**
 * Initializes the actual virtual/physical memory setup, and the global variables associated with it\
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 */
static int init_simulation(PULONG_PTR* vmem_base_storage, ULONG64* virtual_memory_size_storage) {
    BOOL allocated;
    BOOL privilege;
    HANDLE physical_page_handle;

    //
    // Allocate the physical pages that we will be managing.
    //
    // First acquire privilege to do this since physical page control
    // is typically something the operating system reserves the sole
    // right to do.
    //

    privilege = GetPrivilege();

    if (privilege == FALSE) {
        printf ("full_virtual_memory_test : could not get privilege\n");
        return ERROR;
    }    

    physical_page_handle = CreateSharedMemorySection();

    if (physical_page_handle == NULL) {
        printf ("CreateSharedMemorySection failed, error %#x\n", GetLastError());
        return ERROR;
    }

    physical_page_count = NUMBER_OF_PHYSICAL_PAGES;

    physical_page_numbers = malloc(physical_page_count * sizeof (ULONG_PTR));

    if (physical_page_numbers == NULL) {
        fprintf (stderr,"init_simulation : could not allocate array to hold physical page numbers\n");
        return ERROR;
    }

    allocated = AllocateUserPhysicalPages(physical_page_handle,
                                           &physical_page_count,
                                           physical_page_numbers);


    if (allocated == FALSE) {
        fprintf (stderr, "init_simulation : could not allocate physical pages\n");
        return ERROR;
    }

    if (physical_page_count != NUMBER_OF_PHYSICAL_PAGES) {

        fprintf (stderr, "init_simulation : allocated only %llX pages out of %llX pages requested\n",
                physical_page_count,
                NUMBER_OF_PHYSICAL_PAGES);
    }

    //
    // Reserve a user address space region using the Windows kernel
    // AWE (address windowing extensions) APIs.
    //
    // This will let us connect physical pages of our choosing to
    // any given virtual address within our allocated region.
    //
    // We deliberately make this much larger than physical memory
    // to illustrate how we can manage the illusion.
    //

    virtual_address_size = VIRTUAL_ADDRESS_SIZE;

    //
    // Round down to a PAGE_SIZE boundary.
    //

    // virtual_address_size &= ~PAGE_SIZE;

    virtual_address_size_in_unsigned_chunks =
                        virtual_address_size / sizeof (ULONG_PTR);


    // vmem_parameters = { 0 };

    //
    // Allocate a MEM_PHYSICAL region that is "connected" to the AWE section
    // created above.
    //

    vmem_parameters.Type = MemExtendedParameterUserPhysicalHandle;
    vmem_parameters.Handle = physical_page_handle;

    vmem_base = VirtualAlloc2 (NULL,
                       NULL,
                       virtual_address_size,
                       MEM_RESERVE | MEM_PHYSICAL,
                       PAGE_READWRITE,
                       &vmem_parameters,
                       1);

    if (vmem_base == NULL) {
        printf ("init : could not reserve memory for usermode simulation\n");
        return ERROR;
    }

    *virtual_memory_size_storage = virtual_address_size;
    *vmem_base_storage = vmem_base;
    
    return SUCCESS;
}


/**
 * Initializes all critical datastructures for our memory manager
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 */
static int init_datastructures() {
    /**
     * Initialize pages, pagetable, all page lists, and the disk
     */

    page_storage_base = initialize_pages(physical_page_numbers, physical_page_count);

    if (page_storage_base == NULL) {
        fprintf(stderr, "Unable to allocate memory for the pages\n");
        return ERROR;
    }

    ULONG64 number_of_virtual_pages = virtual_address_size / PAGE_SIZE;

    pagetable = initialize_pagetable(number_of_virtual_pages, vmem_base);
    if (pagetable == NULL) {
        fprintf(stderr, "Unable to allocate memory for pagetable\n");
        return ERROR;
    }

    // 512 8-byte addresses fit into a 4KB page
    remaining_writable_addresses = pagetable->num_virtual_pages * 512;

    zero_lists = initialize_zeroed_lists(page_storage_base, physical_page_numbers, physical_page_count);
    if (zero_lists == NULL) {
        fprintf(stderr, "Unable to allocate memory for zero_lists\n");
        return ERROR;
    }

    free_frames = initialize_free_frames();
    if (free_frames == NULL) {
        fprintf(stderr, "Unable to allocate memory for free frames\n");
        return ERROR;
    }

    total_available_pages = physical_page_count;
    disk = initialize_disk(&vmem_parameters);


    if (disk == NULL) {
        fprintf(stderr, "Unable to allocate memory for disk\n");
        return ERROR;
    }

    standby_list = initialize_standby_list();
    if (standby_list == NULL) {
        fprintf(stderr, "Unable to initialize standby list\n");
        return ERROR;
    }

    modified_list = initialize_modified_list();
    if (modified_list == NULL) {
        fprintf(stderr, "Unable to initialize modified list\n");
        return ERROR;
    }

    if (initialize_page_zeroing(&vmem_parameters) == ERROR) {
        fprintf(stderr, "Unable to initialize page zeroing structures\n");
        return ERROR;
    }
   

    #if DEBUG_PAGELOCK
    page_log;
    log_idx = 0;
    #endif
    
    page_zeroing = (PAGE_ZEROING_STRUCT*) malloc(sizeof(PAGE_ZEROING_STRUCT));

    if (page_zeroing == NULL) {
        fprintf(stderr, "Unable to initialize page zeroing global structure\n");
        return ERROR;
    }

    page_zeroing->curr_idx = 0;
    page_zeroing->total_slots_used = 0;
    page_zeroing->zeroing_ongoing = FALSE;

    for (ULONG64 i = 0; i < NUM_THREAD_ZERO_SLOTS; i++) {
        page_zeroing->status_map[i] = PAGE_SLOT_OPEN;
    }

    return SUCCESS;
}



/**
 * Initializes all critical threads for our memory manager
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 */
static int init_multithreading(ULONG64 num_usermode_threads) {
    

    //BW: This could later be turned into a manual-reset lock if we are able to write
    // multiple pages to standby simultaneously, but right now we do it one page at a time
    waiting_for_pages_event = CreateEvent(NULL, TRUE, FALSE, NULL);

    aging_event = CreateEvent(NULL, FALSE, FALSE, NULL);

    trimming_event = CreateEvent(NULL, FALSE, FALSE, NULL);

    pagetable_to_modified_event = CreateEvent(NULL, FALSE, FALSE, NULL);

    modified_writer_event = CreateEvent(NULL, FALSE, FALSE, NULL);

    disk_write_available_event = CreateEvent(NULL, FALSE, FALSE, NULL);

    // This event should be used infrequently, but we will allow all threads to proceed when it triggers
    disk_read_available_event = CreateEvent(NULL, TRUE, FALSE, NULL);

    disk_open_slots_event = CreateEvent(NULL, FALSE, FALSE, NULL);

    zero_pages_event = CreateEvent(NULL, FALSE, FALSE, NULL);

    // Does not include this thread that handles page faults, and does not include worker threads
    num_worker_threads = 4; 

    threads = (HANDLE*) malloc(sizeof(HANDLE) * num_worker_threads);

    if (threads == NULL) {
        fprintf(stderr, "Failed to allocate memory for thread creation in init_multithreading\n");
        return ERROR;
    }


    /**
     * All thread (simulation and worker) local storage setup
     */
    THREAD_LOCAL_STORAGE* thread_storage = (THREAD_LOCAL_STORAGE*) malloc(sizeof(THREAD_LOCAL_STORAGE) * (num_usermode_threads + num_worker_threads));

    if (thread_storage == NULL) {
        fprintf(stderr, "Failed to allocate memory for thread local storage\n");
        return ERROR;
    }

    thread_information.total_thread_count = num_usermode_threads + num_worker_threads;
    thread_information.thread_local_storages = thread_storage;

    
    /** 
     * Aging setup
     */
    WORKER_THREAD_PARAMETERS* aging_params = (WORKER_THREAD_PARAMETERS*) malloc(sizeof(WORKER_THREAD_PARAMETERS));

    if (aging_params == NULL) {
        fprintf(stderr, "Failed to allocate memory for worker thread parameters in init_multithreading\n");
    }
    
    aging_params->thread_idx = num_usermode_threads;
    aging_params->other_parameters = NULL;

    threads[0] = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE) thread_aging, aging_params, 0, NULL);

    /**
     * Trimming setup
     */
    WORKER_THREAD_PARAMETERS* trim_params = (WORKER_THREAD_PARAMETERS*) malloc(sizeof(WORKER_THREAD_PARAMETERS));

    if (trim_params == NULL) {
        fprintf(stderr, "Failed to allocate memory for worker thread parameters in init_multithreading\n");
    }
    
    trim_params->thread_idx = num_usermode_threads + 1;
    trim_params->other_parameters = NULL;

    threads[1] = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE) thread_trimming, trim_params, 0, NULL);

    /**
     * Mod writer setup
     */
    WORKER_THREAD_PARAMETERS* mod_writer_params = (WORKER_THREAD_PARAMETERS*) malloc(sizeof(WORKER_THREAD_PARAMETERS));

    if (mod_writer_params == NULL) {
        fprintf(stderr, "Failed to allocate memory for worker thread parameters in init_multithreading\n");
    }
    
    mod_writer_params->thread_idx = num_usermode_threads + 2;
    mod_writer_params->other_parameters = NULL;

    threads[2] = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE) thread_modified_writer, mod_writer_params, 0, NULL);


    /**
     * Zeroing thread setup
     */
    WORKER_THREAD_PARAMETERS* zeroer_params = (WORKER_THREAD_PARAMETERS*) malloc(sizeof(WORKER_THREAD_PARAMETERS));

    if (zeroer_params == NULL) {
        fprintf(stderr, "Failed to allocate memory for worker thread parameters in init_multithreading\n");
    }
    
    zeroer_params->thread_idx = num_usermode_threads + 3;
    zeroer_params->other_parameters = &vmem_parameters;

    threads[3] = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE) thread_populate_zero_lists, zeroer_params, 0, NULL);


    for (ULONG64 thread_idx = 0; thread_idx < num_usermode_threads + num_worker_threads; thread_idx++) {
        thread_storage[thread_idx].list_refresh_status = LIST_REFRESH_NOT_ONGOING;
        // The thread handles will be initialized when we create the usermode threads in the parent usermode_simulation thread
    }

    /**
     * Only the faulting threads need to have their disk resources allocated to them
     */
    ULONG64 num_readsections_per_thread = DISK_READSECTIONS / num_usermode_threads;

    for (ULONG64 thread_idx = 0; thread_idx < num_usermode_threads; thread_idx++) {
        THREAD_DISK_READ_RESOURCES* disk_resources = &thread_storage[thread_idx].disk_resources;
        
        PULONG_PTR thread_read_base = disk->disk_read_base_addr + (thread_idx * PAGE_SIZE * num_readsections_per_thread * DISK_READSECTION_SIZE / sizeof(PULONG_PTR));

        ULONG64 min_idx = num_readsections_per_thread * thread_idx;
        ULONG64 max_idx = num_readsections_per_thread * (thread_idx + 1) - 1;

        disk_resources->thread_disk_read_base = thread_read_base;
        disk_resources->min_readsection_idx = min_idx;
        disk_resources->max_readsection_idx = max_idx;
        disk_resources->num_allocated_readsections = num_readsections_per_thread;
        disk_resources->curr_readsection_idx = min_idx;
    }


    return SUCCESS;
}