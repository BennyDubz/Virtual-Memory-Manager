/**
 * @author Ben Williams
 * @date June 18th, 2024
 * 
 * Header for free list datastructure and functions
 * 
 */

#include "./db_linked_list.h"

/**
 * Note: Not sure if we need a free_frames_list type if we only keep one free frames list.
 * 
 * However, if we keep more than one free frames list (so that multiple threads can use with less contention)
 * then it might be helpful.
 */
typedef struct free_frames_list {
    db_linked_list_t* list;
    // LOCK (idk windows implementation)
} free_frames_list_t;


typedef struct free_frame {
    ULONG64 physical_address;
    // Other information?
} free_frame_t;

// Define a few db linked list function wrappers that help implement the free list functionality
