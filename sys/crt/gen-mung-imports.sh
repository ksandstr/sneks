#!/bin/sh
set -e
for mod in $@; do
	if test -f "../../ext/mung/${mod}.c"; then
		echo ": &(mungdir)/${mod}.c |> !cc |> mung-${mod}.o"
	elif test -f "../../ext/mung/lib/${mod}.c"; then
		echo ": &(mungdir)/lib/${mod}.c |> !cc |> mung-${mod}.o"
	else
		echo "can't find mung module ${mod}!" >&2
		exit 1
	fi
done

# FIXME: this script is duplicated with the one in root/.
