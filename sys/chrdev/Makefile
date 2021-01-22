
export CFGDIR:=$(abspath ../..)
include $(CFGDIR)/config.mk

LIB_SRC:=$(wildcard *.c)
LIB_OBJS:=$(LIB_SRC:.c=.o) chrdev-impl-common.o chrdev-impl-service.o

CFLAGS:=$(subst -D__KERNEL__,,$(CFLAGS)) -D__SNEKS__=1 -mstackrealign


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
