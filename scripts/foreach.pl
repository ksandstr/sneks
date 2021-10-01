#!/usr/bin/perl
use strict;
# this script runs very often and represents a 10 ms CPU charge on a 1.6ghz
# mobile core 2 (x61s), so over 3 seconds for the 300-ish jobs to completely
# rebuild all of sneks. it would be very good to have the same function
# already within "tup", much as Make has $(foreach).
my $pattern = shift @ARGV // die "no pattern";
for my $arg (@ARGV) {
	my $tmp = $pattern;
	$tmp =~ s/\+/$arg/;
	print "$tmp\n";
}
