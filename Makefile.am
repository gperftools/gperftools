## Process this file with automake to produce Makefile.in

# Note: for every library we create, we're explicit about what symbols
# we export.  In order to avoid complications with C++ mangling, we always
# use the regexp for of specifying symbols.

# Make sure that when we re-make ./configure, we get the macros we need
ACLOCAL_AMFLAGS = -I m4
AUTOMAKE_OPTIONS = subdir-objects

# This is so we can #include <gperftools/foo>
AM_CPPFLAGS = -I$(top_srcdir)/src -I$(top_builddir)/src

if !WITH_STACK_TRACE
AM_CPPFLAGS += -DNO_TCMALLOC_SAMPLES
endif !WITH_STACK_TRACE

# This is mostly based on configure options
AM_CXXFLAGS = $(PTHREAD_CFLAGS)

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
AM_LDFLAGS = -no-undefined $(NANOSLEEP_LIBS) $(PTHREAD_LIBS)

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
if MINGW
TCMALLOC_FLAGS = -Wl,-u__tcmalloc
else
TCMALLOC_FLAGS =
endif !MINGW

perftoolsincludedir = $(includedir)/gperftools
# The .h files you want to install (that is, .h files that people
# who install this package can include in their own applications.)
# We'll add to this later, on a library-by-library basis
perftoolsinclude_HEADERS = src/gperftools/tcmalloc.h

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

### ------- various support library routines

# Having set of common helpers helps with unit testing various "guts"
noinst_LTLIBRARIES += libcommon.la
libcommon_la_SOURCES = src/base/logging.cc \
                       src/base/generic_writer.cc \
                       src/base/sysinfo.cc \
                       src/base/proc_maps_iterator.cc \
                       src/base/dynamic_annotations.cc \
                       src/base/spinlock.cc \
                       src/base/spinlock_internal.cc

noinst_LTLIBRARIES += liblow_level_alloc.la
liblow_level_alloc_la_SOURCES = src/base/low_level_alloc.cc

if MINGW
libcommon_la_SOURCES += src/windows/port.cc \
                        src/windows/ia32_modrm_map.cc \
                        src/windows/ia32_opcode_map.cc \
                        src/windows/mini_disassembler.cc \
                        src/windows/preamble_patcher.cc \
                        src/windows/preamble_patcher_with_stub.cc

# windows bits need some extra libraries
AM_LDFLAGS += -lpsapi -lsynchronization -lshlwapi

# patch_functions.cc #includes tcmalloc.cc, so no need to link it in.
TCMALLOC_CC = src/windows/patch_functions.cc
SYSTEM_ALLOC_CC = src/windows/system-alloc.cc

else !MINGW

TCMALLOC_CC = src/tcmalloc.cc
SYSTEM_ALLOC_CC = src/system-alloc.cc

endif !MINGW

# On MSVC, we need couple more tests.
WINDOWS_EXTRA = src/windows/preamble_patcher_test.cc \
                src/windows/shortproc.asm \
                src/windows/auto_testing_hook.h

### Unittests

noinst_LTLIBRARIES += libgtest.la
libgtest_la_SOURCES = vendor/googletest/googletest/src/gtest_main.cc \
 vendor/googletest/googletest/src/gtest-assertion-result.cc \
 vendor/googletest/googletest/src/gtest-death-test.cc \
 vendor/googletest/googletest/src/gtest-filepath.cc \
 vendor/googletest/googletest/src/gtest-matchers.cc \
 vendor/googletest/googletest/src/gtest-port.cc \
 vendor/googletest/googletest/src/gtest-printers.cc \
 vendor/googletest/googletest/src/gtest-test-part.cc \
 vendor/googletest/googletest/src/gtest-typed-test.cc \
 vendor/googletest/googletest/src/gtest.cc

libgtest_la_CPPFLAGS = -I$(top_srcdir)/vendor/googletest/googletest/include \
 -I$(top_srcdir)/vendor/googletest/googletest/ $(AM_CPPFLAGS)
libgtest_la_LIBADD = $(REGEX_LIBS)

gtest_CPPFLAGS = -I$(top_srcdir)/vendor/googletest/googletest/include $(AM_CPPFLAGS)

# Note, we skip this test on mingw (and windows in general). It uses
# unsetenv, which is not available on win32.
if !MINGW
TESTS += unique_path_unittest
unique_path_unittest_SOURCES = src/tests/unique_path_unittest.cc
unique_path_unittest_LDFLAGS = $(PTHREAD_CFLAGS) $(AM_LDFLAGS)
unique_path_unittest_CPPFLAGS = $(gtest_CPPFLAGS)
unique_path_unittest_LDADD = libcommon.la libgtest.la
endif !MINGW

TESTS += generic_writer_test
generic_writer_test_SOURCES = src/tests/generic_writer_test.cc
generic_writer_test_CPPFLAGS = $(gtest_CPPFLAGS)
generic_writer_test_LDADD = libcommon.la libgtest.la

TESTS += proc_maps_iterator_test
proc_maps_iterator_test_SOURCES = src/tests/proc_maps_iterator_test.cc
proc_maps_iterator_test_CPPFLAGS = $(gtest_CPPFLAGS)
proc_maps_iterator_test_LDADD = libcommon.la libgtest.la

