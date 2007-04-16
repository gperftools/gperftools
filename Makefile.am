## Process this file with automake to produce Makefile.in

# Note: for every library we create, we're explicit about what symbols
# we export.  In order to avoid complications with C++ mangling, we always
# use the regexp for of specifying symbols.

# Make sure that when we re-make ./configure, we get the macros we need
ACLOCAL_AMFLAGS = -I `pwd`/../autoconf

# This is so we can #include <google/foo>
AM_CPPFLAGS = -I$(top_srcdir)/src
# This is initialized in configure.ac (though we might add to it later)
AM_CXXFLAGS = $(EXTRA_CXXFLAGS)

googleincludedir = $(includedir)/google
# The .h files you want to install (that is, .h files that people
# who install this package can include in their own applications.)
# We'll add to this later, on a library-by-library basis
googleinclude_HEADERS =

perftoolsincludedir = $(googleincludedir)/perftools
# The 'private' header files go into /usr/local/include/google/perftools.
# We'll add to this later, on a library-by-library basis
perftoolsinclude_HEADERS =

docdir = $(prefix)/share/doc/$(PACKAGE)-$(VERSION)
# This is for HTML and other documentation you want to install.
# Add your documentation files (in doc/) in addition to these
# top-level boilerplate files.  Also add a TODO file if you have one.
# We'll add to this later, on a library-by-library basis
dist_doc_DATA = AUTHORS COPYING ChangeLog INSTALL NEWS README TODO

# The libraries (.so's) you want to install
# We'll add to this later, on a library-by-library basis
lib_LTLIBRARIES =
# This is for 'convenience libraries' -- basically just a container for sources
noinst_LTLIBRARIES =

# unittests you want to run when people type 'make check'.
# Note: tests cannot take any arguments!
# In theory, unittests that are scripts should be added to check_SCRIPTS
# instead.  But check_SCRIPTS is definitely a second-class testing mechanims:
# it don't get TESTS_ENVIRONMENT, and it doesn't get success/failure counting
# (in fact, a script failure aborts all the rest of the tests, even with -k).
# So, for scripts, we add the script to tests, and also put in an empty
# rule so automake doesn't try to build the script as a C binary.
TESTS =
# TESTS_ENVIRONMENT sets environment variables for when you run unittest.
# We always get "srcdir" set for free.
# We'll add to this later, on a library-by-library basis.
TESTS_ENVIRONMENT =
# All script tests should be added her
noinst_SCRIPTS =
# If your test calls another program that, like the test itself, shouldn't
# be installed, add it here.  (Stuff in TESTS is automatically added later).
noinst_PROGRAMS =


## vvvv RULES TO MAKE THE LIBRARIES, BINARIES, AND UNITTESTS

dist_doc_DATA += doc/index.html doc/designstyle.css


### ------- library routines, in src/base

# spinlock is the only code that uses atomicops.
SPINLOCK_INCLUDES = src/base/spinlock.h \
                    src/base/atomicops.h \
                    src/base/atomicops-internals-macosx.h \
                    src/base/atomicops-internals-x86-msvc.h \
                    src/base/atomicops-internals-x86.h

# This is a 'convenience library' -- it's not actually installed or anything
noinst_LTLIBRARIES += libspinlock.la
libspinlock_la_SOURCES = src/base/spinlock.cc \
                         src/base/atomicops-internals-x86.cc \
                         $(SPINLOCK_INCLUDES)

LOGGING_INCLUDES = src/base/logging.h \
                   src/base/commandlineflags.h \
                   src/base/basictypes.h
noinst_LTLIBRARIES += liblogging.la
liblogging_la_SOURCES = src/base/logging.cc \
                        $(LOGGING_INCLUDES)

### Unittests
TESTS += low_level_alloc_unittest
LOW_LEVEL_ALLOC_UNITTEST_INCLUDES = src/base/low_level_alloc.h \
                                    src/base/basictypes.h \
                                    src/google/malloc_hook.h \
                                    $(SPINLOCK_INCLUDES) \
                                    $(LOGGING_INCLUDES)
low_level_alloc_unittest_SOURCES = src/base/low_level_alloc.cc \
                                   src/malloc_hook.cc \
                                   src/tests/low_level_alloc_unittest.cc \
                                   $(LOW_LEVEL_ALLOC_UNITTEST_INCLUDES)
