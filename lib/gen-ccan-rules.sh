#!/bin/sh
set -e
for module in $@; do
	echo ": &(ccandir)/ccan/$module/$module.c |> !cc |> ccan-$module.o {lib_objs}"
done
