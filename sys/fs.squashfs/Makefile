
export CFGDIR:=$(abspath ../..)
include $(CFGDIR)/config.mk

LDSCRIPT=$(CFGDIR)/sys/crt/systask.ld
LDFLAGS:=$(LDFLAGS) -e_start -N -nostdlib -L $(MUNG_DIR)/user/lib/l4 \
	-L $(CFGDIR)/lib -L $(CFGDIR)/sys/crt -L $(CFGDIR)/sys/io
# under -O3, GCC may produce SSE instructions in a runtime that doesn't align
# the stack properly. FIXME: repair that, then remove -mstackrealign.
CFLAGS:=$(CFLAGS) -D__SNEKS__=1 -mstackrealign

OUTPUTS=fs.squashfs
IDL_OBJS=squashfs-impl-common.o squashfs-impl-service.o


all: $(OUTPUTS)


clean:
	@rm -f *.o $(CLEAN_PATS)


distclean:
	@rm -rf .deps $(OUTPUTS)


# FIXME: the linker hax to resolve the epoch-to-nbsl dependency thing should
# properly be done by importing all of libsneks into libsneks-systask, making
# the latter a superset of the former, which follows usage.
fs.squashfs: $(LDSCRIPT) squashfs.o $(IDL_OBJS) \
		ccan-opt.a
	@echo "  LD $@"
	@$(LD) $(LDFLAGS) -static -o $@ -b elf32-i386 -T $(LDSCRIPT) \
		$(filter %.o,$^) $(filter %.a,$^) $(LIBS) \
		-lsneks-systask -lsneks-io -lsneks -lsneks-systask -ll4 -lm \
		$(shell gcc $(CFLAGS) -print-libgcc-file-name)


include $(wildcard .deps/*.d)
