#!/bin/sh
set -e

# these are all here like this so that we can parameterize the group
# identifier.

STAGING=$1
TESTDIR=$2
GROUP=$3

# io:reg
echo ": |> ^ ECHO >io/reg/testfile^ echo -n '0123456789abcdef' >%o |> $STAGING/user/test/io/reg/testfile $GROUP"

# io:dir
for i in a b c d e f g h i j k l; do
	echo ": |> ^ TOUCH io/dir/$i^ touch %o |> $STAGING/user/test/io/dir/$i/placeholder $GROUP"
done
for i in 0 1 2 3 4 5 6 7 8 9; do
	echo ": |> ^ ECHO >io/dir/$i^ echo 'shoop da woop $i' >%o |> $STAGING/user/test/io/dir/$i $GROUP"
done

# io:stat
for i in r x rx; do
	echo ": |> ^ FOO $i^ echo 'exit 123' >%o; chmod u=$i %o |> $STAGING/user/test/io/stat/$i $GROUP"
done
echo ": |> ^ LN %o^ ln -s does_not_exist %o |> $STAGING/user/test/io/stat/exploding_symlink $GROUP"

# io:symlink
link() {
	echo ": |> ^ LN %o^ ln -s $1 %o |> $STAGING/user/test/io/symlink/$2 $GROUP"
}

linksrc=${TESTDIR}/user/test/io

link foo/bar/zot teh_linkz0r
link looping_terminal looping_terminal
link looping_middle/deez/nutz looping_middle
link ../reg/testfile terminal_relative
link terminal_relative terminal_iterated_relative
link $linksrc/reg/testfile terminal_absolute
link $linksrc/symlink/terminal_relative terminal_iterated_absolute
link ../reg nonterminal_relative
link nonterminal_relative nonterminal_iterated_relative
link $linksrc/reg nonterminal_absolute
link $linksrc/symlink/nonterminal_relative nonterminal_iterated_absolute


# cstd:fileio
echo ": |> ^ ECHO >cstd/fileio/test-file^ echo 'hello, test file' >%o |> $STAGING/user/test/cstd/fileio/test-file $GROUP"

# for process
base=$STAGING/user/test/exec
echo ": |> ^ MKDIR isdir/exit_with_0^ touch %o |> $base/isdir/exit_with_0/placeholder $GROUP"
echo ": |> ^ TESTBITS %o^ (echo '#!${TESTDIR}/noexec/exit_with_0'; echo 'exit 666') >%o; chmod u+x %o |> $base/noexec_script/exit_with_0 $GROUP"
echo ": |> ^ TESTBITS %o^ echo 'exit 667' >%o |> $base/xnotset/exit_with_0 $GROUP"
