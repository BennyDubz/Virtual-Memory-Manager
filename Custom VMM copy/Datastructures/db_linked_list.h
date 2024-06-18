/**
 * @author Ben Williams
 * @date June 18th, 2024
 * 
 * Implementation of a simple doubly linked list
 */

#include <stdlib.h>
#include <windows.h>
#include "../macros.h"

#ifndef DBLIST
#define DBLIST
typedef struct db_node {
    void* item;
    db_node_t* flink;
    db_node_t* blink;
} db_node_t;

typedef struct db_linked_list {
    db_node_t* head;
    db_node_t* tail;
    ULONG64 length;
} db_linked_list_t;
#endif

/**
 * Returns a memory allocated pointer to a doubly linked list, with the given item
 * in the head & tail node
 * 
 * Returns NULL in case of error
 */
db_linked_list_t* create_db_list(void* first_item);


/**
 * Inserts the item at the head of the doubly linked list
 * 
 * Returns SUCCESS if the memory was allocated properly and the item was inserted, ERROR otherwise
 */
int db_insert_at_head(db_linked_list_t* list, void* item);


/**
 * Inserts the item at the tail of the doubly linked list
 * 
 * Returns SUCCESS if the memory was allocated properly and the item was inserted, ERROR otherwise
 */
int db_insert_at_tail(db_linked_list_t* list, void* item);


/**
 * Removes and returns the item at the head of the doubly linked list
 * 
 * Returns NULL if the list is empty or if it does not exist
 */
void* db_pop_from_head(db_linked_list_t* list);

/**
 * Removes and returns the item at the tail of the doubly linked list
 * 
 * Returns NULL if the list is empty or if it does not exist
 */
void* db_pop_from_tail(db_linked_list_t* list);

