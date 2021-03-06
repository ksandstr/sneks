
export CFGDIR:=$(abspath ../..)
include $(CFGDIR)/config.mk

LDSCRIPT=$(CFGDIR)/sys/crt/systask.ld
LDFLAGS:=$(LDFLAGS) -e_start -N -nostdlib -L $(MUNG_DIR)/user/lib/l4 \
	-L $(CFGDIR)/lib -L $(CFGDIR)/sys/crt -L $(CFGDIR)/sys/chrdev
# under -O3, GCC may produce SSE instructions in a runtime that doesn't align
# the stack properly. FIXME: repair that, then remove -mstackrealign.
CFLAGS:=$(CFLAGS) -D__SNEKS__=1 -mstackrealign \
	#-DDEBUG_ME_HARDER	# extensive invariant checking ahoy

OUTPUTS=nullserv


all: $(OUTPUTS)


clean:
	@rm -f *.o $(CLEAN_PATS)


distclean:
	@rm -rf .deps $(OUTPUTS)


initrd-install: nullserv
	@mkdir -p $(INITRD)/lib/sneks-0.0p0
	@install $< $(INITRD)/lib/sneks-0.0p0


nullserv: $(LDSCRIPT) nullserv.o ccan-membuf.o
	@echo "  LD $@"
	@$(LD) $(LDFLAGS) -static -o $@ -b elf32-i386 -T $(LDSCRIPT) \
		$(filter %.o,$^) $(LIBS) \
		-lsneks-chrdev -lsneks-systask -lsneks -lsneks-systask \
		-ll4 -lm \
		$(shell gcc $(CFLAGS) -print-libgcc-file-name)


include $(wildcard .deps/*.d)
