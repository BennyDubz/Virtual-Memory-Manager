/**
 * @author Ben Williams
 * @date June 19th, 2024
 */

#include "../pagetable.h"

typedef struct {
    ULONG64 status:2;
    PTE* pte;
    /**
     * Whether the page has been cleaned out before it was freed by the previous VA using it
     * 
     * We need to zero out pages that are going to a different process than the one it was at before
     */
    ULONG64 zeroed_out:1; 

} FREE_PAGE;

typedef struct {
    ULONG64 status:2;
    PTE* pte;
} MODIFIED_PAGE;

typedef struct {
    ULONG64 status:2;
    PTE* pte;
    
} STANDBY_PAGE;

typedef struct {
    PTE* pte;
    // PTE
    // Need info for pagefile

} PAGE;