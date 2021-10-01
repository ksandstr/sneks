#!/usr/bin/perl
use Modern::Perl '2020';
use IO::Dir;
tie my %dir, 'IO::Dir', '.';
DIR: for my $up (keys %dir) {
	next unless -d $up;
	for(qw/. .. tools hostinclude scripts/) {
		next DIR if $up eq $_;
	}
	say ": foreach $up/*.c |> !cc |> $up/%B.o {tests}";
}
