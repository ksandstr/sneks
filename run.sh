#!/bin/sh

# utility script for execution of sneks' test suites in serial console mode.
# used by test reporting, manual testing, and derived utility scripts. passes
# $SYSTEST_OPTS down as command-line arguments to systest and $UTEST_OPTS to
# the userspace testsuite (executed as init(8)), and further command line
# arguments to `kvm'.

MUNG="../mung"

INITRD_TAIL=""
if [ -n "$UTEST_OPTS" ] || [ -z "$SYSTEST" ]; then
	PACK_UTEST_OPTS=`echo $UTEST_OPTS | sed 's/ /;/g'`
	if [ -n "$PACK_UTEST_OPTS" ]; then
		PACK_UTEST_OPTS=";$PACK_UTEST_OPTS"
	fi
	INITRD_TAIL=" init=/initrd/user/testsuite$PACK_UTEST_OPTS"
fi

# systests can be run with "SYSTEST=1 ./run.sh yada yada"
SYSTEST_PART=""
if [ -n "$SYSTEST_OPTS" ] || [ -n "$SYSTEST" ]; then
	INITRD_TAIL="${INITRD_TAIL} waitmod=systest"
	SYSTEST_PART=",sys/test/systest $SYSTEST_OPTS"
fi

exec kvm -serial stdio -display none -no-reboot -net none -kernel $MUNG/mbiloader/mbiloader -initrd "$MUNG/ia32-kernel,$MUNG/user/sigma0,root/root,sys/sysmem/sysmem,sys/vm/vm,sys/fs.squashfs/fs.squashfs,initrd.img${INITRD_TAIL}${SYSTEST_PART}" $@
