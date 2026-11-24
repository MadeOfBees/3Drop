#!/usr/bin/env perl
# bridge.pl <ip:port> <port> <temp-suffix>: bridge.sh's FTP server, a process a session and a
# device.sh a command; device.sh holds the key, never perl. Core modules alone, so it runs wherever
# perl does (macOS, Debian's perl-base, Git Bash).
use strict;
use warnings;
use Fcntl qw(:flock);
use IO::Select;
use IO::Socket::INET;
use POSIX qw(WNOHANG);

my ($console, $port, $temp) = @ARGV;
my $pw = delete $ENV{THREEDROP_BRIDGE_PASSWORD};
my $dir = $ENV{THREEDROP_BRIDGE_DIR};
die "bridge.pl: tools/bridge.sh runs it\n" unless defined $temp && $pw && $dir && -d $dir;
(my $dev = $0) =~ s{[^/]*\z}{device.sh};
my $owner = $$;
END { local $?; system 'rm', '-rf', $dir if $$ == $owner }
# Its terminal closing (HUP) and Ctrl-\ (QUIT) stop it as Ctrl-C does, its scratch taken.
my @stops = qw(INT TERM HUP QUIT);
$SIG{$_} = sub { exit 130 } for @stops;
$SIG{PIPE} = 'IGNORE';
$SIG{CHLD} = 'IGNORE';

# Never ReuseAddr on Windows, where Cygwin then drops the exclusive bind that keeps another program
# off the port; elsewhere it lets a restart bind at once.
# A failure is the exit table's 1, never die's errno (48 for a port taken on macOS).
my $srv = IO::Socket::INET->new(Listen => 5, LocalAddr => '127.0.0.1', LocalPort => $port, Proto => 'tcp',
	ReuseAddr => $^O !~ /cygwin|msys|MSWin32/) or do { print STDERR "bridge: 127.0.0.1:$port: $!\n"; exit 1 };
# Written by no one: its end of file tells each session the bridge stopped.
pipe my $parent, my $alive or die "bridge: pipe: $!\n";
$| = 1;
print 'ftp://127.0.0.1:', $srv->sockport, " user 3drop password $pw\n";

# The session's control connection and its unread bytes, its state, the lines that came during a
# transfer, the device.sh running, and the last one's stdout and stderr.
my ($c, $in, $cwd, $type, $pasv, $rest, $rnfr, $auth, $child, $out, $err, @queue);
my @mon = qw(Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec);
# The refusal of REST before STOR, and of APPE: an upload resumes by itself, from the pieces kept.
my $from0 = q(an upload starts at byte 0: overwrite, and it goes on where a cut one stopped);

sub reply {
	my $l = join ' ', @_;
	$l =~ s/\n/\r\n/g;
	print STDERR "bridge: $$ < $l\n";
	syswrite $c, "$l\r\n";
}

# A device line's control bytes, CR from a native openssl too, never end a reply early.
sub clean { (my $s = shift) =~ s/[\x00-\x1f\x7f]+/ /g; $s }

