
export CFGDIR:=$(abspath ..)
include ../config.mk



# it's our utility library, we get to call it whatever we damn well please. in
# this case it'll be just libsneks, and woe betide the fool who depends on it
# out of tree.
all: libsneks.a


clean:
	@rm -f *.o *.a


distclean:
	@rm -rf .deps


libsneks.a: string.o
	@echo "  AR $@"
	@ar cr $@ $^
	@echo "  RANLIB $@"
	@ranlib $@


include $(wildcard .deps/*.d)
