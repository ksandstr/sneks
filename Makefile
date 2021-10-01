.PHONY: all clean distclean check qcheck


# TODO: depend-build ext/muidl before mung
all: tags ext/mung/ia32-kernel initrd.img
	+@tup

clean:
	@echo "There is no 'clean' target. Use 'git clean' instead, carefully."
	@exit 1

distclean: clean


# originally qcheck would skip framework selftests, but the name has stuck for
# running all tests which are relevant rather than the whole shebang. if,
# sometime in the future, scripting becomes clever enough to only execute tests
# that've been rebuilt between last run and now, qcheck may do that instead.
qcheck: check


# run the test suite on the host. this may become a silent part of `make check'
# in the future, but for now it's useful for manual test validation.
hostcheck:
	+@tup user/test/hostsuite
	@TEST_CMDLINE="user/test/hostsuite 2>&1" ext/mung/user/testbench/report.pl


check: ext/mung/ia32-kernel initrd.img
	@echo "-- system tests..."
	@SYSTEST=1 ext/mung/user/testbench/report.pl
	@echo "-- userspace tests..."
	@ext/mung/user/testbench/report.pl
	@echo "-- all tests completed!"


initrd.img: Tupfile
	+@tup initrd.img
