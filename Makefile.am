## Process this file with automake to produce Makefile.in

# Note: for every library we create, we're explicit about what symbols
# we export.  In order to avoid complications with C++ mangling, we always
# use the regexp for of specifying symbols.

# Make sure that when we re-make ./configure, we get the macros we need
ACLOCAL_AMFLAGS = -I m4
AUTOMAKE_OPTIONS = subdir-objects

# This is so we can #include <gperftools/foo>
AM_CPPFLAGS = -I$(top_srcdir)/src

if !WITH_STACK_TRACE
AM_CPPFLAGS += -DNO_TCMALLOC_SAMPLES
endif !WITH_STACK_TRACE

# This is mostly based on configure options
AM_CXXFLAGS =

# These are good warnings to turn on by default.
if GCC
AM_CXXFLAGS += -Wall -Wwrite-strings -Woverloaded-virtual \
               -Wno-sign-compare
endif GCC

if HAVE_SIZED_DEALLOCATION
AM_CXXFLAGS += -fsized-deallocation
endif HAVE_SIZED_DEALLOCATION

if ENABLE_W_THREAD_SAFETY
AM_CXXFLAGS += -Wthread-safety
endif ENABLE_W_THREAD_SAFETY

# The -no-undefined flag allows libtool to generate shared libraries for
# Cygwin and MinGW.
AM_LDFLAGS = -no-undefined

# respect --enable-frame-pointers regardless of architecture
if ENABLE_FRAME_POINTERS
AM_CXXFLAGS += -fno-omit-frame-pointer -DFORCED_FRAME_POINTERS

else !ENABLE_FRAME_POINTERS

if ENABLE_FP_FLAGS
AM_CXXFLAGS += -fno-omit-frame-pointer -momit-leaf-frame-pointer
endif ENABLE_FP_FLAGS

endif !ENABLE_FRAME_POINTERS

# For windows systems (at least, mingw), we need to tell all our
# tests to link in libtcmalloc using -u.  This is because libtcmalloc
# accomplishes its tasks via patching, leaving no work for the linker
# to identify, so the linker will ignore libtcmalloc by default unless
# we explicitly create a dependency via -u.
TCMALLOC_FLAGS =
if MINGW
TCMALLOC_FLAGS += -Wl,-u__tcmalloc
endif MINGW

perftoolsincludedir = $(includedir)/gperftools
# The .h files you want to install (that is, .h files that people
# who install this package can include in their own applications.)
# We'll add to this later, on a library-by-library basis
perftoolsinclude_HEADERS =
# tcmalloc.h is a special case, because it's a .h.in file
nodist_perftoolsinclude_HEADERS = src/gperftools/tcmalloc.h
noinst_HEADERS = src/gperftools/tcmalloc.h.in

# This is for HTML and other documentation you want to install.  Add
# your documentation files (in doc/) in addition to these top-level
# boilerplate files.  We'll add to this later, on a library-by-library
# basis
dist_doc_DATA = AUTHORS COPYING INSTALL NEWS README README_windows.txt \
                ChangeLog.old

# The libraries (.so's) you want to install
# We'll add to this later, on a library-by-library basis
lib_LTLIBRARIES =
# This is for 'convenience libraries' -- basically just a container for sources
noinst_LTLIBRARIES =

# unittests you want to run when people type 'make check'.
# Note: tests cannot take any arguments!
TESTS =
# TESTS_ENVIRONMENT sets environment variables for when you run unittest.
# We always get "srcdir" set for free.
# We'll add to this later, on a library-by-library basis.
TESTS_ENVIRONMENT =
# All script tests should be added here
noinst_SCRIPTS =
# If your test calls another program that, like the test itself, shouldn't
# be installed, add it here.  (Stuff in TESTS is automatically added later).
noinst_PROGRAMS =

# Binaries we might build that should be installed
bin_PROGRAMS =

# This is my own var, used for extra libraries I make that I need installed
EXTRA_INSTALL =

## vvvv RULES TO MAKE THE LIBRARIES, BINARIES, AND UNITTESTS

dist_doc_DATA += docs/index.html docs/designstyle.css


### ------- library routines, in src/base

# This is a 'convenience library' -- it's not actually installed or anything
noinst_LTLIBRARIES += liblogging.la
liblogging_la_SOURCES = src/base/logging.cc \
                        src/base/generic_writer.cc

noinst_LTLIBRARIES += libsysinfo.la
libsysinfo_la_SOURCES = src/base/sysinfo.cc \
                        src/base/proc_maps_iterator.cc \
                        src/base/dynamic_annotations.cc
libsysinfo_la_LIBADD = $(NANOSLEEP_LIBS)

# For MinGW, we use also have to use libwindows Luckily, we need the
# windows.a library in exactly the same place we need spinlock.a
# (pretty much everywhere), so we can use the same variable name for
# each.  We can also optimize the MinGW rule a bit by leaving out
# files we know aren't used on windows. libwindows also obsoletes the
# need for other files like system_alloc.cc.
if MINGW
noinst_LTLIBRARIES += libwindows.la
libwindows_la_SOURCES = src/windows/port.cc \
                        src/windows/system-alloc.cc \
                        src/windows/ia32_modrm_map.cc \
                        src/windows/ia32_opcode_map.cc \
                        src/windows/mini_disassembler.cc \
                        src/windows/patch_functions.cc \
                        src/windows/preamble_patcher.cc \
                        src/windows/preamble_patcher_with_stub.cc
# patch_functions.cc uses Psapi.lib.  MSVC has a #pragma for that, but not us.
libwindows_la_LIBADD = -lpsapi

noinst_LTLIBRARIES += libspinlock.la
libspinlock_la_SOURCES = src/base/spinlock.cc \
                         src/base/spinlock_internal.cc
libspinlock_la_LIBADD = -lsynchronization

LIBSPINLOCK = libwindows.la libspinlock.la libsysinfo.la liblogging.la

# We also need to tell mingw that sysinfo.cc needs shlwapi.lib.
# (We do this via a #pragma for msvc, but need to do it here for mingw).
libsysinfo_la_LIBADD += -lshlwapi

# patch_functions.cc #includes tcmalloc.cc, so no need to link it in.
TCMALLOC_CC =
SYSTEM_ALLOC_CC =
else !MINGW
noinst_LTLIBRARIES += libspinlock.la
libspinlock_la_SOURCES = src/base/spinlock.cc \
                         src/base/spinlock_internal.cc
libspinlock_la_LIBADD = $(NANOSLEEP_LIBS)
# spinlock also needs NumCPUs, from libsysinfo, which in turn needs liblogging
LIBSPINLOCK = libspinlock.la libsysinfo.la liblogging.la

TCMALLOC_CC = src/tcmalloc.cc
SYSTEM_ALLOC_CC = src/system-alloc.cc
endif !MINGW

# On MSVC, we need our own versions of addr2line and nm to work with
# pprof, plus couple more tests.
WINDOWS_EXTRA = src/windows/preamble_patcher_test.cc \
                src/windows/shortproc.asm \
                src/windows/auto_testing_hook.h \
                src/windows/nm-pdb.c \
                src/windows/addr2line-pdb.c

### Unittests

if WITH_HEAP_PROFILER_OR_CHECKER
TESTS += low_level_alloc_unittest
low_level_alloc_unittest_SOURCES = src/base/low_level_alloc.cc \
                                   src/malloc_hook.cc \
                                   src/mmap_hook.cc \
                                   src/tests/low_level_alloc_unittest.cc
# By default, MallocHook takes stack traces for use by the heap-checker.
# We don't need that functionality here, so we turn it off to reduce deps.
low_level_alloc_unittest_CXXFLAGS = -DNO_TCMALLOC_SAMPLES
low_level_alloc_unittest_LDADD = $(LIBSPINLOCK)
endif WITH_HEAP_PROFILER_OR_CHECKER

### ------- stack trace

if WITH_STACK_TRACE

### The header files we use.  We divide into categories based on directory

perftoolsinclude_HEADERS += src/gperftools/stacktrace.h

### Making the library
noinst_LTLIBRARIES += libstacktrace.la
libstacktrace_la_SOURCES = src/stacktrace.cc \
                           src/base/elf_mem_image.cc \
                           src/base/vdso_support.cc
libstacktrace_la_LIBADD = $(UNWIND_LIBS) $(LIBSPINLOCK)

