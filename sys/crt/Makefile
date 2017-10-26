
export CFGDIR:=$(abspath ../..)
include $(CFGDIR)/config.mk

IDL_FILES=sysmem.idl kmsg.idl info.idl
IDL_OBJS:=$(patsubst %.idl,%-common.o,$(IDL_FILES)) \
	$(patsubst %.idl,%-client.o,$(IDL_FILES))

LIB_SRC:=$(wildcard *.c)
LIB_OBJS:=crt0.o $(IDL_OBJS) $(LIB_SRC:.c=.o) \
	ccan-htable.o


# the system task runtime library is, much like the utility library (which it
# depends on), unsuitable for use out of tree.
all: libsneks-systask.a


clean:
	@rm -f *.o *.a


distclean:
	@rm -rf .deps


libsneks-systask.a: $(LIB_OBJS)
	@echo "  AR $@"
	@ar cr $@ $^
	@echo "  RANLIB $@"
	@ranlib $@


crt1.o: crt1.c sysmem-defs.h kmsg-defs.h info-defs.h
threads.o: threads.c sysmem-defs.h


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
