/**
 * @author Ben Williams
 * @date June 18th, 2024
 * 
 * Implementation of a simple doubly linked list
 */

#include <stdlib.h>
#include <windows.h>

#define DEBUG_LISTS 0

#ifndef DBLIST
#define DBLIST

typedef struct NODE {
    struct NODE* flink;
    struct NODE* blink;
    #if DEBUG_LISTS
    struct NODE* listhead_ptr;
    struct NODE* prev_listhead;
    #endif
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
 * Allocates memory for and initializes a db node with the given item
 * 
 * Returns a pointer to the node, or NULL upon error
 */
DB_LL_NODE* db_create_node(void* item);


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
 * Adds the given node at the head, allows for conservation of listnodes across lists
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 * 
 */
int db_insert_node_at_head(DB_LL_NODE* listhead, DB_LL_NODE* node);


/**
 * Adds the given node at the tail, allows for conservation of listnodes across lists
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 * 
 */
int db_insert_node_at_tail(DB_LL_NODE* listhead, DB_LL_NODE* node);


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
void* db_remove_from_middle(DB_LL_NODE* listhead, DB_LL_NODE* middle_node);


/**
 * Removes the entire section from the list. Assumes all nodes between
 * the beginning and end node are in the list
 */
void db_remove_section(DB_LL_NODE* beginning, DB_LL_NODE* end);


/**
 * Adds the section with the two nodes at its beginning and end to the tail of the list
 * 
 * The end node will be the new tail node of the list
 */
void db_insert_section_at_tail(DB_LL_NODE* listhead, DB_LL_NODE* beginning, DB_LL_NODE* end);


/**
 * Adds the section with the two nodes at its beginning and end to the tail of the list
 * 
 * The beginning node will be the new head node of the list
 */
void db_insert_section_at_head(DB_LL_NODE* listhead, DB_LL_NODE* beginning, DB_LL_NODE* end);