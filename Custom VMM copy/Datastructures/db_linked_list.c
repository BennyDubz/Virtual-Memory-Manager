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
static db_node_t* create_db_node(void* item) {
    db_node_t* new_node = malloc(sizeof(db_node_t));

    if (new_node == NULL) {
        fprintf(stderr, "Unable to allocate memory for new db node\n");
        return NULL;
    }

    new_node->item = item;
    
    return new_node;
}

/**
 * Creates and returns a new db node who's blink/flink pointers point to itself
 * 
 * Returns NULL in case of error
 */
static db_node_t* create_lone_node(void* item) {
    db_node_t* lone_node = create_db_node(item);
    if (lone_node == NULL) {
        return NULL;
    }

    lone_node->blink = lone_node;
    lone_node->flink = lone_node;

    return lone_node;
}

/**
 * Returns a memory allocated pointer to a doubly linked list, with the given item
 * in the head & tail node
 * 
 * Returns NULL in case of error
 */
db_linked_list_t* create_db_list(void* first_item) {
    db_linked_list_t* new_list = (db_linked_list_t*) malloc(sizeof(db_linked_list_t));

    if (new_list == NULL) {
        fprintf(stderr, "Unable to allocate memory to create doubly linked list\n");
        return NULL;
    }

    db_node_t* first_node = create_lone_node(first_item);
    if (first_node == NULL) return NULL;

    new_list->head = first_node;
    new_list->tail = first_node;
    new_list->length = 1;

    return new_list;
}


/**
 * Inserts the item at the head of the doubly linked list
 * 
 * Returns SUCCESS if the memory was allocated properly and the item was inserted, ERROR otherwise
 */
int db_insert_at_head(db_linked_list_t* list, void* item) {
    if (list == NULL) {
        fprintf(stderr, "Null list given in db_insert_at_head\n");
        return ERROR;
    } 

    db_node_t* old_head = list->head;

    // Edge case for empty list
    if (old_head == NULL) {
        db_node_t* new_node = create_lone_node(item);
        if (new_node == NULL) return ERROR;

        list->head = new_node;
        list->tail = new_node;
        list->length = 1;
        return SUCCESS;
    }


    db_node_t* new_node = create_db_node(item);

    if (new_node == NULL) return ERROR;

    // Modify the new node
    list->head = new_node;
    new_node->flink = old_head;
    new_node->blink = list->tail;

    // Correct the old head and tail
    list->tail->flink = new_node;
    old_head->blink = new_node;

    list->length += 1;

    return SUCCESS;
}


/**
 * Inserts the item at the tail of the doubly linked list
 * 
 * Returns SUCCESS if the memory was allocated properly and the item was inserted, ERROR otherwise
 */
int db_insert_at_tail(db_linked_list_t* list, void* item) {
    if (list == NULL) {
        fprintf(stderr, "Null list given in db_insert_at_tail\n");
        return ERROR;
    } 

    db_node_t* old_tail = list->tail;

    // Edge case for empty list
    if (old_tail == NULL) {
        db_node_t* new_node = create_lone_node(item);
        if (new_node == NULL) return ERROR;

        list->head = new_node;
        list->tail = new_node;
        list->length = 1;
        return SUCCESS;
    }


    db_node_t* new_node = create_db_node(item);

    if (new_node == NULL) return ERROR;


    // Modify the new node
    list->tail = new_node;
    new_node->flink = list->head;
    new_node->blink = old_tail;

    // Correct the rest of the list
    list->head->blink = new_node;
    old_tail->flink = new_node;

    list->length += 1;
    return SUCCESS;
}


/**
 * Removes and returns the item at the head of the doubly linked list
 * 
 * Returns NULL if the list is empty or if it does not exist
 */
void* db_pop_from_head(db_linked_list_t* list) {
    if (list == NULL) {
        fprintf(stderr, "Warning: NULL list given to db_pop_from_head");
        return NULL;
    }

    // Empty list
    if (list->head == NULL) return NULL;

    db_node_t* head_node = list->head;

    // Remove references to the current head from the list
    if (list->length == 1) {
        list->head = NULL;
        list->tail = NULL;
    } else { list->tail->flink = head_node->flink;
        list->head = head_node->flink;
        list->head->blink = list->tail;
    }

    void* item = head_node->item;

    free(head_node);
    list->length -= 1;
    return item;
}

/**
 * Removes and returns the item at the tail of the doubly linked list
 * 
 * Returns NULL if the list is empty or if it does not exist
 */
void* db_pop_from_tail(db_linked_list_t* list) {
     if (list == NULL) {
        fprintf(stderr, "Warning: NULL list given to db_pop_from_head");
        return NULL;
    }

    // Empty list
    if (list->tail == NULL) return NULL;

    db_node_t* tail_node = list->tail;

    // Remove references to the current tail from the list
    if (list->length == 1) {
        list->head = NULL;
        list->tail = NULL;
    } else {
        list->head->blink = tail_node->blink;
        list->tail = tail_node->blink;
        list->tail->flink = list->head;
    }

    void* item = tail_node->item;

    free(tail_node);
    list->length -= 1;
    return item;
}