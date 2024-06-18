/**
 * @author Ben Williams
 * @date June 18th, 2024
 * 
 * Testing for the doubly linked list implementation
 */

#define NUM_ITEMS 5

#include <stdio.h>
#include <assert.h>
#include <stdio.h>
#include "../../macros.h"
#include "../db_linked_list.h"

int main(int argc, char** argv) {

    int* items[NUM_ITEMS];
    
    for (int i = 0; i < NUM_ITEMS; i++) {
        int* new_item = (int*) malloc(sizeof(int));
        *new_item = i;
        items[i] = new_item;
    }

    DB_LL_NODE* listhead = create_db_list();  


    // Adding items to the head and 
    for (int i = 0; i < NUM_ITEMS; i++) {
        assert(db_insert_at_tail(listhead, items[i]) == SUCCESS);
    }  



    printf("Iterating through the list forwards:\n");

    DB_LL_NODE* curr_node = listhead->flink;
    for (int i = 0; i < NUM_ITEMS; i++) {
        printf("\tLooking at item: %d\n", *(int*) curr_node->item);
        assert(*(int*) curr_node->item == i);
        curr_node = curr_node->flink;
    }
    
    printf("Iterating through the list backwards:\n");
    curr_node = listhead->blink;
    for (int i = NUM_ITEMS - 1; i >= 0; i--) {
        printf("\tLooking at item: %d\n", *(int*) curr_node->item);
        assert(*(int*) curr_node->item == i);
        curr_node = curr_node->blink;
    }

    printf("Popping from the list until it is empty\n");
    for (int i = NUM_ITEMS - 1; i >= 0; i--) {
        void* item = db_pop_from_tail(listhead);
        printf("\tLooking at item: %d\n", *(int*) item);
        assert(*(int*) item == i);
    }

    assert(db_pop_from_head(listhead) == NULL);
    assert(db_pop_from_tail(listhead) == NULL);

    return 0;
}
