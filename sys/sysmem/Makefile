
export CFGDIR:=$(abspath ../..)
include $(CFGDIR)/config.mk

LDFLAGS=-e_start -N -nostdlib -L $(MUNG_DIR)/user/lib/l4 -L $(CFGDIR)/lib
# under -O3, GCC may produce SSE instructions in a runtime that doesn't align
# the stack properly. FIXME: repair that, then remove -mstackrealign.
CFLAGS:=$(CFLAGS) -D__KERNEL__ -mstackrealign

OUTPUTS=sysmem
IDL_OBJS=sysmem-impl-common.o sysmem-impl-service.o \
	abend-client.o


all: $(OUTPUTS)


clean:
	@rm -f *.o $(CLEAN_PATS)


distclean:
	@rm -rf .deps $(OUTPUTS)


sysmem: sysmem.ld crt0.o sysmem.o $(IDL_OBJS) ccan-list.o
	@echo "  LD $@"
	@$(LD) $(LDFLAGS) -static -o $@ -b elf32-i386 -T sysmem.ld \
		$(filter %.o,$^) $(LIBS) \
		-lsneks -ll4 -L /usr/lib32 -lm \
		$(shell gcc $(CFLAGS) -print-libgcc-file-name)


sysmem.o: sysmem.c sysmem-impl-defs.h info-defs.h abend-defs.h


# muidl outputs.
# TODO: we don't really need to hear first about the services being generated,
# and then the common bit, the client, and then the defs. there's AS chatter
# about the first three too.
%-service.s: %.idl
	@echo "  IDL $< <service>"
	@$(MUIDL) $(MUIDLFLAGS) --service $<

%-client.s: %.idl
	@echo "  IDL $< <client>"
	@$(MUIDL) $(MUIDLFLAGS) --client $<

%-common.s: %.idl
	@echo "  IDL $< <common>"
	@$(MUIDL) $(MUIDLFLAGS) --common $<

%-defs.h: %.idl
	@echo "  IDL $< <defs>"
	@$(MUIDL) $(MUIDLFLAGS) --defs $<

%-defs.h: $(CFGDIR)/idl/sys/%.idl
	@echo "  IDL $< <defs>"
	@$(MUIDL) $(MUIDLFLAGS) --defs $<

%-client.s: $(CFGDIR)/idl/sys/%.idl
	@echo "  IDL $< <client>"
	@$(MUIDL) $(MUIDLFLAGS) --client $<


include $(wildcard .deps/*.d)
