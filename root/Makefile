
export CFGDIR:=$(abspath ..)
include $(CFGDIR)/config.mk

vpath %.idl $(CFGDIR)/idl/api

LDFLAGS:=$(LDFLAGS) -e_start -N -nostdlib -L $(MUNG_DIR)/user/lib/l4 \
	-L $(CFGDIR)/lib
# under -O3, GCC may produce SSE instructions in a runtime that doesn't align
# the stack properly. FIXME: repair that, then remove -mstackrealign.
CFLAGS:=$(subst -D__KERNEL__,,$(CFLAGS)) -mstackrealign \
	-D__SNEKS__=1

OUTPUTS=root


all: $(OUTPUTS)


clean:
	@rm -f *.o $(CLEAN_PATS)


distclean: clean
	@rm -rf .deps $(OUTPUTS)


root: root.ld crt0.o main.o runtime.o thrd.o uapi.o sig.o mm.o ser_io.o \
		fsio.o \
		sysmem-common.o sysmem-client.o kmsg-common.o kmsg-service.o \
		rootserv-service.o root-uapi-service.o bootcon-service.o \
		proc-common.o proc-client.o vm-common.o vm-client.o \
		fs-common.o fs-client.o info-common.o info-client.o \
		msg-common.o msg-client.o \
		mung-rangealloc.o mung-slab.o \
		lfht-epoch.o lfht-nbsl.o lfht-percpu.o
	@echo "  LD $@"
	@$(LD) $(LDFLAGS) -static -o $@ -b elf32-i386 -T root.ld \
		$(filter %.o,$^) $(LIBS) -lsneks -ll4 -lm \
		$(shell gcc $(CFLAGS) -print-libgcc-file-name)

main.o: sysmem-defs.h kmsg-defs.h rootserv-defs.h proc-defs.h bootcon-defs.h
thrd.o: proc-defs.h
uapi.o: root-uapi-defs.h proc-defs.h vm-defs.h fs-defs.h info-defs.h \
	msg-defs.h
fsio.o: fs-defs.h info-defs.h


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
