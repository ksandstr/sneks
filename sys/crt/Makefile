
export CFGDIR:=$(abspath ../..)
include $(CFGDIR)/config.mk

IDL_FILES=sysmem.idl kmsg.idl info.idl proc.idl fs.idl msg.idl
IDL_OBJS:=$(patsubst %.idl,%-common.o,$(IDL_FILES)) \
	$(patsubst %.idl,%-client.o,$(IDL_FILES))

LIB_SRC:=$(wildcard *.c)
LIB_OBJS:=crt0.o $(IDL_OBJS) $(LIB_SRC:.c=.o) \
	mung-rangealloc.o mung-slab.o mung-memdesc.o \
	lfht-percpu.o lfht-epoch.o lfht-nbsl.o

vpath %.idl $(CFGDIR)/idl/api

CFLAGS:=$(subst -D__KERNEL__,,$(CFLAGS)) -mstackrealign

# the system task runtime library is, much like the utility library (which it
# depends on), unsuitable for use out of tree.
all: libsneks-systask.a


clean:
	@rm -f *.o *.a $(CLEAN_PATS)


distclean:
	@rm -rf .deps


libsneks-systask.a: $(LIB_OBJS)
	@echo "  AR $@"
	@ar cr $@ $^
	@echo "  RANLIB $@"
	@ranlib $@


crt1.o: sysmem-defs.h kmsg-defs.h info-defs.h
threads.o: proc-defs.h info-defs.h
fileio.o: fs-defs.h
misc.o: proc-defs.h
msg.o: msg-defs.h info-defs.h


# build things from mung as guests.
#
# NOTE: this is here rather than config.mk because imports of these things
# should happen into sys/crt and user/crt, and then nowhere else; so having the
# rule at toplevel is wrong.
mung-%.o ::
	@echo "  CC $@ <mung>"
	@$(CC) -c -o $@ $(wildcard $(MUNG_DIR)/$*.c) $(wildcard $(MUNG_DIR)/lib/$*.c) \
		$(CFLAGS) -nostartfiles -nodefaultlibs

# same for lfht.
lfht-%.o ::
	@echo "  CC $@ <lfht>"
	@$(CC) -c -o $@ $(LFHT_DIR)/$*.c \
		$(CFLAGS) -nostartfiles -nodefaultlibs


include $(wildcard .deps/*.d)
