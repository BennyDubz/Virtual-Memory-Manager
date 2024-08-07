#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <excpt.h>
#include <stdbool.h>
#include <assert.h>
#include <time.h>

#include "./init.h"
#include "./Machinery/pagefault.h"
#include "./hardware.h"
#include "./Machinery/debug_checks.h"


#define NUM_USERMODE_THREADS        12
#define MAX_CONSECUTIVE_ACCESSES    64
#define TOTAL_ACCESS_AMOUNT         MB(10)

/**
 * Reads are more common in the real world - if (random_number % WRITE_PROBABILITY_MODULO == 0) then we write to the address
 * This allows us to properly demonstrate how the fault handler distinguishes reads and writes, and how we
 * take advantage of using READONLY permissions to preserve pagefile space and therefore speed up trimming and improve page availability
 */
#define WRITE_PROBABILITY_MODULO  5

HANDLE simulation_threads[NUM_USERMODE_THREADS];
HANDLE thread_start_event;
volatile ULONG64 total_fault_failures = 0;
volatile ULONG64 fault_results[NUM_FAULT_RETURN_VALS];

// For passing to each thread for the simulation
typedef struct {
    PULONG_PTR vmem_base;
    ULONG_PTR virtual_address_size;
    ULONG64* time_storage;
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

    SIM_PARAMS thread_params[NUM_USERMODE_THREADS];

    ULONG64 thread_times[NUM_USERMODE_THREADS];

    thread_start_event = CreateEvent(NULL, TRUE, FALSE, NULL);

    #ifdef DEBUG_CHECKING
    printf("Debug checking is on\n");
    #endif

    #ifdef LOCK_SPINNING
    printf("Lock spinning is on for performance analysis\n");
    #endif

    #ifdef SMALL_SIM
    printf("Running small simulation\n");
    #endif

    #ifdef LARGE_SIM
    printf("Running large simulation\n");
    #endif

    #ifdef LENIENT_DISK
    printf("No simulated disk slowdown: lenient disk is on\n");
    #endif

    // Initialize fault failure tracking array
    for (int i = 0; i < NUM_FAULT_RETURN_VALS; i++) {
        fault_results[i] = 0;
    }

    // Start off all threads
    for (int user_thread_num = 0; user_thread_num < NUM_USERMODE_THREADS; user_thread_num++) {
        thread_params[user_thread_num].time_storage = &thread_times[user_thread_num];
        thread_params[user_thread_num].virtual_address_size = virtual_address_size;
        thread_params[user_thread_num].vmem_base = vmem_base;
        simulation_threads[user_thread_num] = CreateThread(NULL, 0, 
                (LPTHREAD_START_ROUTINE) thread_access_random_addresses, (LPVOID) &thread_params[user_thread_num], 0, NULL);
    }

    // Give a little time for the other threads to start up
    Sleep(50);

    clock_t timer;
    timer = clock();

    SetEvent(thread_start_event);

    // Wait for thread completion
    for (int user_thread_num = 0; user_thread_num < NUM_USERMODE_THREADS; user_thread_num++) {
        WaitForSingleObject(simulation_threads[user_thread_num], INFINITE);
    }   
    

    total_fault_failures = 0;
    // Ignore successful faults
    for (int i = 1; i < NUM_FAULT_RETURN_VALS; i++) {
        total_fault_failures += fault_results[i];
    }

    timer = clock() - timer;
    double time_taken = (double) (timer) / CLOCKS_PER_SEC;
    printf("usermode_virtual_memory_simulation : finished accessing %d random virtual addresses over %d threads\n", TOTAL_ACCESS_AMOUNT, NUM_USERMODE_THREADS);
    printf("usermode_virtual_memory_simulation : total of %lld fault failures\n", total_fault_failures);
    printf("usermode_virtual_memory_simulation : total time was %f seconds\n", time_taken);
    printf("usermode_virtual_memory_simulation : fault breakdown:\n");

    printf("\tSuccessful faults: %lld\n", fault_results[SUCCESSFUL_FAULT]);
    printf("\tFailures due to rejection (invalid address): %lld\n", fault_results[REJECTION_FAIL]);
    printf("\tFailures due to lack of available pages: %lld\n", fault_results[NO_AVAILABLE_PAGES_FAIL]);
    printf("\tFailures due to failed rescues of transition PTEs: %lld\n", fault_results[RESCUE_FAIL]);
    printf("\tFailures due to disk waiting: %lld\n", fault_results[DISK_FAIL]);
    printf("\tFailures due to races between user threads: %lld\n", fault_results[RACE_CONDITION_FAIL]);

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

    // This ensures that we access distinct 64 bit chunks that do not overlap
    ULONG64 virtual_address_size_in_unsigned_chunks = virtual_address_size / sizeof(ULONG_PTR);
    
    int fault_result;
    arbitrary_va = NULL;
    int consecutive_accesses = 0;   

    /**
     * Normally, the CPU would tell the operating system what the access type was for a fault.
     * In the simulation, we will have to tell the fault handler this ourselves
     */
    ULONG64 access_type;


    WaitForSingleObject(thread_start_event, INFINITE);

    printf("Thread starting \n");
    for (i = 0; i < TOTAL_ACCESS_AMOUNT / NUM_USERMODE_THREADS; i += 1) {

        if (consecutive_accesses == 0) {
            arbitrary_va = NULL;
        }
        
        /**
         * We want to make consecutive accesses very common. If we are doing consecutive accesses, we will increment the VA
         * into the next page
         */
        if (consecutive_accesses != 0 && page_faulted == FALSE) {
            if ((ULONG64) arbitrary_va + PAGE_SIZE < (ULONG64) vmem_base + VIRTUAL_ADDRESS_SIZE) {
                arbitrary_va += (PAGE_SIZE / sizeof(ULONG_PTR));
            } else {
                arbitrary_va = NULL;
            }
            consecutive_accesses --;
        }
        
        if (arbitrary_va == NULL) {

            // Not cryptographically strong, but good enough to get a spread-out distribution
            random_number = ReadTimeStampCounter();

            random_number %= virtual_address_size_in_unsigned_chunks;

            arbitrary_va = vmem_base + random_number;

            consecutive_accesses = ReadTimeStampCounter() % MAX_CONSECUTIVE_ACCESSES;
        }

        //
        // Write the virtual address into each page.  If we need to
        // debug anything, we'll be able to see these in the pages.
        //

        page_faulted = FALSE;
        access_type = READ_ACCESS;

        __try {
            //BW: Switch to this when we are actually zeroing-out pages
            if (*arbitrary_va == 0) {
                if (ReadTimeStampCounter() % WRITE_PROBABILITY_MODULO == 0) {
                    access_type = WRITE_ACCESS;
                    *arbitrary_va = (ULONG_PTR) arbitrary_va;
                }
            } else if((ULONG_PTR) *arbitrary_va != (ULONG_PTR) arbitrary_va) {
                debug_break_all_va_info(arbitrary_va);
            }

        } __except (EXCEPTION_EXECUTE_HANDLER) {

            page_faulted = TRUE;
        }

        if (page_faulted) {
            fault_result = pagefault(arbitrary_va, access_type);

            InterlockedIncrement64(&fault_results[fault_result]);

            // We will not try a unique random address again, so we do not incrment i
            i--; 
        } else {
            /**
             * This is purely to simulate editing access bits in the PTE, which normally the CPU does for us
             * 
             * We have to do this ourselves at the beginning of the pagefault
             */

            // pagefault(arbitrary_va);
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
