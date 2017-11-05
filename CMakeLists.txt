cmake_minimum_required(VERSION 3.0)

# Based on configure.ac

project(gperftools VERSION 2.6.1 LANGUAGES C CXX)

set(tcmalloc_so_version 8.5.4)
set(profiler_so_version 4.14.4)

set(default_build_cpu_profiler ON)
set(default_build_heap_profiler ON)
set(default_build_heap_checker ON)
set(default_build_debugalloc ON)
set(default_build_minimal OFF)

set(default_tcmalloc_alignment 16)
set(need_nanosleep ON)

set(host string(TOLOWER "${CMAKE_SYSTEM_NAME}"))

if(MINGW)
  set(default_build_minimal ON)
  set(default_build_debugalloc OFF)
  set(need_nanosleep OFF)
elseif(CYGWIN)
  set(default_build_heap_checker OFF)
  set(default_build_cpu_profiler OFF)
elseif(host MATCHES "freebsd")
  set(default_build_heap_checker OFF)
elseif(APPLE)
  set(default_build_heap_checker OFF)
endif()

include(CMakeDependentOption)
include(CheckCCompilerFlag)
include(CheckCSourceCompiles)
include(CheckCXXSourceCompiles)
include(CheckFunctionExists)
include(CheckIncludeFile)
include(CheckLibraryExists)
include(CheckSymbolExists)
include(CheckTypeSize)
include(CheckVariableExists)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(CheckNoFPByDefault)
include(DefineTargetVariables)
include(FindObjcopyWithWeaken)
include(GPerftoolsAddTarget)
include(PCFromUContext)

define_target_variables()

if(s390)
  set(default_enable_libunwind OFF)
  set(default_enable_backtrace ON)
else()
  set(default_enable_libunwind ON)
  set(default_enable_backtrace OFF)
endif()

if(PPC64)
  set(default_enable_libunwind OFF)
  set(default_tcmalloc_pagesize 64)
else()
  set(default_enable_libunwind ON)
  set(default_tcmalloc_pagesize 8)
endif()

option(
  gperftools_build_minimal
  "Build only tcmalloc-minimal (and maybe tcmalloc-minimal-debug)"
  ${default_build_minimal})
cmake_dependent_option(
  gperftools_build_cpu_profiler "Build cpu-profiler" ${default_build_cpu_profiler}
  "NOT gperftools_build_minimal" OFF)
cmake_dependent_option(
  gperftools_build_heap_profiler "Build heap-profiler" ${default_build_heap_profiler}
  "NOT gperftools_build_minimal" OFF)
cmake_dependent_option(
  gperftools_build_heap_checker "Build heap-checker" ${default_build_heap_checker}
  "NOT gperftools_build_minimal" OFF)
cmake_dependent_option(
  gperftools_build_debugalloc "Build debugalloc" ${default_build_debugalloc}
  "NOT gperftools_build_minimal" OFF)

option(gperftools_enable_stacktrace_via_backtrace
       "Enable use of backtrace() for stacktrace capturing (may deadlock)"
       ${default_enable_backtrace})
option(gperftools_enable_libunwind
       "Enable libunwind linking"
       ${default_enable_libunwind})

set(enable_backtrace ${gperftools_enable_stacktrace_via_backtrace})
set(enable_libunwind ${gperftools_enable_libunwind})

set(gperftools_tcmalloc_pagesize ${default_tcmalloc_pagesize}
  CACHE STRING "Set the tcmalloc internal page size")
set_property(CACHE gperftools_tcmalloc_pagesize PROPERTY STRINGS "8" "32" "64")
if(NOT gperftools_tcmalloc_pagesize STREQUAL "8" AND
   NOT gperftools_tcmalloc_pagesize STREQUAL "32" AND
   NOT gperftools_tcmalloc_pagesize STREQUAL "64")
  message(WARNING
    "Invalid gperftools_tcmalloc_pagesize (${gperftools_tcmalloc_pagesize}), "
    "setting to default value (${default_tcmalloc_pagesize})")
  set(gperftools_tcmalloc_pagesize ${default_tcmalloc_pagesize})
endif()
if (gperftools_tcmalloc_pagesize STREQUAL "32" OR
  gperftools_tcmalloc_pagesize STREQUAL "64")
  set(TCMALLOC_${gperftools_tcmalloc_pagesize}K_PAGES ON)
endif()

set(gperftools_tcmalloc_alignment ${default_tcmalloc_alignment}
  CACHE STRING "Set the tcmalloc allocation alignment")
