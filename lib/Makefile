
export CFGDIR:=$(abspath ..)
include ../config.mk


LIB_SRC:=$(wildcard *.c)
LIB_OBJS:=$(subst .c,.o,$(LIB_SRC)) ccan-hash.o ccan-autodata.o \
	ccan-likely.o ccan-htable.o ccan-list.o ccan-str.o \
	ccan-bitmap.o

CFLAGS:=$(CFLAGS) -D__SNEKS__=1


# it's our utility library, we get to call it whatever we damn well please. in
# this case it'll be just libsneks, and woe betide the fool who depends on it
# out of tree.
all: libsneks.a


clean:
	@rm -f *.o *.a


distclean:
	@rm -rf .deps


libsneks.a: $(LIB_OBJS) setjmp.o
	@echo "  AR $@"
	@ar cr $@ $^
	@echo "  RANLIB $@"
	@ranlib $@


include $(wildcard .deps/*.d)
