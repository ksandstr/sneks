#!/usr/bin/perl
use Modern::Perl '2017';

# local packages also plz
use FindBin qw($Bin);
use lib "$Bin/perl5";

use File::Temp qw/tempdir/;
use Cwd;
use Jobserver;

my $VERBOSE = $ENV{VERBOSE} // 0;
my $opt_add_host = $ENV{HOSTSUITE} // 0;
$ENV{CC} //= 'cc';

my $modname = shift @ARGV // die "needs module name!";
my @cflags = @ARGV;

my @sources = <$modname/*.c>;
my $outnam = 'ccan-' . ($modname =~ /([^\/]+)$/)[0]
	. ($opt_add_host ? ".host" : "") . '.a';
my $outdir = getcwd();

if($VERBOSE) {
	print STDERR "source: $_\n" for @sources;
	print STDERR "output path is $outdir/$outnam\n";
}

my $dir = tempdir(CLEANUP => 1);
print STDERR "tempdir=`$dir'\n" if $VERBOSE;
chdir $dir;

my $jobs = Jobserver->new;	# per St. Jobs and his Vitamin Cure
my @objs;
for(@sources) {
	my ($m, $s) = /([^\/]+)\/([^\/]+)$/;
	my $o = $s;
	$o =~ s/\.c$/.o/;
	print "  CC ccan/$m/$o <ccan" . ($opt_add_host ? "/host" : "") . ">\n";
	my $opath = $dir . "/" . $o;
	$jobs->spawn("$ENV{CC} -c '$_' -o '$opath' " . join(" ", @cflags));
	push @objs, $o;
}
$jobs->waitall;
for(@objs) {
	if(!-f $_) {
		print STDERR "*** compiler didn't output $_!\n";
		exit 1;
	}
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
