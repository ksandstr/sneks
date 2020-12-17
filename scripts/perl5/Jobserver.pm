package Jobserver;
use Moo;
use IO::Handle;
use IO::Select;

# NOTE: don't call system() while jobs are in flight, i.e. until ->waitall has
# returned. the SIGCHLD handler will reap them and confuse the Perl runtime.
# also don't have jobs in flight on multiple instances of Jobserver at once,
# for the same reason.

# TODO: this doesn't return jobserver tokens on unexpected exit. that should
# be set up for with SIGINT handlers and so forth, as described in the ``POSIX
# Jobserver'' part of the GNU make manual.

has 'concurrency' => ( is => 'rwp' ); # MAKEFLAGS -jNNN, && jobserver present
has 'dryrun' => ( is => 'ro' ); # set if -n passed to make

# privates
has [qw/_jobw _jobr/] => ( is => 'rw' ); # IO::Handle, write and read sides
has 'oldchld' => ( is => 'rw' );
has [qw/exitstatus/] => ( is => 'ro', default => sub { [] } );
has 'children' => ( is => 'ro', default => sub { {} } );
has 'implicit' => ( is => 'rw', default => sub { 0 } );	# implicit token PID


sub BUILD {
	my ($self, $args) = @_;
	my ($conc, $jobserver);
	for (split /\s/, $ENV{MAKEFLAGS} // '') {
		if(/^-j(\d+)$/) {
			$conc = $1;
		} elsif(/^--jobserver-auth=(\d+),(\d+)$/) {
			$jobserver = [ $1, $2 ];
		}
	}
	if(!$args->{dryrun}) {
		$args->{dryrun} = ((split /\s/, $ENV{MAKEFLAGS})[0] =~ /n/);
	}
	if($jobserver && $conc) {
		$self->_set_concurrency($conc);
		$self->_jobr(IO::Handle->new_from_fd($jobserver->[0], 'r'));
		$self->_jobw(IO::Handle->new_from_fd($jobserver->[1], 'w'));
	}
}


sub chld {
	my ($self, $dead, $status) = @_;
	return if $dead < 0;	# spurious

	my $c = $self->children->{$dead};
	if(!$c) {
		warn "dead child pid=$dead not found?";
		return;
	}
	delete $self->children->{$dead};
	if($self->implicit == $dead) {
		die if ref($c->{token}) ne 'ARRAY';
		$self->implicit(0);
	} else {
		die if ref($c->{token}) eq 'ARRAY';
		if(!$self->_jobw->syswrite($c->{token}, 1)) {
			warn "jobserver release for pid=$dead failed: $!";
		}
	}
	delete $c->{token};
	$c->{status} = $status;
	$c->{code} = $status & 0xff;
	push @{$self->exitstatus}, $c;
}


sub get_token {
	my $self = shift;

	return [] unless $self->implicit > 0;
	my $token;
	while(!$token && $self->implicit > 0) {
		my $s = IO::Select->new;
		$s->add($self->_jobr);
		my @ready = $s->can_read;
		next unless @ready;

		my $n = $self->_jobr->sysread($token, 1);
		if(!defined $n || $n < 0) {
			next if $! =~ /temporarily unavailable/;
			warn "in sysread, $!";
			undef $token;
			last;
		}
	}
	if($self->implicit == 0 && !$token) {
		# implicit slot was released during wait, and an actual token not yet
		# assigned; use the implicit slot.
		return [];
	}
	return $token;
}


sub spawn {
	my $self = shift;
	# rest of @_ passed to exec (or system, as may happen)

	if($self->dryrun) {
		print STDERR "dry run: @_\n";
		return 0;
	}

	if(!$self->concurrency) {
		die if $self->oldchld;
		my $ec = system @_;
		push @{$self->exitstatus}, { pid => 0, status => $?, code => $ec };
	} else {
		my $token = $self->get_token;
		if(!$token) {
			print STDERR "jobserver EOF'd?\n";
			$self->waitall;
			# drop the jobsewer and redo from start
			$self->_set_concurrency(0);
			$self->_jobr->close;
			$self->_jobw->close;
			return $self->spawn(@_);
		}
		my $cs = $self->children;
		if(!%$cs && !$self->oldchld) {
			# first child; replace chld handler. restored in ->waitall.
			$self->oldchld($SIG{CHLD});
			$SIG{CHLD} = sub { $self->chld(wait, $?); };
		}
		my $cpid = fork;
		exec @_ unless $cpid > 0;
		$cs->{$cpid} = { token => $token };
		if(ref($token) eq 'ARRAY') {
			die if $self->implicit > 0;
			$self->implicit($cpid);
		}
	}
}


sub waitall {
	my $self = shift;

	$self->chld(wait, $?) while(%{$self->children});
	if($self->oldchld) {
		$SIG{CHLD} = $self->oldchld;
		$self->oldchld(undef);
	}
}


1;
