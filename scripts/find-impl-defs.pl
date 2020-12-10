#!/usr/bin/perl
use Modern::Perl '2020';
use IO::File;


sub find_impl_defs {
	my $filename = shift;
	my $fh = IO::File->new("< $filename") || die "can't open $filename: $!";
	my @names;
	while(<$fh>) {
		last if /^[{}]/; # stops at end of first function, struct, or union
		push @names, $1 if /^#include\s+"(\w+-impl-defs\.h)"$/;
	}
	$fh->close;
	return @names;
}


my $dir_present = -d '.deps';
for my $filename (<*.c>) {
	# don't crawl the file if deps already exist, such as when the tree was
	# already built.
	my $depsname = ".deps/$filename";
	$depsname =~ s/\.c$/.d/;
	next if -f $depsname;

	my @defs = find_impl_defs($filename);
	next unless @defs;
	if(!$dir_present) {
		mkdir '.deps' || die "can't mkdir .deps: $!";
		$dir_present = 1;
	}
	my $dfh = IO::File->new("> $depsname") || die "can't open $depsname: $!";
	my $objname = $filename;
	$objname =~ s/\.c$/.o/;
	# output a rudimentary dependency spec for this.
	print $dfh "$objname: $filename @defs\n";
	$dfh->close;
}
