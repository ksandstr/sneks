
export CFGDIR:=$(abspath ..)
include $(CFGDIR)/config.mk

vpath %.idl $(CFGDIR)/idl/api

LDFLAGS:=$(LDFLAGS) -e_start -N -nostdlib -L $(MUNG_DIR)/user/lib/l4 \
	-L $(CFGDIR)/lib -L $(CFGDIR)/sys/crt
# under -O3, GCC may produce SSE instructions in a runtime that doesn't align
# the stack properly. FIXME: repair that, then remove -mstackrealign.
CFLAGS:=$(subst -D__KERNEL__,,$(CFLAGS)) -mstackrealign \
	-D__SNEKS__=1

IDL_OBJS=root-impl-service.o root-impl-common.o

OUTPUTS=root


all: $(OUTPUTS)


clean:
	@rm -f *.o $(CLEAN_PATS)


distclean: clean
	@rm -rf .deps $(OUTPUTS)


root: root.ld crt0.o $(IDL_OBJS) main.o runtime.o thrd.o uapi.o sig.o mm.o \
		ser_io.o fsio.o \
		mung-rangealloc.o mung-slab.o \
		lfht-epoch.o lfht-nbsl.o lfht-percpu.o
	@echo "  LD $@"
	@$(LD) $(LDFLAGS) -static -o $@ -b elf32-i386 -T root.ld \
		$(filter %.o,$^) $(LIBS) -lsneks-sysidl -lsneks -ll4 -lm \
		$(shell gcc $(CFLAGS) -print-libgcc-file-name)


test-initrd:
	@echo "hello, root:fsio test" >$(INITRD)/fsiohello.txt


# secret nazi forest
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
