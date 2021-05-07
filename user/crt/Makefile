
export CFGDIR:=$(abspath ../..)
include $(CFGDIR)/config.mk

IDL_FILES:=$(subst $(CFGDIR)/idl/api/,,$(wildcard $(CFGDIR)/idl/api/*.idl))
IDL_OBJS:=$(patsubst %.idl,%-common.o,$(IDL_FILES)) \
	$(patsubst %.idl,%-client.o,$(IDL_FILES))

# imports from $(CFGDIR)/lib
LIB_OBJS=bsearch.o dlmalloc.o hash.o pidof.o sort.o rbtree.o string.o \
	strscpy.o strtol.o vsnprintf.o env.o stdio_portable.o setjmp.o \
	$(IDL_OBJS)

CRT_SRC:=$(wildcard *.c)
CRT_ASM:=$(wildcard *-32.S)
CRT_OBJS:=crt0.o $(CRT_SRC:.c=.o) $(CRT_ASM:-32.S=.o) \
	$(patsubst %,$(CFGDIR)/lib/%,$(LIB_OBJS)) \
	ccan-intmap.o

CFLAGS:=$(subst -D__KERNEL__,,$(CFLAGS)) -D__SNEKS__=1 -mstackrealign

# while the userspace runtime bits will eventually be suitable for linking
# out-of-tree code, this isn't true quite yet; and much of it will depend on
# the sneks utility library which isn't namespace clean.
all: libsneks-user-crt.a


clean:
	@rm -f *.o *.a $(CLEAN_PATS)


distclean:
	@rm -rf .deps


libsneks-user-crt.a: $(CRT_OBJS)
	@echo "  AR $@"
	@ar cr $@ $^
	@echo "  RANLIB $@"
	@ranlib $@


# flipping CCAN intmap insists on setting errno willy nilly, and it flapping
# shouldn't. flip this spot, man, flip it to heck. goddamn friendly piece of
# ripened snot from stan's peehole. funk!
ccan-intmap.o:: CFLAGS += -Derrno="*(&(int){ 0 })" -DCCAN_INTMAP_WAS_BORN_TOO_EARLY_NOT_TO_BE_A_PIECE_OF_FRIENDLY_SNOT=1


# build things from mung as guests.
#
# NOTE: this is here rather than config.mk because imports of these things
# should happen into sys/crt and user/crt, and then nowhere else; so having the
# rule at toplevel is wrong.
mung-%.o ::
	@echo "  CC $@ <mung>"
	@$(CC) -c -o $@ $(wildcard $(MUNG_DIR)/$*.c) $(wildcard $(MUNG_DIR)/lib/$*.c) \
		$(CFLAGS) -nostartfiles -nodefaultlibs


include $(wildcard .deps/*.d)