if WITH_HEAP_PROFILER_OR_CHECKER
TESTS += low_level_alloc_unittest
low_level_alloc_unittest_SOURCES = src/tests/low_level_alloc_unittest.cc
# By default, MallocHook takes stack traces for use by the heap-checker.
# We don't need that functionality here, so we turn it off to reduce deps.
low_level_alloc_unittest_CXXFLAGS = -DNO_TCMALLOC_SAMPLES $(AM_CXXFLAGS)
low_level_alloc_unittest_CPPFLAGS = $(gtest_CPPFLAGS)
low_level_alloc_unittest_LDADD = liblow_level_alloc.la libcommon.la libgtest.la
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
libstacktrace_la_LIBADD = $(UNWIND_LIBS)

### Unittests
TESTS += stacktrace_unittest
stacktrace_unittest_SOURCES = src/tests/stacktrace_unittest.cc \
                              $(libstacktrace_la_SOURCES)
stacktrace_unittest_CXXFLAGS = $(AM_CXXFLAGS) -DSTACKTRACE_IS_TESTED
stacktrace_unittest_LDADD = $(libstacktrace_la_LIBADD) $(STACKTRACE_UNITTEST_LIBS) libcommon.la
# nice to have. Allows glibc's backtrace_symbols to work.
stacktrace_unittest_LDFLAGS = -export-dynamic

TESTS += check_address_test
check_address_test_SOURCES = src/tests/check_address_test.cc
check_address_test_CPPFLAGS = $(gtest_CPPFLAGS)
check_address_test_LDADD = libcommon.la libgtest.la

endif WITH_STACK_TRACE

### ------- tcmalloc_minimal (thread-caching malloc)

perftoolsinclude_HEADERS += src/gperftools/malloc_hook.h \
                            src/gperftools/malloc_hook_c.h \
                            src/gperftools/malloc_extension.h \
                            src/gperftools/malloc_extension_c.h \
                            src/gperftools/nallocx.h

### Making the library

MINIMAL_MALLOC_SRC = src/common.cc \
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
                     src/thread_cache.cc \
                     src/thread_cache_ptr.cc \
                     src/malloc_hook.cc \
                     src/malloc_extension.cc

lib_LTLIBRARIES += libtcmalloc_minimal.la
libtcmalloc_minimal_la_SOURCES = $(TCMALLOC_CC) $(MINIMAL_MALLOC_SRC)

libtcmalloc_minimal_la_CXXFLAGS = -DNO_TCMALLOC_SAMPLES \
                                  -DNDEBUG $(AM_CXXFLAGS)
# -version-info gets passed to libtool
libtcmalloc_minimal_la_LDFLAGS = -version-info @TCMALLOC_SO_VERSION@ $(AM_LDFLAGS)
libtcmalloc_minimal_la_LIBADD = libcommon.la

### ------- unit tests for various internal modules of tcmalloc

TESTS += addressmap_unittest
addressmap_unittest_SOURCES = src/tests/addressmap_unittest.cc
addressmap_unittest_CPPFLAGS = $(gtest_CPPFLAGS)
addressmap_unittest_LDADD = libcommon.la libgtest.la

TESTS += packed_cache_test
packed_cache_test_SOURCES = src/tests/packed-cache_test.cc src/internal_logging.cc
packed_cache_test_CPPFLAGS = $(gtest_CPPFLAGS)
packed_cache_test_LDADD = libcommon.la libgtest.la

TESTS += safe_strerror_test
safe_strerror_test_SOURCES = src/tests/safe_strerror_test.cc \
                             src/safe_strerror.cc
safe_strerror_test_CPPFLAGS = $(gtest_CPPFLAGS)
safe_strerror_test_LDADD = libcommon.la libgtest.la

TESTS += cleanup_test
cleanup_test_SOURCES = src/tests/cleanup_test.cc
cleanup_test_CPPFLAGS = $(gtest_CPPFLAGS)
cleanup_test_LDADD = libgtest.la

TESTS += function_ref_test
function_ref_test_SOURCES = src/tests/function_ref_test.cc
function_ref_test_CPPFLAGS = $(gtest_CPPFLAGS)
function_ref_test_LDADD = libgtest.la

TESTS += trivialre_test
trivialre_test_SOURCES = benchmark/trivialre_test.cc
trivialre_test_CPPFLAGS = $(gtest_CPPFLAGS)
trivialre_test_LDADD = libgtest.la

TESTS += pagemap_unittest
pagemap_unittest_SOURCES = src/tests/pagemap_unittest.cc \
                           src/internal_logging.cc
pagemap_unittest_CPPFLAGS = $(gtest_CPPFLAGS)
pagemap_unittest_LDADD = libcommon.la libgtest.la

# note, it is not so great that page heap testing requires bringing
# almost entirety of tcmalloc (short of tcmalloc.cc), but it is what
# we have.
TESTS += page_heap_test
page_heap_test_SOURCES = src/tests/page_heap_test.cc \
                         $(libtcmalloc_minimal_la_SOURCES)
