cmake_minimum_required(VERSION 3.12)

# Please note that cmake support is very preliminary. Autotools-based
# build is the only fully supported build for now.

set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Choose the build type")

project(gperftools VERSION 2.16 LANGUAGES C CXX
        DESCRIPTION "Performance tools for C++"
        HOMEPAGE_URL https://github.com/gperftools/gperftools)

# Update this value for every release!
set(TCMALLOC_SO_VERSION 9.18.5)
set(PROFILER_SO_VERSION 5.13.5)
set(TCMALLOC_AND_PROFILER_SO_VERSION 10.13.6)

# The user can choose not to compile in the heap-profiler, or the
# cpu-profiler.  There's also the possibility for a 'fully minimal'
# compile, which leaves out the stacktrace code as well.  By default,
# we include all of these that the target system supports.
set(DEFAULT_BUILD_CPU_PROFILER ON)
set(DEFAULT_BUILD_HEAP_PROFILER ON)
set(DEFAULT_BUILD_DEBUGALLOC ON)
set(DEFAULT_BUILD_MINIMAL OFF)

set(DEFAULT_TCMALLOC_ALIGNMENT 16)

set(HOST string(TOLOWER "${CMAKE_SYSTEM_NAME}"))

if(MINGW OR MSVC)
  set(DEFAULT_BUILD_MINIMAL ON)
  set(DEFAULT_BUILD_DEBUGALLOC OFF)
elseif(CYGWIN)
  set(DEFAULT_BUILD_CPU_PROFILER OFF)
endif()

include(CheckCCompilerFlag)
include(CheckCXXCompilerFlag)
include(CheckCSourceCompiles)
include(CheckCXXSourceCompiles)
include(CheckFunctionExists)
include(CheckIncludeFile)
include(CheckLibraryExists)
include(CheckSymbolExists)
include(CheckTypeSize)
include(CheckVariableExists)
include(CMakeDependentOption)
include(CTest)

option(BUILD_SHARED_LIBS "Build using shared libraries" ON)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(DefineTargetVariables)

define_target_variables()

# Currently only backtrace works on s390.
if(s390 OR OSX)
  set(default_enable_libunwind OFF)
  set(default_enable_backtrace ON)
else()
  set(default_enable_libunwind ON)
  set(default_enable_backtrace OFF)
endif()

# Disable libunwind linking on ppc64 by default.
if(PPC64)
  set(default_enable_libunwind OFF)
  set(default_tcmalloc_pagesize 64)
else()
  if(PPC)
    set(default_enable_libunwind OFF)
  else()
    set(default_enable_libunwind ON)
  endif()
  set(default_tcmalloc_pagesize 8)
endif()

cmake_dependent_option(
  GPERFTOOLS_BUILD_CPU_PROFILER "Build cpu-profiler" ${DEFAULT_BUILD_CPU_PROFILER}
  "NOT gperftools_build_minimal" OFF)
cmake_dependent_option(
  GPERFTOOLS_BUILD_HEAP_PROFILER "Build heap-profiler" ${DEFAULT_BUILD_HEAP_PROFILER}
  "NOT gperftools_build_minimal" OFF)
cmake_dependent_option(
  GPERFTOOLS_BUILD_DEBUGALLOC "Build debugalloc" ${DEFAULT_BUILD_DEBUGALLOC}
  "NOT gperftools_build_minimal" OFF)
option(
        gperftools_build_minimal
        "Build only tcmalloc-minimal (and maybe tcmalloc-minimal-debug)"
        ${DEFAULT_BUILD_MINIMAL})
if(gperftools_build_minimal)
  set(GPERFTOOLS_BUILD_CPU_PROFILER OFF)
  set(GPERFTOOLS_BUILD_HEAP_PROFILER OFF)
endif()

cmake_dependent_option(
  gperftools_build_benchmark "Build benchmark" ON "NOT MINGW AND NOT MSVC" OFF)

option(gperftools_enable_stacktrace_via_backtrace
       "Enable use of backtrace() for stacktrace capturing (may deadlock)"
       ${default_enable_backtrace})
option(gperftools_enable_libunwind
       "Enable libunwind linking"
       ${default_enable_libunwind})

set(enable_backtrace ${gperftools_enable_stacktrace_via_backtrace})
set(enable_libunwind ${gperftools_enable_libunwind})

option(gperftools_enable_libgcc_unwinder_by_default
       "Prefer libgcc's _Unwind_Backtrace as default stacktrace capturing method"
       OFF)
set(PREFER_LIBGCC_UNWINDER ${gperftools_enable_libgcc_unwinder_by_default})

set(gperftools_tcmalloc_pagesize ${default_tcmalloc_pagesize}
  CACHE STRING "Set the tcmalloc internal page size")
set(allowed_page_sizes LIST "4;8;16;32;64;128;256")
set_property(CACHE gperftools_tcmalloc_pagesize PROPERTY STRINGS ${allowed_page_sizes})
if(NOT gperftools_tcmalloc_pagesize IN_LIST allowed_page_sizes)
  message(WARNING
    "Invalid gperftools_tcmalloc_pagesize (${gperftools_tcmalloc_pagesize}), "
    "setting to default value (${default_tcmalloc_pagesize})")
  set(gperftools_tcmalloc_pagesize ${default_tcmalloc_pagesize})
endif()
if (gperftools_tcmalloc_pagesize EQUAL 4)
  set(TCMALLOC_PAGE_SIZE_SHIFT 12)
elseif(gperftools_tcmalloc_pagesize EQUAL 8)
  # default page size
elseif(gperftools_tcmalloc_pagesize EQUAL 16)
  set(TCMALLOC_PAGE_SIZE_SHIFT 14)
elseif(gperftools_tcmalloc_pagesize EQUAL 32)
  set(TCMALLOC_PAGE_SIZE_SHIFT 15)
elseif(gperftools_tcmalloc_pagesize EQUAL 64)
  set(TCMALLOC_PAGE_SIZE_SHIFT 16)
elseif(gperftools_tcmalloc_pagesize EQUAL 128)
  set(TCMALLOC_PAGE_SIZE_SHIFT 17)
elseif(gperftools_tcmalloc_pagesize EQUAL 256)
  set(TCMALLOC_PAGE_SIZE_SHIFT 18)
else()
  message(WARNING
  "${gperftools_tcmalloc_pagesize}K size not supported, using default tcmalloc page size.")
endif()

set(gperftools_tcmalloc_alignment ${DEFAULT_TCMALLOC_ALIGNMENT}
  CACHE STRING "Set the tcmalloc allocation alignment")
set_property(CACHE gperftools_tcmalloc_alignment PROPERTY STRINGS "8" "16")
if(NOT gperftools_tcmalloc_alignment STREQUAL "8" AND
   NOT gperftools_tcmalloc_alignment STREQUAL "16")
  message(WARNING
      "Invalid gperftools_tcmalloc_alignment (${gperftools_tcmalloc_alignment}), "
      "setting to default value (${DEFAULT_TCMALLOC_ALIGNMENT})")
  set(gperftools_tcmalloc_alignment ${DEFAULT_TCMALLOC_ALIGNMENT})
endif()
if(gperftools_tcmalloc_alignment STREQUAL "8")
  set(TCMALLOC_ALIGN_8BYTES ON)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

# AX_C___ATTRIBUTE__
check_c_source_compiles("#include <stdlib.h>
                         static void foo(void) __attribute__ ((unused));
                         void foo(void) { exit(1); }
                         int main() { return 0; }"
        HAVE___ATTRIBUTE__)

check_c_source_compiles("#include <stdlib.h>
                        void foo(void) __attribute__((aligned(128)));
                        void foo(void) { exit(1); }
                        int main() { return 0; }"
        HAVE___ATTRIBUTE__ALIGNED_FN)

set(CMAKE_EXTRA_INCLUDE_FILES "malloc.h")
check_type_size("struct mallinfo" STRUCT_MALLINFO LANGUAGE CXX)
check_type_size("struct mallinfo2" STRUCT_MALLINFO2 LANGUAGE CXX)
check_function_exists("sbrk" HAVE_SBRK) # for tcmalloc to get memory
check_function_exists("geteuid" HAVE_GETEUID) # for turning off services when run as root
check_include_file("features.h" HAVE_FEATURES_H) # for vdso_support.h, Where __GLIBC__ is defined
check_include_file("malloc.h" HAVE_MALLOC_H) # some systems define stuff there, others not
check_include_file("glob.h" HAVE_GLOB_H) # for heap-profile-table (cleaning up profiles)
check_include_file("execinfo.h" HAVE_EXECINFO_H) # for stacktrace
check_include_file("sched.h" HAVE_SCHED_H) # for being nice in our spinlock code
check_include_file("sys/syscall.h" HAVE_SYS_SYSCALL_H)
check_include_file("fcntl.h" HAVE_FCNTL_H) # for tcmalloc_unittest
check_include_file("sys/cdefs.h" HAVE_SYS_CDEFS_H) # Where glibc defines __THROW

check_include_file("sys/ucontext.h" HAVE_SYS_UCONTEXT_H)
check_include_file("ucontext.h" HAVE_UCONTEXT_H)
check_include_file("cygwin/signal.h" HAVE_CYGWIN_SIGNAL_H) # ucontext on cywgin
check_include_file("asm/ptrace.h" HAVE_ASM_PTRACE_H) # get ptrace macros, e.g. PT_NIP

check_include_file("unistd.h" HAVE_UNISTD_H)
# We also need <ucontext.h>/<sys/ucontext.h>, but we get those from
# AC_PC_FROM_UCONTEXT, below.

# We override a lot of memory allocation routines, not all of which are
# standard.  For those the system doesn't declare, we'll declare ourselves.
set(CMAKE_REQUIRED_DEFINITIONS -D_XOPEN_SOURCE=600)
check_symbol_exists("cfree" "stdlib.h;malloc.h" HAVE_DECL_CFREE)
check_symbol_exists("posix_memalign" "stdlib.h;malloc.h" HAVE_DECL_POSIX_MEMALIGN)
check_symbol_exists("memalign" "stdlib.h;malloc.h" HAVE_DECL_MEMALIGN)
check_symbol_exists("valloc" "stdlib.h;malloc.h" HAVE_DECL_VALLOC)
check_symbol_exists("pvalloc" "stdlib.h;malloc.h" HAVE_DECL_PVALLOC)
set(CMAKE_REQUIRED_DEFINITIONS)

if(HAVE_STRUCT_MALLINFO)
    set(HAVE_STRUCT_MALLINFO 1)
else()
    set(HAVE_STRUCT_MALLINFO 0)
endif()

if(HAVE_STRUCT_MALLINFO2)
    set(HAVE_STRUCT_MALLINFO2 1)
else()
    set(HAVE_STRUCT_MALLINFO2 0)
endif()

# We hardcode HAVE_MMAP to 1. There are no interesting systems anymore
# without functional mmap. And our windows (except mingw) builds
# aren't using autoconf. So we keep HAVE_MMAP define, but only to
# distingush windows and rest.
if(NOT WIN32)
  set(HAVE_MMAP 1)
endif()

if(gperftools_enable_libunwind)
  check_include_file("libunwind.h" HAVE_LIBUNWIND_H)
  if(HAVE_LIBUNWIND_H)
    find_library(libunwind_location NAMES unwind)
    if(libunwind_location)
      check_library_exists(
        unwind backtrace ${libunwind_location} have_libunwind)
    endif()
    if(have_libunwind)
      set(unwind_libs ${libunwind_location})
      set(will_use_libunwind ON)
      set(USE_LIBUNWIND 1)
      set(USE_LIBUNWIND_FAST 1)
    endif()
  endif()
endif()

check_c_compiler_flag("-fno-omit-frame-pointer -momit-leaf-frame-pointer" have_omit_leaf_fp)
check_c_source_compiles("
  #if !(__i386__ || __x86_64__ || __riscv || __aarch64__)
  #error unsupported arch
  #endif
  int main() { return 0; }
  "
  use_omit_leaf_fp)

if (use_omit_leaf_fp)
  add_compile_options(-fno-omit-frame-pointer -momit-leaf-frame-pointer)
endif()

option(gperftools_dynamic_sized_delete_support
       "Try to build run-time switch for sized delete operator"
       OFF)
if(gperftools_dynamic_sized_delete_support)
  set(ENABLE_DYNAMIC_SIZED_DELETE 1)
endif()

option(gperftools_sized_delete "Build sized delete operator" OFF)
if(gperftools_sized_delete)
  set(ENABLE_SIZED_DELETE 1)
endif()

if(NOT MSVC)
  set(CMAKE_REQUIRED_FLAGS -fsized-deallocation)
  check_cxx_source_compiles("
  #include <new>
  int main() { (::operator delete)(0, 256); return 0; }"
          have_sized_deallocation)
  set(CMAKE_REQUIRED_FLAGS)
endif()

check_cxx_source_compiles("
  #include <unwind.h>
  int main()
  {
#if __APPLE__ || __FreeBSD__
#error OSX _Unwind_Backtrace recurses back to malloc
#endif
    &_Unwind_Backtrace;
    return 0;
  }"
  HAVE_UNWIND_BACKTRACE)

check_cxx_source_compiles("
#include <cxxabi.h>
int main() {
  &__cxxabiv1::__cxa_demangle;
}"
  HAVE_CXA_DEMANGLE)

if(enable_backtrace)
  set(default_emergency_malloc ON)
else()
  set(default_emergency_malloc OFF)
endif()

if(will_use_libunwind AND ARM)
  set(default_emergency_malloc ON)
endif()

option(gperftools_emergency_malloc
       "Build emergency malloc"
       ${default_emergency_malloc})

check_c_source_compiles(
  "int main() { return __builtin_expect(main != 0, 1); }"
  HAVE_BUILTIN_EXPECT)

check_c_source_compiles("
  #include <unistd.h>
  int main()
  {
    char** env = __environ;
    return 0;
  }"
  HAVE___ENVIRON)

if(enable_backtrace)
  check_symbol_exists("backtrace" "execinfo.h" HAVE_DECL_BACKTRACE)
  check_function_exists("backtrace" backtrace_exists)
  if(NOT backtrace_exists)
    set(CMAKE_REQUIRED_LIBRARIES execinfo)
    check_function_exists("backtrace" backtrace_exists)
    set(CMAKE_REQUIRED_LIBRARIES)
    if(backtrace_exists)
      list(INSERT unwind_libs 0 execinfo)
    endif()
  endif()
endif()

find_package(Threads REQUIRED)
link_libraries(Threads::Threads)

check_variable_exists("program_invocation_name" HAVE_PROGRAM_INVOCATION_NAME)

if(MINGW)
  check_symbol_exists("sleep" "unistd.h" HAVE_DECL_SLEEP)
  check_symbol_exists("nanosleep" "time.h" HAVE_DECL_NANOSLEEP)
endif()

if(LINUX)
  check_c_source_compiles("
    #include <signal.h>
    #include <time.h>
    int main() { return SIGEV_THREAD_ID || CLOCK_THREAD_CPUTIME_ID; }"
    HAVE_LINUX_SIGEV_THREAD_ID)
endif()

# Disable large allocation report by default.
option(gperftools_enable_large_alloc_report
      "Report very large allocations to stderr"
      OFF)
set(ENABLE_LARGE_ALLOC_REPORT ${gperftools_enable_large_alloc_report})

# Enable aggressive decommit by default
option(gperftools_enable_aggressive_decommit_by_default
      "Enable aggressive decommit by default"
      OFF)
set(ENABLE_AGGRESSIVE_DECOMMIT_BY_DEFAULT ${gperftools_enable_aggressive_decommit_by_default})


configure_file(cmake/config.h.in ${CMAKE_CURRENT_BINARY_DIR}/config.h @ONLY)
configure_file(cmake/tcmalloc.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/gperftools/tcmalloc.h
               @ONLY)

if(GPERFTOOLS_BUILD_CPU_PROFILER OR
   GPERFTOOLS_BUILD_HEAP_PROFILER)
  set(WITH_STACK_TRACE ON)
endif()

include_directories($<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/>)
# This is so we can #include <gperftools/foo>
include_directories($<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)

if(NOT WITH_STACK_TRACE)
  add_compile_definitions(NO_TCMALLOC_SAMPLES)
endif()

# These are good warnings to turn on by default.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wwrite-strings -Woverloaded-virtual -Wno-sign-compare")
endif()

if(have_sized_deallocation)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsized-deallocation")
endif()

option(
  gperftools_enable_frame_pointers
  "Compile with -fno-omit-frame-pointer (see INSTALL)"
  OFF)

if(gperftools_enable_frame_pointers)
  add_compile_options(-fno-omit-frame-pointer -DFORCED_FRAME_POINTERS)
endif()

# On windows when building static library, due to patching, we need to
#  force "core" of tcmalloc to be linked in to activate patching etc. We
#  achieve that by telling linker to assume __tcmalloc is not defined.
set(TCMALLOC_FLAGS)
if(MINGW)
  list(APPEND TCMALLOC_FLAGS "-Wl,-u__tcmalloc")
endif()
if(MSVC)
  list(APPEND TCMALLOC_FLAGS "/INCLUDE:__tcmalloc")
endif()

add_library(common
  STATIC
  src/base/logging.cc
  src/base/generic_writer.cc
  src/base/sysinfo.cc
  src/base/proc_maps_iterator.cc
  src/base/dynamic_annotations.cc
  src/base/spinlock.cc
  src/base/spinlock_internal.cc)

add_library(low_level_alloc
  STATIC
  src/base/low_level_alloc.cc)

if(BUILD_SHARED_LIBS)
  set_property(TARGET common
               PROPERTY POSITION_INDEPENDENT_CODE ON)
  set_property(TARGET low_level_alloc
               PROPERTY POSITION_INDEPENDENT_CODE ON)
else()
  # windows needs this. Doesn't hurt others.
  #
  # TODO: make sure we do something with headers. If/when someone uses
  #  statically built tcmalloc_minimal.lib downstream, they need to see
  #  PERFTOOLS_DLL_DECL set to nothing as well.
  add_compile_definitions(PERFTOOLS_DLL_DECL= )
endif()

set(SYSTEM_ALLOC_CC src/system-alloc.cc)
set(TCMALLOC_CC src/tcmalloc.cc)

if(MINGW OR MSVC)
target_sources(common PRIVATE
  src/windows/port.cc
  src/windows/ia32_modrm_map.cc
  src/windows/ia32_opcode_map.cc
  src/windows/mini_disassembler.cc
  src/windows/preamble_patcher.cc
  src/windows/preamble_patcher_with_stub.cc)

set(SYSTEM_ALLOC_CC src/windows/system-alloc.cc)
set(TCMALLOC_CC src/windows/patch_functions.cc)

# patch_function uses -lpsapi and spinlock bits use -synchronization
# and -lshlwapi
link_libraries(psapi synchronization shlwapi)

endif()

if(BUILD_TESTING)
  add_library(gtest STATIC vendor/googletest/googletest/src/gtest_main.cc
    vendor/googletest/googletest/src/gtest-assertion-result.cc
    vendor/googletest/googletest/src/gtest-death-test.cc
    vendor/googletest/googletest/src/gtest-filepath.cc
    vendor/googletest/googletest/src/gtest-matchers.cc
    vendor/googletest/googletest/src/gtest-port.cc
    vendor/googletest/googletest/src/gtest-printers.cc
    vendor/googletest/googletest/src/gtest-test-part.cc
    vendor/googletest/googletest/src/gtest-typed-test.cc
    vendor/googletest/googletest/src/gtest.cc)
  target_include_directories(gtest PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor/googletest/googletest>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor/googletest/googletest/include>)

  target_include_directories(gtest INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor/googletest/googletest/include>)

  add_executable(low_level_alloc_unittest
          src/tests/low_level_alloc_unittest.cc)
  target_compile_definitions(low_level_alloc_unittest PRIVATE NO_TCMALLOC_SAMPLES PERFTOOLS_DLL_DECL= )
  target_link_libraries(low_level_alloc_unittest low_level_alloc common gtest)
  add_test(low_level_alloc_unittest low_level_alloc_unittest)
endif()

### ------- stack trace

if(WITH_STACK_TRACE)
  ### Making the library
  add_library(stacktrace STATIC
    src/stacktrace.cc
    src/base/elf_mem_image.cc
    src/base/vdso_support.cc)
  target_link_libraries(stacktrace PRIVATE ${unwind_libs})
  if(BUILD_SHARED_LIBS)
    set_property(TARGET stacktrace
                 PROPERTY POSITION_INDEPENDENT_CODE ON)
  endif()

  if(BUILD_TESTING)
    add_executable(stacktrace_unittest
      src/tests/stacktrace_unittest.cc
      src/stacktrace.cc
      src/base/elf_mem_image.cc
      src/base/vdso_support.cc)
    target_link_libraries(stacktrace_unittest ${unwind_libs} common gtest)
    target_compile_definitions(stacktrace_unittest PRIVATE STACKTRACE_IS_TESTED PERFTOOLS_DLL_DECL= )
    add_test(stacktrace_unittest stacktrace_unittest)

    add_executable(check_address_test src/tests/check_address_test.cc)
    target_link_libraries(check_address_test common gtest)
    add_test(check_address_test check_address_test)
  endif()

endif()

### ------- tcmalloc_minimal (thread-caching malloc)

### Making the library

set(MINIMAL_MALLOC_SRC
  src/common.cc
  src/internal_logging.cc
  ${SYSTEM_ALLOC_CC}
  src/memfs_malloc.cc
  src/safe_strerror.cc
  src/central_freelist.cc
  src/page_heap.cc
  src/sampler.cc
  src/span.cc
  src/stack_trace_table.cc
  src/static_vars.cc
  src/thread_cache.cc
  src/thread_cache_ptr.cc
  src/malloc_hook.cc
  src/malloc_extension.cc)

add_library(tcmalloc_minimal ${TCMALLOC_CC} ${MINIMAL_MALLOC_SRC})
target_compile_definitions(tcmalloc_minimal PRIVATE NO_TCMALLOC_SAMPLES)
target_link_options(tcmalloc_minimal INTERFACE ${TCMALLOC_FLAGS})
target_link_libraries(tcmalloc_minimal PRIVATE common)

if(BUILD_TESTING)
  add_executable(tcmalloc_minimal_unittest
    src/tests/tcmalloc_unittest.cc
    src/tests/testutil.cc)
  target_link_libraries(tcmalloc_minimal_unittest tcmalloc_minimal gtest)
  add_test(tcmalloc_minimal_unittest tcmalloc_minimal_unittest)

  add_executable(tcmalloc_minimal_large_unittest
          src/tests/tcmalloc_large_unittest.cc
          src/tests/testutil.cc)
  target_link_libraries(tcmalloc_minimal_large_unittest tcmalloc_minimal)
  add_test(tcmalloc_minimal_large_unittest tcmalloc_minimal_large_unittest)

  add_executable(tcmalloc_minimal_large_heap_fragmentation_unittest
          src/tests/large_heap_fragmentation_unittest.cc)
  target_link_libraries(
          tcmalloc_minimal_large_heap_fragmentation_unittest tcmalloc_minimal gtest)
  add_test(tcmalloc_minimal_large_heap_fragmentation_unittest tcmalloc_minimal_large_heap_fragmentation_unittest)

  add_executable(addressmap_unittest
          src/tests/addressmap_unittest.cc)
  target_link_libraries(addressmap_unittest common gtest)
  add_test(addressmap_unittest addressmap_unittest)

  add_executable(system_alloc_unittest src/tests/system-alloc_unittest.cc)
  target_link_libraries(system_alloc_unittest tcmalloc_minimal gtest)
  add_test(system_alloc_unittest system_alloc_unittest)

  if(NOT MINGW AND NOT MSVC)
    add_executable(unique_path_unittest src/tests/unique_path_unittest.cc)
    target_link_libraries(unique_path_unittest common gtest)
    add_test(unique_path_unittest unique_path_unittest)
  endif()

  add_executable(packed_cache_test src/tests/packed-cache_test.cc src/internal_logging.cc)
  target_compile_definitions(packed_cache_test PRIVATE PERFTOOLS_DLL_DECL= )
  target_link_libraries(packed_cache_test common gtest)
  add_test(packed_cache_test packed_cache_test)

  add_executable(frag_unittest src/tests/frag_unittest.cc)
  target_link_libraries(frag_unittest tcmalloc_minimal gtest)
  add_test(frag_unittest frag_unittest)

  add_executable(markidle_unittest
          src/tests/markidle_unittest.cc
          src/tests/testutil.cc)
  target_link_libraries(markidle_unittest tcmalloc_minimal gtest)
  add_test(markidle_unittest markidle_unittest)

  add_executable(current_allocated_bytes_test
          src/tests/current_allocated_bytes_test.cc)
  target_link_libraries(current_allocated_bytes_test tcmalloc_minimal gtest)
  add_test(current_allocated_bytes_test current_allocated_bytes_test)

  add_executable(malloc_hook_test
          src/tests/malloc_hook_test.cc
          src/malloc_hook.cc
          src/tests/testutil.cc)
  target_compile_definitions(malloc_hook_test PRIVATE NO_TCMALLOC_SAMPLES PERFTOOLS_DLL_DECL= )
  target_link_libraries(malloc_hook_test common gtest)
  add_test(malloc_hook_test malloc_hook_test)

  add_executable(malloc_extension_test src/tests/malloc_extension_test.cc)
  target_link_libraries(malloc_extension_test tcmalloc_minimal gtest)
  add_test(malloc_extension_test malloc_extension_test)

  add_executable(malloc_extension_c_test src/tests/malloc_extension_c_test.cc)
  target_link_libraries(malloc_extension_c_test tcmalloc_minimal gtest)
  add_test(malloc_extension_c_test malloc_extension_c_test)

  if(NOT MINGW AND NOT MSVC AND NOT APPLE)
    add_executable(memalign_unittest src/tests/memalign_unittest.cc src/tests/testutil.cc)
    target_link_libraries(memalign_unittest tcmalloc_minimal gtest)
    add_test(memalign_unittest memalign_unittest)
  endif()

  # page heap test is somewhat special it compiles and links entirety
  # of tcmalloc statically. We'll eventually make it right.
  add_executable(page_heap_test src/tests/page_heap_test.cc ${TCMALLOC_CC} ${MINIMAL_MALLOC_SRC})
  target_compile_definitions(page_heap_test PRIVATE NO_TCMALLOC_SAMPLES PERFTOOLS_DLL_DECL= )
  target_link_libraries(page_heap_test common gtest)
  add_test(page_heap_test page_heap_test)

  add_executable(pagemap_unittest src/tests/pagemap_unittest.cc src/internal_logging.cc)
  target_compile_definitions(pagemap_unittest PRIVATE PERFTOOLS_DLL_DECL= )
  target_link_libraries(pagemap_unittest common gtest)
  add_test(pagemap_unittest pagemap_unittest)

  add_executable(safe_strerror_test src/tests/safe_strerror_test.cc src/safe_strerror.cc)
  target_link_libraries(safe_strerror_test common gtest)
  add_test(safe_strerror_test safe_strerror_test)

  add_executable(cleanup_test src/tests/cleanup_test.cc)
  target_link_libraries(cleanup_test gtest)
  add_test(cleanup_test cleanup_test)

  add_executable(function_ref_test src/tests/function_ref_test.cc)
  target_link_libraries(function_ref_test gtest)
  add_test(function_ref_test function_ref_test)

  add_executable(trivialre_test benchmark/trivialre_test.cc)
  target_link_libraries(trivialre_test gtest)
  add_test(trivialre_test trivialre_test)

  add_executable(generic_writer_test src/tests/generic_writer_test.cc)
  target_link_libraries(generic_writer_test common gtest)
  add_test(generic_writer_test generic_writer_test)

  add_executable(proc_maps_iterator_test src/tests/proc_maps_iterator_test.cc)
  target_link_libraries(proc_maps_iterator_test common gtest)
  add_test(proc_maps_iterator_test proc_maps_iterator_test)

  add_executable(realloc_unittest src/tests/realloc_unittest.cc)
  target_link_libraries(realloc_unittest tcmalloc_minimal gtest)
  add_test(realloc_unittest realloc_unittest)

  add_executable(stack_trace_table_test
    src/tests/stack_trace_table_test.cc
    src/stack_trace_table.cc
    src/internal_logging.cc)
  target_compile_definitions(stack_trace_table_test PRIVATE STACK_TRACE_TABLE_IS_TESTED PERFTOOLS_DLL_DECL= )
  target_link_libraries(stack_trace_table_test common gtest)
  add_test(stack_trace_table_test stack_trace_table_test)

  add_executable(thread_dealloc_unittest
          src/tests/thread_dealloc_unittest.cc
          src/tests/testutil.cc)
  target_link_libraries(thread_dealloc_unittest tcmalloc_minimal)
  add_test(thread_dealloc_unittest thread_dealloc_unittest)

  add_executable(min_per_thread_cache_size_test src/tests/min_per_thread_cache_size_test.cc)
  target_link_libraries(min_per_thread_cache_size_test tcmalloc_minimal gtest)
  add_test(min_per_thread_cache_size_test min_per_thread_cache_size_test)
endif()

### ------- tcmalloc_minimal_debug (thread-caching malloc with debugallocation)

if(GPERFTOOLS_BUILD_DEBUGALLOC)
  add_library(libbacktrace
    STATIC
    vendor/libbacktrace-integration/file-format.c
    vendor/libbacktrace/dwarf.c
    vendor/libbacktrace/fileline.c
    vendor/libbacktrace/posix.c
    vendor/libbacktrace/sort.c
    vendor/libbacktrace/state.c
    vendor/libbacktrace/read.c)
  target_include_directories(libbacktrace BEFORE PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor/libbacktrace-integration>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor/libbacktrace>)
  add_library(symbolize
    STATIC
    src/symbolize.cc
    vendor/libbacktrace-integration/backtrace-alloc.cc)
  target_link_libraries(symbolize PRIVATE libbacktrace)
  if(BUILD_SHARED_LIBS)
    set_property(TARGET libbacktrace PROPERTY POSITION_INDEPENDENT_CODE ON)
    set_property(TARGET symbolize PROPERTY POSITION_INDEPENDENT_CODE ON)
  endif()

  add_library(tcmalloc_minimal_debug src/debugallocation.cc ${MINIMAL_MALLOC_SRC})
  target_compile_definitions(tcmalloc_minimal_debug PRIVATE NO_TCMALLOC_SAMPLES)
  target_link_libraries(tcmalloc_minimal_debug PRIVATE symbolize low_level_alloc common)

  ### Unittests

  if(BUILD_TESTING)
    add_executable(tcmalloc_minimal_debug_unittest src/tests/tcmalloc_unittest.cc src/tests/testutil.cc)
    target_link_libraries(tcmalloc_minimal_debug_unittest tcmalloc_minimal_debug gtest)
    add_test(tcmalloc_minimal_debug_unittest tcmalloc_minimal_debug_unittest)

    add_executable(malloc_extension_debug_test src/tests/malloc_extension_test.cc)
    target_link_libraries(malloc_extension_debug_test tcmalloc_minimal_debug gtest)
    add_test(malloc_extension_debug_test malloc_extension_debug_test)

    if(NOT MINGW AND NOT APPLE)
      add_executable(memalign_debug_unittest src/tests/memalign_unittest.cc src/tests/testutil.cc)
      target_link_libraries(memalign_debug_unittest
              tcmalloc_minimal_debug gtest)
      add_test(memalign_debug_unittest memalign_debug_unittest)
    endif()

    add_executable(realloc_debug_unittest src/tests/realloc_unittest.cc)
    target_link_libraries(realloc_debug_unittest PUBLIC tcmalloc_minimal_debug gtest)
    add_test(realloc_debug_unittest realloc_debug_unittest)

    if(WITH_STACK_TRACE)
      add_executable(debugallocation_test src/tests/debugallocation_test.cc)
      target_link_libraries(debugallocation_test tcmalloc_debug gtest)
      add_test(debugallocation_test debugallocation_test)
    endif()
  endif()
endif()

if(NOT MINGW AND NOT MSVC)
  if(gperftools_build_benchmark)
    add_library(run_benchmark benchmark/run_benchmark.cc)

    add_executable(malloc_bench benchmark/malloc_bench.cc)
    target_link_libraries(malloc_bench tcmalloc_minimal run_benchmark)

    if(GPERFTOOLS_BUILD_HEAP_PROFILER)
      add_executable(malloc_bench_shared_full benchmark/malloc_bench.cc)
      target_link_libraries(malloc_bench_shared_full run_benchmark tcmalloc)
    endif()

    add_executable(binary_trees benchmark/binary_trees.cc)
    target_link_libraries(binary_trees tcmalloc_minimal)
    add_executable(binary_trees_shared benchmark/binary_trees.cc)
    target_link_libraries(binary_trees_shared tcmalloc_minimal)
  endif()
endif()

### ------- tcmalloc (thread-caching malloc + heap profiler)

if(GPERFTOOLS_BUILD_HEAP_PROFILER)
  if(gperftools_emergency_malloc)
    set(EMERGENCY_MALLOC_CC src/emergency_malloc.cc)
    set(EMERGENCY_MALLOC_DEFINE ENABLE_EMERGENCY_MALLOC)
  else()
    set(EMERGENCY_MALLOC_CC )
  endif()

  ### Making the library

  set(FULL_MALLOC_SRC
    ${MINIMAL_MALLOC_SRC}
    src/heap-profile-table.cc
    src/heap-profiler.cc
    ${EMERGENCY_MALLOC_CC}
    src/malloc_backtrace.cc
    src/heap-checker-stub.cc)
  add_library(tcmalloc ${TCMALLOC_CC} ${FULL_MALLOC_SRC})
  target_compile_definitions(tcmalloc PRIVATE ${EMERGENCY_MALLOC_DEFINE})
  target_link_libraries(tcmalloc PRIVATE stacktrace low_level_alloc common)
  target_link_options(tcmalloc INTERFACE ${TCMALLOC_FLAGS})

  ### Unittests
  if(BUILD_TESTING)
    add_executable(tcmalloc_unittest src/tests/tcmalloc_unittest.cc src/tests/testutil.cc)
    target_link_libraries(tcmalloc_unittest tcmalloc gtest)
    add_test(tcmalloc_unittest tcmalloc_unittest)

    add_executable(tcmalloc_large_unittest src/tests/tcmalloc_large_unittest.cc)
    target_link_libraries(tcmalloc_large_unittest tcmalloc)
    add_test(tcmalloc_large_unittest tcmalloc_large_unittest)

    add_executable(tcmalloc_large_heap_fragmentation_unittest src/tests/large_heap_fragmentation_unittest.cc)
    target_link_libraries(tcmalloc_large_heap_fragmentation_unittest tcmalloc gtest)
    add_test(tcmalloc_large_heap_fragmentation_unittest tcmalloc_large_heap_fragmentation_unittest)

    list(APPEND TESTS_ENVIRONMENT PPROF_PATH=${CMAKE_CURRENT_SOURCE_DIR}/src/pprof)

    add_executable(heap_checker_stub_test src/tests/heap-checker-stub-test.cc)
    target_link_libraries(heap_checker_stub_test tcmalloc)
    add_test(heap_checker_stub_test heap_checker_stub_test)

    add_executable(sampler_test src/tests/sampler_test.cc
      src/sampler.cc
      src/base/spinlock.cc
      src/base/spinlock_internal.cc
      src/base/sysinfo.cc
      src/base/logging.cc)
    target_link_libraries(sampler_test gtest)
    add_test(sampler_test sampler_test)

    # These unittests often need to run binaries.  They're in the current dir
    list(APPEND TESTS_ENVIRONMENT BINDIR=. TMPDIR=/tmp/perftools)
    set(sampling_test_SOURCES src/tests/sampling_test.cc)
    add_executable(sampling_test src/tests/sampling_test.cc)
    target_compile_definitions(sampling_test PRIVATE PPROF_PATH=${CMAKE_CURRENT_SOURCE_DIR}/src/pprof)
    target_link_libraries(sampling_test tcmalloc)
    add_test(sampling_test sampling_test)

    if(GPERFTOOLS_BUILD_HEAP_PROFILER)
      add_executable(heap-profiler_unittest src/tests/heap-profiler_unittest.cc)
      target_link_libraries(heap-profiler_unittest tcmalloc)
      add_test(NAME heap-profiler_unittest.sh
              COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/src/tests/heap-profiler_unittest.sh" "${CMAKE_CURRENT_BINARY_DIR}/heap-profiler_unittest")
    endif()
  endif()

endif()

### ------- tcmalloc with debugallocation
if(GPERFTOOLS_BUILD_DEBUGALLOC)
  if(GPERFTOOLS_BUILD_HEAP_PROFILER)
    add_library(tcmalloc_debug src/debugallocation.cc ${FULL_MALLOC_SRC})
    target_compile_definitions(tcmalloc_debug PRIVATE ${EMERGENCY_MALLOC_DEFINE})
    target_link_libraries(tcmalloc_debug PRIVATE symbolize low_level_alloc stacktrace common)

    ### Unittests
    if(BUILD_TESTING)
      add_executable(tcmalloc_debug_unittest src/tests/tcmalloc_unittest.cc src/tests/testutil.cc)
      target_link_libraries(tcmalloc_debug_unittest tcmalloc_debug gtest)
      add_test(tcmalloc_debug_unittest tcmalloc_debug_unittest)

      add_executable(sampling_debug_test src/tests/sampling_test.cc)
          target_compile_definitions(sampling_debug_test PRIVATE PPROF_PATH=${CMAKE_CURRENT_SOURCE_DIR}/src/pprof)
      target_link_libraries(sampling_debug_test tcmalloc_debug)
      add_test(sampling_debug_test sampling_debug_test)

      if(GPERFTOOLS_BUILD_HEAP_PROFILER)
        add_executable(heap-profiler_debug_unittest src/tests/heap-profiler_unittest.cc)
        target_link_libraries(heap-profiler_debug_unittest tcmalloc_debug)
        add_test(heap-profiler_debug_unittest.sh "${CMAKE_CURRENT_SOURCE_DIR}/src/tests/heap-profiler_unittest.sh" "${CMAKE_CURRENT_BINARY_DIR}/heap-profiler_debug_unittest")
      endif()
    endif()
  endif()
endif()

### ------- CPU profiler
if(GPERFTOOLS_BUILD_CPU_PROFILER)

add_library(profiler
    src/profiler.cc
    src/profile-handler.cc
    src/profiledata.cc)
  target_link_libraries(profiler PRIVATE stacktrace common)

  if(BUILD_TESTING)
    add_executable(getpc_test src/tests/getpc_test.cc)
    add_test(getpc_test getpc_test)

    add_executable(profiledata_unittest
      src/tests/profiledata_unittest.cc src/profiledata.cc)
    target_link_libraries(profiledata_unittest stacktrace common gtest)
    add_test(profiledata_unittest profiledata_unittest)

    add_executable(profile_handler_unittest
      src/tests/profile-handler_unittest.cc src/profile-handler.cc)
    target_link_libraries(profile_handler_unittest stacktrace common gtest)
    add_test(profile_handler_unittest profile_handler_unittest)

    add_test(NAME profiler_unittest.sh
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/src/tests/profiler_unittest.sh")
    set(PROFILER_UNITTEST_SRCS src/tests/profiler_unittest.cc
            src/tests/testutil.h src/tests/testutil.cc)
    # TODO: get rid of the {1,2,3,4} multiplicity
    add_executable(profiler1_unittest ${PROFILER_UNITTEST_SRCS})
    target_compile_definitions(profiler1_unittest PRIVATE NO_THREADS)
    target_link_libraries(profiler1_unittest PRIVATE profiler)
    add_executable(profiler2_unittest ${PROFILER_UNITTEST_SRCS})
    target_compile_definitions(profiler2_unittest PRIVATE NO_THREADS)
    target_link_libraries(profiler2_unittest PRIVATE profiler)
    add_executable(profiler3_unittest ${PROFILER_UNITTEST_SRCS})
    target_link_libraries(profiler3_unittest PRIVATE profiler)
    add_executable(profiler4_unittest ${PROFILER_UNITTEST_SRCS})
    target_link_libraries(profiler4_unittest PRIVATE stacktrace profiler)
  endif()
endif()

if(BUILD_TESTING)
  get_directory_property(tests TESTS)
  message("TESTS_ENVIRONMENT:${TESTS_ENVIRONMENT}")
  if(TESTS_ENVIRONMENT)
    foreach(test IN LISTS tests)
      set_tests_properties(${test} PROPERTIES ENVIRONMENT "${TESTS_ENVIRONMENT}")
    endforeach()
  endif()
endif()

if(MSVC)
    add_subdirectory(src/windows)
endif()

message(WARNING "note: gperftools' cmake support is incomplete and is best-effort only")