noinst_LTLIBRARIES += libfake_stacktrace_scope.la
libfake_stacktrace_scope_la_SOURCES = src/fake_stacktrace_scope.cc

### Unittests
TESTS += stacktrace_unittest
stacktrace_unittest_SOURCES = src/tests/stacktrace_unittest.cc \
                              $(libstacktrace_la_SOURCES)
stacktrace_unittest_CXXFLAGS = $(AM_CXXFLAGS) -DSTACKTRACE_IS_TESTED
stacktrace_unittest_LDADD = $(libstacktrace_la_LIBADD) liblogging.la libfake_stacktrace_scope.la $(STACKTRACE_UNITTEST_LIBS)
# nice to have. Allows glibc's backtrace_symbols to work.
stacktrace_unittest_LDFLAGS = -export-dynamic

TESTS += check_address_unittest
check_address_unittest_SOURCES = src/tests/check_address_test.cc
check_address_unittest_LDADD = liblogging.la $(LIBSPINLOCK)

### Documentation
dist_doc_DATA +=

endif WITH_STACK_TRACE

### ------- pprof

# If we are not compiling with stacktrace support, pprof is worthless
if WITH_STACK_TRACE

bin_SCRIPTS = pprof-symbolize

pprof-symbolize : $(top_srcdir)/src/pprof
	cp -p $(top_srcdir)/src/pprof $@

### Unittests

# Let unittests find pprof if they need to run it
TESTS_ENVIRONMENT += PPROF_PATH=$(top_srcdir)/src/pprof

if INSTALL_PPROF
bin_SCRIPTS += src/pprof
dist_man_MANS = docs/pprof.1
dist_doc_DATA += docs/pprof_remote_servers.html
endif INSTALL_PPROF

### Documentation

endif WITH_STACK_TRACE

### ------- tcmalloc_minimal (thread-caching malloc)

perftoolsinclude_HEADERS += src/gperftools/malloc_hook.h \
                            src/gperftools/malloc_hook_c.h \
                            src/gperftools/malloc_extension.h \
                            src/gperftools/malloc_extension_c.h \
                            src/gperftools/nallocx.h

### Making the library

noinst_LTLIBRARIES += libtcmalloc_minimal_internal.la
libtcmalloc_minimal_internal_la_SOURCES = src/common.cc \
                                          src/internal_logging.cc \
                                          $(SYSTEM_ALLOC_CC) \
                                          src/memfs_malloc.cc \
                                          src/safe_strerror.cc \
                                          src/central_freelist.cc \
                                          src/page_heap.cc \
                                          src/sampler.cc \
                                          src/span.cc \
                                          src/stack_trace_table.cc \
                                          src/static_vars.cc \
                                          src/symbolize.cc \
                                          src/thread_cache.cc \
                                          src/thread_cache_ptr.cc \
                                          src/malloc_hook.cc \
                                          src/malloc_extension.cc
# We #define NO_TCMALLOC_SAMPLES, since sampling is turned off for _minimal.
libtcmalloc_minimal_internal_la_CXXFLAGS = -DNO_TCMALLOC_SAMPLES \
                                           -DNO_HEAP_CHECK \
                                           -DNDEBUG \
                                           $(AM_CXXFLAGS)
libtcmalloc_minimal_internal_la_LDFLAGS =  $(AM_LDFLAGS)
libtcmalloc_minimal_internal_la_LIBADD =  $(LIBSPINLOCK)

lib_LTLIBRARIES += libtcmalloc_minimal.la
libtcmalloc_minimal_la_SOURCES = $(TCMALLOC_CC)
libtcmalloc_minimal_la_CXXFLAGS = -DNO_TCMALLOC_SAMPLES \
                                  $(PTHREAD_CFLAGS) -DNDEBUG $(AM_CXXFLAGS)
# -version-info gets passed to libtool
libtcmalloc_minimal_la_LDFLAGS = -version-info @TCMALLOC_SO_VERSION@ $(AM_LDFLAGS)
libtcmalloc_minimal_la_LIBADD = libtcmalloc_minimal_internal.la

### Unittests

TESTS += tcmalloc_minimal_unittest
tcmalloc_minimal_unittest_SOURCES = src/tests/tcmalloc_unittest.cc \
                                    src/tests/testutil.cc
tcmalloc_minimal_unittest_CXXFLAGS = $(PTHREAD_CFLAGS) $(AM_CXXFLAGS)
tcmalloc_minimal_unittest_LDFLAGS = $(PTHREAD_CFLAGS) $(TCMALLOC_FLAGS)
# We want libtcmalloc last on the link line, but due to a bug in
# libtool involving convenience libs, they need to come last on the
# link line in order to get dependency ordering right.  This is ok:
# convenience libraries are .a's, so tcmalloc is still the last .so.
# We also put pthreads after tcmalloc, because some pthread
# implementations define their own malloc, and we need to go on the
# first linkline to make sure our malloc 'wins'.
tcmalloc_minimal_unittest_LDADD = libtcmalloc_minimal.la \
                                  liblogging.la $(PTHREAD_LIBS)

# lets make sure we exerice ASSERTs in at least in statically linked
# configuration
TESTS += tcm_min_asserts_unittest
tcm_min_asserts_unittest_SOURCES = src/tests/tcmalloc_unittest.cc \
                                    src/tests/testutil.cc \
                                    $(libtcmalloc_minimal_internal_la_SOURCES) \
                                    $(libtcmalloc_minimal_la_SOURCES)
tcm_min_asserts_unittest_CXXFLAGS = -DNO_TCMALLOC_SAMPLES -DNO_HEAP_CHECK \
                                    $(PTHREAD_CFLAGS) $(AM_CXXFLAGS)
tcm_min_asserts_unittest_LDFLAGS = $(PTHREAD_CFLAGS) $(TCMALLOC_FLAGS)
tcm_min_asserts_unittest_LDADD = $(LIBSPINLOCK) \
                                  liblogging.la $(PTHREAD_LIBS)

TESTS += tcmalloc_minimal_large_unittest
tcmalloc_minimal_large_unittest_SOURCES = src/tests/tcmalloc_large_unittest.cc
tcmalloc_minimal_large_unittest_CXXFLAGS = $(PTHREAD_CFLAGS) $(AM_CXXFLAGS)
tcmalloc_minimal_large_unittest_LDFLAGS = $(PTHREAD_CFLAGS) $(TCMALLOC_FLAGS)
tcmalloc_minimal_large_unittest_LDADD = libtcmalloc_minimal.la $(PTHREAD_LIBS)

TESTS += tcmalloc_minimal_large_heap_fragmentation_unittest
tcmalloc_minimal_large_heap_fragmentation_unittest_SOURCES = src/tests/large_heap_fragmentation_unittest.cc
tcmalloc_minimal_large_heap_fragmentation_unittest_CXXFLAGS = $(PTHREAD_CFLAGS) $(AM_CXXFLAGS)
tcmalloc_minimal_large_heap_fragmentation_unittest_LDFLAGS = $(PTHREAD_CFLAGS) $(TCMALLOC_FLAGS)
tcmalloc_minimal_large_heap_fragmentation_unittest_LDADD = libtcmalloc_minimal.la $(PTHREAD_LIBS)

# These all tests components of tcmalloc_minimal

TESTS += addressmap_unittest
addressmap_unittest_SOURCES = src/tests/addressmap_unittest.cc
if MINGW
addressmap_unittest_SOURCES += src/windows/port.h src/windows/port.cc
endif MINGW
addressmap_unittest_CXXFLAGS = -g $(AM_CXXFLAGS)
addressmap_unittest_LDADD = liblogging.la

if !MINGW
TESTS += system_alloc_unittest
system_alloc_unittest_SOURCES = src/config_for_unittests.h \
                                src/tests/system-alloc_unittest.cc
system_alloc_unittest_CXXFLAGS = $(PTHREAD_CFLAGS) $(AM_CXXFLAGS)
system_alloc_unittest_LDFLAGS = $(PTHREAD_CFLAGS) $(TCMALLOC_FLAGS)
system_alloc_unittest_LDADD = libtcmalloc_minimal.la $(PTHREAD_LIBS)

# Note, we skip this test on mingw (and windows in general). It uses
# unsetenv, which is not available on win32.
TESTS += unique_path_unittest
unique_path_unittest_SOURCES = src/tests/unique_path_unittest.cc
unique_path_unittest_CXXFLAGS = $(PTHREAD_CFLAGS) $(AM_CXXFLAGS)
unique_path_unittest_LDFLAGS = $(PTHREAD_CFLAGS) $(AM_LDFLAGS)
unique_path_unittest_LDADD = libsysinfo.la liblogging.la $(PTHREAD_LIBS)
endif !MINGW

