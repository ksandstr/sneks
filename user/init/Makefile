
export CFGDIR:=$(abspath ../..)
include $(CFGDIR)/config.mk

LDSCRIPT:=$(CFGDIR)/user/crt/static.ld
LDFLAGS:=$(LDFLAGS) -e_start -nostdlib -L $(MUNG_DIR)/user/lib/l4 \
	-L $(CFGDIR)/lib -L $(CFGDIR)/user/crt
# under -O3, GCC may produce SSE instructions in a runtime that doesn't align
# the stack properly. FIXME: repair that, then remove -mstackrealign.
CFLAGS:=$(CFLAGS) -D__KERNEL__ -D__sneks__=1 -D__l4x2__=1 -mstackrealign

OUTPUTS=init


all: $(OUTPUTS)


clean:
	@rm -f *.o *.a $(CLEAN_PATS)


distclean:
	@rm -rf .deps $(OUTPUTS)


initrd-install: init
	@mkdir -p $(INITRD)/sbin
	@install $< $(INITRD)/sbin/
	@mkdir -p $(INITRD)/etc
	@install etc/inittab $(INITRD)/etc/


init: $(LDSCRIPT) init.o inittab.o optcompat.o ccan-opt.a
	@echo "  LD $@"
	@$(LD) $(LDFLAGS) -static -o $@ -b elf32-i386 -T $(LDSCRIPT) \
		$(filter %.o,$^) $(filter %.a,$^) \
		-lsneks-user-crt -lsneks -ll4 -L /usr/lib32 -lm \
		$(shell gcc $(CFLAGS) -print-libgcc-file-name)

inittab.o: etc/inittab
	@objcopy -I binary -O elf32-i386 -B i386 $< $@


include $(wildcard .deps/*.d)
