# Makefile for memory management simulation
# Ben Williams
# June 19th, 2024

CC=cl

# Custom flags
LOCK_SPINNING_FLAGS=-DLOCK_SPINNING=1
DEBUG_FLAGS=-DDEBUG_CHECKING=1
LARGE_SIM_FLAGS=-DLARGE_SIM=1
LENIENT_DISK_FLAGS=-DLENIENT_DISK=1

CFLAGS=/Zi /EHsc /I.

#### HEADERS ####
DATASTRUCTURES_H = Datastructures/pagelists.h Datastructures/pagetable.h Datastructures/db_linked_list.h \
       Datastructures/disk.h Datastructures/custom_sync.h

MACHINERY_H = Machinery/pagefault.h Machinery/trim.h Machinery/conversions.h Machinery/debug_checks.h \
		Machinery/disk_operations.h Machinery/pagelist_operations.h

OTHER_H = hardware.h macros.h globals.h init.h

# Combined header file dependencies
DEPS = $(DATASTRUCTURES_H) $(MACHINERY_H) $(OTHER_H)

#### OBJECTS ####
DATASTRUCTURES_O = Datastructures/pagelists.obj Datastructures/db_linked_list.obj Datastructures/pagetable.obj \
      	Datastructures/disk.obj Datastructures/custom_sync.obj

MACHINERY_O = Machinery/pagefault.obj Machinery/trim.obj Machinery/conversions.obj Machinery/debug_checks.obj \
		Machinery/disk_operations.obj Machinery/pagelist_operations.obj

OTHER_O = init.obj vm1.obj

# Object files to compile
OBJ = $(DATASTRUCTURES_O) $(MACHINERY_O) $(OTHER_O)

#### Default target ####
vm.exe: $(OBJ)
	$(CC) $(CFLAGS) /Fevm.exe /Fo:. $^

#### Custom targets ####
large: CFLAGS += $(LARGE_SIM_FLAGS)
large: vm.exe

lenient_disk: CFLAGS += $(LENIENT_DISK_FLAGS)
lenient_disk: vm.exe

large_lenient: CFLAGS += $(LARGE_SIM_FLAGS) $(LENIENT_DISK_FLAGS)
large_lenient: vm.exe

lock_spinning: CFLAGS += $(LOCK_SPINNING_FLAGS)
lock_spinning: vm.exe

lock_spinning_lenient: CFLAGS += $(LOCK_SPINNING_FLAGS) $(LENIENT_DISK_FLAGS)
lock_spinning_lenient: vm.exe

large_spinning_lenient: CFLAGS += $(LOCK_SPINNING_FLAGS) $(LENIENT_DISK_FLAGS) $(LARGE_SIM_FLAGS)
large_spinning_lenient: vm.exe

debug: CFLAGS += $(DEBUG_FLAGS)
debug: vm.exe

lock_spinning_debug: CFLAGS += $(LOCK_SPINNING_FLAGS) $(DEBUG_FLAGS)
lock_spinning_debug: vm.exe

#### Other Targets ####
.PHONY: clean

clean:
	del /f *.exe *.obj *.pdb
	del /f Datastructures\*.obj
	del /f Machinery\*.obj

#### Compilation rules for individual files ####
Datastructures/pagelists.obj: Datastructures/pagelists.c $(DEPS)
	$(CC) $(CFLAGS) /c /Fo:$@ $<

Datastructures/db_linked_list.obj: Datastructures/db_linked_list.c $(DEPS)
	$(CC) $(CFLAGS) /c /Fo:$@ $<

Datastructures/pagetable.obj: Datastructures/pagetable.c $(DEPS)
	$(CC) $(CFLAGS) /c /Fo:$@ $<

Datastructures/disk.obj: Datastructures/disk.c $(DEPS)
	$(CC) $(CFLAGS) /c /Fo:$@ $<

Datastructures/custom_sync.obj: Datastructures/custom_sync.c $(DEPS)
	$(CC) $(CFLAGS) /c /Fo:$@ $<

Machinery/pagefault.obj: Machinery/pagefault.c $(DEPS)
	$(CC) $(CFLAGS) /c /Fo:$@ $<

Machinery/trim.obj: Machinery/trim.c $(DEPS)
	$(CC) $(CFLAGS) /c /Fo:$@ $<

Machinery/debug_checks.obj: Machinery/debug_checks.c $(DEPS)
	$(CC) $(CFLAGS) /c /Fo:$@ $<

Machinery/conversions.obj: Machinery/conversions.c $(DEPS)
	$(CC) $(CFLAGS) /c /Fo:$@ $<

Machinery/disk_operations.obj: Machinery/disk_operations.c $(DEPS)
	$(CC) $(CFLAGS) /c /Fo:$@ $<

Machinery/pagelist_operations.obj: Machinery/pagelist_operations.c $(DEPS)
	$(CC) $(CFLAGS) /c /Fo:$@ $<

init.obj: init.c $(DEPS)
	$(CC) $(CFLAGS) /c /Fo:$@ $<

vm1.obj: vm1.c $(DEPS)
	$(CC) $(CFLAGS) /c /Fo:$@ $<