sub quoted { (my $q = shift) =~ s/"/""/g; qq("$q") }

sub slurp {
	open my $f, '<:raw', shift or return '';
	local $/;
	<$f> // '';
}

# line [wait]: the client's next command, '' when none came within the wait, undef once it hung up or
# the bridge stopped; the Telnet interrupt a client may send ahead of ABOR is dropped.
sub line {
	my ($wait) = @_;
	while ($in !~ /\n/) {
		my @ready = IO::Select->new($c, $parent)->can_read($wait);
		return '' unless @ready;
		return undef if (grep { $_ == $parent } @ready) || length($in) > 4096 || !sysread($c, $in, 4096, length $in);
	}
	$in =~ s/\A([^\n]*)\n//;
	(my $l = $1) =~ s/\r\z//;
	$l =~ s/\xff[\xf0-\xff]?//g;
	$l;
}

# aborted [wait]: ABOR, or `gone` once the client hung up or the bridge stopped, within the wait (none
# by default), else ''; another command meanwhile waits its turn.
sub aborted {
	while (1) {
		my $l = line($_[0] // 0);
		return 'gone' unless defined $l;
		return 'ABOR' if $l =~ /^ABOR\b/i;
		return '' if $l eq '';
		push @queue, $l;
	}
}

sub path {
	my $p = shift // '';
	$p = "$cwd/$p" unless $p =~ m{^/};
	my @o;
	for (split m{/+}, $p) {
		if ($_ eq '..') { pop @o }
		elsif ($_ ne '' && $_ ne '.') { push @o, $_ }
	}
	'/' . join '/', @o;
}

sub status { $? & 127 ? 128 + ($? & 127) : $? >> 8 }

# spawn <stdin> <stdout> <verb> <args...>: device.sh's pid, a process group of its own so a stop reaches
# all it runs; without a stdin /dev/null, without a stdout the session's file, stderr another.
sub spawn {
	my ($i, $o, $verb, @args) = @_;
	my $f = "$dir/$$";
	my $pid = fork // return;
	if (!$pid) {
		setpgrp 0, 0;
		$SIG{PIPE} = 'DEFAULT'; # ignored, it would outlive exec: a writer to a closed pipe ends
		($i ? open(STDIN, '<&', $i) : open(STDIN, '<', '/dev/null'))
			and ($o ? open(STDOUT, '>&', $o) : open(STDOUT, '>', "$f.out"))
			and open(STDERR, '>', "$f.err") and exec 'bash', $dev, $verb, $console, @args;
		POSIX::_exit(127);
	}
	print STDERR "bridge: $$ device.sh $verb @args: pid $pid\n";
	$child = $pid;
}

# stop <pid>: TERM to its process group, whose traps take device.sh's scratch, then KILL to what is
# left: a stalled exchange would hold the console until its timeout.
sub stop {
	my ($pid) = @_;
	my $gone;
	kill 'TERM', -$pid;
	for (1 .. 50) {
		last if $gone = waitpid $pid, WNOHANG;
		select undef, undef, undef, 0.1;
	}
	kill 'KILL', -$pid;
	waitpid $pid, 0 unless $gone;
	undef $child;
	# A KILL runs no trap: its scratch stays until a later tool sweeps it.
	print STDERR "bridge: $$ stopped device.sh $pid", $gone ? '' : ' by KILL, 5 s after its TERM', "\n";
}

# dev <verb> <args...>: device.sh's status, its stdout in $out and stderr in $err.
sub dev {
	my $pid = spawn(undef, undef, @_);
	if (!$pid) {
		$err = "device.sh: fork: $!";
		return 4;
	}
	waitpid $pid, 0;
	my $st = status();
	undef $child;
	($out, $err) = (slurp("$dir/$$.out"), slurp("$dir/$$.err"));
	$st;
}

# watch <pid> [why]: a transfer's device.sh's status once it ends, its stderr in $err; with a why (the
# pump's: ABOR, gone, cut, quiet), or once ABOR comes or the client hangs up, stopped instead: undef,
# with ABOR answered and a broken data connection said.
sub watch {
	my ($pid, $why) = @_;
	my $st;
	until ($why) {
		if (waitpid $pid, WNOHANG) {
			$st = status();
			undef $child;
			$err = slurp("$dir/$$.err");
			return $st;
		}
		$why = aborted(0.25);
	}
	stop($pid);
	if ($why eq 'ABOR') {
		reply(426, 'the transfer was stopped');
		reply(226, 'ABOR done');
	}
	reply(426, 'the data connection broke') if $why eq 'cut';
	undef;
}

# verdict <status> [mid]: the reply for device.sh's failure, its words with it: 4xx where the same
# request can work later (FileZilla retries those, and gives up on 5xx), 552 where the card or FAT32
# has no room, 550 for the console's other refusals, and 426 once a transfer's data had begun.
sub verdict {
	my ($st, $mid) = @_;
	(my $why = $err) =~ s/^device: (response \S+ )?//mg;
	$why = clean(join '; ', grep /\S/, split /\n/, $why) || "device.sh ended $st";
	my $code = $st == 2 ? 501 : $st != 1 ? ($mid ? 426 : 451)
		: $why =~ /put rejected space\b|went past/ ? 552
		: $why =~ /put rejected size\b|rejected (stranded|ring|challenge|busy)\b|put (it )?again/ ? 450 : 550;
	($code, substr $why, 0, 400);
}

sub done { my $st = shift; reply($st ? verdict($st) : @_) }

# Busy is said at once, not past device.sh's timeouts; a LIST's is 451: curl takes 450 for empty.
sub held {
	return 0 if xlock(LOCK_SH);
	reply($_[0] // 450, 'a transfer holds the console: ask again once it ends');
	1;
}

# entry <arg> <flags>: (kind, size, mtime, name) as `ls <path> <flags>` gives it, or () once it replied.
sub entry {
	return () if held();
	my $st = dev('ls', path($_[0]), $_[1]);
	my @e = $st ? () : split / /, (split /\n/, $out)[0] // '', 4;
	return @e if @e == 4;
	$st ? done($st) : reply(550, 'the console listed no such entry');
	();
}

# when <mtime|->: the console's clock is its local time stored as UTC; this PC's zone stands in for it.
sub when { $_[0] =~ /^\d+\z/ ? POSIX::mktime((gmtime $_[0])[0 .. 5], 0, 0, -1) : time }

# A listing as `ls -l` prints it, which every client parses.
sub ls_line {
	my ($k, $size, $mt, $name) = @_;
	my $t = when($mt);
	my @t = localtime $t;
	my $hm = abs(time - $t) < 180 * 86400 ? sprintf('%02d:%02d', @t[2, 1]) : 1900 + $t[5];
	sprintf '%s 1 3ds 3ds %12s %s %2d %5s %s', $k eq 'd' ? 'drwxr-xr-x' : '-rw-r--r--', $k eq 'd' ? 0 : $size,
		$mon[$t[4]], $t[3], $hm, $name;
}

sub listen_data {
	close $pasv if $pasv;
	$pasv = IO::Socket::INET->new(Listen => 1, LocalAddr => '127.0.0.1', LocalPort => 0, Proto => 'tcp');
	return $pasv->sockport if $pasv;
	reply(425, "no data port: $!");
	0;
}

# accept_data: the client's data connection, after its 150; undef, said, when none came in 30 s.
sub accept_data {
	my $d = IO::Select->new($pasv)->can_read(30) ? $pasv->accept : undef;
	close $pasv;
	undef $pasv;
	if (!$d) {
		reply(425, 'no data connection came');
		return;
	}
	binmode $d;
	$d;
}

sub list {
	my ($arg, $names) = @_;
	$arg =~ s/^(?:-\S*\s*)+//; # ls options: LIST -la
	return if held(451);
	return reply(425, 'send PASV or EPSV first') unless $pasv;
	my $st = dev('ls', path($arg), 't');
	return done($st) if $st;
	my $body = join '', map {
		my @e = split / /, $_, 4;
		@e == 4 ? ($names ? $e[3] : ls_line(@e)) . "\r\n" : ()
	} split /\n/, $out;
	reply(150, 'listing');
	my $d = accept_data() or return;
	print {$d} $body;
	close $d;
	reply(226, 'listed');
}

# xlock <LOCK_EX|LOCK_SH>: the console's one transfer at a time, held while the handle lives; undef
# at once while another session holds it (a client's second transfer would wait out its timeout
# behind the first), or when it cannot be opened. Shared, two sessions never see each other busy.
sub xlock {
	open my $l, '>>', "$dir/transfer" or return;
	flock($l, $_[0] | LOCK_NB) ? $l : undef;
}

# ready <arg>: the path and the slot, once a transfer may start; () once it replied. The caller keeps
# the slot in a variable till it returns: the lock lives as long as the handle.
sub ready {
	my ($p, $slot) = (path(shift), undef);
	if (!$pasv) { reply(425, 'send PASV or EPSV first') }
	elsif ($type eq 'A') { reply(550, 'ASCII would rewrite line endings: set the client to Binary') }
	elsif (!($slot = xlock(LOCK_EX))) { reply(450, 'one transfer at a time, and another runs: set the client to one connection') }
	$slot ? ($p, $slot) : ();
}

# pump <from> <to>: bytes from one into the other, <to> a handle or what opens one at the first byte;
# `end` once <from> ends, else why it stopped first: ABOR, gone (the client hung up or the bridge
# stopped), cut (<from> broke) or write (<to> did).
sub pump {
	my ($from, $to) = @_;
	my $sel = IO::Select->new($from, $c, $parent);
	while (1) {
		my @ready = $sel->can_read;
		my $why = aborted();
		return $why if $why;
		next unless grep { $_ == $from } @ready;
		my $n = sysread $from, my $b, 1 << 20;
		return defined $n ? 'end' : 'cut' unless $n;
		$to = $to->() || return 'write' if ref $to eq 'CODE';
		for (my $o = 0; $o < $n;) {
			my $m = syswrite $to, $b, $n - $o, $o or return 'write';
			$o += $m;
		}
	}
}

sub retr {
	my $from = $rest;
	$rest = 0;
	my ($p, $slot) = ready(@_) or return;
	pipe my $r, my $w or return reply(451, "pipe: $!");
	my $pid = spawn(undef, $w, 'get', $p, '-', $from ? ('from', $from) : ()) or return reply(451, "fork: $!");
	close $w;
	# The 150 waits for the first verified bytes, so a refusal before them answers RETR itself.
	my $d;
	my $open = sub { reply(150, "getting $p"); $d = accept_data() };
	my $why = pump($r, $open);
	# A data connection the client closed broke; one it never opened was answered.
	$why = $why eq 'end' ? '' : $why ne 'write' ? $why : $d ? 'cut' : 'quiet';
	my $st = watch($pid, $why) // return;
	return if !$st && !$d && !$open->(); # an empty file, whose no byte opened it
	close $d if $d;
	reply($st ? verdict($st, $d) : (226, "got $p"));
}

sub stor {
	if ($rest) {
		$rest = 0;
		return reply(502, $from0);
	}
	my ($p, $slot) = ready(@_) or return;
	reply(150, "putting $p");
	my $d = accept_data() or return;
	pipe my $r, my $w or return reply(451, "pipe: $!");
	# device.sh publishes at stdin's end only once this marks it; a KILLed session leaves it unmarked.
	local $ENV{THREEDROP_BRIDGE_END} = "$dir/$$.end";
	unlink $ENV{THREEDROP_BRIDGE_END};
	my $pid = spawn($r, undef, 'put', '-', $p) or return reply(451, "fork: $!");
	close $r;
	# device.sh takes stdin's end for the file's, so an end that comes with a hang-up or an ABOR (a
	# client quitting closes both) stops it instead: its whole pieces stay for a retry to go on from.
	my $why = pump($d, $w);
	$why = '' if $why eq 'end' || $why eq 'write';
	if (!$why) { # the file's end: its last piece goes, which publishes it
		open my $end, '>', $ENV{THREEDROP_BRIDGE_END};
		close $w;
	}
	my $st = watch($pid, $why) // return;
	return reply(226, "put $p") unless $st;
	my ($code, $words) = verdict($st, 1);
	# Its temp would hold the card's room, or the bytes of another upload this path could not go on from.
	if (($code == 552 || $words =~ /other bytes/) && !dev('delete', "$p$temp")) {
		$words = join '; ', (grep { !/^the console keeps \d+ bytes of it in / } split /; /, $words), "$p$temp, a cut upload's pieces, is deleted";
	}
	reply($code, $words);
}

my %cmd;
%cmd = (
	USER => sub { $auth = 0; reply(331, 'the password bridge.sh printed') },
	PASS => sub { $auth = $_[0] eq $pw; reply($auth ? (230, 'logged in') : (530, 'not the password bridge.sh printed')) },
	SYST => sub { reply(215, 'UNIX Type: L8') },
	FEAT => sub { reply("211-Features:\n SIZE\n MDTM\n REST STREAM\n UTF8\n EPSV\n PASV\n211 End") },
	OPTS => sub { reply($_[0] =~ /^UTF8 ON\z/i ? (200, 'UTF-8 always') : (501, 'no such option')) },
	NOOP => sub { reply(200, 'ok') },
	ALLO => sub { reply(202, 'nothing to set aside') },
	MODE => sub { reply($_[0] =~ /^S\z/i ? (200, 'stream') : (504, 'stream mode only')) },
	STRU => sub { reply($_[0] =~ /^F\z/i ? (200, 'file') : (504, 'file structure only')) },
	# A is taken for listings, which curl asks for in it; a transfer in it is refused (ready).
	TYPE => sub {
		return reply(504, 'TYPE takes I or A') unless $_[0] =~ /^([AI])(?: N)?\z|^L ?8\z/i;
		$type = uc($1 // 'I');
		reply(200, "type $type");
	},
	PWD => sub { reply(257, quoted($cwd) . ' is the current folder') },
	# Taken unchecked while a transfer holds the console: the next listing there says if it is not.
	CWD => sub {
		my $p = path($_[0]);
		my @e = $p eq '/' || !xlock(LOCK_SH) ? ('d') : entry($p, 'd') or return;
		return reply(550, "$p is not a folder") unless $e[0] eq 'd';
		$cwd = $p;
		reply(250, quoted($p));
	},
	CDUP => sub { $cmd{CWD}->('..') },
	PASV => sub { my $pp = listen_data() or return; reply(227, sprintf 'Entering Passive Mode (127,0,0,1,%d,%d)', $pp >> 8, $pp & 255) },
	EPSV => sub { my $pp = listen_data() or return; reply(229, "Entering Extended Passive Mode (|||$pp|)") },
	PORT => sub { reply(502, 'passive mode only: PASV or EPSV') },
	LIST => sub { list($_[0], 0) },
	NLST => sub { list($_[0], 1) },
	SIZE => sub {
		my @e = entry($_[0], 'd') or return;
		$e[0] eq 'f' ? reply(213, $e[1]) : reply(550, 'a folder has no size');
	},
	MDTM => sub {
		my @e = entry($_[0], 'dt') or return;
		my @t = gmtime when($e[2]);
		reply(213, sprintf '%04d%02d%02d%02d%02d%02d', 1900 + $t[5], 1 + $t[4], @t[3, 2, 1, 0]);
	},
	REST => sub {
		return reply(501, 'REST takes a byte count') unless $_[0] =~ /^\d{1,18}\z/;
		($rest = $_[0]) =~ s/^0+(?=\d)//;
		reply(350, "from byte $rest");
	},
	RETR => \&retr,
	STOR => \&stor,
	APPE => sub { reply(502, $from0) },
	DELE => sub { held() or done(dev('delete', path($_[0])), 250, 'deleted') },
	RMD => sub { held() or done(dev('delete', path($_[0])), 250, 'removed') },
	MKD => sub { my $p = path($_[0]); held() or done(dev('mkdir', $p), 257, quoted($p) . ' made') },
	RNFR => sub { $rnfr = path($_[0]); reply(350, 'send RNTO') },
	RNTO => sub {
		return reply(503, 'send RNFR first') unless defined $rnfr;
		my $from = $rnfr;
		undef $rnfr;
		held() or done(dev('rename', $from, path($_[0])), 250, 'renamed');
	},
	ABOR => sub { reply(226, 'no transfer to abort') },
);
$cmd{"X$_"} = $cmd{$_} for qw(PWD CWD MKD RMD);
$cmd{XCUP} = $cmd{CDUP};
$cmd{EPRT} = $cmd{PORT};

sub session {
	($c) = @_;
	binmode $c;
	$SIG{CHLD} = 'DEFAULT';
	# A session killed by one stops device.sh first, as ABOR does: no upload runs on without its client.
	$SIG{$_} = sub { stop($child) if $child; exit 130 } for @stops;
	# So does one that dies of an error, as it unwinds (an eval's die is not the session's).
	$SIG{__DIE__} = sub { stop($child) if $child && defined $^S && !$^S };
	($in, $cwd, $type, $rest, $auth, $err) = ('', '/', 'I', 0, 0, '');
	reply(220, "3Drop bridge to the console at $console");
	while (defined(my $l = @queue ? shift @queue : line())) {
		my ($verb, $arg) = $l =~ /^(\S+) ?(.*)\z/s or next;
		$verb = uc $verb;
		print STDERR "bridge: $$ > ", $verb eq 'PASS' ? 'PASS ***' : $l, "\n";
		last if $verb eq 'QUIT' && reply(221, 'bye');
		if ($l =~ /\0/) { reply(501, 'no NUL in a command') }
		elsif (!$cmd{$verb}) { reply(502, "$verb is not implemented") }
		elsif (!$auth && $verb !~ /^(USER|PASS|SYST|FEAT|OPTS|NOOP)\z/) { reply(530, 'log in first') }
		elsif ($arg eq '' && $verb =~ /^(DELE|RMD|XRMD|RNFR)\z/) { reply(501, "$verb needs a path") }
		else { $cmd{$verb}->($arg) }
	}
	unlink "$dir/$$.out", "$dir/$$.err", "$dir/$$.end";
}

while (1) {
	my $client = $srv->accept or next;
	my $pid = fork;
	if (defined $pid && !$pid) {
		close $srv;
		close $alive;
		session($client);
		exit 0;
	}
	close $client;
}
