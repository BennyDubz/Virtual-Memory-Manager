#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <excpt.h>
#include <stdbool.h>
#include <assert.h>

#include "./init.h"
#include "./Machinery/pagefault.h"
#include "./hardware.h"


void usermode_virtual_memory_simulation () {
    unsigned i;
    BOOL page_faulted;
    PULONG_PTR arbitrary_va;
    unsigned random_number;
    PULONG_PTR vmem_base;
    ULONG_PTR virtual_address_size;

    if (init_all(&vmem_base, &virtual_address_size) == ERROR) {
        fprintf(stderr, "Unable to initialize usermode memory management simulation\n");
        return;
    }

    ULONG64 virtual_address_size_in_unsigned_chunks = virtual_address_size / sizeof(ULONG_PTR);

    srand (time (NULL));
    int fault_result;
    arbitrary_va = NULL;
    // srand(1);
    ULONG64 total_fault_failures = 0;

    for (i = 0; i < MB (1); i += 1) {

        // // Signal aging (temporary)
        // if (i % 8 == 0) {
        //     SetEvent(aging_event);
        // }

        // If we fail a page fault, we will want to try the address again and not change it
        if (arbitrary_va == NULL) {
            random_number = rand () * rand() * rand();

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

    printf ("usermode_virtual_memory_simulation : finished accessing %u random virtual addresses\n", i);
    printf ("usermode_virtual_memory_simulation : total of %llx fault failures\n", total_fault_failures);

    //
    // Now that we're done with our memory we can be a good
    // citizen and free it.
    //

    VirtualFree (vmem_base, 0, MEM_RELEASE);

    return;
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