low_level_alloc_unittest_LDADD = libspinlock.la liblogging.la libstacktrace.la

TESTS += atomicops_unittest
ATOMICOPS_UNITTEST_INCLUDES = src/base/atomicops.h \
                              src/base/atomicops-internals-macosx.h \
                              src/base/atomicops-internals-x86-msvc.h \
                              src/base/atomicops-internals-x86.h \
                              $(LOGGING_INCLUDES)
atomicops_unittest_SOURCES = src/tests/atomicops_unittest.cc \
                             $(ATOMICOPS_UNITTEST_INCLUDES)
atomicops_unittest_LDADD = libspinlock.la liblogging.la


### ------- stack trace

### The header files we use.  We divide into categories based on directory
S_STACKTRACE_INCLUDES =
SG_STACKTRACE_INCLUDES = src/google/stacktrace.h
SGP_STACKTRACE_INCLUDES = src/stacktrace_generic-inl.h \
			  src/stacktrace_libunwind-inl.h \
			  src/stacktrace_x86_64-inl.h \
			  src/stacktrace_x86-inl.h
STACKTRACE_INCLUDES = $(S_STACKTRACE_INCLUDES) $(SG_STACKTRACE_INCLUDES) $(SGP_STACKTRACE_INCLUDES)
googleinclude_HEADERS += $(SG_STACKTRACE_INCLUDES)
perftoolsinclude_HEADERS += $(SGP_STACKTRACE_INCLUDES)

### Making the library
lib_LTLIBRARIES += libstacktrace.la
libstacktrace_la_SOURCES = src/stacktrace.cc \
                           $(STACKTRACE_INCLUDES)
# TODO(csilvers): only add these two things when stacktrace.cc would
#                 #include "stacktrace_libunwind-inl.h"
libstacktrace_la_LIBADD = $(UNWIND_LIBS) libspinlock.la
STACKTRACE_SYMBOLS = '(GetStackTrace)'
libstacktrace_la_LDFLAGS = -export-symbols-regex $(STACKTRACE_SYMBOLS)

### Unittests
TESTS += stacktrace_unittest
STACKTRACE_UNITTEST_INLCUDES = src/base/commandlineflags.h \
                               $(STACKTRACE_INCLUDES) \
                               $(LOGGING_INCLUDES)
stacktrace_unittest_SOURCES = src/tests/stacktrace_unittest.cc \
                              $(STACKTRACE_UNITTEST_INLCUDES)
stacktrace_unittest_LDADD = libstacktrace.la liblogging.la

### Documentation
dist_doc_DATA +=


### ------- pprof

bin_SCRIPTS = src/pprof

### Unittests
check_SCRIPTS = pprof_unittest
pprof_unittest: $(top_srcdir)/src/pprof
	$< -test

# Let unittests find pprof if they need to run it
TESTS_ENVIRONMENT += PPROF_PATH=$(top_srcdir)/src/pprof

### Documentation
dist_man_MANS = doc/pprof.1
dist_doc_DATA += doc/pprof_remote_servers.html


### ------- tcmalloc_minimal (thread-caching malloc)

### The header files we use.  We divide into categories based on directory
S_TCMALLOC_MINIMAL_INCLUDES = src/internal_logging.h \
                              src/system-alloc.h \
                              $(SPINLOCK_INCLUDES) \
                              src/base/commandlineflags.h \
                              src/base/basictypes.h \
                              src/pagemap.h \
                              src/maybe_threads.h
SG_TCMALLOC_MINIMAL_INCLUDES = src/google/malloc_hook.h \
                               src/google/malloc_extension.h \
                               src/google/stacktrace.h
SGP_TCMALLOC_MINIMAL_INCLUDES = src/google/perftools/hash_set.h
TCMALLOC_MINIMAL_INCLUDES = $(S_TCMALLOC_MINIMAL_INCLUDES) $(SG_TCMALLOC_MINIMAL_INCLUDES) $(SGP_TCMALLOC_MINIMAL_INCLUDES)
googleinclude_HEADERS += $(SG_TCMALLOC_MINIMAL_INCLUDES)
perftoolsinclude_HEADERS += $(SGP_TCMALLOC_MINIMAL_INCLUDES)

