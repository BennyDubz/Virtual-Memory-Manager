/**
 * @author Ben Williams
 * @date June 18th, 2024
 * 
 * Implementation of a simple doubly linked list
 */

#include <stdio.h>
#include <assert.h>
#include "./db_linked_list.h"
#include "../macros.h" 



/**
 * Allocates memory for and initializes a db node with the given item
 * 
 * Returns a pointer to the node, or NULL upon error
 */
DB_LL_NODE* db_create_node(void* item) {
    DB_LL_NODE* new_node = malloc(sizeof(DB_LL_NODE));

    if (new_node == NULL) {
        fprintf(stderr, "Unable to allocate memory for new db node\n");
        return NULL;
    }

    new_node->item = item;
    
    return new_node;
}


/**
 * Returns a memory allocated pointer to the listhead of a doubly linked list
 * 
 * Returns NULL in case of error
 */
DB_LL_NODE* db_create_list() {

    DB_LL_NODE* listhead = db_create_node(NULL);

    if (listhead == NULL) return NULL;

    listhead->blink = listhead;
    listhead->flink = listhead; 
    listhead->item = NULL;

    #if DEBUG_LISTS
    listhead->listhead_ptr = listhead;
    listhead->prev_listhead = NULL;
    #endif

    return listhead;
}


/**
 * Inserts the item at the head of the doubly linked list
 * 
 * Returns the new listnode that was inserted upon success, NULL otherwise
 */
DB_LL_NODE* db_insert_at_head(DB_LL_NODE* listhead, void* item) {
    if (listhead == NULL) {
        fprintf(stderr, "NULL listhead given to db_insert_at_head\n");
        return NULL;
    }

    DB_LL_NODE* new_node = db_create_node(item);

    if (new_node == NULL) return NULL;

    DB_LL_NODE* front_node = listhead->flink;

    listhead->flink = new_node;
    front_node->blink = new_node;

    new_node->blink = listhead;
    new_node->flink = front_node;

    #if DEBUG_LISTS
    new_node->listhead_ptr = listhead;
    #endif

    return new_node;
}


/**
 * Inserts the item at the tail of the doubly linked list
 * 
 * Returns the new listnode that was inserted upon success, NULL otherwise
 */
DB_LL_NODE* db_insert_at_tail(DB_LL_NODE* listhead, void* item) {
    if (listhead == NULL) {
        fprintf(stderr, "NULL listhead given in db_insert_at_tail\n");
        return NULL;
    } 

    DB_LL_NODE* new_node = db_create_node(item);

    if (new_node == NULL) return NULL;

    DB_LL_NODE* backnode = listhead->blink;

    listhead->blink = new_node;
    backnode->flink = new_node;

    new_node->blink = backnode;
    new_node->flink = listhead;

    #if DEBUG_LISTS
    new_node->listhead_ptr = listhead;
    #endif

    return new_node;
}


/**
 * Adds the given node at the head, allows for conservation of listnodes across lists
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 * 
 */
int db_insert_node_at_head(DB_LL_NODE* listhead, DB_LL_NODE* node) {
    if (listhead == NULL || node == NULL) {
        fprintf(stderr, "NULL listhead or node given to db_insert_at_head\n");
        return ERROR;
    }

    DB_LL_NODE* old_head = listhead->flink;
    
    old_head->blink = node;
    node->flink = old_head;
    node->blink = listhead;
    listhead->flink = node;

    #if DEBUG_LISTS
    node->listhead_ptr = listhead;
    #endif

    return SUCCESS;
}


/**
 * Adds the given node at the tail, allows for conservation of listnodes across lists
 * 
 * Returns SUCCESS if there are no issues, ERROR otherwise
 * 
 */