TESTS += packed_cache_test
packed_cache_test_SOURCES = src/tests/packed-cache_test.cc
packed_cache_test_CXXFLAGS = $(PTHREAD_CFLAGS) $(AM_CXXFLAGS)
packed_cache_test_LDFLAGS = $(PTHREAD_CFLAGS) $(TCMALLOC_FLAGS)
packed_cache_test_LDADD = libtcmalloc_minimal.la $(PTHREAD_LIBS)

TESTS += frag_unittest
frag_unittest_SOURCES = src/tests/frag_unittest.cc src/config_for_unittests.h
frag_unittest_CXXFLAGS = $(PTHREAD_CFLAGS) $(AM_CXXFLAGS)
frag_unittest_LDFLAGS = $(PTHREAD_CFLAGS) $(TCMALLOC_FLAGS)
frag_unittest_LDADD = libtcmalloc_minimal.la $(PTHREAD_LIBS)

TESTS += markidle_unittest
markidle_unittest_SOURCES = src/tests/markidle_unittest.cc \
                            src/config_for_unittests.h \
                            src/tests/testutil.h src/tests/testutil.cc
markidle_unittest_CXXFLAGS = $(PTHREAD_CFLAGS) $(AM_CXXFLAGS)
markidle_unittest_LDFLAGS = $(PTHREAD_CFLAGS) $(TCMALLOC_FLAGS)
markidle_unittest_LDADD = libtcmalloc_minimal.la $(PTHREAD_LIBS)

TESTS += current_allocated_bytes_test
current_allocated_bytes_test_SOURCES = src/tests/current_allocated_bytes_test.cc \
                                       src/config_for_unittests.h
current_allocated_bytes_test_CXXFLAGS = $(PTHREAD_CFLAGS) $(AM_CXXFLAGS)
current_allocated_bytes_test_LDFLAGS = $(PTHREAD_CFLAGS) $(TCMALLOC_FLAGS)
current_allocated_bytes_test_LDADD = libtcmalloc_minimal.la $(PTHREAD_LIBS)

TESTS += malloc_hook_test
malloc_hook_test_SOURCES = src/tests/malloc_hook_test.cc \
                           src/config_for_unittests.h \
                           src/base/logging.h \
                           src/gperftools/malloc_hook.h \
                           src/tests/testutil.h src/tests/testutil.cc
malloc_hook_test_CXXFLAGS = $(PTHREAD_CFLAGS) $(AM_CXXFLAGS)
malloc_hook_test_LDFLAGS = $(PTHREAD_CFLAGS) $(TCMALLOC_FLAGS)
malloc_hook_test_LDADD = libtcmalloc_minimal.la $(PTHREAD_LIBS)

if WITH_HEAP_PROFILER_OR_CHECKER
if !MINGW
TESTS += mmap_hook_test
mmap_hook_test_SOURCES = src/tests/mmap_hook_test.cc \
                         src/mmap_hook.cc
mmap_hook_test_LDADD = $(LIBSPINLOCK)
endif !MINGW
endif WITH_HEAP_PROFILER_OR_CHECKER

TESTS += malloc_extension_test
malloc_extension_test_SOURCES = src/tests/malloc_extension_test.cc \
                                src/config_for_unittests.h \
                                src/base/logging.h \
                                src/gperftools/malloc_extension.h \
                                src/gperftools/malloc_extension_c.h
malloc_extension_test_CXXFLAGS = $(PTHREAD_CFLAGS) $(AM_CXXFLAGS)
malloc_extension_test_LDFLAGS = $(PTHREAD_CFLAGS) $(TCMALLOC_FLAGS)
malloc_extension_test_LDADD = libtcmalloc_minimal.la $(PTHREAD_LIBS)

# This doesn't work with mingw, which links foo.a even though it
# doesn't set ENABLE_STATIC.  TODO(csilvers): set enable_static=true
# in configure.ac:36?
if !MINGW
TESTS += malloc_extension_c_test
malloc_extension_c_test_SOURCES = src/tests/malloc_extension_c_test.cc \
                                  src/gperftools/malloc_extension.h \
                                  src/gperftools/malloc_extension_c.h
malloc_extension_c_test_CFLAGS = $(PTHREAD_CFLAGS) $(AM_CFLAGS)
malloc_extension_c_test_LDFLAGS = $(PTHREAD_CFLAGS) $(TCMALLOC_FLAGS)
malloc_extension_c_test_LDADD = libtcmalloc_minimal.la
endif !MINGW

if !MINGW
if !OSX
TESTS += memalign_unittest
memalign_unittest_SOURCES = src/tests/memalign_unittest.cc \
                            src/tcmalloc_internal.h \
                            src/config_for_unittests.h \
                            src/tests/testutil.h src/tests/testutil.cc
memalign_unittest_CXXFLAGS = $(PTHREAD_CFLAGS) $(AM_CXXFLAGS)
memalign_unittest_LDFLAGS = $(PTHREAD_CFLAGS) $(TCMALLOC_FLAGS)
memalign_unittest_LDADD = libtcmalloc_minimal.la $(PTHREAD_LIBS)
endif !OSX
endif !MINGW

TESTS += page_heap_test
page_heap_test_SOURCES = src/tests/page_heap_test.cc \
                         src/config_for_unittests.h \
                         src/base/logging.h \
                         src/common.h \
                         src/page_heap.h
page_heap_test_CXXFLAGS = $(PTHREAD_CFLAGS) $(AM_CXXFLAGS)
page_heap_test_LDFLAGS = $(PTHREAD_CFLAGS) $(TCMALLOC_FLAGS)
page_heap_test_LDADD = libtcmalloc_minimal.la $(PTHREAD_LIBS)

TESTS += pagemap_unittest
pagemap_unittest_SOURCES = src/tests/pagemap_unittest.cc \
                           src/config_for_unittests.h \
                           src/base/logging.h \
                           src/pagemap.h
pagemap_unittest_CXXFLAGS = $(PTHREAD_CFLAGS) $(AM_CXXFLAGS)
pagemap_unittest_LDFLAGS = $(PTHREAD_CFLAGS) $(TCMALLOC_FLAGS)
pagemap_unittest_LDADD = libtcmalloc_minimal.la $(PTHREAD_LIBS)

TESTS += safe_strerror_test
safe_strerror_test_SOURCES = src/tests/safe_strerror_test.cc \
                             src/safe_strerror.cc
safe_strerror_test_LDADD = liblogging.la

TESTS += generic_writer_test
generic_writer_test_SOURCES = src/tests/generic_writer_test.cc
generic_writer_test_LDADD = liblogging.la

TESTS += proc_maps_iterator_test
proc_maps_iterator_test_SOURCES = src/tests/proc_maps_iterator_test.cc
proc_maps_iterator_test_LDADD = libsysinfo.la liblogging.la

TESTS += realloc_unittest
realloc_unittest_SOURCES = src/tests/realloc_unittest.cc \
                           src/config_for_unittests.h \
                           src/base/logging.h
realloc_unittest_CXXFLAGS = $(PTHREAD_CFLAGS) $(AM_CXXFLAGS)
realloc_unittest_LDFLAGS = $(PTHREAD_CFLAGS) $(TCMALLOC_FLAGS)
realloc_unittest_LDADD = libtcmalloc_minimal.la $(PTHREAD_LIBS)

TESTS += stack_trace_table_test
stack_trace_table_test_SOURCES = src/tests/stack_trace_table_test.cc \
                                 src/config_for_unittests.h
stack_trace_table_test_CXXFLAGS = $(PTHREAD_CFLAGS) $(AM_CXXFLAGS)
stack_trace_table_test_LDFLAGS = $(PTHREAD_CFLAGS) $(TCMALLOC_FLAGS)
stack_trace_table_test_LDADD = libtcmalloc_minimal.la $(PTHREAD_LIBS)

TESTS += thread_dealloc_unittest
thread_dealloc_unittest_SOURCES = src/tests/thread_dealloc_unittest.cc \
                                  src/config_for_unittests.h \
                                  src/tests/testutil.h src/tests/testutil.cc
