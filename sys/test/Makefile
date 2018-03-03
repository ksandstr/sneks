
export CFGDIR:=$(abspath ../..)
include $(CFGDIR)/config.mk

LDSCRIPT:=$(CFGDIR)/sys/crt/systask.ld
LDFLAGS=-e_start -N -nostdlib -L $(MUNG_DIR)/user/lib/l4 \
	-L $(CFGDIR)/lib -L $(CFGDIR)/sys/crt
# under -O3, GCC may produce SSE instructions in a runtime that doesn't align
# the stack properly. FIXME: repair that, then remove -mstackrealign.
CFLAGS:=$(CFLAGS) -D__KERNEL__ -mstackrealign

OUTPUTS=systest

# FIXME: this will generate full paths where they're not needed. hack the build
# scripts present a nicer "  CC sys/foo/t/c11sumthing.c" report to conceal teh
# ugly.
TEST_SOURCE:=$(wildcard $(CFGDIR)/sys/*/t/*.c)
TEST_OBJS:=$(TEST_SOURCE:%.c=%.o)


all: $(OUTPUTS)


clean:
	@rm -f *.o *.a $(CLEAN_PATS) $(TEST_OBJS)


distclean:
	@rm -rf .deps $(OUTPUTS)


systest: $(LDSCRIPT) systest.o harness.o $(TEST_OBJS) \
		ccan-opt.a ccan-htable.o ccan-talloc.o ccan-autodata.o
	@echo "  LD $@"
	@$(LD) $(LDFLAGS) -static -o $@ -b elf32-i386 -T $(LDSCRIPT) \
		$(filter %.o,$^) $(filter %.a,$^) $(LIBS) \
		-lsneks-systask -lsneks -ll4 -L /usr/lib32 -lm \
		$(shell gcc $(CFLAGS) -print-libgcc-file-name)


include $(wildcard .deps/*.d)