### Making the library
lib_LTLIBRARIES += libtcmalloc_minimal.la
libtcmalloc_minimal_la_SOURCES = src/internal_logging.cc \
                                 src/system-alloc.cc \
                                 src/tcmalloc.cc \
                                 src/malloc_hook.cc \
                                 src/malloc_extension.cc \
                                 src/maybe_threads.cc \
                                 $(TCMALLOC_MINIMAL_INCLUDES)
libtcmalloc_minimal_la_CXXFLAGS = $(PTHREAD_CFLAGS) -DNDEBUG
TCMALLOC_MINIMAL_SYMBOLS = '(malloc|free|realloc|calloc|cfree|memalign|valloc|pvalloc|posix_memalign|malloc_stats|MallocExtension|MallocHook)'
libtcmalloc_minimal_la_LDFLAGS = $(PTHREAD_CFLAGS) -export-symbols-regex $(TCMALLOC_MINIMAL_SYMBOLS)
libtcmalloc_minimal_la_LIBADD = $(PTHREAD_LIBS) \
                                libstacktrace.la libspinlock.la liblogging.la

### Unittests

# Commented out for the moment because malloc(very_big_num) is broken in
# standard libc!  At least, in some situations, some of the time.
## TESTS += malloc_unittest
## MALLOC_UNITEST_INCLUDES = src/google/malloc_extension.h \
##                           src/google/malloc_hook.h \
##                           src/base/basictypes.h \
##                           src/google/perftools/hash_set.h \
##                           src/maybe_threads.h
## malloc_unittest_SOURCES = src/tests/tcmalloc_unittest.cc \
##                           src/malloc_hook.cc \
##                           src/malloc_extension.cc \
##                           src/maybe_threads.cc \
##                           $(MALLOC_UNITTEST_INCLUDES)
## malloc_unittest_CXXFLAGS = $(PTHREAD_CFLAGS)
## malloc_unittest_LDFLAGS = $(PTHREAD_CFLAGS)
## malloc_unittest_LDADD = $(PTHREAD_LIBS)

TESTS += tcmalloc_unittest
TCMALLOC_UNITTEST_INCLUDES = src/google/malloc_extension.h
tcmalloc_unittest_SOURCES = src/tests/tcmalloc_unittest.cc \
                            $(TCMALLOC_UNITTEST_INCLUDES)
tcmalloc_unittest_CXXFLAGS = $(PTHREAD_CFLAGS)
tcmalloc_unittest_LDFLAGS = $(PTHREAD_CFLAGS)
tcmalloc_unittest_LDADD = libtcmalloc.la liblogging.la $(PTHREAD_LIBS)

TESTS += tcmalloc_minimal_unittest
tcmalloc_minimal_unittest_SOURCES = src/tests/tcmalloc_unittest.cc \
                                    $(TCMALLOC_UNITTEST_INCLUDES)
tcmalloc_minimal_unittest_CXXFLAGS = $(PTHREAD_CFLAGS)
tcmalloc_minimal_unittest_LDFLAGS = $(PTHREAD_CFLAGS)
tcmalloc_minimal_unittest_LDADD = libtcmalloc_minimal.la \
                                  liblogging.la $(PTHREAD_LIBS)

TESTS += tcmalloc_large_unittest
tcmalloc_large_unittest_SOURCES = src/tests/tcmalloc_large_unittest.cc
tcmalloc_large_unittest_CXXFLAGS = $(PTHREAD_CFLAGS)
tcmalloc_large_unittest_LDFLAGS = $(PTHREAD_CFLAGS)
tcmalloc_large_unittest_LDADD = libtcmalloc.la $(PTHREAD_LIBS)

TESTS += tcmalloc_minimal_large_unittest
tcmalloc_minimal_large_unittest_SOURCES = src/tests/tcmalloc_large_unittest.cc
tcmalloc_minimal_large_unittest_CXXFLAGS = $(PTHREAD_CFLAGS)
tcmalloc_minimal_large_unittest_LDFLAGS = $(PTHREAD_CFLAGS)
tcmalloc_minimal_large_unittest_LDADD = libtcmalloc_minimal.la $(PTHREAD_LIBS)

