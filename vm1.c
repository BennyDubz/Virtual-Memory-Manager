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
#include "./Datastructures/pagelists.h"


#define NUM_USERMODE_THREADS        ((ULONG64) (12))
#define MAX_CONSECUTIVE_ACCESSES    64
#define TOTAL_ACCESS_AMOUNT         (MB(10))

// How frequently in milliseconds we print out all of the information about the simulation and our current progress
#define PRINT_FREQUECY_MS          2000 

/**
 * Reads are more common in the real world - if (random_number % WRITE_PROBABILITY_MODULO == 0) then we write to the address
 * This allows us to properly demonstrate how the fault handler distinguishes reads and writes, and how we
 * take advantage of using READONLY permissions to preserve pagefile space and therefore speed up trimming and improve page availability
 */
#define WRITE_PROBABILITY_MODULO  10

HANDLE thread_start_event;
HANDLE simulation_thread_handles[NUM_USERMODE_THREADS];
volatile ULONG64 total_fault_failures = 0;
volatile ULONG64 fault_results[NUM_FAULT_RETURN_VALS];

// For passing to each thread for the simulation
typedef struct {
    PULONG_PTR vmem_base;
    ULONG_PTR virtual_address_size;
    volatile ULONG64 current_access_count;
    ULONG64 thread_index;

    // So we don't waste time bouncing cache lines on the usermode threads
    ULONG64 buffer[4];
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

    if (init_all(&vmem_base, &virtual_address_size, NUM_USERMODE_THREADS) == ERROR) {
        fprintf(stderr, "Unable to initialize usermode memory management simulation\n");
        return;
    }


    ULONG64 virtual_address_size_in_unsigned_chunks = virtual_address_size / sizeof(ULONG_PTR);

    SIM_PARAMS thread_params[NUM_USERMODE_THREADS];

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

    printf("0x%llX threads will perform a total of 0x%llX accesses\n",(ULONG64) NUM_USERMODE_THREADS, (ULONG64) TOTAL_ACCESS_AMOUNT);

    // Initialize fault failure tracking array
    for (int i = 0; i < NUM_FAULT_RETURN_VALS; i++) {
        fault_results[i] = 0;
    }

    // Start off all threads
    for (int user_thread_num = 0; user_thread_num < NUM_USERMODE_THREADS; user_thread_num++) {
        thread_params[user_thread_num].current_access_count = 0;
        thread_params[user_thread_num].virtual_address_size = virtual_address_size;
        thread_params[user_thread_num].vmem_base = vmem_base;
        thread_params[user_thread_num].thread_index = user_thread_num;

        simulation_thread_handles[user_thread_num] = CreateThread(NULL, 0, 
                (LPTHREAD_START_ROUTINE) thread_access_random_addresses, (LPVOID) &thread_params[user_thread_num], 0, NULL);
    }

    // Give a little time for the other threads to start up
    Sleep(50);

    clock_t timer;
    timer = clock();

    SetEvent(thread_start_event);

    // Wait for thread completion
    ULONG64 current_access_count = 0;
    double proportion_of_total;
    ULONG64 wait_code;
    for (int user_thread_num = 0; user_thread_num < NUM_USERMODE_THREADS; user_thread_num++) {
        wait_code = WaitForSingleObject(simulation_thread_handles[user_thread_num], PRINT_FREQUECY_MS);

        // So we don't spam out printfs at the end of the fault
        if (wait_code == WAIT_TIMEOUT) {
            for (ULONG64 i = 0; i < NUM_USERMODE_THREADS; i++) {
                current_access_count += thread_params[i].current_access_count;
            }

            proportion_of_total = (double) current_access_count / (double) TOTAL_ACCESS_AMOUNT;

            printf("Total access count 0x%llx is %.4f%% of the total amount\n", current_access_count, proportion_of_total * 100);
            printf("\tPhys page standby ratio: %f Zeroed: 0x%llX Free: 0x%llX Standby: 0x%llX Mod 0x%llX Num disk slots %llX\n", (double)  standby_list->list_length / physical_page_count, zero_lists->total_available, free_frames->total_available, 
                                            standby_list->list_length, modified_list->list_length, disk->total_available_slots);

            user_thread_num --;

        }

        current_access_count = 0;
    }   
    

    total_fault_failures = 0;
    // Ignore successful faults
    for (int i = 1; i < NUM_FAULT_RETURN_VALS; i++) {
        total_fault_failures += fault_results[i];
    }

    timer = clock() - timer;
    double time_taken = (double) (timer) / CLOCKS_PER_SEC;
    printf("usermode_virtual_memory_simulation : finished accessing 0x%llX random virtual addresses over 0x%llX threads\n", TOTAL_ACCESS_AMOUNT, NUM_USERMODE_THREADS);
    printf("usermode_virtual_memory_simulation : total of 0x%llX fault failures\n", total_fault_failures);
    printf("usermode_virtual_memory_simulation : total time was %f seconds\n", time_taken);
    
    ULONG64 unaccessed_pte_count = 0;
    for (ULONG64 i = 0; i < pagetable->num_virtual_pages; i++) {
        if (is_used_pte(pagetable->pte_list[i]) == FALSE) unaccessed_pte_count++;
    }
    
    printf("usermode_virtual_memory_simulation : num PTEs that were never accessed: 0x%llx\n", unaccessed_pte_count);

    printf("usermode_virtual_memory_simulation : fault breakdown:\n");

    printf("\tSuccessful faults: 0x%llX\n", fault_results[SUCCESSFUL_FAULT]);
    printf("\tFailures due to rejection (invalid parameters): 0x%llX\n", fault_results[REJECTION_FAIL]);
    printf("\tFailures due to lack of available pages: 0x%llX\n", fault_results[NO_AVAILABLE_PAGES_FAIL]);
    printf("\tFailures due to failed rescues of transition PTEs: 0x%llX\n", fault_results[RESCUE_FAIL]);
    printf("\tFailures due to races on disk PTEs: 0x%llX\n", fault_results[DISK_RACE_CONTIION_FAIL]);
    printf("\tFailures due to races on unaccessed PTEs: 0x%llX\n", fault_results[UNACCESSED_RACE_CONDITION_FAIL]);
    printf("\tFailures due to races on valid PTEs: 0x%llx\n", fault_results[VALID_PTE_RACE_CONTIION_FAIL]);

    //
    // Now that we're done with our memory we can be a good
    // citizen and free it.
    //

    SetEvent(shutdown_event);

    WaitForMultipleObjects(num_worker_threads, threads, TRUE, INFINITE);

    VirtualFree (vmem_base, 0, MEM_RELEASE);

    return;
}



