
export CFGDIR:=$(abspath ../..)
include $(CFGDIR)/config.mk

LDSCRIPT:=$(CFGDIR)/sys/crt/systask.ld
LDFLAGS:=$(LDFLAGS) -e_start -nostdlib -L $(MUNG_DIR)/user/lib/l4 \
	-L $(CFGDIR)/lib -L $(CFGDIR)/sys/crt
# under -O3, GCC may produce SSE instructions in a runtime that doesn't align
# the stack properly. FIXME: repair that, then remove -mstackrealign.
CFLAGS:=$(CFLAGS) -D__KERNEL__ -D__sneks__=1 -D__l4x2__=1 -mstackrealign

OUTPUTS=systest initrd_spawn_partner initrd_systask_partner

# FIXME: this will cause the %.c->%.o recipe to print full paths. hack the
# build scripts until they present a nicer "  CC sys/foo/t/c11sumthing.c"
# report so as to conceal teh ugly.
TEST_SOURCE:=$(wildcard $(CFGDIR)/sys/*/t/*.c) $(wildcard $(CFGDIR)/root/t/*.c)
TEST_OBJS:=$(TEST_SOURCE:%.c=%.o)

vpath %.idl $(CFGDIR)/idl/api $(CFGDIR)/idl/sys


all: $(OUTPUTS)


clean:
	@rm -f *.o *.a $(CLEAN_PATS) $(TEST_OBJS)


distclean:
	@rm -rf .deps $(OUTPUTS)


harness.o: proc-defs.h

# FIXME: this is also a bit fugly.
$(CFGDIR)/sys/sysmem/t/flags.o: sysmem-defs.h


systest: $(LDSCRIPT) systest.o harness.o $(TEST_OBJS) \
		ccan-opt.a ccan-talloc.o \
		proc-common.o proc-client.o \
		sysmem-common.o sysmem-client.o
	@echo "  LD $@"
	@$(LD) $(LDFLAGS) -static -o $@ -b elf32-i386 -T $(LDSCRIPT) \
		$(filter %.o,$^) $(filter %.a,$^) $(LIBS) \
		-lsneks-systask -lsneks -lsneks-systask -ll4 -lm \
		$(shell gcc $(CFLAGS) -print-libgcc-file-name)


initrd_spawn_partner: $(CFGDIR)/user/crt/static.ld spawn-partner.o \
		ccan-htable.o
	@echo "  LD $@"
	@$(LD) $(LDFLAGS) -L $(CFGDIR)/user/crt -static -o $@ \
		-b elf32-i386 -T $(CFGDIR)/user/crt/static.ld \
		$(filter %.o,$^) $(filter %.a,$^) \
		-lsneks-user-crt -lsneks -ll4 -lm \
		$(shell gcc $(CFLAGS) -print-libgcc-file-name)


initrd_systask_partner: $(LDSCRIPT) systask-partner.o
	@echo "  LD $@"
	@$(LD) $(LDFLAGS) -static -o $@ -b elf32-i386 -T $(LDSCRIPT) \
		$(filter %.o,$^) $(filter %.a,$^) $(LIBS) \
		-lsneks-systask -lsneks -lsneks-systask -ll4 -lm \
		$(shell gcc $(CFLAGS) -print-libgcc-file-name)


test-initrd: initrd_spawn_partner initrd_systask_partner
	@echo "hello, world" >$(INITRD)/hello.txt
	@install $^ $(INITRD)/


include $(wildcard .deps/*.d)