int db_insert_node_at_tail(DB_LL_NODE* listhead, DB_LL_NODE* node) {
    if (listhead == NULL || node == NULL) {
        fprintf(stderr, "NULL listhead or node given to db_insert_at_tail\n");
        return ERROR;
    }

    DB_LL_NODE* old_tail = listhead->blink;

    node->flink = listhead;
    node->blink = old_tail;
    
    listhead->blink = node;
    old_tail->flink = node;

    #if DEBUG_LISTS
    node->listhead_ptr = listhead;
    #endif

    return SUCCESS;
}


/**
 * Removes and returns the item at the head of the doubly linked list
 * 
 * Returns NULL if the list is empty or if it does not exist
 */
void* db_pop_from_head(DB_LL_NODE* listhead) {
    if (listhead == NULL) {
        fprintf(stderr, "NULL list given to db_pop_from_head\n");
        return NULL;
    }

    // Empty list
    if (listhead->flink == listhead) return NULL;

    DB_LL_NODE* head_node = listhead->flink;

    listhead->flink = head_node->flink;
    head_node->flink->blink = listhead;

    head_node->flink = NULL;
    head_node->blink = NULL;

    #if DEBUG_LISTS
    head_node->prev_listhead = head_node->listhead_ptr;
    head_node->listhead_ptr = NULL;
    #endif

    void* item = head_node->item;

    return item;
}

/**
 * Removes and returns the item at the tail of the doubly linked list
 * 
 * Returns NULL if the list is empty or if it does not exist
 */
void* db_pop_from_tail(DB_LL_NODE* listhead) {
     if (listhead == NULL) {
        fprintf(stderr, "NULL list given to db_pop_from_tail\n");
        return NULL;
    }

    // Empty list
    if (listhead->flink == listhead) return NULL;

    DB_LL_NODE* tail_node = listhead->blink;

    listhead->blink = tail_node->blink;
    tail_node->blink->flink = listhead;

    tail_node->flink = NULL;
    tail_node->blink = NULL;

    #if DEBUG_LISTS
    tail_node->prev_listhead = tail_node->listhead_ptr;
    tail_node->listhead_ptr = NULL;
    #endif

    void* item = tail_node->item;

    // free(tail_node);
    return item;
}

/**
 * Removes the given node from the middle of its list, and returns the item
 * 
 * Returns NULL upon error
 */
void* db_remove_from_middle(DB_LL_NODE* listhead, DB_LL_NODE* middle_node) {
    if (middle_node == NULL) {
        fprintf(stderr, "NULL node given to db_remove_from_middle\n");
        return NULL;
    }

    if (middle_node->flink == middle_node) {
        fprintf(stderr, "Trying to remove listhead from its list\n");
        return NULL;
    }

    DB_LL_NODE* prev = middle_node->blink;
    DB_LL_NODE* next = middle_node->flink;

    assert(prev != middle_node);
    assert(next != middle_node);
    assert(listhead != middle_node);

    middle_node->flink = NULL;
    middle_node->blink = NULL;
    
    prev->flink = next;
    next->blink = prev;

    #if DEBUG_LISTS
    middle_node->prev_listhead =  middle_node->listhead_ptr;
    middle_node->listhead_ptr = NULL;
    #endif

    void* item = middle_node->item;
    
    // free(middle_node);
    return item;
}

/**
 * Removes the entire section from the list. Assumes all nodes between
 * the beginning and end node are in the list
 */
void db_remove_section(DB_LL_NODE* beginning, DB_LL_NODE* end) {
    if (beginning == NULL || end == NULL) {
        fprintf(stderr, "NULL parameters given to db_remove_section\n");
        DebugBreak();
    }

    DB_LL_NODE* front_connection = beginning->blink;
    DB_LL_NODE* back_connection = end->flink;

    front_connection->flink = back_connection;
    back_connection->blink = front_connection;

    #if DEBUG_LISTS
    DB_LL_NODE* curr_node = beginning;

    while (curr_node != end->flink) {
        curr_node->listhead_ptr = NULL;

        curr_node = curr_node->flink;
    }
    #endif
}