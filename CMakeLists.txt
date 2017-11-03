cmake_minimum_required(VERSION 3.0)

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
if(host MATCHES "freebsd")
    set(FREEBSD ON)
elseif(CMAKE_VERSION VERSION_LESS "3.2" AND host MATCHES "mingw")
    set(MINGW ON)
endif()

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
include(CheckCXXCompilerFlag)
include(CheckCXXSourceCompiles)
include(CheckCXXSymbolExists)
include(CheckFunctionExists)
include(CheckIncludeFileCXX)
include(CheckLibraryExists)
include(CheckTypeSize)
include(CheckVariableExists)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(DefineArchVariables)
include(PCFromUContext)

define_arch_variables()

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

option(gperftools_stacktrace_via_backtrace
       "Enable use of backtrace() for stacktrace capturing (may deadlock)"
       ${default_enable_backtrace})
option(gperftools_enable_libunwind
       "Enable libunwind linking"
       ${default_enable_libunwind})

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

# TODO: `target_add_definitions` for above (i.e. `TCMALLOC_32K_PAGES`,
#       `TCMALLOC_ALIGN_8BYTES`)

check_cxx_source_compiles("#include <stdlib.h>
                           static void foo(void) __attribute__ ((unused));
                           void foo(void) { exit(1); }"
                          HAVE___ATTRIBUTE__)

foreach(candidate IN "inline" "__inline" "__inline__" "")
    check_cxx_source_compiles(
        "static ${candidate} void foo() {}" candidate_compiled)
    if(candidate_compiled)
        set(inline "${candidate}")
        break()
    endif()
endforeach()

# TODO: Find `objcopy` with `--weaken` support

check_type_size("__int64" __INT64 LANGUAGE CXX)
set(CMAKE_EXTRA_INCLUDE_FILES "malloc.h")
check_type_size("struct mallinfo" STRUCT_MALLINFO LANGUAGE CXX)
set(CMAKE_EXTRA_INCLUDE_FILES "elf.h")
check_type_size("Elf32_Versym" ELF32_VERSYM LANGUAGE CXX)
set(CMAKE_EXTRA_INCLUDE_FILES)
check_function_exists("sbrk" HAVE_SBRK)
check_function_exists("geteuid" HAVE_GETEUID)
check_function_exists("fork" HAVE_FORK)
check_include_file_cxx("features.h" HAVE_FEATURES_H)
check_include_file_cxx("malloc.h" HAVE_MALLOC_H)
check_include_file_cxx("glob.h" HAVE_GLOB_H)
check_include_file_cxx("execinfo.h" HAVE_EXECINFO_H)
check_include_file_cxx("unwind.h" HAVE_UNWIND_H)
check_include_file_cxx("sched.h" HAVE_SCHED_H)
check_include_file_cxx("conflict-signal.h" HAVE_CONFLICT_SIGNAL_H)
check_include_file_cxx("sys/prctl.h" HAVE_SYS_PRCTL_H)
check_include_file_cxx("linux/ptrace.h" HAVE_LINUX_PTRACE_H)
check_include_file_cxx("sys/syscall.h" HAVE_SYS_SYSCALL_H)
check_include_file_cxx("sys/socket.h" HAVE_SYS_SOCKET_H)
check_include_file_cxx("sys/wait.h" HAVE_SYS_WAIT_H)
check_include_file_cxx("poll.h" HAVE_POLL_H)
check_include_file_cxx("fcntl.h" HAVE_FCNTL_H)
check_include_file_cxx("grp.h" HAVE_GRP_H)
check_include_file_cxx("pwd.h" HAVE_PWD_H)
check_include_file_cxx("sys/resource.h" HAVE_SYS_RESOURCE_H)
check_include_file_cxx("valgrind.h" HAVE_VALGRIND_H)
check_include_file_cxx("sys/cdefs.h" HAVE_SYS_CDEFS_H)
check_include_file_cxx("features.h" HAVE_FEATURES_H)

set(CMAKE_REQUIRED_DEFINITIONS -D_XOPEN_SOURCE=600)
check_cxx_symbol_exists("cfree" "stdlib.h;malloc.h" HAVE_DECL_CFREE)
set(CMAKE_REQUIRED_DEFINITIONS)
check_cxx_symbol_exists("posix_memalign" "stdlib.h;malloc.h" HAVE_DECL_POSIX_MEMALIGN)
check_cxx_symbol_exists("memalign" "stdlib.h;malloc.h" HAVE_DECL_MEMALIGN)
check_cxx_symbol_exists("valloc" "stdlib.h;malloc.h" HAVE_DECL_VALLOC)
check_cxx_symbol_exists("pvalloc" "stdlib.h;malloc.h" HAVE_DECL_PVALLOC)
check_cxx_symbol_exists("mmap" "stdlib.h;unistd.h;sys/mman.h;sys/param.h" HAVE_DECL_MMAP)