set_property(CACHE gperftools_tcmalloc_alignment PROPERTY STRINGS "8" "16")
if(NOT gperftools_tcmalloc_alignment STREQUAL "8" AND
   NOT gperftools_tcmalloc_alignment STREQUAL "16")
  message(WARNING
      "Invalid gperftools_tcmalloc_alignment (${gperftools_tcmalloc_alignment}), "
      "setting to default value (${default_tcmalloc_alignment})")
  set(gperftools_tcmalloc_alignment ${default_tcmalloc_alignment})
endif()
if(gperftools_tcmalloc_alignment STREQUAL "8")
  set(TCMALLOC_ALIGN_8BYTES ON)
endif()

check_c_source_compiles("#include <stdlib.h>
                         static void foo(void) __attribute__ ((unused));
                         void foo(void) { exit(1); }
                         int main() { return 0; }"
                        HAVE___ATTRIBUTE__)

set(inline)
foreach(inline_keyword "inline" "__inline" "__inline__")
  check_c_source_compiles("
      static ${inline_keyword} void foo() {}
      int main() { foo(); return 0; }"
      have_${inline_keyword})
  if(have_${inline_keyword})
      set(inline ${inline_keyword})
      break()
  endif()
endforeach()

option(gperftools_objcopy_path "Path to objcopy if present" "")
find_objcopy_with_weaken(objcopy_exe ${gperftools_objcopy_path})

check_type_size("__int64" __INT64 LANGUAGE CXX)
set(CMAKE_EXTRA_INCLUDE_FILES "malloc.h")
check_type_size("struct mallinfo" STRUCT_MALLINFO LANGUAGE CXX)
set(CMAKE_EXTRA_INCLUDE_FILES "elf.h")
check_type_size("Elf32_Versym" ELF32_VERSYM LANGUAGE CXX)
set(CMAKE_EXTRA_INCLUDE_FILES)
check_function_exists("sbrk" HAVE_SBRK)
check_function_exists("geteuid" HAVE_GETEUID)
check_function_exists("fork" HAVE_FORK)
check_include_file("features.h" HAVE_FEATURES_H)
check_include_file("inttypes.h" HAVE_INTTYPES_H)
check_include_file("stdint.h" HAVE_STDINT_H)
check_include_file("stdlib.h" HAVE_STDLIB_H)
check_include_file("strings.h" HAVE_STRINGS_H)
check_include_file("string.h" HAVE_STRING_H)
check_include_file("dlfcn.h" HAVE_DLFCN_H)
check_include_file("malloc.h" HAVE_MALLOC_H)
check_include_file("sys/malloc.h" HAVE_SYS_MALLOC_H)
check_include_file("sys/param.h" HAVE_SYS_PARAM_H)
check_include_file("glob.h" HAVE_GLOB_H)
check_include_file("execinfo.h" HAVE_EXECINFO_H)
check_include_file("unwind.h" HAVE_UNWIND_H)
check_include_file("unistd.h" HAVE_UNISTD_H)
check_include_file("valgrind.h" HAVE_VALGRIND_H)
check_include_file("sched.h" HAVE_SCHED_H)
check_include_file("conflict-signal.h" HAVE_CONFLICT_SIGNAL_H)
check_include_file("cygwin/signal.h" HAVE_CYGWIN_SIGNAL_H)
check_include_file("sys/prctl.h" HAVE_SYS_PRCTL_H)
check_include_file("linux/ptrace.h" HAVE_LINUX_PTRACE_H)
check_include_file("sys/syscall.h" HAVE_SYS_SYSCALL_H)
check_include_file("sys/socket.h" HAVE_SYS_SOCKET_H)
check_include_file("sys/stat.h" HAVE_SYS_STAT_H)
check_include_file("sys/types.h" HAVE_SYS_TYPES_H)
check_include_file("sys/wait.h" HAVE_SYS_WAIT_H)
check_include_file("poll.h" HAVE_POLL_H)
check_include_file("fcntl.h" HAVE_FCNTL_H)
check_include_file("grp.h" HAVE_GRP_H)
check_include_file("pwd.h" HAVE_PWD_H)
check_include_file("sys/resource.h" HAVE_SYS_RESOURCE_H)
check_include_file("valgrind.h" HAVE_VALGRIND_H)
check_include_file("sys/cdefs.h" HAVE_SYS_CDEFS_H)

