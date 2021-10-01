#!/usr/bin/perl
use Modern::Perl '2020';
use IO::Dir;
my $path = shift @ARGV // '.';
tie my %sys, 'IO::Dir', $path;
for my $base (keys %sys) {
	next if $base =~ /^\./;
	next unless -d "$path/$base/t";
	say ": foreach $path/$base/t/*.c |> !cc |> $path/$base/t/%g.o {systests}";
}
