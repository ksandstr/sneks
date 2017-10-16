
export CFGDIR:=$(abspath ..)
include $(CFGDIR)/config.mk

LDFLAGS=-e_start -N -nostdlib -L $(MUNG_DIR)/user/lib/l4 -L $(CFGDIR)/lib
# under -O3, GCC may produce SSE instructions in a runtime that doesn't align
# the stack properly. FIXME: repair that, then remove -mstackrealign.
CFLAGS:=$(subst -D__KERNEL__,,$(CFLAGS)) -mstackrealign

OUTPUTS=root


all: $(OUTPUTS)


clean:
	@rm -f *.o $(CLEAN_PATS)


distclean:
	@rm -rf .deps $(OUTPUTS)


root: root.ld crt0.o main.o runtime.o mm.o \
		sysmem-common.o sysmem-client.o kmsg-common.o kmsg-service.o \
		ccan-htable.o
	@echo "  LD $@"
	@$(LD) $(LDFLAGS) -static -o $@ -b elf32-i386 -T root.ld \
		$(filter %.o,$^) $(LIBS) \
		-lsneks -ll4 -L /usr/lib32 -lm \
		$(shell gcc $(CFLAGS) -print-libgcc-file-name)

main.o: main.c sysmem-defs.h kmsg-defs.h


# muidl outputs.
# TODO: we don't really need to hear first about the services being generated,
# and then the common bit, the client, and then the defs. there's AS chatter
# about the first three too.
%-service.s: $(CFGDIR)/idl/sys/%.idl
	@echo "  IDL $< <service>"
	@$(MUIDL) $(MUIDLFLAGS) --service $<

%-client.s: $(CFGDIR)/idl/sys/%.idl
	@echo "  IDL $< <client>"
	@$(MUIDL) $(MUIDLFLAGS) --client $<

%-common.s: $(CFGDIR)/idl/sys/%.idl
	@echo "  IDL $< <common>"
	@$(MUIDL) $(MUIDLFLAGS) --common $<

%-defs.h: $(CFGDIR)/idl/sys/%.idl
	@echo "  IDL $< <defs>"
	@$(MUIDL) $(MUIDLFLAGS) --defs $<


include $(wildcard .deps/*.d)