check_cxx_source_compiles(
    "#include <stdint.h>\nint main() { int32_t v1 = 0; intptr_t v2 = 0; return (&v1 - &v2); }"
    INT32_EQUALS_INTPTR)

pc_from_ucontext(pc_field)

set(unwind_libs)
if(gperftools_enable_libunwind)
    check_include_file_cxx("libunwind.h" HAVE_LIBUNWIND_H)
    find_library(libunwind_location NAMES unwind)
    if(libunwind_location)
        check_library_exists(unwind backtrace ${libunwind_location} HAVE_LIBUNWIND)
    endif()
    if(HAVE_LIBUNWIND)
        list(APPEND unwind_libs unwind)
        set(will_use_libunwind ON)
    endif()
endif()

cmake_dependent_option(
    gperftools_enable_frame_pointers
    "On x86_64 systems, compile with -fno-omit-frame-pointer (see INSTALL)"
    OFF
    "x86_64"
    OFF)

check_cxx_compiler_flag("-Wno-unused-result" HAVE_W_NO_UNUSED_RESULT)

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

check_cxx_compiler_flag("-fsized-deallocation" HAVE_SIZED_DEALLOCATION)

check_cxx_source_compiles("include <unwind.h>\nint main() { &Unwind_Backtrace; return 0; }"
                          HAVE_UNWIND_BACKTRACE)

if(enable_backtrace)
    set(default_emergency_malloc ON)
else()
    set(default_emergency_malloc OFF)
endif()

if(will_use_libunwind AND arm)
    set(default_emergency_malloc ON)
endif()

option(gperftools_emergency_malloc
       "Build emergency malloc"
       ${default_emergency_malloc})

check_cxx_source_compiles(
    "int main() { void* sp = __buildin_stack_pointer(); return 0; }"
    HAVE_BUILTIN_STACK_POINTER)
check_cxx_source_compiles(
    "int main() { return __builtin_expect(main != 0, 1); }"
    HAVE_BUILTIN_EXPECT)

check_cxx_source_compiles(
    "#include <unistd.h>\nint main() { char** env = __environ; return 0; }"
    HAVE___ENVIRON)

if(CMAKE_CXX_COMPILER STREQUAL "GNU" AND
   CMAKE_CXX_COMPILER_VERSION VERSION_LESS "4.1.2")
    message(WARNING "gcc has this bug: http://gcc.gnu.org/ml/gcc-bugs/2006-09/msg02275.html")
elseif(APPLE)
    message(WARNING "OSX __thread support is known to call malloc which makes "
                    "it unsafe to use from malloc replacement")
elseif(MINGW)
    message(WARNING "mingw doesn't really support tls")
else()
    check_cxx_source_compiles("static __thread int p = 0;" HAVE_TLS)
endif()

if(need_nanosleep)
    check_cxx_source_compiles(
        "#include <time.h>\nint main() { static struct timespec ts; nanosleep(&ts, NULL); return 0; }"
        nanosleep_ok)
    if(NOT nanosleep_ok)
        set(CMAKE_REQUIRED_LIBRARIES rt)
        check_cxx_source_compiles(
            "int main() { static struct timespec ts; nanosleep(&ts, NULL); return 0; }"
            nanosleep_ok)
        if(nanosleep_ok)
            list(APPEND LIBS rt)
        else()
            message(FATAL_ERROR "cannot find the nanosleep function")
        endif()
        set(CMAKE_REQUIRED_LIBRARIES)
    endif()
endif()

if(EXISTS /usr/sfw/lib/libstdc++.la)
    file(READ /usr/sfw/lib/libstdc++.la _ch LIMIT 1)
    if(string(LENGTH _ch) EQUAL 0)
        # TODO: list(APPEND LIBS "src/solaris")
    endif()
endif()

check_cxx_source_compiles(
    "#include <string>\n#include <vector>\nint main() { pthread_t th; pthread_join(th, 0); return 0; }"
    HAVE_PTHREAD_DESPITE_ASKING_FOR)

check_cxx_source_compiles(
    "#include <vector>\nint main() { vector<int> t; return 0; }"
    STL_NAMESPACE_NONE)
if(STL_NAMESPACE_NONE)
    set(STL_NAMESPACE "")
else()
    check_cxx_source_compiles(
        "#include <vector>\nint main() { std::vector<int> t; return 0; }"
        STL_NAMESPACE_STD)
    if (STL_NAMESPACE_STD)
        set(STL_NAMESPACE "std")
    endif()
endif()

check_variable_exists("program_invocation_name" HAVE_PROGRAM_INVOCATION_NAME)

if(MINGW)
    check_cxx_symbol_exists("sleep" "unistd.h" HAVE_DECL_SLEEP)
    check_cxx_symbol_exists("nanosleep" "time.h" HAVE_DECL_NANOSLEEP)
endif()
