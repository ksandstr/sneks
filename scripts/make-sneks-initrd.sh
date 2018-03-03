#!/bin/sh
set -e
[ -z "$1" ] && (echo "need parameter."; exit 1)

TARGET=`mktemp -d /tmp/make-sneks-initrd.XXXXXX`

# the systest directory, when not disabled.
if [ -z "$DISABLE_TEST_OUTPUT" ]; then
    PARTS=""
    for d in sys/*; do
	if [ -d "$d" ] && grep ^test-initrd "$d/Makefile" >/dev/null; then
	    if [ -n "$PARTS" ]; then PARTS="$PARTS "; fi
	    PARTS="${PARTS}$d"
	fi
    done
    for part in $PARTS; do
	echo "  INITRD <systest> $part"
	dir="$TARGET/systest/$part"
	mkdir -p $dir
	make --quiet -C $part test-initrd INITRD=$dir
    done
fi

# the real initrd.
PARTS=""
for cand in `find * -type d -and -not -path "*/root/*" -print`; do
    if [ -f "$cand/Makefile" ] \
	&& grep ^initrd-install: "$cand/Makefile" >/dev/null;
    then
	if [ -n "$PARTS" ]; then PARTS="$PARTS "; fi
	PARTS="${PARTS}$cand"
    fi
done
for part in $PARTS; do
    echo "  INITRD $part"
    make --quiet -C $part initrd-install INITRD=$TARGET
done

# other test crapola (FIXME: this can be happily removed)
mkdir $TARGET/test-directory
echo -n 'Hello, world!' >$TARGET/test-directory/test-file.txt

echo "  MKSQUASHFS $1"
mksquashfs $TARGET $1 -comp lz4 -all-root -no-fragments -info 2>/dev/null \
    | grep -e '^file' -e '^directory'

rm -r $TARGET