thread_dealloc_unittest_CXXFLAGS = $(PTHREAD_CFLAGS) $(AM_CXXFLAGS)
thread_dealloc_unittest_LDFLAGS = $(PTHREAD_CFLAGS) $(TCMALLOC_FLAGS)
thread_dealloc_unittest_LDADD = libtcmalloc_minimal.la $(PTHREAD_LIBS)

### Documentation
dist_doc_DATA += docs/tcmalloc.html \
                 docs/overview.gif \
                 docs/pageheap.gif \
                 docs/spanmap.gif \
                 docs/threadheap.gif \
                 docs/t-test1.times.txt \
                 docs/tcmalloc-opspercpusec.vs.threads.1024.bytes.png 	\
                 docs/tcmalloc-opspercpusec.vs.threads.128.bytes.png 	\
                 docs/tcmalloc-opspercpusec.vs.threads.131072.bytes.png 	\
                 docs/tcmalloc-opspercpusec.vs.threads.16384.bytes.png 	\
                 docs/tcmalloc-opspercpusec.vs.threads.2048.bytes.png 	\
                 docs/tcmalloc-opspercpusec.vs.threads.256.bytes.png 	\
                 docs/tcmalloc-opspercpusec.vs.threads.32768.bytes.png 	\
                 docs/tcmalloc-opspercpusec.vs.threads.4096.bytes.png 	\
                 docs/tcmalloc-opspercpusec.vs.threads.512.bytes.png 	\
                 docs/tcmalloc-opspercpusec.vs.threads.64.bytes.png 	\
                 docs/tcmalloc-opspercpusec.vs.threads.65536.bytes.png 	\
                 docs/tcmalloc-opspercpusec.vs.threads.8192.bytes.png 	\
                 docs/tcmalloc-opspersec.vs.size.1.threads.png 		\
                 docs/tcmalloc-opspersec.vs.size.12.threads.png 		\
                 docs/tcmalloc-opspersec.vs.size.16.threads.png 		\
                 docs/tcmalloc-opspersec.vs.size.2.threads.png 		\
                 docs/tcmalloc-opspersec.vs.size.20.threads.png 		\
                 docs/tcmalloc-opspersec.vs.size.3.threads.png 		\
                 docs/tcmalloc-opspersec.vs.size.4.threads.png 		\
                 docs/tcmalloc-opspersec.vs.size.5.threads.png 		\
                 docs/tcmalloc-opspersec.vs.size.8.threads.png

# I don't know how to say "distribute the .dot files but don't install them";
# noinst doesn't seem to work with data.  I separate them out anyway, in case
# one day we figure it out.  Regardless, installing the dot files isn't the
# end of the world.
dist_doc_DATA += docs/overview.dot \
                 docs/pageheap.dot \
                 docs/spanmap.dot \
                 docs/threadheap.dot


### ------- tcmalloc_minimal_debug (thread-caching malloc with debugallocation)

if WITH_DEBUGALLOC

lib_LTLIBRARIES += libtcmalloc_minimal_debug.la
libtcmalloc_minimal_debug_la_SOURCES = src/debugallocation.cc
libtcmalloc_minimal_debug_la_CXXFLAGS = $(libtcmalloc_minimal_la_CXXFLAGS) \
                                        -DTCMALLOC_FOR_DEBUGALLOCATION
# version_info gets passed to libtool
libtcmalloc_minimal_debug_la_LDFLAGS = $(libtcmalloc_minimal_la_LDFLAGS) \
                                       -version-info @TCMALLOC_SO_VERSION@
libtcmalloc_minimal_debug_la_LIBADD = $(libtcmalloc_minimal_la_LIBADD)

### Unittests

TESTS += tcmalloc_minimal_debug_unittest
tcmalloc_minimal_debug_unittest_SOURCES = $(tcmalloc_minimal_unittest_SOURCES)
tcmalloc_minimal_debug_unittest_CXXFLAGS = $(tcmalloc_minimal_unittest_CXXFLAGS) \
                                           -DDEBUGALLOCATION
tcmalloc_minimal_debug_unittest_LDFLAGS = $(tcmalloc_minimal_unittest_LDFLAGS)
tcmalloc_minimal_debug_unittest_LDADD = libtcmalloc_minimal_debug.la $(PTHREAD_LIBS)

TESTS += malloc_extension_debug_test
malloc_extension_debug_test_SOURCES = $(malloc_extension_test_SOURCES)
malloc_extension_debug_test_CXXFLAGS = $(malloc_extension_test_CXXFLAGS)
malloc_extension_debug_test_LDFLAGS = $(malloc_extension_test_LDFLAGS)
malloc_extension_debug_test_LDADD = libtcmalloc_minimal_debug.la $(PTHREAD_LIBS)

if !MINGW
if !OSX
TESTS += memalign_debug_unittest
memalign_debug_unittest_SOURCES = $(memalign_unittest_SOURCES)
memalign_debug_unittest_CXXFLAGS = $(memalign_unittest_CXXFLAGS)
memalign_debug_unittest_LDFLAGS = $(memalign_unittest_LDFLAGS)
memalign_debug_unittest_LDADD = libtcmalloc_minimal_debug.la $(PTHREAD_LIBS)
endif !OSX
endif !MINGW

TESTS += realloc_debug_unittest
realloc_debug_unittest_SOURCES = $(realloc_unittest_SOURCES)
realloc_debug_unittest_CXXFLAGS = $(realloc_unittest_CXXFLAGS)
realloc_debug_unittest_LDFLAGS = $(realloc_unittest_LDFLAGS)
realloc_debug_unittest_LDADD = libtcmalloc_minimal_debug.la $(PTHREAD_LIBS)

# debugallocation_test checks that we print a proper stacktrace when
# debug-allocs fail, so we can't run it if we don't have stacktrace info.
if WITH_STACK_TRACE
TESTS += debugallocation_test.sh$(EXEEXT)
debugallocation_test_sh_SOURCES = src/tests/debugallocation_test.sh
noinst_SCRIPTS += $(debugallocation_test_sh_SOURCES)
debugallocation_test.sh$(EXEEXT): $(top_srcdir)/$(debugallocation_test_sh_SOURCES) \
                                  debugallocation_test
	rm -f $@
	cp -p $(top_srcdir)/$(debugallocation_test_sh_SOURCES) $@

# This is the sub-program used by debugallocation_test.sh
noinst_PROGRAMS += debugallocation_test
debugallocation_test_SOURCES = src/tests/debugallocation_test.cc
debugallocation_test_CXXFLAGS = $(PTHREAD_CFLAGS) $(AM_CXXFLAGS)
debugallocation_test_LDFLAGS = $(PTHREAD_CFLAGS) $(TCMALLOC_FLAGS)
debugallocation_test_LDADD = libtcmalloc_debug.la $(PTHREAD_LIBS)
endif WITH_STACK_TRACE

endif WITH_DEBUGALLOC

if !MINGW
noinst_LTLIBRARIES += librun_benchmark.la
librun_benchmark_la_SOURCES = \
	benchmark/run_benchmark.cc benchmark/run_benchmark.h

noinst_PROGRAMS += malloc_bench malloc_bench_shared \
	binary_trees binary_trees_shared

malloc_bench_SOURCES = benchmark/malloc_bench.cc
malloc_bench_CXXFLAGS = $(PTHREAD_CFLAGS) $(AM_CXXFLAGS)
malloc_bench_LDFLAGS = $(PTHREAD_CFLAGS) $(TCMALLOC_FLAGS)
if ENABLE_STATIC
malloc_bench_LDFLAGS += -static
endif ENABLE_STATIC
malloc_bench_LDADD = librun_benchmark.la libtcmalloc_minimal.la $(PTHREAD_LIBS)

malloc_bench_shared_SOURCES = benchmark/malloc_bench.cc
malloc_bench_shared_CXXFLAGS = $(PTHREAD_CFLAGS) $(AM_CXXFLAGS)
malloc_bench_shared_LDFLAGS = $(PTHREAD_CFLAGS) $(TCMALLOC_FLAGS)
malloc_bench_shared_LDADD = librun_benchmark.la libtcmalloc_minimal.la $(PTHREAD_LIBS)

if WITH_HEAP_PROFILER_OR_CHECKER

