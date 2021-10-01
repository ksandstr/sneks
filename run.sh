#!/bin/sh

# utility script for execution of sneks' test suites in serial console mode.
# used by test reporting, manual testing, and derived utility scripts. passes
# $SYSTEST_OPTS down as command-line arguments to systest and $UTEST_OPTS to
# the userspace testsuite (executed by init(8)), and further command line
# arguments to `kvm'.

test -z "$MUNG" && MUNG="ext/mung"
test -z "$BUILDDIR" && BUILDDIR="."

INITRD_TAIL=" init=/initrd/sbin/init"
if [ -n "$UTEST_OPTS" ] || [ -z "$SYSTEST" ]; then
	PACK_INIT_OPTS=`echo $UTEST_OPTS | sed 's/ /&/g'`
	if [ -n "$PACK_INIT_OPTS" ]; then
		PACK_INIT_OPTS=";--setenv=UTEST_OPTS=$PACK_INIT_OPTS"
	fi
	# 6 runs testsuite and waits for it to complete.
	INITRD_TAIL="${INITRD_TAIL}$PACK_INIT_OPTS;6"
fi

# systests can be run with "SYSTEST=1 ./run.sh yada yada"
SYSTEST_PART=""
if [ -n "$SYSTEST" ]; then
	INITRD_TAIL="${INITRD_TAIL} latewaitmod=systest"
	SYSTEST_PART=",$BUILDDIR/sys/test/systest $SYSTEST_OPTS"
fi

exec qemu-system-i386 -machine accel=kvm:tcg -serial stdio -display none -no-reboot -net none -kernel $MUNG/mbiloader/mbiloader -initrd "$MUNG/ia32-kernel,$MUNG/user/sigma0,$BUILDDIR/root/root,$BUILDDIR/sys/sysmem/sysmem,$BUILDDIR/sys/vm/vm,$BUILDDIR/sys/fs.squashfs/fs.squashfs,$BUILDDIR/initrd.img${INITRD_TAIL}${SYSTEST_PART}" $@