# performance/unittests originally from ptmalloc2
TESTS += ptmalloc_unittest1 ptmalloc_unittest2
PTMALLOC_UNITTEST_INCLUDES = src/tests/ptmalloc/t-test.h \
                             src/tests/ptmalloc/thread-m.h \
                             src/tests/ptmalloc/lran2.h \
                             src/tests/ptmalloc/thread-st.h \
                             src/tests/ptmalloc/malloc-machine.h
ptmalloc_unittest1_SOURCES = src/tests/ptmalloc/t-test1.c \
                             $(PTMALLOC_UNITTEST_INCLUDES)
ptmalloc_unittest1_CFLAGS = $(PTHREAD_CFLAGS) -DUSE_PTHREADS
ptmalloc_unittest1_LDFLAGS = $(PTHREAD_CFLAGS)
ptmalloc_unittest1_LDADD = $(PTHREAD_LIBS)
ptmalloc_unittest2_SOURCES = src/tests/ptmalloc/t-test2.c \
                             $(PTMALLOC_UNITTEST_INCLUDES)
ptmalloc_unittest2_CFLAGS = $(PTHREAD_CFLAGS) -DUSE_PTHREADS
ptmalloc_unittest2_LDFLAGS = $(PTHREAD_CFLAGS)
ptmalloc_unittest2_LDADD = $(PTHREAD_LIBS)

TESTS += frag_unittest
frag_unittest_SOURCES = src/tests/frag_unittest.cc
frag_unittest_CXXFLAGS = $(PTHREAD_CFLAGS)
frag_unittest_LDFLAGS = $(PTHREAD_CFLAGS)
frag_unittest_LDADD = libtcmalloc_minimal.la $(PTHREAD_LIBS)

TESTS += markidle_unittest
markidle_unittest_SOURCES = src/tests/markidle_unittest.cc \
                            src/tests/testutil.h src/tests/testutil.cc
markidle_unittest_CXXFLAGS = $(PTHREAD_CFLAGS)
markidle_unittest_LDFLAGS = $(PTHREAD_CFLAGS)
markidle_unittest_LDADD = libtcmalloc_minimal.la $(PTHREAD_LIBS)

TESTS += memalign_unittest
memalign_unittest_SOURCES = src/tests/memalign_unittest.cc
memalign_unittest_CXXFLAGS = $(PTHREAD_CFLAGS)
memalign_unittest_LDFLAGS = $(PTHREAD_CFLAGS)
memalign_unittest_LDADD = libtcmalloc_minimal.la $(PTHREAD_LIBS)

TESTS += thread_dealloc_unittest
thread_dealloc_unittest_SOURCES = src/tests/thread_dealloc_unittest.cc \
                                  src/tests/testutil.h src/tests/testutil.cc
thread_dealloc_unittest_CXXFLAGS = $(PTHREAD_CFLAGS)
thread_dealloc_unittest_LDFLAGS = $(PTHREAD_CFLAGS)
thread_dealloc_unittest_LDADD = libtcmalloc_minimal.la $(PTHREAD_LIBS)

### Documentation
dist_doc_DATA += doc/tcmalloc.html \
                 doc/overview.gif \
                 doc/pageheap.gif \
                 doc/spanmap.gif \
                 doc/threadheap.gif \
                 doc/t-test1.times.txt \
                 doc/tcmalloc-opspercpusec.vs.threads.1024.bytes.png 	\
                 doc/tcmalloc-opspercpusec.vs.threads.128.bytes.png 	\
                 doc/tcmalloc-opspercpusec.vs.threads.131072.bytes.png 	\
                 doc/tcmalloc-opspercpusec.vs.threads.16384.bytes.png 	\
                 doc/tcmalloc-opspercpusec.vs.threads.2048.bytes.png 	\
                 doc/tcmalloc-opspercpusec.vs.threads.256.bytes.png 	\
                 doc/tcmalloc-opspercpusec.vs.threads.32768.bytes.png 	\
                 doc/tcmalloc-opspercpusec.vs.threads.4096.bytes.png 	\
                 doc/tcmalloc-opspercpusec.vs.threads.512.bytes.png 	\
                 doc/tcmalloc-opspercpusec.vs.threads.64.bytes.png 	\
                 doc/tcmalloc-opspercpusec.vs.threads.65536.bytes.png 	\
                 doc/tcmalloc-opspercpusec.vs.threads.8192.bytes.png 	\
                 doc/tcmalloc-opspersec.vs.size.1.threads.png 		\
                 doc/tcmalloc-opspersec.vs.size.12.threads.png 		\
                 doc/tcmalloc-opspersec.vs.size.16.threads.png 		\
                 doc/tcmalloc-opspersec.vs.size.2.threads.png 		\
                 doc/tcmalloc-opspersec.vs.size.20.threads.png 		\
                 doc/tcmalloc-opspersec.vs.size.3.threads.png 		\
                 doc/tcmalloc-opspersec.vs.size.4.threads.png 		\
                 doc/tcmalloc-opspersec.vs.size.5.threads.png 		\
                 doc/tcmalloc-opspersec.vs.size.8.threads.png