page_heap_test_CXXFLAGS = -DNO_TCMALLOC_SAMPLES $(AM_CXXFLAGS)
page_heap_test_CPPFLAGS = $(gtest_CPPFLAGS)
page_heap_test_LDADD = libcommon.la libgtest.la

# note, it is not so great that stack_trace_table testing requires
# bringing almost entirety of tcmalloc (short of tcmalloc.cc), but it
# is what we have.
TESTS += stack_trace_table_test
stack_trace_table_test_SOURCES = src/tests/stack_trace_table_test.cc \
                                 src/stack_trace_table.cc src/internal_logging.cc
stack_trace_table_test_CPPFLAGS = $(gtest_CPPFLAGS)
stack_trace_table_test_CXXFLAGS = -DSTACK_TRACE_TABLE_IS_TESTED $(AM_CXXFLAGS)
stack_trace_table_test_LDADD = libcommon.la libgtest.la

TESTS += malloc_hook_test
malloc_hook_test_SOURCES = src/tests/malloc_hook_test.cc \
                           src/tests/testutil.cc \
                           src/malloc_hook.cc
malloc_hook_test_CXXFLAGS = -DNO_TCMALLOC_SAMPLES $(AM_CXXFLAGS)
malloc_hook_test_CPPFLAGS = $(gtest_CPPFLAGS)
malloc_hook_test_LDADD = libcommon.la libgtest.la

TESTS += sampler_test
sampler_test_SOURCES = src/tests/sampler_test.cc \
                       src/sampler.cc
sampler_test_CPPFLAGS = $(gtest_CPPFLAGS)
sampler_test_LDADD = libcommon.la libgtest.la

### Unittests for libtcmalloc_minimal.la

TESTS += tcmalloc_minimal_unittest
tcmalloc_minimal_unittest_SOURCES = src/tests/tcmalloc_unittest.cc \
                                    src/tests/testutil.cc
tcmalloc_minimal_unittest_LDFLAGS = $(TCMALLOC_FLAGS) $(AM_LDFLAGS)
tcmalloc_minimal_unittest_CPPFLAGS = $(gtest_CPPFLAGS)
tcmalloc_minimal_unittest_LDADD = libtcmalloc_minimal.la libgtest.la

# lets make sure we exerice ASSERTs in at least in statically linked
# configuration
TESTS += tcm_min_asserts_unittest
tcm_min_asserts_unittest_SOURCES = src/tests/tcmalloc_unittest.cc \
                                   src/tests/testutil.cc \
                                   $(libtcmalloc_minimal_la_SOURCES)
tcm_min_asserts_unittest_CXXFLAGS = -DNO_TCMALLOC_SAMPLES \
                                    $(AM_CXXFLAGS)
tcm_min_asserts_unittest_CPPFLAGS = $(gtest_CPPFLAGS)
tcm_min_asserts_unittest_LDFLAGS = $(TCMALLOC_FLAGS) $(AM_LDFLAGS)
tcm_min_asserts_unittest_LDADD = libcommon.la libgtest.la

TESTS += tcmalloc_minimal_large_unittest
tcmalloc_minimal_large_unittest_SOURCES = src/tests/tcmalloc_large_unittest.cc
tcmalloc_minimal_large_unittest_LDFLAGS = $(TCMALLOC_FLAGS) $(AM_LDFLAGS)
tcmalloc_minimal_large_unittest_LDADD = libtcmalloc_minimal.la

TESTS += tcmalloc_minimal_large_heap_fragmentation_unittest
tcmalloc_minimal_large_heap_fragmentation_unittest_SOURCES = src/tests/large_heap_fragmentation_unittest.cc
tcmalloc_minimal_large_heap_fragmentation_unittest_LDFLAGS = $(TCMALLOC_FLAGS) $(AM_LDFLAGS)
tcmalloc_minimal_large_heap_fragmentation_unittest_CPPFLAGS = $(gtest_CPPFLAGS)
tcmalloc_minimal_large_heap_fragmentation_unittest_LDADD = libtcmalloc_minimal.la libgtest.la

if !MINGW
TESTS += system_alloc_unittest
system_alloc_unittest_SOURCES = src/tests/system-alloc_unittest.cc
system_alloc_unittest_LDFLAGS = $(TCMALLOC_FLAGS) $(AM_LDFLAGS)
system_alloc_unittest_CPPFLAGS = $(gtest_CPPFLAGS)
system_alloc_unittest_LDADD = libtcmalloc_minimal.la libgtest.la
endif !MINGW

TESTS += frag_unittest
frag_unittest_SOURCES = src/tests/frag_unittest.cc
frag_unittest_LDFLAGS = $(TCMALLOC_FLAGS) $(AM_LDFLAGS)
frag_unittest_CPPFLAGS = $(gtest_CPPFLAGS)
frag_unittest_LDADD = libtcmalloc_minimal.la libgtest.la

TESTS += markidle_unittest
markidle_unittest_SOURCES = src/tests/markidle_unittest.cc
markidle_unittest_LDFLAGS = $(TCMALLOC_FLAGS) $(AM_LDFLAGS)
markidle_unittest_CPPFLAGS = $(gtest_CPPFLAGS)
markidle_unittest_LDADD = libtcmalloc_minimal.la libgtest.la