noinst_PROGRAMS += malloc_bench_shared_full
malloc_bench_shared_full_SOURCES = benchmark/malloc_bench.cc
malloc_bench_shared_full_CXXFLAGS = $(PTHREAD_CFLAGS) $(AM_CXXFLAGS)
malloc_bench_shared_full_LDFLAGS = $(PTHREAD_CFLAGS) $(TCMALLOC_FLAGS)
malloc_bench_shared_full_LDADD = librun_benchmark.la libtcmalloc.la $(PTHREAD_LIBS)

noinst_PROGRAMS += unwind_bench
unwind_bench_SOURCES = benchmark/unwind_bench.cc
unwind_bench_CXXFLAGS = $(PTHREAD_CFLAGS) $(AM_CXXFLAGS) $(NO_BUILTIN_CXXFLAGS)
unwind_bench_LDFLAGS = $(PTHREAD_CFLAGS) $(TCMALLOC_FLAGS)
unwind_bench_LDADD = librun_benchmark.la libtcmalloc.la $(PTHREAD_LIBS)

endif WITH_HEAP_PROFILER_OR_CHECKER

binary_trees_SOURCES = benchmark/binary_trees.cc
binary_trees_CXXFLAGS = $(PTHREAD_CFLAGS) $(AM_CXXFLAGS)
binary_trees_LDFLAGS = $(PTHREAD_CFLAGS) $(TCMALLOC_FLAGS)
if ENABLE_STATIC
binary_trees_LDFLAGS += -static
endif ENABLE_STATIC
binary_trees_LDADD = libtcmalloc_minimal.la $(PTHREAD_LIBS)

binary_trees_shared_SOURCES = benchmark/binary_trees.cc
binary_trees_shared_CXXFLAGS = $(PTHREAD_CFLAGS) $(AM_CXXFLAGS)
binary_trees_shared_LDFLAGS = $(PTHREAD_CFLAGS) $(TCMALLOC_FLAGS)
binary_trees_shared_LDADD = libtcmalloc_minimal.la $(PTHREAD_LIBS)
endif !MINGW

### ------- tcmalloc (thread-caching malloc + heap profiler + heap checker)

if WITH_HEAP_PROFILER_OR_CHECKER

perftoolsinclude_HEADERS += src/gperftools/heap-profiler.h \
                            src/gperftools/heap-checker.h

if BUILD_EMERGENCY_MALLOC
EMERGENCY_MALLOC_CC = src/emergency_malloc.cc src/emergency_malloc_for_stacktrace.cc
EMERGENCY_MALLOC_DEFINE = -DENABLE_EMERGENCY_MALLOC
else !BUILD_EMERGENCY_MALLOC
EMERGENCY_MALLOC_CC = src/fake_stacktrace_scope.cc
EMERGENCY_MALLOC_DEFINE =
endif !BUILD_EMERGENCY_MALLOC

### Making the library

if WITH_HEAP_CHECKER
# heap-checker-bcad is last, in hopes its global ctor will run first.
# (Note this is added to libtcmalloc.la, not libtcmalloc_internal.la,
# but that's ok; the internal/external distinction is only useful for
# cygwin, and cygwin doesn't use HEAP_CHECKER anyway.)
HEAP_CHECKER_SOURCES = src/base/linuxthreads.cc \
                       src/heap-checker.cc \
                       src/heap-checker-bcad.cc
MAYBE_NO_HEAP_CHECK =
else !WITH_HEAP_CHECKER
HEAP_CHECKER_SOURCES =
MAYBE_NO_HEAP_CHECK = -DNO_HEAP_CHECK
endif !WITH_HEAP_CHECKER

noinst_LTLIBRARIES += libtcmalloc_internal.la
libtcmalloc_internal_la_SOURCES = $(libtcmalloc_minimal_internal_la_SOURCES) \
                                  src/base/low_level_alloc.cc \
                                  src/heap-profile-table.cc \
                                  src/heap-profiler.cc \
                                  src/raw_printer.cc \
                                  $(EMERGENCY_MALLOC_CC) \
                                  src/mmap_hook.cc \
                                  src/memory_region_map.cc
libtcmalloc_internal_la_CXXFLAGS = $(PTHREAD_CFLAGS) -DNDEBUG \
                                   $(AM_CXXFLAGS) $(EMERGENCY_MALLOC_DEFINE) \
                                   $(MAYBE_NO_HEAP_CHECK)
libtcmalloc_internal_la_LDFLAGS = $(PTHREAD_CFLAGS)
libtcmalloc_internal_la_LIBADD = libstacktrace.la $(PTHREAD_LIBS)

lib_LTLIBRARIES += libtcmalloc.la
libtcmalloc_la_SOURCES = $(TCMALLOC_CC) \
                         $(HEAP_CHECKER_SOURCES)
libtcmalloc_la_CXXFLAGS = $(PTHREAD_CFLAGS) -DNDEBUG $(AM_CXXFLAGS) \
                          $(MAYBE_NO_HEAP_CHECK) $(EMERGENCY_MALLOC_DEFINE)
libtcmalloc_la_LDFLAGS = $(PTHREAD_CFLAGS) -version-info @TCMALLOC_SO_VERSION@
libtcmalloc_la_LIBADD = libtcmalloc_internal.la $(PTHREAD_LIBS)

# same as above with without -DNDEBUG
noinst_LTLIBRARIES += libtcmalloc_internal_with_asserts.la
libtcmalloc_internal_with_asserts_la_SOURCES = $(libtcmalloc_internal_la_SOURCES)
libtcmalloc_internal_with_asserts_la_CXXFLAGS = $(PTHREAD_CFLAGS) \
                                   $(AM_CXXFLAGS) $(EMERGENCY_MALLOC_DEFINE) \
                                   $(MAYBE_NO_HEAP_CHECK)
libtcmalloc_internal_with_asserts_la_LDFLAGS = $(PTHREAD_CFLAGS)
libtcmalloc_internal_with_asserts_la_LIBADD = libstacktrace.la $(PTHREAD_LIBS)

noinst_LTLIBRARIES += libtcmalloc_with_asserts.la
libtcmalloc_with_asserts_la_SOURCES = $(libtcmalloc_la_SOURCES)
libtcmalloc_with_asserts_la_CXXFLAGS =  $(PTHREAD_CFLAGS) $(AM_CXXFLAGS) \
                          $(MAYBE_NO_HEAP_CHECK) $(EMERGENCY_MALLOC_DEFINE)
libtcmalloc_with_asserts_la_LDFLAGS = $(PTHREAD_CFLAGS)
libtcmalloc_with_asserts_la_LIBADD = libtcmalloc_internal_with_asserts.la $(PTHREAD_LIBS)

LIBTCMALLOC = libtcmalloc.la

### Unittests

TESTS += tcmalloc_unittest.sh$(EXEEXT)
tcmalloc_unittest_sh_SOURCES = src/tests/tcmalloc_unittest.sh
noinst_SCRIPTS += $(tcmalloc_unittest_sh_SOURCES)
tcmalloc_unittest.sh$(EXEEXT): $(top_srcdir)/$(tcmalloc_unittest_sh_SOURCES) \
                               tcmalloc_unittest
	rm -f $@
	cp -p $(top_srcdir)/$(tcmalloc_unittest_sh_SOURCES) $@

noinst_PROGRAMS += tcmalloc_unittest
tcmalloc_unittest_SOURCES = src/tests/tcmalloc_unittest.cc \
                            src/tcmalloc_internal.h \
                            src/tests/testutil.h src/tests/testutil.cc
tcmalloc_unittest_CXXFLAGS = $(PTHREAD_CFLAGS) $(AM_CXXFLAGS)
tcmalloc_unittest_LDFLAGS = $(PTHREAD_CFLAGS) $(TCMALLOC_FLAGS)
# We want libtcmalloc last on the link line, but due to a bug in
# libtool involving convenience libs, they need to come last on the
# link line in order to get dependency ordering right.  This is ok:
# convenience libraries are .a's, so tcmalloc is still the last .so.
# We also put pthreads after tcmalloc, because some pthread
# implementations define their own malloc, and we need to go on the
# first linkline to make sure our malloc 'wins'.
tcmalloc_unittest_LDADD = $(LIBTCMALLOC) liblogging.la $(PTHREAD_LIBS)

TESTS += tcm_asserts_unittest
tcm_asserts_unittest_SOURCES = src/tests/tcmalloc_unittest.cc \
                            src/tcmalloc_internal.h \
                            src/tests/testutil.h src/tests/testutil.cc
