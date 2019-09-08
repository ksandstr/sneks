
export CFGDIR:=$(abspath ../..)
include $(CFGDIR)/config.mk

LDSCRIPT=$(CFGDIR)/sys/crt/systask.ld
LDFLAGS:=$(LDFLAGS) -e_start -N -nostdlib -L $(MUNG_DIR)/user/lib/l4 \
	-L $(CFGDIR)/lib -L $(CFGDIR)/sys/crt
# under -O3, GCC may produce SSE instructions in a runtime that doesn't align
# the stack properly. FIXME: repair that, then remove -mstackrealign.
CFLAGS:=$(CFLAGS) -D__SNEKS__=1 -mstackrealign \
	#-DDEBUG_ME_HARDER	# extensive invariant checking ahoy

OUTPUTS=vm
IDL_OBJS=vm-impl-common.o vm-impl-service.o \
	proc-common.o proc-client.o info-common.o info-client.o \
	fs-common.o fs-client.o


all: $(OUTPUTS)


clean:
	@rm -f *.o $(CLEAN_PATS)


distclean:
	@rm -rf .deps $(OUTPUTS)


vm: $(LDSCRIPT) vm.o boot.o $(IDL_OBJS) \
		ccan-list.o ccan-htable.o ccan-bitmap.o
	@echo "  LD $@"
	@$(LD) $(LDFLAGS) -static -o $@ -b elf32-i386 -T $(LDSCRIPT) \
		$(filter %.o,$^) $(LIBS) \
		-lsneks-systask -lsneks -ll4 -lm \
		$(shell gcc $(CFLAGS) -print-libgcc-file-name)


vm.o: vm.c vm-impl-defs.h info-defs.h proc-defs.h fs-defs.h


include $(wildcard .deps/*.d)
