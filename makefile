PROJNAME = tsh
VIEWER = less

IDIR = include
ODIR = build
SRCDIR = src

DEPF = inputlib.h dynamic_array.h linked_list.h red_black_tree.h set.h stack.h str.h utils.h
DEPS = $(patsubst %,$(IDIR)/%,$(DEPF))

OBJF = inputlib.o dynamic_array.o linked_list.o red_black_tree.o stack.o str.o main.o
OBJ = $(patsubst %,$(ODIR)/%,$(OBJF))

DRIVER = $(SRCDIR)/main.c

CC = gcc
CCOPTS = -g -I$(IDIR)

.PHONY: all view clean
.DEFAULT: all

$(ODIR)/%.o: $(SRCDIR)/%.c $(DEPS)
	-@ $(CC) -c -o $@ $< $(CCOPTS)

all : $(OBJ)
	-@ $(CC) -o $(PROJNAME) $^ $(CCOPTS)

view :
	-@ $(VIEWER) $(DRIVER) $(DEPS)

clean :
	-@ \rm -f $(ODIR)/*.o $(SRCDIR)/*~ core $(INCDIR)/*~ $(PROJNAME)

