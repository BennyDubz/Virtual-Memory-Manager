# Makefile for datastructures
# Author: Ben Williams
# Date: June 19th, 2024

CC=cl
CFLAGS=/Zi /c /EHsc
OBJS = db_linked_list.obj pagetable.obj pagelists.obj
LIB = vm_datastructures.lib

# Rule to create the library
$(LIB) : $(OBJS)
	lib /OUT:$(LIB) $^

# Compilation rules for each object file
db_linked_list.obj : db_linked_list.c db_linked_list.h
	$(CC) $(CFLAGS) /Fo$@ db_linked_list.c

pagetable.obj : pagetable.c pagetable.h
	$(CC) $(CFLAGS) /Fo$@ pagetable.c

pagelists.obj : pagelists.c pagelists.h
	$(CC) $(CFLAGS) /Fo$@ pagelists.c

.PHONY: clean

clean:
	del *.obj *.lib *.exe
