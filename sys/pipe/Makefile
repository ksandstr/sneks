
export CFGDIR:=$(abspath ../..)
include $(CFGDIR)/config.mk

LDSCRIPT=$(CFGDIR)/sys/crt/systask.ld
LDFLAGS:=$(LDFLAGS) -e_start -N -nostdlib -L $(MUNG_DIR)/user/lib/l4 \
	-L $(CFGDIR)/lib -L $(CFGDIR)/sys/crt
# under -O3, GCC may produce SSE instructions in a runtime that doesn't align
# the stack properly. FIXME: repair that, then remove -mstackrealign.
CFLAGS:=$(CFLAGS) -D__SNEKS__=1 -mstackrealign \
	#-DDEBUG_ME_HARDER	# extensive invariant checking ahoy

OUTPUTS=pipeserv
IDL_OBJS=pipeserv-impl-common.o pipeserv-impl-service.o


all: $(OUTPUTS)


clean:
	@rm -f *.o $(CLEAN_PATS)


distclean:
	@rm -rf .deps $(OUTPUTS)


initrd-install: pipeserv
	@mkdir -p $(INITRD)/lib/sneks-0.0p0
	@install $< $(INITRD)/lib/sneks-0.0p0


pipeserv: $(LDSCRIPT) pipeserv.o $(IDL_OBJS) ccan-membuf.o
	@echo "  LD $@"
	@$(LD) $(LDFLAGS) -static -o $@ -b elf32-i386 -T $(LDSCRIPT) \
		$(filter %.o,$^) $(LIBS) \
		-lsneks-systask -lsneks -lsneks-systask -ll4 -lm \
		$(shell gcc $(CFLAGS) -print-libgcc-file-name)


pipeserv.o: pipeserv-impl-defs.h io-defs.h


include $(wildcard .deps/*.d)
