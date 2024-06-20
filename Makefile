# Makefile for memory management simulation
# Ben Williams
# June 19th, 2024

CC=cl
CFLAGS=/Zi /EHsc /I.

# Header file dependencies
DEPS = Datastructures/pagelists.h Datastructures/pagetable.h Datastructures/db_linked_list.h \
       hardware.h macros.h

# Object files to compile
OBJ = Datastructures/pagelists.obj Datastructures/db_linked_list.obj Datastructures/pagetable.obj \
      vm1.obj

# Default target
vm.exe: $(OBJ)
	$(CC) $(CFLAGS) /Fevm.exe /Fo:. $^

# Compilation rules for individual files
Datastructures/pagelists.obj: Datastructures/pagelists.c $(DEPS)
	$(CC) $(CFLAGS) /c /Fo:$@ $<

Datastructures/db_linked_list.obj: Datastructures/db_linked_list.c $(DEPS)
	$(CC) $(CFLAGS) /c /Fo:$@ $<

Datastructures/pagetable.obj: Datastructures/pagetable.c $(DEPS)
	$(CC) $(CFLAGS) /c /Fo:$@ $<

vm1.obj: vm1.c $(DEPS)
	$(CC) $(CFLAGS) /c /Fo:$@ $<

.PHONY: clean

clean:
	del *.exe *.obj *.pdb
