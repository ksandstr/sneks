
What
====

This is `sneks', an experimental operating system for the x86 architecture
built on a L4.X2 microkernel. The goal is to develop a POSIX-compatible set of
operating system services and libraries so as to eventually run most of the
GNU userspace.


Licensing
---------

`sneks' is licensed under the GNU GPL version 3 or, at your convenience, a
later version published by the FSF. A copy of GPLv3 is found in the COPYING
file in this directory.

In practice, many of the files making up the source tree are licensed under
weaker terms which nonetheless permit aggregation under a GPLv3+ rubric.
Therefore, any file that lacks an explicit statement of license is to be
regarded as licensed under GPLv3+ terms; and files that have such a statement
describe their own licensing. Modifications to source files are licensed under
either each file's licensing terms (as described in the previous), or one
that's weaker but compatible.


Goals
-----

It's intended that eventually various advantages of IPC architecture (as
dictated by use of a microkernel) will be put to use towards latency
minimization, compartmentalization of poorly-behaved software ("the JavaScript
question"), and bold new approaches to distributed and concurrent data access.

That's to say: IPC scheduling should make the entire processing chain from an
input device driver, through an X server, to user application, and back
through the X server and into the graphics stack, execute as though it were a
single program. In particular that program should run without taking fifty
milliseconds to spin an advertiser's browser malware in the background every
1200th time. Similarly, the UNIX directory tree should exhibit atomicity,
consistency, isolation, and durability regardless of how its component storage
volumes are set up.

However, since IPC scheduling is easily undone by concurrency-obstructing
primitives in the chain members, unorthodox methods are required. In sneks'
case, we'll implement basic services single-threaded so that they'll always
terminate when CPU time is available, while applying lock-free/wait-free
and/or transactional primitives where gains can be had from executing multiple
hardware threads concurrently.

This is expected to take years upon years; in the meantime just run GNU/Linux.


How?
----

Beats me, it's way under development. Check this section out again once
virtual memory, filesystems, block device access, and so forth have been
added. The Makefiles etc. are customizable but the source tree has inobvious
dependencies to various bits from a mix of Devuan's `jessie' and `ascii'
releases, so any actual use of the source tree may be challenging for the time
being.


### What's your target audience? ###

Neckbeards.


#### ... and target platform? ####

For now, the KVM virtualizer of Linux. Ultimately, ThinkPad laptops between
the generations of T41p and W520 models, inclusive.

In terms of processor architecture, sneks will support PC-style ia32 and amd64
as first priority; no other platform shall retard development.


### Ooh I like the sound of that. Can I chip in? ###

Sadly, visionary nonsense like this isn't for newbies or the easily
intimidated, and for practical reasons this project is not here to teach text
editors or typing. Get good, then we'll talk.

In the future there may be opportunities for non-core efforts associated with
`sneks', such as maintenance and development of the test harness or the IDL
compiler, porting and maintenance of drivers from Linux and elsewhere,
interfacing with the GNU project, maintenance of non-x86 platform support,
targets in the "non-ThinkPad or bad ThinkPad" set, and so forth. But for now,
there aren't.

Of course I can't stop you from coming up with your own weird-arse
experiments. Go right ahead, make my day.


### But what's with the name? ###

It's Volapük for the plural of snake, after the national sport of Finland.


### Something about this bothers me personally! ###

That's by design. Go run NetBSD instead.


  -- Kalle A. Sandström <ksandstr@iki.fi>
