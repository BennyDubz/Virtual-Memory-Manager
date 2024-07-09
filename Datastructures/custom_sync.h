/**
 * @author Ben Williams
 * @date July 8th, 2024
 * 
 * Custom synchronization lock creation and others
 *
 * Used to help make performance analysis on lock contention more clear
 */

#include <windows.h>

void initialize_lock(CRITICAL_SECTION* critsec);