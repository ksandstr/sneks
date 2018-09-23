
export CFGDIR:=$(abspath ../..)
include $(CFGDIR)/config.mk

IDL_FILES:=$(subst $(CFGDIR)/idl/api/,,$(wildcard $(CFGDIR)/idl/api/*.idl))
IDL_OBJS:=$(patsubst %.idl,%-common.o,$(IDL_FILES)) \
	$(patsubst %.idl,%-client.o,$(IDL_FILES))

# imports from $(CFGDIR)/lib
LIB_OBJS=bsearch.o dlmalloc.o hash.o pidof.o sort.o rbtree.o string.o \
	strtol.o vsnprintf.o

CRT_SRC:=$(wildcard *.c)
CRT_ASM:=$(wildcard *-32.S)
CRT_OBJS:=crt0.o $(IDL_OBJS) $(CRT_SRC:.c=.o) $(CRT_ASM:-32.S=.o) \
	$(patsubst %,$(CFGDIR)/lib/%,$(LIB_OBJS))

vpath %.idl $(CFGDIR)/idl/api

CFLAGS:=$(subst -D__KERNEL__,,$(CFLAGS)) -D__SNEKS__=1 -mstackrealign

# while the userspace runtime bits will eventually be suitable for linking
# out-of-tree code, this isn't true quite yet; and much of it will depend on
# the sneks utility library which isn't namespace clean.
all: libsneks-user-crt.a


clean:
	@rm -f *.o *.a $(CLEAN_PATS)


distclean:
	@rm -rf .deps


# IDL header dependencies (which should really get figured out automagically;
# c'est la guerre.)
process.o: process.c proc-defs.h
heap.o: heap.c vm-defs.h
stdio.o: stdio.c io-defs.h


libsneks-user-crt.a: $(CRT_OBJS)
	@echo "  AR $@"
	@ar cr $@ $^
	@echo "  RANLIB $@"
	@ranlib $@


include $(wildcard .deps/*.d)
