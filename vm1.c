#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <excpt.h>
#include <stdbool.h>
#include <assert.h>

#include "./init.h"
#include "./Machinery/pagefault.h"
#include "./hardware.h"


#define NUM_USERMODE_THREADS        1
#define ACCESS_AMOUNT       MB(1)

HANDLE simulation_threads[NUM_USERMODE_THREADS];
volatile ULONG64 total_fault_failures = 0;

// For passing to each thread for the simulation
typedef struct {
    PULONG_PTR vmem_base;
    ULONG_PTR virtual_address_size;
} SIM_PARAMS;

int thread_access_random_addresses(void* params);

/**
 * Initializes the virtual memory state machine simulation and creates threads to access addresses
 * and stress-test the system
 */
void usermode_virtual_memory_simulation () {
    ULONG64 i;
    BOOL page_faulted;
    PULONG_PTR arbitrary_va;
    ULONG64 random_number;
    PULONG_PTR vmem_base;
    ULONG_PTR virtual_address_size;

    if (init_all(&vmem_base, &virtual_address_size) == ERROR) {
        fprintf(stderr, "Unable to initialize usermode memory management simulation\n");
        return;
    }

    ULONG64 virtual_address_size_in_unsigned_chunks = virtual_address_size / sizeof(ULONG_PTR);

    SIM_PARAMS thread_params;

    thread_params.virtual_address_size = virtual_address_size;
    thread_params.vmem_base = vmem_base;
    srand (time (NULL));

    // Start off all threads
    for (int user_thread_num = 0; user_thread_num < NUM_USERMODE_THREADS; user_thread_num++) {
        simulation_threads[user_thread_num] = CreateThread(NULL, 0, 
                (LPTHREAD_START_ROUTINE) thread_access_random_addresses, (LPVOID) &thread_params, 0, NULL);
    }


    // Wait for thread completion
    for (int user_thread_num = 0; user_thread_num < NUM_USERMODE_THREADS; user_thread_num++) {
        WaitForSingleObject(simulation_threads[user_thread_num], INFINITE);
    }
    
    printf ("usermode_virtual_memory_simulation : finished accessing %d random virtual addresses over %d threads\n", ACCESS_AMOUNT * NUM_USERMODE_THREADS, NUM_USERMODE_THREADS);
    printf ("usermode_virtual_memory_simulation : total of 0x%llx fault failures\n", total_fault_failures);

    //
    // Now that we're done with our memory we can be a good
    // citizen and free it.
    //

    VirtualFree (vmem_base, 0, MEM_RELEASE);

    return;
}


int thread_access_random_addresses(void* params) {
    ULONG64 i;
    BOOL page_faulted;
    PULONG_PTR arbitrary_va;
    ULONG64 random_number;
    PULONG_PTR vmem_base;
    ULONG_PTR virtual_address_size;


    SIM_PARAMS* parameters = (SIM_PARAMS*) params;

    virtual_address_size = parameters->virtual_address_size;
    vmem_base = parameters->vmem_base;

    ULONG64 virtual_address_size_in_unsigned_chunks = virtual_address_size / sizeof(ULONG_PTR);

    int fault_result;
    arbitrary_va = NULL;

    printf("Thread starting\n");

    for (i = 0; i < ACCESS_AMOUNT; i += 1) {

        // // Signal aging (temporary)
        // if (i % 8 == 0) {
        //     SetEvent(aging_event);
        // }

        // If we fail a page fault, we will want to try the address again and not change it
        if (arbitrary_va == NULL) {
            random_number = rand () * rand() * rand() * rand() * rand();

            random_number %= virtual_address_size_in_unsigned_chunks;

            arbitrary_va = vmem_base + random_number;
        }

        //
        // Write the virtual address into each page.  If we need to
        // debug anything, we'll be able to see these in the pages.
        //

        page_faulted = FALSE;

        __try {

            *arbitrary_va = (ULONG_PTR) arbitrary_va;

            arbitrary_va = NULL;

        } __except (EXCEPTION_EXECUTE_HANDLER) {

            page_faulted = TRUE;
        }

        if (page_faulted) {
            fault_result = pagefault(arbitrary_va);

            //BW: This is likely a temporary solution, we will want to further separate the usermode code
            if (fault_result == ERROR) {
                total_fault_failures++;
                // printf("Fault failed - retrying fault\n");
            }
        } 
        //BW: ELSE: we mark the relevant PTE as accessed (real hardware would do this for us)
    }

    printf("Thread complete\n");

    return SUCCESS;
}


void main (int argc, char** argv) {

    //
    // Test our very complicated usermode virtual implementation.
    // 
    // We will control the virtual and physical address space management
    // ourselves with the only two exceptions being that we will :
    //
    // 1. Ask the operating system for the physical pages we'll use to
    //    form our pool.
    //
    // 2. Ask the operating system to connect one of our virtual addresses
    //    to one of our physical pages (from our pool).
    //
    // We would do both of those operations ourselves but the operating
    // system (for security reasons) does not allow us to.
    //
    // But we will do all the heavy lifting of maintaining translation
    // tables, PFN data structures, management of physical pages,
    // virtual memory operations like handling page faults, materializing
    // mappings, freeing them, trimming them, writing them out to backing
    // store, bringing them back from backing store, protecting them, etc.
    //
    // This is where we can be as creative as we like, the sky's the limit !
    //

    usermode_virtual_memory_simulation();

    return;
}
