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

    DB_LL_NODE* listhead = db_create_list();  

    // Adding items to the head and 
    for (int i = 0; i < NUM_ITEMS; i++) {
        assert(db_insert_at_tail(listhead, items[i]) != NULL);
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

    printf("Adding items back to the list and checking that the node returns\n");
    DB_LL_NODE* nodes[NUM_ITEMS];
    // Adding items to the tail 
    for (int i = 0; i < NUM_ITEMS; i++) {
        DB_LL_NODE* inserted_node = db_insert_at_tail(listhead, items[i]);
        nodes[i] = inserted_node;
        assert((int*)inserted_node->item == items[i]);
    }  

    // printf("Ensuring that the middle node can be removed and the list stays correct\n");
    // assert((int*)db_remove_from_middle(nodes[2]) == items[2]);

    printf("Should have removed %d, iterating forward and backward to ensure it isn't there\n", *items[2]);
    curr_node = listhead->flink;
    for (int i = 0; i < NUM_ITEMS; i++) {
        if (i == 2) continue;
        printf("\tLooking at item: %d\n", *(int*) curr_node->item);
        assert(*(int*) curr_node->item == i);
        curr_node = curr_node->flink;
    }
    
    printf("Iterating through the list backwards:\n");
    curr_node = listhead->blink;
    for (int i = NUM_ITEMS - 1; i >= 0; i--) {
        if (i == 2) continue;
        printf("\tLooking at item: %d\n", *(int*) curr_node->item);
        assert(*(int*) curr_node->item == i);
        curr_node = curr_node->blink;
    }

    return 0;
}
