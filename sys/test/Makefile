
export CFGDIR:=$(abspath ../..)
include $(CFGDIR)/config.mk

LDSCRIPT:=$(CFGDIR)/sys/crt/systask.ld
LDFLAGS=-e_start -N -nostdlib -L $(MUNG_DIR)/user/lib/l4 \
	-L $(CFGDIR)/lib -L $(CFGDIR)/sys/crt
# under -O3, GCC may produce SSE instructions in a runtime that doesn't align
# the stack properly. FIXME: repair that, then remove -mstackrealign.
CFLAGS:=$(CFLAGS) -D__KERNEL__ -D__sneks__=1 -D__l4x2__=1 -mstackrealign

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


initrd_spawn_partner: $(CFGDIR)/user/crt/static.ld spawn-partner.o
	@echo "  LD $@"
	@$(LD) $(LDFLAGS) -L $(CFGDIR)/user/crt -static -o $@ \
		-b elf32-i386 -T $(CFGDIR)/user/crt/static.ld \
		$(filter %.o,$^) $(filter %.a,$^) \
		-lsneks-user-crt -ll4 -L /usr/lib32 -lm \
		$(shell gcc $(CFLAGS) -print-libgcc-file-name)


test-initrd: initrd_spawn_partner
	@echo "hello, world" >$(INITRD)/hello.txt
	@install initrd_spawn_partner $(INITRD)/


include $(wildcard .deps/*.d)
