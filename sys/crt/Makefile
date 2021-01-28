
export CFGDIR:=$(abspath ../..)
include $(CFGDIR)/config.mk

SYS_IDL:=$(subst $(CFGDIR)/idl/sys/,,$(wildcard $(CFGDIR)/idl/sys/*.idl))
API_IDL:=$(subst $(CFGDIR)/idl/api/,,$(wildcard $(CFGDIR)/idl/api/*.idl))
LIBSNEKS_OBJS:=$(API_IDL:.idl=-common.o) $(API_IDL:.idl=-client.o)
IDL_OBJS:=$(API_IDL:.idl=-service.o) $(SYS_IDL:.idl=-service.o) \
	$(SYS_IDL:.idl=-common.o) $(SYS_IDL:.idl=-client.o)

SYS_IDL_DEFS:=$(patsubst %.idl,$(CFGDIR)/include/sneks/sys/%-defs.h,$(SYS_IDL))

LIB_SRC:=$(wildcard *.c)
LIB_OBJS:=crt0.o $(foreach o,$(LIBSNEKS_OBJS),$(CFGDIR)/lib/$(o)) \
	$(IDL_OBJS) $(LIB_SRC:.c=.o) \
	mung-rangealloc.o mung-slab.o mung-memdesc.o \
	lfht-percpu.o lfht-epoch.o lfht-nbsl.o

CFLAGS:=$(subst -D__KERNEL__,,$(CFLAGS)) -mstackrealign

# the system task runtime library is, much like the utility library (which it
# depends on), unsuitable for use out of tree.
all: libsneks-systask.a libsneks-sysidl.a


clean:
	@rm -f *.o *.a $(CLEAN_PATS)


distclean:
	@rm -rf .deps


libsneks-systask.a: $(LIB_OBJS)
	@echo "  AR $@"
	@ar cr $@ $^
	@echo "  RANLIB $@"
	@ranlib $@


libsneks-sysidl.a: $(IDL_OBJS)
	@echo "  AR $@"
	@ar cr $@ $^
	@echo "  RANLIB $@"
	@ranlib $@


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

# IDL clients and %-defs.h for systemspace interfaces.
%-client.s %-common.s $(CFGDIR)/include/sneks/sys/%-defs.h: $(CFGDIR)/idl/sys/%.idl
	@echo "  IDL $< <client>"
	@$(MUIDL) $(MUIDLFLAGS) --client --common --defs $<
	@mkdir -p $(CFGDIR)/include/sneks/sys
	@mv $(patsubst $(CFGDIR)/idl/sys/%.idl,%-defs.h,$<) $(CFGDIR)/include/sneks/sys

# services for all of them
%-service.s: %.idl
	@echo "  IDL $< <service>"
	@$(MUIDL) $(MUIDLFLAGS) --service $<

# also make all syscrt source depend on all systemside IDL headers, just to
# enforce build order.
$(patsubst %.c,%.o,$(LIB_SRC)): $(SYS_IDL_DEFS)


include $(wildcard .deps/*.d)