set(CMAKE_REQUIRED_DEFINITIONS -D_XOPEN_SOURCE=600)
check_symbol_exists("cfree" "stdlib.h;malloc.h" HAVE_DECL_CFREE)
set(CMAKE_REQUIRED_DEFINITIONS)
check_symbol_exists("posix_memalign" "stdlib.h;malloc.h" HAVE_DECL_POSIX_MEMALIGN)
check_symbol_exists("memalign" "stdlib.h;malloc.h" HAVE_DECL_MEMALIGN)
check_symbol_exists("valloc" "stdlib.h;malloc.h" HAVE_DECL_VALLOC)
check_symbol_exists("pvalloc" "stdlib.h;malloc.h" HAVE_DECL_PVALLOC)
check_symbol_exists("mmap" "stdlib.h;unistd.h;sys/mman.h;sys/param.h" HAVE_MMAP)
check_symbol_exists("uname" "sys/utsname.h" HAVE_DECL_UNAME)

check_function_exists("snprintf" HAVE_SNPRINTF)

if(WIN32)
  set(HAVE_GETPAGESIZE ON)
else()
  check_function_exists("getpagesize" HAVE_GETPAGESIZE)
endif()

check_c_source_compiles("
  #include <stdint.h>
  int main()
  {
    int32_t v1 = 0;
    intptr_t v2 = 0;
    return (&v1 - &v2);
  }"
  INT32_EQUALS_INTPTR)

pc_from_ucontext(PC_FROM_UCONTEXT_DEF)

if(gperftools_enable_libunwind)
  check_include_file("libunwind.h" HAVE_LIBUNWIND_H)
  find_library(libunwind_location NAMES unwind)
  if(libunwind_location)
    check_library_exists(
      unwind backtrace ${libunwind_location} have_libunwind)
  endif()
  if(have_libunwind)
    set(unwind_libs unwind)
    set(will_use_libunwind ON)
  endif()
endif()

if(x86_64)
  check_omit_fp_by_default(omit_fp_by_default)
endif()

if(x86_64 AND omit_fp_by_default)
  set(fp_option_enabled "Success")
else()
  set(fp_option_enabled "Failed")
  if(NOT x86_64)
    set(fp_option_enabled "${fp_option_enabled}, not x86_64")
  elseif(NOT omit_fp_by_default)
    set(fp_option_enabled
        "${fp_option_enabled}, frame pointer not omitted by default")
  endif()
  message(STATUS
    "Enable option gperftools_enable_frame_pointers - ${fp_option_enabled}")
endif()

cmake_dependent_option(
  gperftools_enable_frame_pointers
  "On x86_64 systems, compile with -fno-omit-frame-pointer (see INSTALL)"
  OFF
  "x86_64;omit_fp_by_default"
  OFF)

check_c_compiler_flag("-Wno-unused-result" have_w_no_unused_result)

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

set(CMAKE_REQUIRED_FLAGS -fsized-deallocation)
check_cxx_source_compiles("
  #include <new>
  int main() { (::operator delete)(0, 256); return 0; }"
  have_sized_deallocation)
set(CMAKE_REQUIRED_FLAGS)

check_c_source_compiles(
  "namespace abc { void foo() {} } int main() { abc::foo(); return 0; }"
  HAVE_NAMESPACES)

check_c_source_compiles("
  #include <unwind.h>
  int main()
  {
    &_Unwind_Backtrace;
    return 0;
  }"
  HAVE_UNWIND_BACKTRACE)

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
  "int main() { void* sp = __builtin_stack_pointer(); return 0; }"
  HAVE_BUILTIN_STACK_POINTER)
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

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
   CMAKE_CXX_COMPILER_VERSION VERSION_LESS "4.1.2")
  message(WARNING "gcc has this bug: http://gcc.gnu.org/ml/gcc-bugs/2006-09/msg02275.html")
elseif(APPLE)
  message(WARNING "OSX __thread support is known to call malloc which makes "
                    "it unsafe to use from malloc replacement")
elseif(MINGW)
  message(WARNING "mingw doesn't really support tls")
else()
  check_c_source_compiles("static __thread int p = 0;" HAVE_TLS)
endif()

if(need_nanosleep)
  check_c_source_compiles(
    "#include <time.h>
     int main()
     { static struct timespec ts; nanosleep(&ts, NULL); return 0; }"
    nanosleep_ok)
  if(NOT nanosleep_ok)
    set(CMAKE_REQUIRED_LIBRARIES rt)
    check_c_source_compiles(
      "#include <time.h>
       int main()
       { static struct timespec ts; nanosleep(&ts, NULL); return 0; }"
      nanosleep_ok)
    if(nanosleep_ok)
      set(nanosleep_libs rt)
    else()
      message(FATAL_ERROR "cannot find the nanosleep function")
    endif()
    set(CMAKE_REQUIRED_LIBRARIES)
  endif()
