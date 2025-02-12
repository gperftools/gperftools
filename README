gperftools
----------
(originally Google Performance Tools)

OVERVIEW
---------

gperftools is a collection of a high-performance multi-threaded
malloc() implementation, plus some pretty nifty performance analysis
tools.

gperftools is distributed under the terms of the BSD License. Join our
mailing list at gperftools@googlegroups.com for updates:
https://groups.google.com/forum/#!forum/gperftools

gperftools was original home for pprof program. The original pprof was
a Perl script and there is not that many Perl experts
nowadays. Thankfully, pprof has been rewritten in Go around 2016 and
that version of pprof has gotten a lot more feature-ful. So the
original pprof is now removed in favor of Go version at
https://github.com/google/pprof. Go's own profiling facilities are
based on that pprof version as well.


TCMALLOC
--------
Just link in -ltcmalloc or -ltcmalloc_minimal to get the advantages of
tcmalloc -- a replacement for malloc and new.  See below for some
environment variables you can use with tcmalloc, as well.

tcmalloc functionality is available on all systems we've tested; see
INSTALL for more details.  See README_windows.txt for instructions on
using tcmalloc on Windows.


HEAP PROFILER
-------------
See docs/heapprofile.adoc for information about how to use tcmalloc's
heap profiler and analyze its output.

As a quick-start, do the following after installing this package:

1) Link your executable with -ltcmalloc
2) Run your executable with the HEAPPROFILE environment var set:
     $ HEAPPROFILE=/tmp/heapprof <path/to/binary> [binary args]
3) Run pprof to analyze the heap usage
     $ pprof <path/to/binary> /tmp/heapprof.0045.heap  # run 'ls' to see options
     $ pprof --gv <path/to/binary> /tmp/heapprof.0045.heap

You can also use LD_PRELOAD to heap-profile an executable that you
didn't compile.

There are other environment variables, besides HEAPPROFILE, you can
set to adjust the heap-profiler behavior; c.f. "ENVIRONMENT VARIABLES"
below.

The heap profiler is available on all unix-based systems we've tested;
see INSTALL for more details.  It is not currently available on Windows.


CPU PROFILER
------------
See docs/cpuprofile.adoc for information about how to use the CPU
profiler and analyze its output.

As a quick-start, do the following after installing this package:

1) Link your executable with -lprofiler
2) Run your executable with the CPUPROFILE environment var set:
     $ CPUPROFILE=/tmp/prof.out <path/to/binary> [binary args]
3) Run pprof to analyze the CPU usage
     $ pprof <path/to/binary> /tmp/prof.out      # -pg-like text output
     $ pprof --gv <path/to/binary> /tmp/prof.out # really cool graphical output

There are other environment variables, besides CPUPROFILE, you can set
to adjust the cpu-profiler behavior; cf "ENVIRONMENT VARIABLES" below.

The CPU profiler is available on all unix-based systems we've tested;
see INSTALL for more details.  It is not currently available on Windows.

NOTE: CPU profiling doesn't work after fork (unless you immediately
      do an exec()-like call afterwards).  Furthermore, if you do
      fork, and the child calls exit(), it may corrupt the profile
      data.  You can use _exit() to work around this.  We hope to have
      a fix for both problems in the next release of perftools
      (hopefully perftools 1.2).


CONFIGURATION OPTIONS
---------------------
For advanced users, there are several flags you can pass to
'./configure' that tweak tcmalloc performance.  (These are in addition
to the environment variables you can set at runtime to affect
tcmalloc, described below.)  See the INSTALL file for details.


ENVIRONMENT VARIABLES
---------------------
The cpu profiler, heap checker, and heap profiler will lie dormant,
using no memory or CPU, until you turn them on.  (Thus, there's no
harm in linking -lprofiler into every application, and also -ltcmalloc
assuming you're ok using the non-libc malloc library.)

The easiest way to turn them on is by setting the appropriate
environment variables.  We have several variables that let you
enable/disable features as well as tweak parameters.

Here are some of the most important variables:

HEAPPROFILE=<pre> -- turns on heap profiling and dumps data using this prefix
HEAPCHECK=<type>  -- turns on heap checking with strictness 'type'
CPUPROFILE=<file> -- turns on cpu profiling and dumps data to this file.
PROFILESELECTED=1 -- if set, cpu-profiler will only profile regions of code
                     surrounded with ProfilerEnable()/ProfilerDisable().
CPUPROFILE_FREQUENCY=x-- how many interrupts/second the cpu-profiler samples.

PERFTOOLS_VERBOSE=<level> -- the higher level, the more messages malloc emits
MALLOCSTATS=<level>    -- prints memory-use stats at program-exit

For a full list of variables, see the documentation pages:
   docs/cpuprofile.adoc
   docs/heapprofile.adoc

See also TCMALLOC_STACKTRACE_METHOD_VERBOSE and
TCMALLOC_STACKTRACE_METHOD environment variables briefly documented in
our INSTALL file and on our wiki page at:
https://github.com/gperftools/gperftools/wiki/gperftools'-stacktrace-capturing-methods-and-their-issues


COMPILING ON NON-LINUX SYSTEMS
------------------------------

Perftools was developed and tested on x86, aarch64 and riscv Linux
systems, and it works in its full generality only on those systems.

However, we've successfully ported much of the tcmalloc library to
FreeBSD, Solaris x86 (not tested recently though), and Mac OS X
(aarch64; x86 and ppc have not been tested recently); and we've ported
the basic functionality in tcmalloc_minimal to Windows.  See INSTALL
for details.  See README_windows.txt for details on the Windows port.


---
Originally written: 17 May 2011
Last refreshed: 10 Aug 2023
