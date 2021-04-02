
export CFGDIR:=$(abspath ../..)
include $(CFGDIR)/config.mk

LIB_SRC:=$(wildcard *.c)
LIB_OBJS:=$(LIB_SRC:.c=.o)

CFLAGS:=$(subst -D__KERNEL__,,$(CFLAGS)) -D__SNEKS__=1 -mstackrealign


all: libsneks-io.a


clean:
	@rm -f *.o *.a $(CLEAN_PATS)


distclean:
	@rm -rf .deps


libsneks-io.a: $(LIB_OBJS)
	@echo "  AR $@"
	@ar crT $@ $^
	@echo "  RANLIB $@"
	@ranlib $@


include $(wildcard .deps/*.d)
