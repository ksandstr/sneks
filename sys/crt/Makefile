
export CFGDIR:=$(abspath ../..)
include $(CFGDIR)/config.mk

IDL_OBJS=sysmem-common.o sysmem-client.o

LIB_SRC:=$(wildcard *.c)
LIB_OBJS:=crt0.o $(IDL_OBJS) $(subst .c,.o,$(LIB_SRC))


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


crt1.o: crt1.c sysmem-defs.h


# muidl outputs.
# TODO: we don't really need to hear first about the services being generated,
# and then the common bit, the client, and then the defs. there's AS chatter
# about the first three too.
%-service.s: idl/%.idl
	@echo "  IDL $< <service>"
	@$(MUIDL) $(MUIDLFLAGS) --service $<

%-client.s: idl/%.idl
	@echo "  IDL $< <client>"
	@$(MUIDL) $(MUIDLFLAGS) --client $<

%-common.s: idl/%.idl
	@echo "  IDL $< <common>"
	@$(MUIDL) $(MUIDLFLAGS) --common $<

%-defs.h: idl/%.idl
	@echo "  IDL $< <defs>"
	@$(MUIDL) $(MUIDLFLAGS) --defs $<


include $(wildcard .deps/*.d)
