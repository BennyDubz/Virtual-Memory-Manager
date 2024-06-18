/**
 * @author Ben Williams
 * @date June 18th, 2024
 * 
 * Testing for the doubly linked list implementation
 */

#define NUM_ITEMS 5

#include <stdio.h>
#include <assert.h>
#include "../../macros.h"
#include "../db_linked_list.h"

int main(int argc, char** argv) {

    int* items[NUM_ITEMS];
    
    for (int i = 0; i < NUM_ITEMS; i++) {
        int* new_item = (int*) malloc(sizeof(int));
        *new_item = i;
        items[i] = new_item;
    }

    db_linked_list_t* list = create_db_list(items[0]);

    // Head should be zero 
    assert (*(int*) (list->head->item) == 0);    

    // Tail should be zero
    assert (*(int*) (list->tail->item) == 0);   

    // List should wrap around
    assert (*(int*) (list->head->flink->item) == 0);    
    assert (*(int*) (list->tail->blink->item) == 0);  


    // Adding items to the head and 
    for (int i = 1; i < NUM_ITEMS; i++) {
        assert(db_insert_at_head(list, items[i]) == SUCCESS);
    }  

    assert(list->length == NUM_ITEMS);

    printf("Iterating through the list forwards, twice:\n");

    db_node_t* curr_node = list->head;
    for (int i = 0; i < NUM_ITEMS * 2; i++) {
        assert(*(int*) curr_node->item % NUM_ITEMS == i % NUM_ITEMS);
        curr_node = curr_node->flink;
    }
    
    printf("Iterating through the list backwards, twice:\n");
    db_node_t* curr_node = list->tail;
    for (int i = 9; i >= 0; i--) {
        assert(*(int*) curr_node->item % NUM_ITEMS == i % NUM_ITEMS);
        curr_node = curr_node->blink;
    }

    printf("Popping from the list until it is empty\n");
    for (int i = 4; i >= 0; i--) {
        void* item = db_pop_from_tail(list);
        assert(*(int*) item == i);
    }

    assert(list->length == 0);
    assert(list->head == NULL);
    assert(list->tail == NULL);

    return 0;
}
