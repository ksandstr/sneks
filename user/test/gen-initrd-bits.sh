#!/bin/sh
set -e

STAGING=$1	# what appears under TESTDIR
TESTDIR=$2	# what TESTDIR appears under
GROUP=$3	# tup dependency group for initrd contents

# define ex-nihilo actions the Tupfile should take.
do_mkdir() {
	# emits a placeholder because tup won't depend on directories.
	echo ": |> ^ MKDIR $1^ install -D /dev/null %o |> $STAGING/user/test/$1/.placeholder $GROUP"
}
do_echo() {
	echo ": |> ^ ECHO >$2^ echo -n '$1' >%o |> $STAGING/user/test/$2 $GROUP"
}
do_touch() {
	echo ": |> ^ TOUCH $1^ touch %o |> $STAGING/user/test/$1 $GROUP"
}

# io:reg
do_echo '0123456789abcdef' io/reg/testfile

# io:dir
for i in a b c d e f g h i j k l; do
	do_touch io/dir/$i/placeholder
done
for i in 0 1 2 3 4 5 6 7 8 9; do
	do_echo "shoop da woop $i" io/dir/$i
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

# path:mount
do_mkdir path/mount/sub
do_echo "indeed" path/mount/sub/not-mounted-yet
do_echo "hello, superior filesystem" path/mount/super-test-file

# cstd:fileio
do_echo 'hello, test file' cstd/fileio/test-file

# for process
base=$STAGING/user/test/exec
echo ": |> ^ MKDIR isdir/exit_with_0^ touch %o |> $base/isdir/exit_with_0/placeholder $GROUP"
echo ": |> ^ TESTBITS %o^ (echo '#!${TESTDIR}/noexec/exit_with_0'; echo 'exit 666') >%o; chmod u+x %o |> $base/noexec_script/exit_with_0 $GROUP"
echo ": |> ^ TESTBITS %o^ echo 'exit 667' >%o |> $base/xnotset/exit_with_0 $GROUP"
