
export CFGDIR:=$(abspath ../..)
include $(CFGDIR)/config.mk

LDFLAGS:=$(LDFLAGS) -e_start -N -nostdlib -L $(MUNG_DIR)/user/lib/l4 \
	-L $(CFGDIR)/lib
# under -O3, GCC may produce SSE instructions in a runtime that doesn't align
# the stack properly. FIXME: repair that, then remove -mstackrealign.
CFLAGS:=$(CFLAGS) -D__KERNEL__ -D__SNEKS__=1 -mstackrealign

OUTPUTS=sysmem
IDL_OBJS=sysmem-impl-common.o sysmem-impl-service.o


all: $(OUTPUTS)


clean:
	@rm -f *.o $(CLEAN_PATS)


distclean:
	@rm -rf .deps $(OUTPUTS)


sysmem: sysmem.ld crt0.o sysmem.o $(IDL_OBJS)
	@echo "  LD $@"
	@$(LD) $(LDFLAGS) -static -o $@ -b elf32-i386 -T sysmem.ld \
		$(filter %.o,$^) $(LIBS) -lsneks -ll4 -lm \
		$(shell gcc $(CFLAGS) -print-libgcc-file-name)


include $(wildcard .deps/*.d)
