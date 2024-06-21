#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <excpt.h>
#include <stdbool.h>
#include "./Datastructures/db_linked_list.h"
#include "./Datastructures/pagetable.h"
#include "./Datastructures/pagelists.h"

#include "./hardware.h"

// Allows us to access the windows permission libraries so that we can actually get physical frames
#pragma comment(lib, "advapi32.lib")



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

VOID
full_virtual_memory_test (
    VOID
    )
{
    unsigned i;
    PULONG_PTR vmem_base;
    PULONG_PTR arbitrary_va;
    unsigned random_number;
    BOOL allocated;
    BOOL page_faulted;
    BOOL privilege;
    BOOL obtained_pages;
    ULONG_PTR physical_page_count;
    PULONG_PTR physical_page_numbers;
    HANDLE physical_page_handle;
    ULONG_PTR virtual_address_size;
    ULONG_PTR virtual_address_size_in_unsigned_chunks;

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
        return;
    }    

    physical_page_handle = GetCurrentProcess ();

    physical_page_count = NUMBER_OF_PHYSICAL_PAGES;

    physical_page_numbers = malloc (physical_page_count * sizeof (ULONG_PTR));

    if (physical_page_numbers == NULL) {
        printf ("full_virtual_memory_test : could not allocate array to hold physical page numbers\n");
        return;
    }

    allocated = AllocateUserPhysicalPages (physical_page_handle,
                                           &physical_page_count,
                                           physical_page_numbers);


    if (allocated == FALSE) {
        printf ("full_virtual_memory_test : could not allocate physical pages\n");
        return;
    }

    if (physical_page_count != NUMBER_OF_PHYSICAL_PAGES) {

        printf ("full_virtual_memory_test : allocated only %llu pages out of %u pages requested\n",
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

    virtual_address_size = 64 * physical_page_count * PAGE_SIZE;

    //
    // Round down to a PAGE_SIZE boundary.
    //

    virtual_address_size &= ~PAGE_SIZE;

    virtual_address_size_in_unsigned_chunks =
                        virtual_address_size / sizeof (ULONG_PTR);

    vmem_base = VirtualAlloc (NULL,
                      virtual_address_size,
                      MEM_RESERVE | MEM_PHYSICAL,
                      PAGE_READWRITE);

    if (vmem_base == NULL) {
        printf ("full_virtual_memory_test : could not reserve memory\n");
        return;
    }
    
    /**
     * Initialize pagetable and free frames
     */

    ULONG64 number_of_virtual_pages = virtual_address_size / PAGE_SIZE;

    PAGETABLE* pagetable = initialize_pagetable(number_of_virtual_pages, vmem_base);
    if (pagetable == NULL) {
        fprintf(stderr, "Unable to allocate memory for pagetable\n");
        return;
    }

    FREE_FRAMES_LISTS* free_frames = initialize_free_frames(physical_page_numbers, physical_page_count);
    if (free_frames == NULL) {
        fprintf(stderr, "Unable to allocate memory for free frames\n");
    }

    printf("VAS is %llX, PS is %X, num phys %llX, multiplied %llX\n", virtual_address_size, PAGE_SIZE, physical_page_count, PAGE_SIZE * physical_page_count);


    //
    // Now perform random accesses.
    //

    srand (time (NULL));

    for (i = 0; i < MB (1); i += 1) {

        //
        // Randomly access different portions of the virtual address
        // space we obtained above.
        //
        // If we have never accessed the surrounding page size (4K)
        // portion, the operating system will receive a page fault
        // from the CPU and proceed to obtain a physical page and
        // install a PTE to map it - thus connecting the end-to-end
        // virtual address translation.  Then the operating system
        // will tell the CPU to repeat the instruction that accessed
        // the virtual address and this time, the CPU will see the
        // valid PTE and proceed to obtain the physical contents
        // (without faulting to the operating system again).
        //
        random_number = rand () * rand() * rand();

        random_number %= virtual_address_size_in_unsigned_chunks;
        // random_number %= virtual_address_size - sizeof(ULONG_PTR);

        //
        // Write the virtual address into each page.  If we need to
        // debug anything, we'll be able to see these in the pages.
        //

        page_faulted = FALSE;

        arbitrary_va = vmem_base + random_number;

        __try {

            *arbitrary_va = (ULONG_PTR) arbitrary_va;

        } __except (EXCEPTION_EXECUTE_HANDLER) {

            page_faulted = TRUE;
        }

        if (page_faulted) {
            //
            // Connect the virtual address now - if that succeeds then
            // we'll be able to access it from now on.
            //
            // THIS IS JUST REUSING THE SAME PHYSICAL PAGE OVER AND OVER !
            //
            // IT NEEDS TO BE REPLACED WITH A TRUE MEMORY MANAGEMENT
            // STATE MACHINE !
            //

            // TODO: Look at the PTE for this va to make sure we are doing the right thing
            // Might be trimmed, might be first access, etc...

            // GET FROM FREE LIST
            // CHECK FOR FAILURE --> OUT OF FREE FRAMES


            // BW: Need to be able to accurately translate between VA and pagenumber!
            // Adjust PTE to reflect its allocated page
            PTE* accessed_pte = va_to_pte(pagetable, arbitrary_va);

            if (accessed_pte == NULL) {
                fprintf(stderr, "Unable to find the accessed PTE\n");
                return;
            }

            if (accessed_pte->memory_format.valid == VALID) {
                printf("Accessing already valid pte\n");
                // Skip to the next random access, another thread has already validated this address
                continue;
            }

            // BW: Check the pte to see if it is in the standby or modified to see if it can be rescued
            // Then, check free frames

            PAGE* new_page = allocate_free_frame(free_frames);
            // Then, check standby
            ULONG64 pfn;
            if (new_page == NULL) {
                // printf("Stealing frame...\n");
                pfn = steal_lowest_frame(pagetable);
                // printf("Stolen pfn: %llX\n", pfn);
            } else {
                pfn = new_page->free_page.frame_number;
                free(new_page);
            }

            accessed_pte->format_id = VALID_FORMAT;
            accessed_pte->memory_format.age = 0;
            accessed_pte->memory_format.frame_number = pfn;
            accessed_pte->memory_format.valid = VALID;


            // Allocate the physical frame to the virtual address
            if (MapUserPhysicalPages (arbitrary_va, 1, &pfn) == FALSE) {

                printf ("full_virtual_memory_test : could not map VA %p to page %llX\n", arbitrary_va, pfn);

                return;
            }

            //
            // No exception handler needed now since we have connected
            // the virtual address above to one of our physical pages
            // so no subsequent fault can occur.
            //

            *arbitrary_va = (ULONG_PTR) arbitrary_va;

            #if 0
            //
            // Unmap the virtual address translation we installed above
            // now that we're done writing our value into it.
            //

            // 
            if (MapUserPhysicalPages (arbitrary_va, 1, NULL) == FALSE) {

                printf ("full_virtual_memory_test : could not unmap VA %p\n", arbitrary_va);

                return;
            }
            #endif

        }
    }

    printf ("full_virtual_memory_test : finished accessing %u random virtual addresses\n", i);

    //
    // Now that we're done with our memory we can be a good
    // citizen and free it.
    //

    VirtualFree (vmem_base, 0, MEM_RELEASE);

    return;
}

VOID 
main (
    int argc,
    char** argv
    )
{

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

    full_virtual_memory_test ();

    return;
}