TESTS += current_allocated_bytes_test
current_allocated_bytes_test_SOURCES = src/tests/current_allocated_bytes_test.cc
current_allocated_bytes_test_LDFLAGS = $(TCMALLOC_FLAGS) $(AM_LDFLAGS)
current_allocated_bytes_test_CPPFLAGS = $(gtest_CPPFLAGS)
current_allocated_bytes_test_LDADD = libtcmalloc_minimal.la libgtest.la

TESTS += malloc_extension_test
malloc_extension_test_SOURCES = src/tests/malloc_extension_test.cc
malloc_extension_test_LDFLAGS = $(TCMALLOC_FLAGS) $(AM_LDFLAGS)
malloc_extension_test_CPPFLAGS = $(gtest_CPPFLAGS)
malloc_extension_test_LDADD = libtcmalloc_minimal.la libgtest.la

TESTS += malloc_extension_c_test
malloc_extension_c_test_SOURCES = src/tests/malloc_extension_c_test.cc
malloc_extension_c_test_LDFLAGS = $(TCMALLOC_FLAGS) $(AM_LDFLAGS)
malloc_extension_c_test_CPPFLAGS = $(gtest_CPPFLAGS)
malloc_extension_c_test_LDADD = libtcmalloc_minimal.la libgtest.la

if !MINGW
if !OSX
TESTS += memalign_unittest
memalign_unittest_SOURCES = src/tests/memalign_unittest.cc \
                            src/tests/testutil.cc
memalign_unittest_LDFLAGS = $(TCMALLOC_FLAGS) $(AM_LDFLAGS)
memalign_unittest_CPPFLAGS = $(gtest_CPPFLAGS)
memalign_unittest_LDADD = libtcmalloc_minimal.la libgtest.la
endif !OSX
endif !MINGW

TESTS += realloc_unittest
realloc_unittest_SOURCES = src/tests/realloc_unittest.cc
realloc_unittest_LDFLAGS = $(TCMALLOC_FLAGS) $(AM_LDFLAGS)
realloc_unittest_CPPFLAGS = $(gtest_CPPFLAGS)
realloc_unittest_LDADD = libtcmalloc_minimal.la libgtest.la

TESTS += thread_dealloc_unittest
thread_dealloc_unittest_SOURCES = src/tests/thread_dealloc_unittest.cc \
                                  src/tests/testutil.cc
thread_dealloc_unittest_LDFLAGS = $(TCMALLOC_FLAGS) $(AM_LDFLAGS)
thread_dealloc_unittest_LDADD = libtcmalloc_minimal.la

TESTS += min_per_thread_cache_size_test
min_per_thread_cache_size_test_SOURCES = src/tests/min_per_thread_cache_size_test.cc
min_per_thread_cache_size_test_LDFLAGS = $(TCMALLOC_FLAGS) $(AM_LDFLAGS)
min_per_thread_cache_size_test_CPPFLAGS = $(gtest_CPPFLAGS)
min_per_thread_cache_size_test_LDADD = libtcmalloc_minimal.la libgtest.la

