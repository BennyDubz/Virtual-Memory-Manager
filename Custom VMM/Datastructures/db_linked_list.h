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
DB_LL_NODE* create_db_list();


/**
 * Inserts the item at the head of the doubly linked list
 * 
 * Returns SUCCESS if the memory was allocated properly and the item was inserted, ERROR otherwise
 */
int db_insert_at_head(DB_LL_NODE* listhead, void* item);


/**
 * Inserts the item at the tail of the doubly linked list
 * 
 * Returns SUCCESS if the memory was allocated properly and the item was inserted, ERROR otherwise
 */
int db_insert_at_tail(DB_LL_NODE* listhead, void* item);


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

