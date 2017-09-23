#!/usr/bin/perl
use Modern::Perl '2017';
use File::Temp qw/tempdir/;
use Cwd;

my $VERBOSE = $ENV{VERBOSE} // 0;

my $modname = shift @ARGV // die "needs module name!";
my @cflags = @ARGV;

my @sources = <$modname/*.c>;
my $outnam = 'ccan-' . ($modname =~ /([^\/]+)$/)[0] . '.a';
my $outdir = getcwd();

if($VERBOSE) {
	print STDERR "source: $_\n" for @sources;
	print STDERR "output path is $outdir/$outnam\n";
}

my $dir = tempdir(CLEANUP => 1);
print STDERR "tempdir=`$dir'\n" if $VERBOSE;
chdir $dir;

my @objs;
for(@sources) {
	my ($m, $s) = /([^\/]+)\/([^\/]+)$/;
	my $o = $s;
	$o =~ s/\.c$/.o/;
	print "  CC ccan/$m/$o <ccan>\n";
	my $opath = $dir . "/" . $o;
	system "$ENV{CC} -c '$_' -o '$opath' " . join(" ", @cflags);
	if(!-f $opath) {
		print STDERR "*** compiler didn't output $opath!\n";
		exit 1;
	}
	push @objs, $o;
	die unless -f $o;
}

print "  AR $outnam <ccan>\n";
system "ar cr '$outnam' " . join(" ", @objs);
if(!-f $outnam) {
	print STDERR "*** ar(1) didn't output $outnam!\n";
	exit 1;
}
print "  RANLIB $outnam <ccan>\n";
system "ranlib '$outnam'";
system "cp '$outnam' $outdir/";