tcm_asserts_unittest_CXXFLAGS = $(PTHREAD_CFLAGS) $(AM_CXXFLAGS)
tcm_asserts_unittest_LDFLAGS = $(PTHREAD_CFLAGS) $(TCMALLOC_FLAGS)
tcm_asserts_unittest_LDADD = libtcmalloc_with_asserts.la liblogging.la $(PTHREAD_LIBS)

# This makes sure it's safe to link in both tcmalloc and
# tcmalloc_minimal.  (One would never do this on purpose, but perhaps
# by accident...)  When we can compile libprofiler, we also link it in
# to make sure that works too.  NOTE: On OS X, it's *not* safe to
# link both in (we end up with two copies of every global var, and
# the code tends to pick one arbitrarily), so don't run the test there.
# (We define these outside the 'if' because they're reused below.)
tcmalloc_both_unittest_srcs = src/tests/tcmalloc_unittest.cc \
                              src/tests/testutil.h src/tests/testutil.cc
tcmalloc_both_unittest_cflags = $(PTHREAD_CFLAGS) $(AM_CXXFLAGS)
tcmalloc_both_unittest_lflags = $(PTHREAD_CFLAGS) $(TCMALLOC_FLAGS)
if WITH_CPU_PROFILER
# We want libtcmalloc last on the link line, but due to a bug in
# libtool involving convenience libs, they need to come last on the
# link line in order to get dependency ordering right.  This is ok:
# convenience libraries are .a's, so tcmalloc is still the last .so.
# We also put pthreads after tcmalloc, because some pthread
# implementations define their own malloc, and we need to go on the
# first linkline to make sure our malloc 'wins'.
tcmalloc_both_unittest_ladd = $(LIBTCMALLOC) libtcmalloc_minimal.la \
                              libprofiler.la liblogging.la $(PTHREAD_LIBS)
else
tcmalloc_both_unittest_ladd = $(LIBTCMALLOC) libtcmalloc_minimal.la \
                              liblogging.la $(PTHREAD_LIBS)
endif !WITH_CPU_PROFILER
if !OSX
TESTS += tcmalloc_both_unittest
tcmalloc_both_unittest_SOURCES = $(tcmalloc_both_unittest_srcs)
tcmalloc_both_unittest_CXXFLAGS = $(tcmalloc_both_unittest_cflags)
tcmalloc_both_unittest_LDFLAGS = $(tcmalloc_both_unittest_lflags)
tcmalloc_both_unittest_LDADD = $(tcmalloc_both_unittest_ladd)
endif !OSX

TESTS += tcmalloc_large_unittest
tcmalloc_large_unittest_SOURCES = src/tests/tcmalloc_large_unittest.cc
tcmalloc_large_unittest_CXXFLAGS = $(PTHREAD_CFLAGS) $(AM_CXXFLAGS)
tcmalloc_large_unittest_LDFLAGS = $(PTHREAD_CFLAGS) $(TCMALLOC_FLAGS)
tcmalloc_large_unittest_LDADD = $(LIBTCMALLOC) $(PTHREAD_LIBS)

TESTS += tcmalloc_large_heap_fragmentation_unittest
tcmalloc_large_heap_fragmentation_unittest_SOURCES = src/tests/large_heap_fragmentation_unittest.cc
tcmalloc_large_heap_fragmentation_unittest_CXXFLAGS = $(PTHREAD_CFLAGS) $(AM_CXXFLAGS)
tcmalloc_large_heap_fragmentation_unittest_LDFLAGS = $(PTHREAD_CFLAGS) $(TCMALLOC_FLAGS)
tcmalloc_large_heap_fragmentation_unittest_LDADD = $(LIBTCMALLOC) $(PTHREAD_LIBS)

TESTS += raw_printer_test
raw_printer_test_SOURCES = src/tests/raw_printer_test.cc
raw_printer_test_CXXFLAGS = $(PTHREAD_CFLAGS) $(AM_CXXFLAGS)
raw_printer_test_LDFLAGS = $(PTHREAD_CFLAGS) $(TCMALLOC_FLAGS)
raw_printer_test_LDADD = $(LIBTCMALLOC) $(PTHREAD_LIBS)

# sampler_test and sampling_test both require sampling to be turned
# on, which it's not by default.  Use the "standard" value of 2^19.
TESTS_ENVIRONMENT += TCMALLOC_SAMPLE_PARAMETER=524288

TESTS += sampler_test
sampler_test_SOURCES = src/tests/sampler_test.cc \
                       src/config_for_unittests.h
sampler_test_CXXFLAGS = $(PTHREAD_CFLAGS) $(AM_CXXFLAGS)
sampler_test_LDFLAGS = $(PTHREAD_CFLAGS) $(TCMALLOC_FLAGS)
sampler_test_LDADD = $(LIBTCMALLOC) $(PTHREAD_LIBS) -lm


# These unittests often need to run binaries.  They're in the current dir
TESTS_ENVIRONMENT += BINDIR=.
TESTS_ENVIRONMENT += TMPDIR=/tmp/perftools

TESTS += sampling_test.sh$(EXEEXT)
sampling_test_sh_SOURCES = src/tests/sampling_test.sh
noinst_SCRIPTS += $(sampling_test_sh_SOURCES)
sampling_test.sh$(EXEEXT): $(top_srcdir)/$(sampling_test_sh_SOURCES) \
                           sampling_test
	rm -f $@
	cp -p $(top_srcdir)/$(sampling_test_sh_SOURCES) $@

# This is the sub-program used by sampling_test.sh
# The -g is so pprof can get symbol information.
noinst_PROGRAMS += sampling_test
sampling_test_SOURCES = src/tests/sampling_test.cc
sampling_test_CXXFLAGS = -g $(PTHREAD_CFLAGS) $(AM_CXXFLAGS)
sampling_test_LDFLAGS = -g $(PTHREAD_CFLAGS) $(TCMALLOC_FLAGS)
sampling_test_LDADD = $(LIBTCMALLOC) $(PTHREAD_LIBS)

endif WITH_HEAP_PROFILER_OR_CHECKER

if WITH_HEAP_PROFILER

TESTS += heap-profiler_unittest.sh$(EXEEXT)
heap_profiler_unittest_sh_SOURCES = src/tests/heap-profiler_unittest.sh
noinst_SCRIPTS += $(heap_profiler_unittest_sh_SOURCES)
heap-profiler_unittest.sh$(EXEEXT): $(top_srcdir)/$(heap_profiler_unittest_sh_SOURCES) \
                                    heap-profiler_unittest
	rm -f $@
	cp -p $(top_srcdir)/$(heap_profiler_unittest_sh_SOURCES) $@

# These are sub-programs used by heap-profiler_unittest.sh
noinst_PROGRAMS += heap-profiler_unittest
heap_profiler_unittest_SOURCES = src/tests/heap-profiler_unittest.cc
heap_profiler_unittest_CXXFLAGS = -g $(PTHREAD_CFLAGS) $(AM_CXXFLAGS)
heap_profiler_unittest_LDFLAGS = -g $(PTHREAD_CFLAGS) $(TCMALLOC_FLAGS)
heap_profiler_unittest_LDADD = $(LIBTCMALLOC) $(PTHREAD_LIBS)

endif WITH_HEAP_PROFILER

if WITH_HEAP_CHECKER

TESTS += heap-checker-death_unittest.sh$(EXEEXT)
heap_checker_death_unittest_sh_SOURCES = src/tests/heap-checker-death_unittest.sh
noinst_SCRIPTS += $(top_srcdir)/$(heap_checker_death_unittest_sh_SOURCES)
heap-checker-death_unittest.sh$(EXEEXT): $(heap_checker_death_unittest_sh_SOURCES) \
                                         heap-checker_unittest
	rm -f $@
	cp -p $(top_srcdir)/$(heap_checker_death_unittest_sh_SOURCES) $@

# These are sub-programs used by heap-checker_unittest.sh
TESTS += heap-checker_unittest
heap_checker_unittest_SOURCES = src/tests/heap-checker_unittest.cc
heap_checker_unittest_CXXFLAGS = -g $(PTHREAD_CFLAGS) $(AM_CXXFLAGS)
heap_checker_unittest_LDFLAGS = -g $(PTHREAD_CFLAGS) $(TCMALLOC_FLAGS)
# We want libtcmalloc last on the link line, but due to a bug in
# libtool involving convenience libs, they need to come last on the
# link line in order to get dependency ordering right.  This is ok:
# convenience libraries are .a's, so tcmalloc is still the last .so.
# We also put pthreads after tcmalloc, because some pthread
# implementations define their own malloc, and we need to go on the
# first linkline to make sure our malloc 'wins'.
heap_checker_unittest_LDADD = $(LIBTCMALLOC) liblogging.la $(PTHREAD_LIBS)

