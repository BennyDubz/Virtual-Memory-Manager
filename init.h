/**
 * @author Ben Williams 
 * @date June 28th, 2024
 * 
 * Contains everything related to initializing all of the global variables, datastructures, and
 * the simulation
 */


/**
 * Initializes the simulation, all memory management datastructures, and initializes all threads
 * 
 * Stores the base address of virtual memory and the total amount of usable virtual memory vmem_base_storage
 * and virtual_memory_size_storage respectively.
 * 
 * Takes the number of usermode threads that we will use as a parameter. This allows us to initialize all of the thread local storage
 * that the threads can take advantage of
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 */
int init_all(PULONG_PTR* vmem_base_storage, ULONG64* virtual_memory_size_storage, ULONG64 num_usermode_threads);
