/**
 * @author Ben Williams
 * @date June 18th, 2024
 * 
 * Implementation of a simple doubly linked list
 */

#include <stdlib.h>
#include <windows.h>

#ifndef DBLIST
#define DBLIST

typedef struct NODE {
    struct NODE* flink;
    struct NODE* blink;
    void* item;
} DB_LL_NODE;

#endif

/**
 * Returns a memory allocated pointer to the head of a doubly linked list
 * 
 * Returns NULL in case of error
 */
DB_LL_NODE* db_create_list();


/**
 * Inserts the item at the head of the doubly linked list
 * 
 * Returns the new listnode that was inserted upon success, NULL otherwise
 */
DB_LL_NODE* db_insert_at_head(DB_LL_NODE* listhead, void* item);


/**
 * Inserts the item at the tail of the doubly linked list
 * 
 * Returns the new listnode that was inserted upon success, NULL otherwise
 */
DB_LL_NODE* db_insert_at_tail(DB_LL_NODE* listhead, void* item);


/**
 * Removes and returns the item at the head of the doubly linked list
 * 
 * Returns NULL if the list is empty or if it does not exist
 */
void* db_pop_from_head(DB_LL_NODE* listhead);


/**
 * Removes and returns the item at the tail of the doubly linked list
 * 
 * Returns NULL if the list is empty or if it does not exist
 */
void* db_pop_from_tail(DB_LL_NODE* listhead);


/**
 * Removes the given node from the middle of its list, and returns the item
 * 
 * Returns NULL upon error
 */
void* db_remove_from_middle(DB_LL_NODE* middle_node);