# I don't know how to say "distribute the .dot files but don't install them";
# noinst doesn't seem to work with data.  I separate them out anyway, in case
# one day we figure it out.  Regardless, installing the dot files isn't the
# end of the world.
dist_doc_DATA += doc/overview.dot \
                 doc/pageheap.dot \
                 doc/spanmap.dot \
                 doc/threadheap.dot

### ------- tcmalloc (thread-caching malloc + heap profiler + heap checker)

### The header files we use.  We divide into categories based on directory
S_TCMALLOC_INCLUDES = src/internal_logging.h \
                      src/system-alloc.h \
                      src/pagemap.h \
                      src/addressmap-inl.h \
                      src/heap-profile-table.h \
                      src/base/basictypes.h \
                      src/base/commandlineflags.h \
                      src/base/googleinit.h \
                      src/base/elfcore.h \
                      src/base/linux_syscall_support.h \
                      src/base/linuxthreads.h \
                      src/base/thread_lister.h \
                      src/base/sysinfo.h \
                      src/base/stl_allocator.h \
                      src/maybe_threads.h \
                      $(SPINLOCK_INCLUDES) \
                      $(LOGGING_INCLUDES)
SG_TCMALLOC_INCLUDES = src/google/malloc_hook.h \
                       src/google/malloc_extension.h \
                       src/google/heap-profiler.h \
                       src/google/heap-checker.h \
                       src/google/stacktrace.h
SGP_TCMALLOC_INCLUDES = src/google/perftools/hash_set.h
TCMALLOC_INCLUDES = $(S_TCMALLOC_INCLUDES) $(SG_TCMALLOC_INCLUDES) $(SGP_TCMALLOC_INCLUDES)
googleinclude_HEADERS += $(SG_TCMALLOC_INCLUDES)
perftoolsinclude_HEADERS += $(SGP_TCMALLOC_INCLUDES)

### Making the library
lib_LTLIBRARIES += libtcmalloc.la
# Note: heap-checker-bcad is last, in hopes its global ctor will run first
libtcmalloc_la_SOURCES = src/internal_logging.cc \
                         src/system-alloc.cc \
                         src/tcmalloc.cc \
                         src/malloc_hook.cc \
                         src/malloc_extension.cc \
                         src/maybe_threads.cc \
                         src/memory_region_map.cc \
                         src/heap-profiler.cc \
                         src/heap-profile-table.cc \
                         src/heap-checker.cc \
                         src/base/linuxthreads.c \
                         src/base/thread_lister.c \
                         src/base/sysinfo.cc \
                         src/base/low_level_alloc.cc \
                         $(TCMALLOC_INCLUDES) \
                         src/heap-checker-bcad.cc
libtcmalloc_la_CXXFLAGS = $(PTHREAD_CFLAGS) -DNDEBUG
TCMALLOC_SYMBOLS = '(malloc|free|realloc|calloc|cfree|memalign|valloc|pvalloc|posix_memalign|malloc_stats|MallocExtension|MallocHook|HeapProfilerStart|HeapProfilerStop|HeapProfilerDump|GetHeapProfile|HeapCleaner|HeapLeakChecker)'
libtcmalloc_la_LDFLAGS = $(PTHREAD_CFLAGS) -export-symbols-regex $(TCMALLOC_SYMBOLS)
libtcmalloc_la_LIBADD = $(PTHREAD_LIBS) \
                        libstacktrace.la libspinlock.la liblogging.la

