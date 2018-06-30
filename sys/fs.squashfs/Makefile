
export CFGDIR:=$(abspath ../..)
include $(CFGDIR)/config.mk

LDSCRIPT=$(CFGDIR)/sys/crt/systask.ld
LDFLAGS=-e_start -N -nostdlib -L $(MUNG_DIR)/user/lib/l4 \
	-L $(CFGDIR)/lib -L $(CFGDIR)/sys/crt
# under -O3, GCC may produce SSE instructions in a runtime that doesn't align
# the stack properly. FIXME: repair that, then remove -mstackrealign.
CFLAGS:=$(CFLAGS) -D__SNEKS__=1 -mstackrealign

OUTPUTS=fs.squashfs
IDL_OBJS=squashfs-impl-common.o squashfs-impl-service.o


all: $(OUTPUTS)


clean:
	@rm -f *.o $(CLEAN_PATS)


distclean:
	@rm -rf .deps $(OUTPUTS)


fs.squashfs: $(LDSCRIPT) squashfs.o $(IDL_OBJS) \
		ccan-opt.a ccan-list.o ccan-htable.o
	@echo "  LD $@"
	@$(LD) $(LDFLAGS) -static -o $@ -b elf32-i386 -T $(LDSCRIPT) \
		$(filter %.o,$^) $(filter %.a,$^) $(LIBS) \
		-lsneks-systask -lsneks -ll4 -L /usr/lib32 -lm \
		$(shell gcc $(CFLAGS) -print-libgcc-file-name)


squashfs.o: squashfs.c squashfs-impl-defs.h


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


include $(wildcard .deps/*.d)
