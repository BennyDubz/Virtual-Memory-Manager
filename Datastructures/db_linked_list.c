/**
 * @author Ben Williams
 * @date June 18th, 2024
 * 
 * Implementation of a simple doubly linked list
 */

#include <stdio.h>
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

    node->flink = old_head;
    node->blink = listhead;
    
    listhead->flink = node;
    old_head->blink = node;

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

    void* item = head_node->item;

    free(head_node);

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
    void* item = tail_node->item;

    free(tail_node);
    return item;
}

/**
 * Removes the given node from the middle of its list, and returns the item
 * 
 * Returns NULL upon error
 */
void* db_remove_from_middle(DB_LL_NODE* middle_node) {
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
    
    prev->flink = next;
    next->blink = prev;

    void* item = middle_node->item;
    
    free(middle_node);
    return item;
}