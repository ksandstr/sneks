
export CFGDIR:=$(abspath ../..)
include $(CFGDIR)/config.mk

LDSCRIPT:=$(CFGDIR)/user/crt/static.ld
LDFLAGS=-e_start -N -nostdlib -L $(MUNG_DIR)/user/lib/l4 \
	-L $(CFGDIR)/lib -L $(CFGDIR)/user/crt
# under -O3, GCC may produce SSE instructions in a runtime that doesn't align
# the stack properly. FIXME: repair that, then remove -mstackrealign.
CFLAGS:=$(CFLAGS) -D__KERNEL__ -mstackrealign

OUTPUTS=testsuite

# here, we actually want the two-part path shown.
TEST_SOURCE:=$(wildcard */*.c)
TEST_OBJS:=$(TEST_SOURCE:%.c=%.o)

vpath %.idl $(CFGDIR)/idl/api


all: $(OUTPUTS)


clean:
	@rm -f *.o *.a $(CLEAN_PATS) $(TEST_OBJS)


distclean:
	@rm -rf .deps $(OUTPUTS)


testsuite: $(LDSCRIPT) main.o harness.o optcompat.o $(TEST_OBJS) \
		$(CFGDIR)/lib/tap.o \
		ccan-opt.a ccan-htable.o ccan-talloc.o ccan-autodata.o
	@echo "  LD $@"
	@$(LD) $(LDFLAGS) -static -o $@ -b elf32-i386 -T $(LDSCRIPT) \
		$(filter %.o,$^) $(filter %.a,$^) \
		-lsneks-user-crt -ll4 -L /usr/lib32 -lm \
		$(shell gcc $(CFLAGS) -print-libgcc-file-name)


initrd-install: testsuite
	@mkdir -p $(INITRD)/user
	@install $< $(INITRD)/user/


include $(wildcard .deps/*.d)
