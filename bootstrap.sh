#!/bin/sh
set -e

# it's painful to generate tools used in tup rules within that same tup
# hierarchy, so we bootstrap various tools and utilities ourselves instead.

# get modules, and their modules
git submodule update --init
for i in ext/*; do (cd $i; git submodule update --init); done

# initialize them as necessary
for b in muidl mung; do
	echo "  MAKE ext/$b"
	make -C ext/$b
done
echo "  MAKE ext/ccan <config.h>"; make -C ext/ccan config.h
