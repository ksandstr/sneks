
export CFGDIR:=$(abspath ..)
include ../config.mk


# for now, API client-sides are included in libsneks.a.

IDL_SRC:=$(wildcard $(CFGDIR)/idl/api/*.idl)
IDL_OBJS:=$(patsubst $(CFGDIR)/idl/api/%.idl,%-common.o,$(IDL_SRC)) \
	$(patsubst $(CFGDIR)/idl/api/%.idl,%-client.o,$(IDL_SRC))

LIB_SRC:=$(wildcard *.c)
LIB_OBJS:=$(subst .c,.o,$(LIB_SRC)) $(IDL_OBJS) \
	ccan-hash.o ccan-autodata.o ccan-likely.o ccan-htable.o \
	ccan-list.o ccan-str.o ccan-bitmap.o ccan-siphash.o

CFLAGS:=$(CFLAGS) -D__SNEKS__=1


# it's our utility library, we get to call it whatever we damn well please. in
# this case it'll be just libsneks, and woe betide the fool who depends on it
# out of tree.
all: libsneks.a $(wildcard $(CFGDIR)/include/sneks/{api,sys}/*-defs.h)


clean:
	@rm -f *-*.s *-defs.h *.o *.a


distclean:
	@rm -rf .deps
	@rm -f $(CFGDIR)/include/sneks/api/*-defs.h
	@mkdir -p $(CFGDIR)/include/sneks/api
	@rmdir --ignore-fail-on-non-empty $(CFGDIR)/include/sneks/api


libsneks.a: $(LIB_OBJS) setjmp.o
	@echo "  AR $@"
	@ar cr $@ $^
	@echo "  RANLIB $@"
	@ranlib $@


%-client.s %-common.s $(CFGDIR)/include/sneks/api/%-defs.h: $(CFGDIR)/idl/api/%.idl
	@echo "  IDL $< <client>"
	@$(MUIDL) $(MUIDLFLAGS) --client --common --defs $<
	@mkdir -p $(CFGDIR)/include/sneks/api
	@mv $(patsubst $(CFGDIR)/idl/api/%.idl,%-defs.h,$<) $(CFGDIR)/include/sneks/api


include $(wildcard .deps/*.d)
