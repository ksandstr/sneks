#!/usr/bin/perl
use Modern::Perl '2015';
use Getopt::Long;

# script that, given a sequence of pairs of test name and optional iteration
# spec, runs the corresponding tests using the "runonly" syntax. an absence of
# an iteration spec is the same as a "*".

my $verbose = 0;
my $debug = 0;
my $test_user = 0;
my $test_system = 0;
GetOptions(
		"verbose" => \$verbose, "debug" => \$debug,
		"user" => \$test_user, "system" => \$test_system)
	or die "malformed command-line arguments";

die "must specify exactly one of --user or --system"
	unless $test_user xor $test_system;

-f 'run.sh' || die "not in the right directory";
die "this script's not very useful without at least one parameter."
	unless @ARGV;


sub parse_test_spec {
	my $spec = shift;
	if($spec =~ /(\w+)(:([-0-9,*]+))/) {
		return [ $1, [ parse_iters($3) ] ];
	} elsif($spec =~ /(\w+)/) {
		return [ $1, [ '*' ] ];
	} else {
		print STDERR "malformed test spec `$spec' ignored.\n";
		return;
	}
}

sub parse_iters {
	my $spec = shift;
	return '*' if $spec =~ /\*/;
	my @output;
	for(split /,/, $spec) {
		if(/(\d+)-(\d+)/) {
			for(my $i = int($1); $i <= int($2); $i++) {
				push @output, $i;
			}
		} elsif(/(\d+)/) {
			push @output, int($1);
		}
	}

	return @output;
}

sub combine_spec {
	my $s = shift;
	my $out = '';
	for(@{$s->[1]}) {
		$out .= '+' if $out;
		$out .= "$s->[0]:$_";
	}
	return $out;
}


my @tests = map { parse_test_spec($_); } @ARGV;
print STDERR @tests . " test specs\n" if $verbose;
if($debug) {
	for(@tests) {
		print STDERR $_->[0] . ", iters " . join(',', @{$_->[1]}) . "\n";
	}
}

my $envvar;
if($test_user) {
	die unless $test_user;
	$envvar = "UTEST_OPTS";
} else {
	die unless $test_system;
	$envvar = "SYSTEST_OPTS";
}
$ENV{$envvar} = "--run-only " . join('+', map { combine_spec($_) } @tests);
print STDERR "$envvar <- $ENV{$envvar}\n" if $verbose;
exec "./run.sh -display none 2>&1";
