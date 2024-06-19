# Author: Ben Williams
# Date: June 19th, 2024

OBJS = db_linked_list.obj pagetable.obj
LIB = vm_datastructures.a

$(LIB) : $(OBJS)
	ar cr $(LIB) $^

db_linked_list.obj : db_linked_list.h
pagetable.obj : pagetable.h

.PHONY clean

clean:
	Remove-Item *.obj
	Remove-Item *.exe
	