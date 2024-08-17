/**
 * @author Ben Williams
 * @date July 8th, 2024
 * 
 * Custom synchronization lock creation and others
 *
 * Used to help make performance analysis on lock contention more clear
 */

#include <windows.h>
#include "../macros.h"

void initialize_lock(CRITICAL_SECTION* critsec) {
    InitializeCriticalSection(critsec);

    #ifdef LOCK_SPINNING
    SetCriticalSectionSpinCount(critsec, MB(16) - 1);
    #endif
}

