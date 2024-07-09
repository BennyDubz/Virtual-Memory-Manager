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
#include "./init.h"
#include "./globals.h"
#include "./hardware.h"

// Allows us to access the windows permission libraries so that we can actually get physical frames
#pragma comment(lib, "advapi32.lib")

// ########## DEFINED GLOBALS ##########

/**
 * GLOBAL VALUES OR POINTERS
 */

PAGE* page_storage_base;

volatile ULONG64 total_available_pages;

ULONG64 physical_page_count;

/**
 * GLOBAL DATASTRUCTURES
 */
PAGETABLE* pagetable;

DISK* disk;

FREE_FRAMES_LISTS* free_frames;

STANDBY_LIST* standby_list;

MODIFIED_LIST* modified_list;


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

HANDLE modified_to_standby_event;

ULONG64 num_child_threads;

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


static BOOL
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

static int init_simulation(PULONG_PTR* vmem_base_storage, ULONG64* virtual_memory_size_storage);
static int init_datastructures();
static int init_multithreading();


/**
 * Initializes the simulation, all memory management datastructures, and initializes all threads
 * 
 * Stores the base address of virtual memory and the total amount of usable virtual memory vmem_base_storage
 * and virtual_memory_size_storage respectively.
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 */
int init_all(PULONG_PTR* vmem_base_storage, ULONG64* virtual_memory_size_storage) {
    int return_code;

    return_code = init_simulation(vmem_base_storage, virtual_memory_size_storage);
    printf("Initialized simulation\n");
    if (return_code == ERROR) return ERROR;

    return_code = init_datastructures();
    printf("Initialized datastrucures\n");

    if (return_code == ERROR) return ERROR;

    return_code = init_multithreading();
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

    privilege = GetPrivilege ();

    if (privilege == FALSE) {
        printf ("full_virtual_memory_test : could not get privilege\n");
        return ERROR;
    }    

    physical_page_handle = GetCurrentProcess ();

    physical_page_count = NUMBER_OF_PHYSICAL_PAGES;

    physical_page_numbers = malloc (physical_page_count * sizeof (ULONG_PTR));

    if (physical_page_numbers == NULL) {
        fprintf (stderr,"init_simulation : could not allocate array to hold physical page numbers\n");
        return ERROR;
    }

    allocated = AllocateUserPhysicalPages (physical_page_handle,
                                           &physical_page_count,
                                           physical_page_numbers);


    if (allocated == FALSE) {
        fprintf (stderr, "init_simulation : could not allocate physical pages\n");
        return ERROR;
    }

    if (physical_page_count != NUMBER_OF_PHYSICAL_PAGES) {

        fprintf (stderr, "init_simulation : allocated only %llu pages out of %u pages requested\n",
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

    vmem_base = VirtualAlloc (NULL,
                      virtual_address_size,
                      MEM_RESERVE | MEM_PHYSICAL,
                      PAGE_READWRITE);

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

    free_frames = initialize_free_frames(page_storage_base, physical_page_numbers, physical_page_count);
    if (free_frames == NULL) {
        fprintf(stderr, "Unable to allocate memory for free frames\n");
        return ERROR;
    }

    total_available_pages = physical_page_count;

    disk = initialize_disk();
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

    return SUCCESS;
}



/**
 * Initializes all critical threads for our memory manager
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 */
static int init_multithreading() {
    

    //BW: This could later be turned into a manual-reset lock if we are able to write
    // multiple pages to standby simultaneously, but right now we do it one page at a time
    waiting_for_pages_event = CreateEvent(NULL, TRUE, FALSE, NULL);

    aging_event = CreateEvent(NULL, FALSE, FALSE, NULL);

    trimming_event = CreateEvent(NULL, FALSE, FALSE, NULL);

    pagetable_to_modified_event = CreateEvent(NULL, FALSE, FALSE, NULL);

    modified_to_standby_event = CreateEvent(NULL, FALSE, FALSE, NULL);

    disk_write_available_event = CreateEvent(NULL, FALSE, FALSE, NULL);

    disk_read_available_event = CreateEvent(NULL, FALSE, FALSE, NULL);

    disk_open_slots_event = CreateEvent(NULL, FALSE, FALSE, NULL);

    // Does not include this thread that handles page faults, and does not include worker threads
    num_child_threads = 3; 

    threads = (HANDLE*) malloc(sizeof(HANDLE) * num_child_threads);

    if (threads == NULL) {
        fprintf(stderr, "Failed to allocate memory for thread creation in init_multithreading\n");
        return ERROR;
    }

    threads[0] = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE) thread_aging, NULL, 0, NULL);

    threads[1] = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE) thread_trimming, NULL, 0, NULL);

    threads[2] = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE) thread_modified_to_standby, NULL, 0, NULL);

    return SUCCESS;
}