### Unittests

# These unittests often need to run binaries.  They're in the current dir
TESTS_ENVIRONMENT += BINDIR=.

TESTS += addressmap_unittest
ADDRESSMAP_UNITTEST_INCLUDES = src/addressmap-inl.h \
                               src/base/commandlineflags.h \
                               $(LOGGING_INCLUDES)
addressmap_unittest_SOURCES = src/tests/addressmap_unittest.cc \
                              $(ADDRESSMAP_UNITTEST_INCLUDES)
addressmap_unittest_CXXFLAGS = -g
addressmap_unittest_LDADD = liblogging.la

TESTS += heap-profiler_unittest.sh
heap_profiler_unittest_sh_SOURCES = src/tests/heap-profiler_unittest.sh
noinst_SCRIPTS += $(heap_profiler_unittest_sh_SOURCES)
heap-profiler_unittest.sh: $(heap_profiler_unittest_sh_SOURCES)
	cp -a $(heap_profiler_unittest_sh_SOURCES) $@

# These are sub-programs used by heap-profiler_unittest.sh
noinst_PROGRAMS += heap-profiler_unittest
HEAP_PROFILER_UNITTEST_INCLUDES = src/google/heap-profiler.h
heap_profiler_unittest_SOURCES = src/tests/heap-profiler_unittest.cc \
                                 $(HEAP_PROFILER_UNITTEST_INCLUDES)
heap_profiler_unittest_CXXFLAGS = -g
heap_profiler_unittest_LDFLAGS = -g $(PTHREAD_CFLAGS)
heap_profiler_unittest_LDADD = libtcmalloc.la $(PTHREAD_LIBS)

TESTS += heap-checker_unittest.sh
heap_checker_unittest_sh_SOURCES = src/tests/heap-checker_unittest.sh
noinst_SCRIPTS += $(heap_checker_unittest_sh_SOURCES)
heap-checker_unittest.sh: $(heap_checker_unittest_sh_SOURCES)
	cp -a $(heap_checker_unittest_sh_SOURCES) $@

TESTS += heap-checker-death_unittest.sh
heap_checker_death_unittest_sh_SOURCES = src/tests/heap-checker-death_unittest.sh
noinst_SCRIPTS += $(heap_checker_death_unittest_sh_SOURCES)
heap-checker-death_unittest.sh: $(heap_checker_death_unittest_sh_SOURCES)
	cp -a $(heap_checker_death_unittest_sh_SOURCES) $@

# These are sub-programs used by heap-checker_unittest.sh
noinst_PROGRAMS += heap-checker_unittest
HEAP_CHECKER_UNITTEST_INCLUDES = src/memory_region_map.h \
                                 src/base/commandlineflags.h \
                                 src/base/googleinit.h \
                                 src/google/heap-checker.h \
                                 $(LOGGING_INCLUDES)
heap_checker_unittest_SOURCES = src/tests/heap-checker_unittest.cc \
                                $(HEAP_CHECKER_UNITTEST_INCLUDES)
heap_checker_unittest_CXXFLAGS = -g $(PTHREAD_CFLAGS)
heap_checker_unittest_LDFLAGS = -g $(PTHREAD_CFLAGS)
# tcmalloc has to be specified last!
heap_checker_unittest_LDADD =  $(PTHREAD_LIBS) liblogging.la -ltcmalloc

### Documentation (above and beyond tcmalloc_minimal documentation)
dist_doc_DATA += doc/heapprofile.html \
                 doc/heap-example1.png \
                 doc/heap_checker.html


### ------- CPU profiler

### The header files we use.  We divide into categories based on directory
S_CPU_PROFILER_INCLUDES = src/base/commandlineflags.h \
                          src/base/googleinit.h \
                          src/base/logging.h \
                          src/base/mutex.h \
                          $(SPINLOCK_INCLUDES) \
                          $(LOGGING_INCLUDES)
SG_CPU_PROFILER_INCLUDES = src/google/profiler.h \
                           src/google/stacktrace.h
