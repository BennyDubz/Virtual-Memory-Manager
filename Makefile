# Makefile for memory management simulation
# Ben Williams
# June 19th, 2024

CC=cl
CFLAGS=/Zi /EHsc /I.


#### HEADERS ####
DATASTRUCTURES_H = Datastructures/pagelists.h Datastructures/pagetable.h Datastructures/db_linked_list.h \
       Datastructures/disk.h

MACHINERY_H = Machinery/pagefault.h Machinery/trim.h Machinery/conversions.h

OTHER_H = hardware.h macros.h globals.h

# Combined header file dependencies
DEPS = $(DATASTRUCTURES_H) $(MACHINERY_H) $(OTHER_H)

#### OBJECTS ####
DATASTRUCTURES_O = Datastructures/pagelists.obj Datastructures/db_linked_list.obj Datastructures/pagetable.obj \
      Datastructures/disk.obj

MACHINERY_O = Machinery/pagefault.obj Machinery/trim.obj Machinery/conversions.obj

OTHER_O = vm1.obj

# Object files to compile
OBJ = $(DATASTRUCTURES_O) $(MACHINERY_O) $(OTHER_O)

#### Default target ####
vm.exe: $(OBJ)
	$(CC) $(CFLAGS) /Fevm.exe /Fo:. $^


#### Other Targets ####
.PHONY: clean

clean:
	del /f *.exe *.obj *.pdb

#### Compilation rules for individual files ####
Datastructures/pagelists.obj: Datastructures/pagelists.c $(DEPS)
	$(CC) $(CFLAGS) /c /Fo:$@ $<

Datastructures/db_linked_list.obj: Datastructures/db_linked_list.c $(DEPS)
	$(CC) $(CFLAGS) /c /Fo:$@ $<

Datastructures/pagetable.obj: Datastructures/pagetable.c $(DEPS)
	$(CC) $(CFLAGS) /c /Fo:$@ $<

Datastructures/disk.obj: Datastructures/disk.c $(DEPS)
	$(CC) $(CFLAGS) /c /Fo:$@ $<

Machinery/pagefault.obj: Machinery/pagefault.c $(DEPS)
	$(CC) $(CFLAGS) /c /Fo:$@ $<

Machinery/trim.obj: Machinery/trim.c $(DEPS)
	$(CC) $(CFLAGS) /c /Fo:$@ $<

Machinery/conversions.obj: Machinery/conversions.c $(DEPS)
	$(CC) $(CFLAGS) /c /Fo:$@ $<

vm1.obj: vm1.c $(DEPS)
	$(CC) $(CFLAGS) /c /Fo:$@ $<

