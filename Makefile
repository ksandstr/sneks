
export CFGDIR:=$(abspath .)
include config.mk

.PHONY: all clean distclean check qcheck


# NOTE: the sys/test line should be last!
all: tags
	+@make -C lib all
	+@make -C root all
	+@make -C sys all
	+@make -C sys/test all
	+@make initrd.img


clean:
	@rm -f *.o initrd.img $(CLEAN_PATS)
	+@make -C sys clean
	+@make -C root clean
	+@make -C lib clean


distclean: clean
	@rm -f tags
	@rm -rf .deps
	+@make -C sys distclean
	+@make -C root distclean
	+@make -C lib distclean
	@find . -name ".deps" -type d -print|xargs rm -rf


qcheck: check


check: all
	@echo "-- system tests..."
	@$(CFGDIR)/../mung/user/testbench/report.pl
	@echo "-- all tests completed!"


initrd.img: scripts/make-sneks-initrd.sh
	@rm -f $@
	+@scripts/make-sneks-initrd.sh $@


tags: $(shell find . -iname "*.[ch]" -or -iname "*.p[lm]")
	@ctags -R *