endif WITH_HEAP_CHECKER

### Documentation (above and beyond tcmalloc_minimal documentation)
if WITH_HEAP_PROFILER
dist_doc_DATA += docs/heapprofile.html docs/heap-example1.png
endif WITH_HEAP_PROFILER

if WITH_HEAP_CHECKER
dist_doc_DATA += docs/heap_checker.html
endif WITH_HEAP_CHECKER


### ------- tcmalloc with debugallocation

if WITH_DEBUGALLOC
if WITH_HEAP_PROFILER_OR_CHECKER

lib_LTLIBRARIES += libtcmalloc_debug.la
libtcmalloc_debug_la_SOURCES = src/debugallocation.cc $(HEAP_CHECKER_SOURCES)
libtcmalloc_debug_la_CXXFLAGS = $(libtcmalloc_la_CXXFLAGS) \
                                -DTCMALLOC_FOR_DEBUGALLOCATION
libtcmalloc_debug_la_LDFLAGS = $(libtcmalloc_la_LDFLAGS) \
                               -version-info @TCMALLOC_SO_VERSION@
libtcmalloc_debug_la_LIBADD = $(libtcmalloc_la_LIBADD)

### Unittests

TESTS += tcmalloc_debug_unittest
tcmalloc_debug_unittest_SOURCES = $(tcmalloc_unittest_SOURCES)
tcmalloc_debug_unittest_CXXFLAGS = $(tcmalloc_unittest_CXXFLAGS) \
                                   -DDEBUGALLOCATION
tcmalloc_debug_unittest_LDFLAGS = $(tcmalloc_unittest_LDFLAGS)
tcmalloc_debug_unittest_LDADD = libtcmalloc_debug.la $(PTHREAD_LIBS)

TESTS += sampler_debug_test
sampler_debug_test_SOURCES = $(sampler_test_SOURCES)
sampler_debug_test_CXXFLAGS = $(samples_test_CXXFLAGS)
sampler_debug_test_LDFLAGS = $(sampler_test_LDFLAGS)
sampler_debug_test_LDADD = libtcmalloc_debug.la $(PTHREAD_LIBS) -lm

TESTS += sampling_debug_test.sh$(EXEEXT)
sampling_debug_test_sh_SOURCES = src/tests/sampling_test.sh
sampling_debug_test.sh$(EXEEXT): $(top_srcdir)/$(sampling_test_sh_SOURCES) \
                                 sampling_debug_test
	rm -f $@
	sed -e 's/sampling_test/sampling_debug_test/g' <$(top_srcdir)/$(sampling_test_sh_SOURCES) >$@
	chmod +x $@

# This is the sub-program using by sampling_debug_test.sh
# The -g is so pprof can get symbol information.
noinst_PROGRAMS += sampling_debug_test
sampling_debug_test_SOURCES = $(sampling_test_SOURCES)
sampling_debug_test_CXXFLAGS = $(sampling_test_CXXFLAGS)
sampling_debug_test_LDFLAGS = $(sampling_test_LDFLAGS)
sampling_debug_test_LDADD = libtcmalloc_debug.la $(PTHREAD_LIBS)

endif WITH_HEAP_PROFILER_OR_CHECKER

if WITH_HEAP_PROFILER

TESTS += heap-profiler_debug_unittest.sh$(EXEEXT)
heap_profiler_debug_unittest_sh_SOURCES = src/tests/heap-profiler_unittest.sh
heap-profiler_debug_unittest.sh$(EXEEXT): $(top_srcdir)/$(heap_profiler_unittest_sh_SOURCES) \
                                    heap-profiler_debug_unittest
	rm -f $@
	cp -p $(top_srcdir)/$(heap_profiler_unittest_sh_SOURCES) $@

# These are sub-programs used by heap-profiler_debug_unittest.sh
noinst_PROGRAMS += heap-profiler_debug_unittest
heap_profiler_debug_unittest_SOURCES = $(heap_profiler_unittest_SOURCES)
heap_profiler_debug_unittest_CXXFLAGS = $(heap_profiler_unittest_CXXFLAGS)
heap_profiler_debug_unittest_LDFLAGS = $(heap_profiler_unittest_LDFLAGS)
heap_profiler_debug_unittest_LDADD = libtcmalloc_debug.la $(PTHREAD_LIBS)

endif WITH_HEAP_PROFILER

if WITH_HEAP_CHECKER

TESTS += heap-checker_debug_unittest
heap_checker_debug_unittest_SOURCES = $(heap_checker_unittest_SOURCES)
heap_checker_debug_unittest_CXXFLAGS = $(heap_checker_unittest_CXXFLAGS)
heap_checker_debug_unittest_LDFLAGS = $(heap_checker_unittest_LDFLAGS)
# We want libtcmalloc last on the link line, but due to a bug in
# libtool involving convenience libs, they need to come last on the
# link line in order to get dependency ordering right.  This is ok:
# convenience libraries are .a's, so tcmalloc is still the last .so.
heap_checker_debug_unittest_LDADD = libtcmalloc_debug.la liblogging.la \
                                    $(PTHREAD_LIBS)

endif WITH_HEAP_CHECKER
endif WITH_DEBUGALLOC


### ------- CPU profiler

if WITH_CPU_PROFILER

perftoolsinclude_HEADERS += src/gperftools/profiler.h

### Making the library
lib_LTLIBRARIES += libprofiler.la
libprofiler_la_SOURCES = src/profiler.cc \
                         src/profile-handler.cc \
                         src/profiledata.cc
libprofiler_la_LIBADD = libstacktrace.la libfake_stacktrace_scope.la $(PTHREAD_LIBS)
libprofiler_la_CXXFLAGS = $(PTHREAD_CFLAGS) $(AM_CXXFLAGS)
# We have to include ProfileData for profiledata_unittest
CPU_PROFILER_SYMBOLS = '(ProfilerStart|ProfilerStartWithOptions|ProfilerStop|ProfilerFlush|ProfilerEnable|ProfilerDisable|ProfilingIsEnabledForAllThreads|ProfilerRegisterThread|ProfilerGetCurrentState|ProfilerState|ProfileData|ProfileHandler|ProfilerGetStackTrace)'
libprofiler_la_LDFLAGS = -export-symbols-regex $(CPU_PROFILER_SYMBOLS) \
                         -version-info @PROFILER_SO_VERSION@

### Unittests
TESTS += getpc_test
getpc_test_SOURCES = src/tests/getpc_test.cc src/getpc.h

TESTS += profiledata_unittest
profiledata_unittest_SOURCES = src/tests/profiledata_unittest.cc \
                               src/profiledata.h \
                               src/base/commandlineflags.h \
                               src/base/logging.h \
                               src/base/threading.h \
                               src/base/basictypes.h
profiledata_unittest_LDADD = libprofiler.la

TESTS += profile_handler_unittest
profile_handler_unittest_SOURCES = src/tests/profile-handler_unittest.cc \
                                   src/profile-handler.h
profile_handler_unittest_CXXFLAGS = $(PTHREAD_CFLAGS) $(AM_CXXFLAGS)
profile_handler_unittest_LDFLAGS = $(PTHREAD_CFLAGS) $(AM_LDFLAGS)
profile_handler_unittest_LDADD = libprofiler.la $(PTHREAD_LIBS)

TESTS += profiler_unittest.sh$(EXEEXT)
profiler_unittest_sh_SOURCES = src/tests/profiler_unittest.sh
noinst_SCRIPTS += $(profiler_unittest_sh_SOURCES)
profiler_unittest.sh$(EXEEXT): $(top_srcdir)/$(profiler_unittest_sh_SOURCES) \
                               profiler1_unittest profiler2_unittest \
                               profiler3_unittest profiler4_unittest
	rm -f $@
	cp -p $(top_srcdir)/$(profiler_unittest_sh_SOURCES) $@

# These are sub-programs used by profiler_unittest.sh
noinst_PROGRAMS += profiler1_unittest profiler2_unittest profiler3_unittest \
                   profiler4_unittest
