
export CFGDIR:=$(abspath .)
include config.mk

.PHONY: all clean distclean check qcheck


# TODO: replace user/test etc. with just a make -C user all, making user/crt
# the first subdir of that.
all: tags
	+@make -C lib all
	+@make -C root all
	+@make -C sys all
	+@make -C user/crt all
	+@make -C sys/test all
	+@make -C user/test all
	+@make initrd.img


clean:
	@rm -f *.o initrd.img $(CLEAN_PATS)
	+@make -C user/crt clean
	+@make -C user/test clean
	+@make -C sys clean
	+@make -C root clean
	+@make -C lib clean


distclean: clean
	@rm -f tags
	@rm -rf .deps
	+@make -C user/test distclean
	+@make -C user/crt distclean
	+@make -C sys distclean
	+@make -C root distclean
	+@make -C lib distclean
	@find . -name ".deps" -type d -print|xargs rm -rf


# originally qcheck would skip framework selftests, but the name has stuck for
# running all tests which are relevant rather than the whole shebang. if,
# sometime in the future, scripting becomes clever enough to only execute tests
# that've been rebuilt between last run and now, qcheck may do that instead.
qcheck: check


# run the test suite on the host. this may become a silent part of `make check'
# in the future, but for now it's useful for manual test validation.
hostcheck:
	+@make -C user/test hostsuite
	@TEST_CMDLINE="user/test/hostsuite 2>&1" $(MUNG_DIR)/user/testbench/report.pl


check: all
	@echo "-- system tests..."
	@SYSTEST=1 $(MUNG_DIR)/user/testbench/report.pl
	@echo "-- userspace tests..."
	@$(MUNG_DIR)/user/testbench/report.pl
	@echo "-- all tests completed!"


initrd.img: scripts/make-sneks-initrd.sh
	@rm -f $@
	+@scripts/make-sneks-initrd.sh $@


tags: $(shell find . -iname "*.[ch]" -or -iname "*.p[lm]")
	@ctags -R *
