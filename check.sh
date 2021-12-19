#!/bin/sh
set -e

# runs all the test suites. use the source, luke.

QUIET="--quiet"

die() {	# die die die fornicate 666
	echo >&2 $@
	exit 1
}

REPORT=ext/mung/user/testbench/report.pl
test -x $REPORT || die "report.pl at '$REPORT' doesn't exist, or isn't executable"

prepare_suite() {
	case $1 in
		user)
			tup $QUIET user/test/testsuite >/dev/null
			;;
		sys)
			tup $QUIET sys/test/systest >/dev/null
			;;
		host)
			tup $QUIET user/test/hostsuite >/dev/null
			;;
		*)
			die "unknown suite $1"
			;;
	esac
}

# option parsing.
while getopts 'vhs:' OPT; do
	case $OPT in
		h)
			echo "would print help here"
			exit 0
			;;
		v)
			echo "sneks ./check.sh v0"
			exit 0
			;;
		s)
			test -z "$SUITES" || SUITES="$SUITES "
			SUITES="${SUITES}$OPTARG"
			;;
		\?) die "getopts error (!)" ;;
	esac
done
shift `expr $OPTIND - 1`
test $# -gt 0 && die "extraneous command line elements: $@"
test -z "$SUITES" && SUITES="sys user"	# "host" is super sekrit mode

for suite in $SUITES; do
	echo "-- $suite ..."
	prepare_suite $suite
	case $suite in
		host)
			TEST_CMDLINE="user/test/hostsuite 2>&1" $REPORT
			;;
		user)
			$REPORT
			;;
		sys)
			SYSTEST=1 $REPORT
			;;
	esac
done
