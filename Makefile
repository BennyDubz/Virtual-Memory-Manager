CC=cl
CFLAGS=/EHsc /I.

DEPS = Datastructures/freelist.h Datastructures/modifiedlist.h Datastructures/page.h Datastructures/standbylist.h \
       Testing/db_linked_list.h Testing/pagelists.h Testing/pagetable.h \
       hardware.h macros.h

OBJ = Datastructures/freelist.c Datastructures/modifiedlist.c Datastructures/page.c Datastructures/standbylist.c \
      Testing/db_linked_list.c Testing/pagelists.c Testing/pagetable.c \
      vm1.c

vm.exe: $(OBJ)
	$(CC) $(CFLAGS) /Fe$@ $^ 

.PHONY: clean

clean:
	del *.exe *.obj
