
export CFGDIR:=$(abspath ../..)
include $(CFGDIR)/config.mk

LDSCRIPT:=$(CFGDIR)/sys/crt/systask.ld
LDFLAGS=-e_start -N -nostdlib -L $(MUNG_DIR)/user/lib/l4 \
	-L $(CFGDIR)/lib -L $(CFGDIR)/sys/crt
# under -O3, GCC may produce SSE instructions in a runtime that doesn't align
# the stack properly. FIXME: repair that, then remove -mstackrealign.
CFLAGS:=$(CFLAGS) -D__KERNEL__ -mstackrealign

OUTPUTS=systest initrd_spawn_partner

# FIXME: this will generate full paths where they're not needed. hack the build
# scripts present a nicer "  CC sys/foo/t/c11sumthing.c" report to conceal teh
# ugly.
TEST_SOURCE:=$(wildcard $(CFGDIR)/sys/*/t/*.c)
TEST_OBJS:=$(TEST_SOURCE:%.c=%.o)

vpath %.idl $(CFGDIR)/idl/api $(CFGDIR)/idl/sys


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


# note that the initrd spawn partner is not linked into the main userspace
# library, since it might not be built at this point yet.
initrd_spawn_partner: $(CFGDIR)/user/crt/static.ld spawn-partner.o \
		proc-client.o proc-common.o
	@echo "  LD $@"
	@$(LD) $(LDFLAGS) -static -o $@ -b elf32-i386 \
		-T $(CFGDIR)/user/crt/static.ld \
		$(CFGDIR)/sys/crt/crt0.o $(filter %.o,$^) $(filter %.a,$^) \
		$(LIBS) -lsneks -ll4 -L /usr/lib32 -lm \
		$(shell gcc $(CFLAGS) -print-libgcc-file-name)

spawn-partner.o: spawn-partner.c proc-defs.h


test-initrd: initrd_spawn_partner
	@echo "hello, world" >$(INITRD)/hello.txt
	@install initrd_spawn_partner $(INITRD)/


include $(wildcard .deps/*.d)