int thread_access_random_addresses(void* params) {
    ULONG64 i;
    BOOL page_faulted;
    PULONG_PTR arbitrary_va;
    ULONG64 random_number = 0;
    PULONG_PTR vmem_base;
    ULONG_PTR virtual_address_size;
    ULONG64 thread_idx;
    volatile ULONG64* access_count;

    SIM_PARAMS* parameters = (SIM_PARAMS*) params;

    virtual_address_size = parameters->virtual_address_size;
    vmem_base = parameters->vmem_base;
    thread_idx = parameters->thread_index;
    access_count = &parameters->current_access_count;

    #if DEBUG_THREAD_STORAGE
    thread_information.thread_local_storages[thread_idx].thread_id = GetCurrentThreadId();
    #endif

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

    ULONG64 curr_rand_number_idx = 0;
    ULONG64 last_random_number;

    ULONG64 random_number_idx_storages[32];

    for (ULONG64 i = 0; i < 32; i++) random_number_idx_storages[i] = 0;

    WaitForSingleObject(thread_start_event, INFINITE);

    for (i = 0; i < TOTAL_ACCESS_AMOUNT / NUM_USERMODE_THREADS; i++) {
        *access_count = i;

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
            last_random_number = random_number;
            random_number = ReadTimeStampCounter();

            if (i != 0) {
                 ULONG64 idx = (random_number - last_random_number) / 512;

                if (idx >= 32) idx = 31;

                random_number_idx_storages[idx]++;
            }

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
                    InterlockedDecrement64(&remaining_writable_addresses);
                }
            } else if((ULONG_PTR) *arbitrary_va != (ULONG_PTR) arbitrary_va) {
                debug_break_all_va_info(arbitrary_va);
            }

        } __except (EXCEPTION_EXECUTE_HANDLER) {

            page_faulted = TRUE;
        }

        if (page_faulted) {
            fault_result = pagefault(arbitrary_va, access_type, thread_idx);

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

    return SUCCESS;
}



void main (int argc, char** argv) {

    usermode_virtual_memory_simulation();

    return;
}
