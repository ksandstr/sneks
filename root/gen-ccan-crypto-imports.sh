#!/bin/sh
set -e
for mod in $@; do
	echo ": &(ccandir)/ccan/crypto/${mod}/${mod}.c |> !cc |> ccan-crypto-${mod}.o"
done
