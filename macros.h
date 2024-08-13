/**
 * All macros for the code of this project
 */


/**
 * FUNCTION MACROS
 */
#define ERROR 0

#define SUCCESS 1


/**
 * This allows the simulation to differentiate between read and write accesses. Typically,
 * the CPU would tell us what type of access it was - but we need to do this manually for the
 * simulation.
 * 
 * This allows us to see if we need to free pagefile space when trimming pages, or if we can
 * put them straight onto standby
 * 
 * Note that we are not considering execute permissions in the simulation
 */
#define READ_ACCESS 0
#define WRITE_ACCESS 1


/**
 * CONVERSION MACROS
 */
#define KB(x) (((ULONG64) x) * 1024)

#define MB(x) (((ULONG64) x) * KB(1024))

#define GB(x) (((ULONG64) x) * MB(1024))

