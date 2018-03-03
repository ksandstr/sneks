#!/bin/sh
# utility script for execution of the OS personality's test suite in serial
# console mode. used by test reporting, manual testing, and derived utility
# scripts. passes $SNEKS_OPTS down as command-line arguments to the root task
# module, and further command line arguments to `kvm'.
MUNG="../mung"
exec kvm -serial stdio -display none -no-reboot -net none -kernel $MUNG/mbiloader/mbiloader -initrd "$MUNG/ia32-kernel,$MUNG/user/sigma0,root/root,sys/sysmem/sysmem,sys/vm/vm,sys/fs.squashfs/fs.squashfs,initrd.img,sys/test/systest $SNEKS_OPTS" $@