endif()

if(enable_backtrace)
  check_symbol_exists("backtrace" "execinfo.h" HAVE_DECL_BACKTRACE)
  check_function_exists("backtrace" backtrace_exists)
  if(NOT backtrace_exists)
    set(CMAKE_REQUIRED_LIBRARIES execinfo)
    check_function_exists("backtrace" backtrace_exists)
    message("backtrace_exists: ${backtrace_exists}")
    set(CMAKE_REQUIRED_LIBRARIES)
    if(backtrace_exists)
      list(INSERT unwind_libs 0 execinfo)
    endif()
  endif()
endif()

find_package(Threads REQUIRED)
set(HAVE_PTHREAD ${CMAKE_USE_PTHREADS_INIT})
foreach(attr "PTHREAD_CREATE_JOINABLE" "PTHREAD_CREATE_UNDETACHED")
  check_c_source_compiles("
    #include <pthread.h>
    int main() { int attr = ${attr}; return attr; }"
    ${attr}_ATTR)
  if(${attr}_ATTR)
    set(PTHREAD_CREATE_JOINABLE ${attr})
    break()
  endif()
endforeach()
list(APPEND libs Threads::Threads)

if(FreeBSD)
  set(PTHREADS_CRASHES_IF_RUN_TOO_EARLY ON)
endif()

set(libstdcxx_la_linker_flag)
if(EXISTS /usr/sfw/lib/libstdc++.la)
  file(READ /usr/sfw/lib/libstdc++.la _ch LIMIT 1)
  if(string(LENGTH _ch) EQUAL 0)
    set(libstdcxx_la_linker_flag "-L${CMAKE_CURRENT_SOURCE_DIR}/src/solaris")
  endif()
endif()

check_cxx_source_compiles(
  "#include <string>
     #include <vector>
     int main() { pthread_t th; pthread_join(th, 0); return 0; }"
  have_pthread_despite_asking_for)

check_cxx_source_compiles(
  "#include <vector>
     int main() { vector<int> t; return 0; }"
  STL_NAMESPACE_NONE)
if(STL_NAMESPACE_NONE)
  set(STL_NAMESPACE "")
else()
  check_cxx_source_compiles(
    "#include <vector>
         int main() { std::vector<int> t; return 0; }"
    STL_NAMESPACE_STD)
  if (STL_NAMESPACE_STD)
    set(STL_NAMESPACE "std")
  endif()
endif()

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

configure_file(cmake/config.h.in ${CMAKE_BINARY_DIR}/config.h @ONLY)
configure_file(cmake/tcmalloc.h.in
               ${CMAKE_BINARY_DIR}/gperftools/tcmalloc.h
               @ONLY)

if(gperftools_build_cpu_profiler OR
   gperftools_build_heap_profiler OR
   gperftools_build_heap_checker)
  set(with_stack_trace ON)
endif()

if(gperftools_enable_frame_pointers AND
   unwind_libs STREQUAL "" AND
   NOT gperftools_build_minimal)
  if(HAVE_UNWIND_BACKTRACE)
    message(WARNING "No frame pointers and no libunwind. "
                    "Using experimental backtrace capturing via libgcc. "
                    "Expect crashy cpu profiler.")
    if(gperftools_enable_stacktrace_via_backtrace)
      message(WARNING "No frame pointers and no libunwind. "
                      "Using experimental backtrace(). "
                      "Expect crashy cpu profiler.")
    else()
      message(FATAL_ERROR "No frame pointers and no libunwind. "
                          "The compilation will fail.")
    endif()
  endif()
endif()

# Based on Makefile.am

set(CMAKE_INCLUDE_CURRENT_DIR ON)

