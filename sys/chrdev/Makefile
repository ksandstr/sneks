
export CFGDIR:=$(abspath ../..)
include $(CFGDIR)/config.mk

LIB_SRC:=$(wildcard *.c)
LIB_OBJS:=$(LIB_SRC:.c=.o)

CFLAGS:=$(subst -D__KERNEL__,,$(CFLAGS)) -mstackrealign


all: libsneks-chrdev.a


clean:
	@rm -f *.o *.a $(CLEAN_PATS)


distclean:
	@rm -rf .deps


libsneks-chrdev.a: $(LIB_OBJS)
	@echo "  AR $@"
	@ar cr $@ $^
	@echo "  RANLIB $@"
	@ranlib $@


include $(wildcard .deps/*.d)