PROFILER_UNITTEST_SRCS = src/tests/profiler_unittest.cc \
                         src/tests/testutil.h src/tests/testutil.cc
profiler1_unittest_SOURCES = $(PROFILER_UNITTEST_SRCS)
profiler1_unittest_CXXFLAGS = -g -DNO_THREADS $(AM_CXXFLAGS)
profiler1_unittest_LDADD = libprofiler.la
profiler2_unittest_SOURCES = $(PROFILER_UNITTEST_SRCS)
profiler2_unittest_CXXFLAGS = -g -DNO_THREADS $(AM_CXXFLAGS)
profiler2_unittest_LDADD = -lstacktrace -lprofiler
# We depend on -lprofiler but haven't yet said how to build it.  Do so now.
profiler2_unittest_DEPENDENCIES = libprofiler.la
profiler3_unittest_SOURCES = $(PROFILER_UNITTEST_SRCS)
profiler3_unittest_CXXFLAGS = -g $(PTHREAD_CFLAGS) $(AM_CXXFLAGS)
profiler3_unittest_LDFLAGS = $(PTHREAD_CFLAGS)
profiler3_unittest_LDADD = libprofiler.la $(PTHREAD_LIBS)
profiler4_unittest_SOURCES = $(PROFILER_UNITTEST_SRCS)
profiler4_unittest_CXXFLAGS = -g $(PTHREAD_CFLAGS) $(AM_CXXFLAGS)
profiler4_unittest_LDFLAGS = $(PTHREAD_CFLAGS)
profiler4_unittest_LDADD = -lstacktrace -lprofiler $(PTHREAD_LIBS)
# We depend on -lprofiler but haven't yet said how to build it.  Do so now.
profiler4_unittest_DEPENDENCIES = libprofiler.la


### Documentation
dist_doc_DATA += docs/cpuprofile.html \
                 docs/cpuprofile-fileformat.html \
                 docs/pprof-test-big.gif \
                 docs/pprof-test.gif \
                 docs/pprof-vsnprintf-big.gif \
                 docs/pprof-vsnprintf.gif

endif WITH_CPU_PROFILER


### ------- CPU profiler and heap checker, in one!

# Ideally, folks who wanted to use both tcmalloc and libprofiler,
# could just link them both into their application.  But while this
# works fine for .so files, it does not for .a files.  The easiest way
# around this -- and I've tried a bunch of the hard ways -- is to just
# to create another set of libraries that has both functionality in it.

if WITH_HEAP_PROFILER_OR_CHECKER
if WITH_CPU_PROFILER

lib_LTLIBRARIES += libtcmalloc_and_profiler.la
libtcmalloc_and_profiler_la_SOURCES = $(libtcmalloc_la_SOURCES) $(libprofiler_la_SOURCES)
libtcmalloc_and_profiler_la_CXXFLAGS = $(libtcmalloc_la_CXXFLAGS) $(libprofiler_la_CXXFLAGS)
# Since this library is meant to be used as a .a, I don't worry as much
# about .so versioning.  I just give the libtcmalloc version number.
# TODO(csilvers): use -export-symbols-regex?
libtcmalloc_and_profiler_la_LDFLAGS = $(PTHREAD_CFLAGS) \
                                      -version-info @TCMALLOC_AND_PROFILER_SO_VERSION@
# We don't include libprofiler_la_LIBADD here because all it adds is
# libstacktrace.la, which we already get via libtcmalloc.  Trying to
# specify it twice causes link-time duplicate-definition errors. :-(
libtcmalloc_and_profiler_la_LIBADD = $(libtcmalloc_la_LIBADD)

TESTS += tcmalloc_and_profiler_unittest
tcmalloc_and_profiler_unittest_SOURCES = $(tcmalloc_both_unittest_srcs)
tcmalloc_and_profiler_unittest_CXXFLAGS = $(tcmalloc_both_unittest_cflags)
tcmalloc_and_profiler_unittest_LDFLAGS = $(tcmalloc_both_unittest_lflags)
tcmalloc_and_profiler_unittest_LDADD = libtcmalloc_and_profiler.la

endif WITH_CPU_PROFILER
endif WITH_HEAP_PROFILER_OR_CHECKER

## ^^^^ END OF RULES TO MAKE YOUR LIBRARIES, BINARIES, AND UNITTESTS

# This should always include $(TESTS), but may also include other
# binaries that you compile but don't want automatically installed.
# We'll add to this later, on a library-by-library basis
noinst_PROGRAMS += $(TESTS)

# http://linux.die.net/man/1/pkg-config, http://pkg-config.freedesktop.org/wiki
pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = libtcmalloc_minimal.pc
if WITH_DEBUGALLOC
pkgconfig_DATA += libtcmalloc_minimal_debug.pc
endif WITH_DEBUGALLOC

if WITH_HEAP_PROFILER_OR_CHECKER
pkgconfig_DATA += libtcmalloc.pc
if WITH_DEBUGALLOC
pkgconfig_DATA += libtcmalloc_debug.pc
endif WITH_DEBUGALLOC
endif WITH_HEAP_PROFILER_OR_CHECKER

if WITH_CPU_PROFILER
pkgconfig_DATA += libprofiler.pc
endif WITH_CPU_PROFILER
CLEANFILES = $(pkgconfig_DATA)

libtcmalloc.pc: Makefile
	echo 'prefix=$(prefix)' > "$@".tmp
	echo 'exec_prefix='`echo '$(exec_prefix)' | sed 's@^$(prefix)@$${prefix}@'` >> "$@".tmp
	echo 'libdir='`echo '$(libdir)' | sed 's@^$(exec_prefix)@$${exec_prefix}@'` >> "$@".tmp
	echo 'includedir='`echo '$(includedir)' | sed 's@^$(prefix)@$${prefix}@'` >> "$@".tmp
	echo '' >> "$@".tmp
	echo 'Name: $(PACKAGE)' >> "$@".tmp
	echo 'Version: $(VERSION)' >> "$@".tmp
	echo 'Description: Performance tools for C++' >> "$@".tmp
	echo 'URL: https://github.com/gperftools/gperftools' >> "$@".tmp
	echo 'Requires:' >> "$@".tmp
	echo 'Libs: -L$${libdir} -ltcmalloc' >> "$@".tmp
	echo 'Libs.private: $(PTHREAD_CFLAGS) $(PTHREAD_LIBS)' >> "$@".tmp
	echo 'Cflags: -I$${includedir}' >> "$@".tmp
	mv -f "$@".tmp "$@"

libtcmalloc_minimal.pc: libtcmalloc.pc
	cat libtcmalloc.pc | sed s/-ltcmalloc/-ltcmalloc_minimal/ > "$@"

libtcmalloc_debug.pc: libtcmalloc.pc
	cat libtcmalloc.pc | sed s/-ltcmalloc/-ltcmalloc_debug/ > "$@"

libtcmalloc_minimal_debug.pc: libtcmalloc.pc
	cat libtcmalloc.pc | sed s/-ltcmalloc/-ltcmalloc_minimal_debug/ > "$@"

libprofiler.pc: libtcmalloc.pc
	cat libtcmalloc.pc | sed s/-ltcmalloc/-lprofiler/ > "$@"

$(top_distdir)/ChangeLog:
	if test ! -f $(top_srcdir)/ChangeLog ; \
	  then git --git-dir=$(top_srcdir)/.git --work-tree=$(top_srcdir) \
	        log --stat -M -C --name-status --no-color \
	       | fmt --split-only >$(top_distdir)/ChangeLog; \
	  else cp $(top_srcdir)/ChangeLog $(top_distdir)/ChangeLog; fi

EXTRA_DIST = $(SCRIPTS) \
             src/windows/get_mangled_names.cc src/windows/override_functions.cc \
             docs/pprof.see_also src/windows/TODO \
             $(WINDOWS_EXTRA) \
             gperftools.sln vsprojects \
             $(top_srcdir)/src/*h $(top_srcdir)/src/base/*h \
             $(top_srcdir)/src/windows/*h $(top_srcdir)/src/gperftools/*h \
             CMakeLists.txt cmake

# Windows wants write permission to .vcxproj files and maybe even sln files.
dist-hook: $(top_distdir)/ChangeLog
	test -e "$(distdir)/vsprojects" \
	   && chmod -R u+w $(distdir)/*.sln $(distdir)/vsprojects/
	rm -f $(distdir)/src/config.h $(distdir)/src/gperftools/tcmalloc.h