list(APPEND inc $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
if(NOT with_stack_trace)
  list(APPEND def NO_TCMALLOC_SAMPLES)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  list(APPEND options
    -Wall -Wwrite-strings -Woverloaded-virtual
    -Wno-sign-compare
    -fno-builtin-malloc -fno-builtin-free -fno-builtin-realloc
    -fno-builtin-calloc -fno-builtin-cfree
    -fno-builtin-memalign -fno-builtin-posix_memalign
    -fno-builtin-valloc -fno-builtin-pvalloc)

  if(i386)
    list(APPEND options -mmmx -fno-omit-frame-pointer)
  endif()
endif()

if(have_w_no_unused_result)
  list(APPEND options -Wno-unused-result)
endif()

if(have_sized_deallocation)
  list(APPEND options -fsized-deallocation)
endif()

list(APPEND libs ${libstdcxx_la_linker_flag})

if(gperftools_enable_frame_pointers)
  list(APPEND options -fno-omit-frame-pointer)
elseif(NOT CMAKE_CXX_FLAGS MATCHES "-fno-omit-frame-pointer")
  list(APPEND defs NO_FRAME_POINTER)
endif()

if(MINGW)
  list(APPEND tcmalloc_lib_options "-Wl,-u__tcmalloc")
endif()

if(objcopy_exe)
  set(weaken
      -W malloc -W free -W realloc -W calloc -W cfree
      -W memalign -W posix_memalign -W valloc -W pvalloc
      -W aligned_alloc
      -W malloc_stats -W mallopt -W mallinfo -W nallocx
      -W _Znwm -W _ZnwmRKSt9nothrow_t -W _Znam -W _ZnamRKSt9nothrow_t
      -W _ZdlPv -W _ZdaPv
      -W __Znwm -W __ZnwmRKSt9nothrow_t -W __Znam -W __ZnamRKSt9nothrow_t
      -W __ZdlPv -W __ZdaPv)
endif()

message(STATUS "includes: ${inc}")
message(STATUS "libraries: ${libs}")
message(STATUS "definitions: ${defs}")
message(STATUS "options: ${options}")

set(logging_headers
  src/base/logging.h
  src/base/commandlineflags.h
  src/base/basictypes.h
  src/base/dynamic_annotations.h
  src/third_party/valgrind.h)
add_library(logging
  src/base/logging.cc
  src/base/dynamic_annotations.c
  ${logging_headers})
gperftools_add_target(logging)

set(sysinfo_headers
  src/base/sysinfo.h
  src/getenv_safe.h
  src/base/arm_instruction_set_select.h
  src/base/basictypes.h)
add_library(sysinfo
  src/base/sysinfo.cc
  ${sysinfo_headers})
target_link_libraries(sysinfo PUBLIC logging ${nanosleep_libs})

set(spinlock_headers
  src/base/spinlock.h
  src/base/spinlock_internal.h
  src/base/spinlock_win32-inl.h
  src/base/spinlock_linux-inl.h
  src/base/spinlock_posix-inl.h
  src/base/atomicops-internals-macosx.h
  src/base/atomicops-internals-linuxppc.h
  src/base/atomicops-internals-arm-generic.h
  src/base/atomicops-internals-arm-v6plus.h
  src/base/atomicops-internals-mips.h
  src/base/atomicops-internals-windows.h
  src/base/atomicops-internals-gcc.h
  src/base/atomicops-internals-x86.h)
add_library(spinlock
  src/base/spinlock.cc
  src/base/spinlock_internal.cc
  src/base/atomicops-internals-x86.cc
  ${spinlock_headers})
target_link_libraries(spinlock PUBLIC sysinfo ${nanosleep_libs})

set(maybe_threads_headers src/maybe_threads.h)

if(MINGW)
  set(windows_headers
    src/windows/port.h
    src/windows/mingw.h
    src/windows/mini_disassembler.h
    src/windows/mini_disassembler_types.h
    src/windows/preamble_patcher.h)
  add_library(windows
    src/windows/port.cc
    src/windows/system-alloc.cc
    src/windows/ia32_modrm_map.cc
    src/windows/ia32_opcode_map.cc
    src/windows/mini_disassembler.cc
    src/windows/patch_functions.cc
    src/windows/preamble_patcher.cc
    src/windows/preamble_patcher_with_stub.cc
    ${windows_headers})
  gperftools_add_target(windows)
  target_link_libraries(windows PUBLIC psapi)

  target_link_libraries(spinlock PUBLIC windows)
  target_link_libraries(sysinfo PUBLIC shlwapi)

  if(have_pthread_despite_asking_for)
    add_library(maybe_threads src/maybe_threads.cc ${maybe_threads_headers})
    gperftools_add_target(maybe_threads)
    set(maybe_threads_lib maybe_threads)
  endif()
else()
  set(tcmalloc_cc "src/tcmalloc.cc")
  set(system_alloc_cc "src/system-alloc.cc")

  add_library(maybe_threads src/maybe_threads.cc ${maybe_threads_headers})
  gperftools_add_target(maybe_threads)
  set(maybe_threads_lib maybe_threads)
endif()

if(BUILD_TESTING)
  add_executable(low_level_alloc_unittest
    src/base/low_level_alloc.cc
    src/malloc_hook.cc
    src/tests/low_level_alloc_unittest.cc)
  target_compile_definitions(low_level_alloc_unittest PUBLIC
    NO_TCMALLOC_SAMPLES)
  target_link_libraries(low_level_alloc_unittest PUBLIC spinlock maybe_threads)

  add_executable(atomicops_unittest src/tests/atomicops_unittest.cc)
  target_link_libraries(atomicops_unittest spinlock)
endif()

if(with_stack_trace)
  set(stacktrace_headers
    src/stacktrace_impl_setup-inl.h
    src/stacktrace_generic-inl.h
    src/stacktrace_libgcc-inl.h
    src/stacktrace_libunwind-inl.h
    src/stacktrace_arm-inl.h
    src/stacktrace_powerpc-inl.h
    src/stacktrace_powerpc-darwin-inl.h
    src/stacktrace_powerpc-linux-inl.h
    src/stacktrace_x86-inl.h
    src/stacktrace_win32-inl.h
    src/stacktrace_instrument-inl.h
    src/base/elf_mem_image.h
    src/base/vdso_support.h
    src/gperftools/stacktrace.h)
  add_library(stacktrace
    src/stacktrace.cc
    src/base/elf_mem_image.cc
    src/base/vdso_support.cc
    ${stacktrace_headers})
  target_link_libraries(stacktrace PUBLIC unwind spinlock)
  gperftools_add_target(stacktrace)

  add_library(fake_stacktrace_scope src/fake_stacktrace_scope.cc)

  if(BUILD_TESTING)
    set(stacktrace_unittest_headers
      src/config_for_unittests.h
      src/base/commandlineflags.h)
    add_executable(stacktrace_unittest
      src/tests/stacktrace_unittest.cc
      ${stacktrace_unittest_headers})
    target_link_libraries(stacktrace_unittest PUBLIC
      stacktrace logging fake_stacktrace_scope)
    gperftools_add_target(stacktrace_unittest)

    add_custom_target(pprof_unittest
      COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/src/pprof -test
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/pprof)
  endif()
endif()

set(tcmalloc_minimal_headers
  src/common.h
  src/internal_logging.h
  src/system-alloc.h
  src/packed-cache-inl.h
  src/tcmalloc_guard.h
  src/base/commandlineflags.h
  src/base/basictypes.h
  src/pagemap.h
  src/sampler.h
  src/central_freelist.h
  src/linked_list.h
  src/libc_override.h
  src/libc_override_gcc_and_weak.h
  src/libc_override_glibc.h
  src/libc_override_osx.h
  src/libc_override_redefine.h
  src/page_heap.h
  src/page_heap_allocator.h
  src/span.h
  src/static_vars.h
  src/symbolize.h
  src/thread_cache.h
  src/stack_trace_table.h
  src/base/thread_annotations.h
  src/malloc_hook-inl.h
  src/malloc_hook_mmap_linux.h
  src/malloc_hook_mmap_freebsd.h)
add_library(tcmalloc_minimal
  src/common.cc
  src/internal_logging.cc
  ${system_alloc_cc}
  src/memfs_malloc.cc
  src/central_freelist.cc
  src/page_heap.cc
  src/sampler.cc
  src/span.cc
  src/stack_trace_table.cc
  src/static_vars.cc
  src/symbolize.cc
  src/thread_cache.cc
  src/malloc_hook.cc
  src/malloc_extension.cc
  ${tcmalloc_minimal_headers})
target_compile_definitions(tcmalloc_minimal PUBLIC
  NO_TCMALLOC_SAMPLES NO_HEAP_CHECK)
target_link_libraries(tcmalloc_minimal PUBLIC
  spinlock ${maybe_threads_lib} ${tcmalloc_lib_options})

if(MINGW)
  target_link_libraries(tcmalloc_minimal PUBLIC stacktrace)
endif()

if(BUILD_TESTING)
  add_executable(tcmalloc_minimal_unittest
    src/tests/tcmalloc_unittest.cc
    src/tests/testutil.cc
    src/tests/testutil.h)
  target_link_libraries(tcmalloc_minimal_unittest PUBLIC tcmalloc_minimal)
endif()