SGP_CPU_PROFILER_INCLUDES =
CPU_PROFILER_INCLUDES = $(S_CPU_PROFILER_INCLUDES) $(SG_CPU_PROFILER_INCLUDES) $(SGP_CPU_PROFILER_INCLUDES)
googleinclude_HEADERS += $(SG_CPU_PROFILER_INCLUDES)
perftoolsinclude_HEADERS += $(SGP_CPU_PROFILER_INCLUDES)

### Making the library
lib_LTLIBRARIES += libprofiler.la
libprofiler_la_SOURCES = src/profiler.cc \
                         src/base/mutex.cc \
                         $(CPU_PROFILER_INCLUDES)
libprofiler_la_LIBADD = libspinlock.la liblogging.la libstacktrace.la
CPU_PROFILER_SYMBOLS = '(ProfilerStart|ProfilerStop|ProfilerEnable|ProfilerDisable|ProfilerFlush|ProfilerRegisterThread|ProfilerThreadState)'
libprofiler_la_LDFLAGS = -export-symbols-regex $(CPU_PROFILER_SYMBOLS)

### Unittests
TESTS += profiler_unittest.sh
profiler_unittest_sh_SOURCES = src/tests/profiler_unittest.sh
noinst_SCRIPTS += $(profiler_unittest_sh_SOURCES)
profiler_unittest.sh: $(profiler_unittest_sh_SOURCES)
	cp -a $(profiler_unittest_sh_SOURCES) $@

# These are sub-programs used by profiler_unittest.sh
noinst_PROGRAMS += profiler1_unittest profiler2_unittest profiler3_unittest \
                   profiler4_unittest
PROFILER_UNITTEST_INCLUDES = src/google/profiler.h
PROFILER_UNITTEST_SRCS = src/tests/profiler_unittest.cc \
                         $(PROFILER_UNITTEST_INCLUDES)
profiler1_unittest_SOURCES = $(PROFILER_UNITTEST_SRCS)
profiler1_unittest_CXXFLAGS = -g
profiler1_unittest_LDADD = libprofiler.la
profiler2_unittest_SOURCES = $(PROFILER_UNITTEST_SRCS)
profiler2_unittest_CXXFLAGS = -g
profiler2_unittest_LDADD = -lprofiler
profiler3_unittest_SOURCES = $(PROFILER_UNITTEST_SRCS)
profiler3_unittest_CXXFLAGS = -g $(PTHREAD_CFLAGS) -DWITH_THREADS
profiler3_unittest_LDFLAGS = $(PTHREAD_CFLAGS)
profiler3_unittest_LDADD = libprofiler.la $(PTHREAD_LIBS)
profiler4_unittest_SOURCES = $(PROFILER_UNITTEST_SRCS)
profiler4_unittest_CXXFLAGS = -g $(PTHREAD_CFLAGS) -DWITH_THREADS
profiler4_unittest_LDFLAGS = $(PTHREAD_CFLAGS)
profiler4_unittest_LDADD = -lprofiler $(PTHREAD_LIBS)

### Documentation
dist_doc_DATA += doc/cpuprofile.html \
                 doc/pprof-test-big.gif \
                 doc/pprof-test.gif \
                 doc/pprof-vsnprintf-big.gif \
                 doc/pprof-vsnprintf.gif

## ^^^^ END OF RULES TO MAKE YOUR LIBRARIES, BINARIES, AND UNITTESTS


# This should always include $(TESTS), but may also include other
# binaries that you compile but don't want automatically installed.
# We'll add to this later, on a library-by-library basis
noinst_PROGRAMS += $(TESTS)

rpm: dist-gzip packages/rpm.sh packages/rpm/rpm.spec
	@cd packages && ./rpm.sh ${PACKAGE} ${VERSION}

deb: dist-gzip packages/deb.sh packages/deb/*
	@cd packages && ./deb.sh ${PACKAGE} ${VERSION}

libtool: $(LIBTOOL_DEPS)
	$(SHELL) ./config.status --recheck

EXTRA_DIST = packages/rpm.sh packages/rpm/rpm.spec packages/deb.sh packages/deb \
             $(SCRIPTS) libtool \
             src/solaris/libstdc++.la

DISTCLEANFILES = src/google/perftools/hash_set.h
