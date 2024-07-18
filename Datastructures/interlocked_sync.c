/**
 * Examples of creating locks using interlocked operations
 */

#include <windows.h>

long num_modified_pages;
void trimmer() {

    /**
     * Value is the new value of num_modified_pages
     * 
     * Increments in a single indivisible block
     */
    long value = InterlockedIncrement(&num_modified_pages);

    if (value == 1) {
        /**
         * We have just put the first page on
         * 
         * 
         */
    }

    long old_val = 0;
    long new_val = 1;

    old_val = num_modified_pages;

    /**
     * First parameter is our "lock field", or the one that we want to change
     * 
     * 
     */
    long real_old_value = InterlockedCompareExchange(&num_modified_pages, new_val, old_val);

    if (real_old_value == old_val) {
        /**
         * Succeeded
         */
    } else {
        old_val = real_old_value;
        new_val++;
        /**
         * Go to line 38, we failed and need to try again
         */
    }

    // Decrements in a single indivisible block
    InterlockedDecrement(&num_modified_pages);

}


void acquire_lock(long* lock) {
    /**
     * 1 means the lock is owned
     */
    long new_val = 1;

    /**
     * 0 means the lock is not owned
     */
    long old_val = 0;

    /**
     * First parameter is our "lock field", or the one that we want to change
     * 
     * Second parameter is the value that we want to change the "lock field" to (the "new value")
     * 
     * Third parameter is what the current value of the "lock field" must be in order to change the value. 
     * Otherwise, we fail and do not change the value at all
     * 
     * This is a CPU level instruction that keeps the "lock field" exclusive in the cache during that single instruction
     * 
     * This means that the given address cannot straddle two cache lines - as the CPU could end in deadlock
     */
    while (TRUE) {
        long real_old_value = InterlockedCompareExchange(lock, new_val, old_val);

        if (real_old_value == 0) {
            /**
             * Succeeded, it was not owned, but now it is set to 1 and we own it
             */
            break;
        }

        /**
         * We have failed, we have to keep trying until we can set it to 1 at 87
         */
    }
    
}

typedef struct {
    /**
     * This cannot be modified by anything except an Interlocked instruction
     */
    long lock_field;
    long max_spincount;
    HANDLE event_structure;
} HEAVY_LOCK;

void heavy_acquire_lock(HEAVY_LOCK* lock) {
    /**
     * 1 means the lock is owned
     */
    long new_val = 1;

    /**
     * 0 means the lock is not owned
     */
    long old_val = 0;

    /**
     * The number of times that we have spun so far trying to acquire the lock
     */
    long spin_count = 0;

    /**
     * First parameter is our "lock field", or the one that we want to change
     * 
     * Second parameter is the value that we want to change the "lock field" to (the "new value")
     * 
     * Third parameter is what the current value of the "lock field" must be in order to change the value. 
     * Otherwise, we fail and do not change the value at all
     */
    while (TRUE) {
        long real_old_value = InterlockedCompareExchange(lock, new_val, old_val);

        if (real_old_value == 0) {
            /**
             * Succeeded, it was not owned, but now it is set to 1 and we own it
             */
            break;
        }

        /**
         * We have failed, we will increment the spin count
         */
        spin_count++;


        /**
         * If we have spun the maximum amount allowed, we put the thread to sleep and relinquish the CPU until
         * we are signaled that the lock is free. We don't want to waste too much CPU time trying to acquire the lock
         * and failing
         */
        if (spin_count == lock->max_spincount) {
            WaitForSingleObject(lock->event_structure, INFINITE);
            spin_count = 0;
        }

        /**
         * Try again to acquire lock
         */
    }
    
}