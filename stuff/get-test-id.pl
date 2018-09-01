#!/usr/bin/perl
use Modern::Perl '2015';
use Getopt::Long;

# script that, given a test name, finds the corresponding test ID for use with
# the "runonly" testbench syntax.

my $test_user = 0;
my $test_system = 0;
GetOptions("user" => \$test_user, "system" => \$test_system)
	or die "malformed command-line arguments";

die "must specify exactly one of --user or --system"
	unless $test_user xor $test_system;

-f 'run.sh' || die "not in the right directory";
die "gotta have a parameter!" unless @ARGV;

my $opts_name = $test_user ? "UTEST_OPTS" : "SYSTEST_OPTS";

my @matched;
$SIG{PIPE} = sub { };		# and smoke it
$ENV{opts_name} = "--describe";
$ENV{SYSTEST} = 1 if $test_system;
open(DESCRIBE, "./run.sh -display none 2>&1 |")
	|| die "can't open pipe from run.sh!";
while(<DESCRIBE>) {
	chomp;
	my $match = 0;	# of @ARGV patterns matched on this line.
	for my $pat (@ARGV) {
		/desc\s+test\s+\`(\w*$pat\w*)'\s+(.*)$/ or next;
		my %p = map { /(\w+):(\w+)/; ($1, $2) } split /\s+/;
		print "matched `$pat': $1 -> $p{id}";
		my $add = $p{id};
		if(exists $p{low} && exists $p{high} && $p{high} > 0) {
			print " [$p{low}..$p{high}]";
			$add .= ':*';
		}
		print "\n";
		$match++;
		push @matched, $add;
	}
	if(!$match && (/\*\*\*\s+begin\s+suite\s+/ || /\*\*\* .+ completed/)) {
		# kill the piped child process and finish the loop. the "describe"
		# output has ended so there'll be no further desc lines.
		$SIG{TERM} = sub { };
		kill -TERM, $$;
		delete $SIG{TERM};
		last;
	}
}
close DESCRIBE;

# final output.
if(@matched) {
	my $side = $test_user ? "--user" : "--system";
	print "command: stuff/run-only.pl $side " . join(' ', @matched) . "\n";
} else {
	print "no matches.\n";
}