### Documentation
dist_doc_DATA += $(top_srcdir)/docs/*adoc $(top_srcdir)/docs/*gif $(top_srcdir)/docs/*png $(top_srcdir)/docs/dots/*dot

gperftools_HTMLDOCS = docs/tcmalloc.html docs/heapprofile.html \
       docs/cpuprofile.html docs/cpuprofile-fileformat.html \
       docs/pprof_integration.html

if !MISSING_ASCIIDOCTOR
doc_DATA = $(gperftools_HTMLDOCS)
MOSTLYCLEANFILES = $(gperftools_HTMLDOCS)

.adoc.html:
	$(ASCIIDOCTOR) $(ASCIIDOCTOR_FLAGS) -o $@ $<
endif !MISSING_ASCIIDOCTOR

### ------- tcmalloc_minimal_debug (thread-caching malloc with debugallocation)

if WITH_DEBUGALLOC

noinst_LTLIBRARIES += libbacktrace.la
libbacktrace_la_SOURCES = vendor/libbacktrace-integration/file-format.c \
                          vendor/libbacktrace/dwarf.c \
                          vendor/libbacktrace/fileline.c \
                          vendor/libbacktrace/posix.c \
                          vendor/libbacktrace/sort.c \
                          vendor/libbacktrace/state.c \
                          vendor/libbacktrace/read.c
# note, we're not including our "usual" project-wide AM_CPPFLAGS here
libbacktrace_la_CPPFLAGS = -I$(top_srcdir)/vendor/libbacktrace-integration \
                           -I$(top_srcdir)/vendor/libbacktrace


noinst_LTLIBRARIES += libsymbolize.la
libsymbolize_la_SOURCES = src/symbolize.cc vendor/libbacktrace-integration/backtrace-alloc.cc
libsymbolize_la_LIBADD = libbacktrace.la

lib_LTLIBRARIES += libtcmalloc_minimal_debug.la
libtcmalloc_minimal_debug_la_SOURCES = src/debugallocation.cc \
                                       $(MINIMAL_MALLOC_SRC)
libtcmalloc_minimal_debug_la_CXXFLAGS = $(libtcmalloc_minimal_la_CXXFLAGS)
libtcmalloc_minimal_debug_la_LDFLAGS = $(libtcmalloc_minimal_la_LDFLAGS)
libtcmalloc_minimal_debug_la_LIBADD = libsymbolize.la liblow_level_alloc.la $(libtcmalloc_minimal_la_LIBADD)

### Unittests

TESTS += tcmalloc_minimal_debug_unittest
tcmalloc_minimal_debug_unittest_SOURCES = $(tcmalloc_minimal_unittest_SOURCES)
tcmalloc_minimal_debug_unittest_LDFLAGS = $(tcmalloc_minimal_unittest_LDFLAGS)
tcmalloc_minimal_debug_unittest_CPPFLAGS = $(gtest_CPPFLAGS)
tcmalloc_minimal_debug_unittest_LDADD = libtcmalloc_minimal_debug.la libgtest.la

TESTS += malloc_extension_debug_test
malloc_extension_debug_test_SOURCES = $(malloc_extension_test_SOURCES)
malloc_extension_debug_test_CXXFLAGS = $(malloc_extension_test_CXXFLAGS)
malloc_extension_debug_test_LDFLAGS = $(malloc_extension_test_LDFLAGS)
malloc_extension_debug_test_CPPFLAGS = $(gtest_CPPFLAGS)
malloc_extension_debug_test_LDADD = libtcmalloc_minimal_debug.la libgtest.la

if !MINGW
if !OSX
TESTS += memalign_debug_unittest
memalign_debug_unittest_SOURCES = $(memalign_unittest_SOURCES)
memalign_debug_unittest_CXXFLAGS = $(memalign_unittest_CXXFLAGS)
memalign_debug_unittest_LDFLAGS = $(memalign_unittest_LDFLAGS)
memalign_debug_unittest_CPPFLAGS = $(gtest_CPPFLAGS)
memalign_debug_unittest_LDADD = libtcmalloc_minimal_debug.la libgtest.la
endif !OSX
endif !MINGW

TESTS += realloc_debug_unittest
realloc_debug_unittest_SOURCES = $(realloc_unittest_SOURCES)
realloc_debug_unittest_CXXFLAGS = $(realloc_unittest_CXXFLAGS)
realloc_debug_unittest_LDFLAGS = $(realloc_unittest_LDFLAGS)
realloc_debug_unittest_CPPFLAGS = $(gtest_CPPFLAGS)
realloc_debug_unittest_LDADD = libtcmalloc_minimal_debug.la libgtest.la

# debugallocation_test checks that we print a proper stacktrace when
# debug-allocs fail, so we can't run it if we don't have stacktrace info.
if WITH_STACK_TRACE

TESTS += debugallocation_test
debugallocation_test_SOURCES = src/tests/debugallocation_test.cc
debugallocation_test_LDFLAGS = $(TCMALLOC_FLAGS) $(AM_LDFLAGS)
debugallocation_test_CPPFLAGS = $(gtest_CPPFLAGS)
debugallocation_test_LDADD = libtcmalloc_debug.la libgtest.la
endif WITH_STACK_TRACE

endif WITH_DEBUGALLOC

noinst_LTLIBRARIES += librun_benchmark.la
librun_benchmark_la_SOURCES = \
	benchmark/run_benchmark.cc

noinst_PROGRAMS += malloc_bench malloc_bench_shared \
	binary_trees binary_trees_shared

malloc_bench_SOURCES = benchmark/malloc_bench.cc
malloc_bench_LDFLAGS = $(TCMALLOC_FLAGS) $(AM_LDFLAGS)
if ENABLE_STATIC
malloc_bench_LDFLAGS += -static
endif ENABLE_STATIC
malloc_bench_LDADD = librun_benchmark.la libtcmalloc_minimal.la

malloc_bench_shared_SOURCES = benchmark/malloc_bench.cc
malloc_bench_shared_LDFLAGS = $(TCMALLOC_FLAGS) $(AM_LDFLAGS)
malloc_bench_shared_LDADD = librun_benchmark.la libtcmalloc_minimal.la

binary_trees_SOURCES = benchmark/binary_trees.cc
binary_trees_LDFLAGS = $(TCMALLOC_FLAGS) $(AM_LDFLAGS)
if ENABLE_STATIC
binary_trees_LDFLAGS += -static
endif ENABLE_STATIC
binary_trees_LDADD = libtcmalloc_minimal.la

binary_trees_shared_SOURCES = benchmark/binary_trees.cc
binary_trees_shared_LDFLAGS = $(TCMALLOC_FLAGS) $(AM_LDFLAGS)
binary_trees_shared_LDADD = libtcmalloc_minimal.la

if !MINGW
if WITH_HEAP_PROFILER_OR_CHECKER

noinst_PROGRAMS += malloc_bench_shared_full
malloc_bench_shared_full_SOURCES = benchmark/malloc_bench.cc
malloc_bench_shared_full_LDFLAGS = $(TCMALLOC_FLAGS) $(AM_LDFLAGS)
malloc_bench_shared_full_LDADD = librun_benchmark.la libtcmalloc.la

noinst_PROGRAMS += unwind_bench
unwind_bench_SOURCES = benchmark/unwind_bench.cc
unwind_bench_LDFLAGS = $(TCMALLOC_FLAGS) $(AM_LDFLAGS)
unwind_bench_LDADD = librun_benchmark.la libtcmalloc.la

endif WITH_HEAP_PROFILER_OR_CHECKER
endif !MINGW

### ------- tcmalloc (thread-caching malloc + heap profiler)

if WITH_HEAP_PROFILER_OR_CHECKER

perftoolsinclude_HEADERS += src/gperftools/heap-profiler.h \
                            src/gperftools/heap-checker.h

if BUILD_EMERGENCY_MALLOC
EMERGENCY_MALLOC_CC = src/emergency_malloc.cc
EMERGENCY_MALLOC_DEFINE = -DENABLE_EMERGENCY_MALLOC
else !BUILD_EMERGENCY_MALLOC
EMERGENCY_MALLOC_CC =
EMERGENCY_MALLOC_DEFINE =
endif !BUILD_EMERGENCY_MALLOC

### Making the library

FULL_MALLOC_SRC = $(MINIMAL_MALLOC_SRC) \
                  src/heap-profile-table.cc \
                  src/heap-profiler.cc \
                  $(EMERGENCY_MALLOC_CC) \
                  src/malloc_backtrace.cc \
                  src/heap-checker-stub.cc

lib_LTLIBRARIES += libtcmalloc.la
libtcmalloc_la_SOURCES = $(TCMALLOC_CC) $(FULL_MALLOC_SRC)
libtcmalloc_la_CXXFLAGS = -DNDEBUG $(AM_CXXFLAGS) \
                          $(EMERGENCY_MALLOC_DEFINE)
libtcmalloc_la_LDFLAGS = -version-info @TCMALLOC_SO_VERSION@ $(AM_LDFLAGS)
libtcmalloc_la_LIBADD = libstacktrace.la liblow_level_alloc.la libcommon.la

### Unittests

TESTS += tcmalloc_unittest
tcmalloc_unittest_SOURCES = src/tests/tcmalloc_unittest.cc \
                            src/tests/testutil.cc
tcmalloc_unittest_LDFLAGS = $(TCMALLOC_FLAGS) $(AM_LDFLAGS)
tcmalloc_unittest_CPPFLAGS = $(gtest_CPPFLAGS)
tcmalloc_unittest_LDADD = libtcmalloc.la libgtest.la

TESTS += tcm_asserts_unittest
tcm_asserts_unittest_SOURCES = src/tests/tcmalloc_unittest.cc \
                               src/tests/testutil.cc \
                               $(libtcmalloc_la_SOURCES)
# same cxxflags as libtcmalloc.la but without NDEBUG
tcm_asserts_unittest_CXXFLAGS = $(AM_CXXFLAGS) \
                                $(EMERGENCY_MALLOC_DEFINE)
tcm_asserts_unittest_CPPFLAGS = $(gtest_CPPFLAGS)
tcm_asserts_unittest_LDADD = libstacktrace.la liblow_level_alloc.la libcommon.la libgtest.la

# This makes sure it's safe to link in both tcmalloc and
# tcmalloc_minimal.  (One would never do this on purpose, but perhaps
# by accident...)  When we can compile libprofiler, we also link it in
# to make sure that works too.  NOTE: On OS X, it's *not* safe to
# link both in (we end up with two copies of every global var, and
# the code tends to pick one arbitrarily), so don't run the test there.
# (We define these outside the 'if' because they're reused below.)
tcmalloc_both_unittest_srcs = src/tests/tcmalloc_unittest.cc \
                              src/tests/testutil.h src/tests/testutil.cc
tcmalloc_both_unittest_lflags = $(TCMALLOC_FLAGS) $(AM_LDFLAGS)
tcmalloc_both_unittest_ladd = libtcmalloc.la libtcmalloc_minimal.la libgtest.la
if WITH_CPU_PROFILER
tcmalloc_both_unittest_ladd += libprofiler.la
endif WITH_CPU_PROFILER

if !OSX
TESTS += tcmalloc_both_unittest
tcmalloc_both_unittest_SOURCES = $(tcmalloc_both_unittest_srcs)
tcmalloc_both_unittest_LDFLAGS = $(tcmalloc_both_unittest_lflags)
tcmalloc_both_unittest_CPPFLAGS = $(gtest_CPPFLAGS)
tcmalloc_both_unittest_LDADD = $(tcmalloc_both_unittest_ladd)
endif !OSX

TESTS += tcmalloc_large_unittest
tcmalloc_large_unittest_SOURCES = src/tests/tcmalloc_large_unittest.cc
tcmalloc_large_unittest_LDFLAGS = $(TCMALLOC_FLAGS) $(AM_LDFLAGS)
tcmalloc_large_unittest_LDADD = libtcmalloc.la $(PTHREAD_LIBS)

TESTS += tcmalloc_large_heap_fragmentation_unittest
tcmalloc_large_heap_fragmentation_unittest_SOURCES = src/tests/large_heap_fragmentation_unittest.cc
tcmalloc_large_heap_fragmentation_unittest_LDFLAGS = $(TCMALLOC_FLAGS) $(AM_LDFLAGS)
tcmalloc_large_heap_fragmentation_unittest_CPPFLAGS = $(gtest_CPPFLAGS)
tcmalloc_large_heap_fragmentation_unittest_LDADD = libtcmalloc.la libgtest.la

# These unittests often need to run binaries.  They're in the current dir
TESTS_ENVIRONMENT += BINDIR=.

TESTS += heap_checker_stub_test
heap_checker_stub_test_SOURCES = src/tests/heap-checker-stub-test.cc
heap_checker_stub_test_LDFLAGS = $(TCMALLOC_FLAGS) $(AM_LDFLAGS)
heap_checker_stub_test_LDADD = libtcmalloc.la

if !SKIP_PPROF_TESTS
TESTS += sampling_test
sampling_test_SOURCES = src/tests/sampling_test.cc
sampling_test_LDFLAGS = $(TCMALLOC_FLAGS) $(AM_LDFLAGS)
sampling_test_CPPFLAGS = $(gtest_CPPFLAGS)
sampling_test_LDADD = libtcmalloc.la $(REGEX_LIBS)
endif !SKIP_PPROF_TESTS

endif WITH_HEAP_PROFILER_OR_CHECKER

if WITH_HEAP_PROFILER

if !SKIP_PPROF_TESTS
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
heap_profiler_unittest_LDFLAGS = $(TCMALLOC_FLAGS) $(AM_LDFLAGS)
heap_profiler_unittest_LDADD = libtcmalloc.la
endif !SKIP_PPROF_TESTS

endif WITH_HEAP_PROFILER

### ------- tcmalloc with debugallocation

if WITH_DEBUGALLOC
if WITH_HEAP_PROFILER_OR_CHECKER

lib_LTLIBRARIES += libtcmalloc_debug.la
libtcmalloc_debug_la_SOURCES = src/debugallocation.cc $(FULL_MALLOC_SRC)
libtcmalloc_debug_la_CXXFLAGS = $(libtcmalloc_la_CXXFLAGS)
libtcmalloc_debug_la_LDFLAGS = $(libtcmalloc_la_LDFLAGS)
libtcmalloc_debug_la_LIBADD = libsymbolize.la $(libtcmalloc_la_LIBADD)

### Unittests

TESTS += tcmalloc_debug_unittest
tcmalloc_debug_unittest_SOURCES = $(tcmalloc_unittest_SOURCES)
tcmalloc_debug_unittest_LDFLAGS = $(tcmalloc_unittest_LDFLAGS)
tcmalloc_debug_unittest_CPPFLAGS = $(gtest_CPPFLAGS)
tcmalloc_debug_unittest_LDADD = libtcmalloc_debug.la libgtest.la

if !SKIP_PPROF_TESTS
TESTS += sampling_debug_test
sampling_debug_test_SOURCES = $(sampling_test_SOURCES)
sampling_debug_test_CXXFLAGS = $(sampling_test_CXXFLAGS)
sampling_debug_test_CPPFLAGS = $(sampling_test_CPPFLAGS)
sampling_debug_test_LDFLAGS = $(sampling_test_LDFLAGS)
sampling_debug_test_LDADD = libtcmalloc_debug.la $(REGEX_LIBS)
endif !SKIP_PPROF_TESTS

endif WITH_HEAP_PROFILER_OR_CHECKER

if WITH_HEAP_PROFILER

if !SKIP_PPROF_TESTS
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
heap_profiler_debug_unittest_LDADD = libtcmalloc_debug.la
endif !SKIP_PPROF_TESTS

endif WITH_HEAP_PROFILER

endif WITH_DEBUGALLOC


### ------- CPU profiler

if WITH_CPU_PROFILER

perftoolsinclude_HEADERS += src/gperftools/profiler.h

### Making the library
lib_LTLIBRARIES += libprofiler.la
libprofiler_la_SOURCES = src/profiler.cc \
                         src/profile-handler.cc \
                         src/profiledata.cc
libprofiler_la_LIBADD = libstacktrace.la libcommon.la
# We have to include ProfileData for profiledata_unittest
CPU_PROFILER_SYMBOLS = '(ProfilerStart|ProfilerStartWithOptions|ProfilerStop|ProfilerFlush|ProfilerEnable|ProfilerDisable|ProfilingIsEnabledForAllThreads|ProfilerRegisterThread|ProfilerGetCurrentState|ProfilerState|ProfileData|ProfileHandler|ProfilerGetStackTrace)'
libprofiler_la_LDFLAGS = -export-symbols-regex $(CPU_PROFILER_SYMBOLS) \
                         -version-info @PROFILER_SO_VERSION@

### Unittests
TESTS += getpc_test
getpc_test_SOURCES = src/tests/getpc_test.cc src/getpc.h

TESTS += profiledata_unittest
profiledata_unittest_SOURCES = src/tests/profiledata_unittest.cc src/profiledata.cc
profiledata_unittest_CPPFLAGS = $(gtest_CPPFLAGS)
profiledata_unittest_LDADD = libstacktrace.la libcommon.la libgtest.la

TESTS += profile_handler_unittest
profile_handler_unittest_SOURCES = src/tests/profile-handler_unittest.cc src/profile-handler.cc
profile_handler_unittest_CPPFLAGS = $(gtest_CPPFLAGS)
profile_handler_unittest_LDADD = libstacktrace.la libcommon.la libgtest.la

if !SKIP_PPROF_TESTS
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
                         src/tests/testutil.cc
profiler1_unittest_SOURCES = $(PROFILER_UNITTEST_SRCS)
profiler1_unittest_CXXFLAGS = -g -DNO_THREADS $(AM_CXXFLAGS)
profiler1_unittest_LDADD = libprofiler.la
profiler2_unittest_SOURCES = $(PROFILER_UNITTEST_SRCS)
profiler2_unittest_CXXFLAGS = -g -DNO_THREADS $(AM_CXXFLAGS)
profiler2_unittest_LDADD = -lstacktrace -lprofiler
# We depend on -lprofiler but haven't yet said how to build it.  Do so now.
profiler2_unittest_DEPENDENCIES = libprofiler.la
profiler3_unittest_SOURCES = $(PROFILER_UNITTEST_SRCS)
profiler3_unittest_CXXFLAGS = -g $(AM_CXXFLAGS)
profiler3_unittest_LDADD = libprofiler.la
profiler4_unittest_SOURCES = $(PROFILER_UNITTEST_SRCS)
profiler4_unittest_CXXFLAGS = -g $(AM_CXXFLAGS)
profiler4_unittest_LDADD = -lstacktrace -lprofiler
# We depend on -lprofiler but haven't yet said how to build it.  Do so now.
profiler4_unittest_DEPENDENCIES = libprofiler.la
endif !SKIP_PPROF_TESTS

endif WITH_CPU_PROFILER


### ------- CPU and heap profiler, in one!

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
libtcmalloc_and_profiler_la_LDFLAGS = -version-info @TCMALLOC_AND_PROFILER_SO_VERSION@ \
                                      $(AM_LDFLAGS)
libtcmalloc_and_profiler_la_LIBADD = $(libtcmalloc_la_LIBADD)

TESTS += tcmalloc_and_profiler_unittest
tcmalloc_and_profiler_unittest_SOURCES = $(tcmalloc_both_unittest_srcs)
tcmalloc_and_profiler_unittest_LDFLAGS = $(tcmalloc_both_unittest_lflags)
tcmalloc_and_profiler_unittest_CPPFLAGS = $(gtest_CPPFLAGS)
tcmalloc_and_profiler_unittest_LDADD = libtcmalloc_and_profiler.la libgtest.la

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
	echo 'Requires: $(UNWIND_PC_DEP)' >> "$@".tmp
	echo 'Libs: -L$${libdir} -ltcmalloc' >> "$@".tmp
	echo 'Libs.private: $(PTHREAD_CFLAGS) $(PTHREAD_LIBS)' >> "$@".tmp
	echo 'Cflags: -I$${includedir}' >> "$@".tmp
	mv -f "$@".tmp "$@"

libtcmalloc_minimal.pc: libtcmalloc.pc
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
	echo 'Libs: -L$${libdir} -ltcmalloc_minimal' >> "$@".tmp
	echo 'Libs.private: $(PTHREAD_CFLAGS) $(PTHREAD_LIBS)' >> "$@".tmp
	echo 'Cflags: -I$${includedir}' >> "$@".tmp
	mv -f "$@".tmp "$@"

libtcmalloc_debug.pc: libtcmalloc.pc
	cat libtcmalloc.pc | sed s/-ltcmalloc/-ltcmalloc_debug/ > "$@"

libtcmalloc_minimal_debug.pc: libtcmalloc_minimal.pc
	cat libtcmalloc_minimal.pc | sed s/-ltcmalloc_minimal/-ltcmalloc_minimal_debug/ > "$@"

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
             src/windows/CMakeLists.txt \
             $(WINDOWS_EXTRA) \
             gperftools.sln vsprojects vendor \
             $(top_srcdir)/src/*h $(top_srcdir)/src/base/*h \
             $(top_srcdir)/benchmark/*h \
             $(top_srcdir)/src/tests/*h \
             $(top_srcdir)/src/windows/*h $(top_srcdir)/src/gperftools/*h \
             CMakeLists.txt cmake \
             generic-config/config.h .bazelrc BUILD MODULE.bazel

# Windows wants write permission to .vcxproj files and maybe even sln files.
dist-hook: $(top_distdir)/ChangeLog
	test -e "$(distdir)/vsprojects" \
	   && chmod -R u+w $(distdir)/*.sln $(distdir)/vsprojects/
	rm -rf $(distdir)/vendor/googletest/googletest/src/.deps
	rm -rf $(distdir)/vendor/libbacktrace/.deps
	rm -rf $(distdir)/vendor/libbacktrace-integration/.deps
	rm -f $(distdir)/src/config.h
