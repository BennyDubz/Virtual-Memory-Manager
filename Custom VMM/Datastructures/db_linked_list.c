/**
 * @author Ben Williams
 * @date June 18th, 2024
 * 
 * Implementation of a simple doubly linked list
 */

#include "db_linked_list.h"
#include "stdio.h"


/**
 * Allocates memory for and initializes a db node with the given item
 * 
 * Used for insertion into the list
 */
static DB_LL_NODE* create_db_node() {
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
DB_LL_NODE* create_db_list() {

    DB_LL_NODE* listhead = create_db_node();

    if (listhead == NULL) return NULL;

    listhead->blink = listhead;
    listhead->flink = listhead; 

    return listhead;
}


/**
 * Inserts the item at the head of the doubly linked list
 * 
 * Returns SUCCESS if the memory was allocated properly and the item was inserted, ERROR otherwise
 */
int db_insert_at_head(DB_LL_NODE* listhead, void* item) {
    if (listhead == NULL) {
        fprintf(stderr, "NULL listhead given to db_insert_at_head\n");
        return ERROR;
    }

    DB_LL_NODE* new_node = create_db_node();

    if (new_node == NULL) return ERROR;

    DB_LL_NODE* front_node = listhead->flink;

    listhead->flink = new_node;
    front_node->blink = new_node;

    new_node->blink = listhead;
    new_node->flink = front_node;
    new_node->item = item;

    return SUCCESS;
}


/**
 * Inserts the item at the tail of the doubly linked list
 * 
 * Returns SUCCESS if the memory was allocated properly and the item was inserted, ERROR otherwise
 */
int db_insert_at_tail(DB_LL_NODE* listhead, void* item) {
    if (listhead == NULL) {
        fprintf(stderr, "NULL listhead given in db_insert_at_tail\n");
        return ERROR;
    } 

    DB_LL_NODE* new_node = create_db_node();

    if (new_node == NULL) return ERROR;

    DB_LL_NODE* backnode = listhead->blink;

    listhead->blink = new_node;
    backnode->flink = new_node;

    new_node->blink = backnode;
    new_node->flink = listhead;
    new_node->item = item;

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