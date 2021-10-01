#!/bin/sh
set -e
for mod in $@; do
	echo ": ../ext/lfht/${mod}.c |> !cc |> lfht-${mod}.o {lib_objs}"
done
