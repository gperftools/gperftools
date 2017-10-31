project(gperftools VERSION 2.6.1 LANGUAGES CXX)


set(tcmalloc_so_version 8.5.4)
set(profiler_so_version 4.14.4)

set(default_build_cpu_profiler ON)
set(default_build_heap_profiler ON)
set(default_build_heap_checker ON)
set(default_build_debugalloc ON)
set(default_build_minimal OFF)

set(default_tcmalloc_alignment 16)
set(need_nanosleep ON)

set(host "${CMAKE_HOST_SYSTEM_NAME}")
if(host MATCHES ".*-mingw.*")
    set(default_build_minimal ON)
    set(default_build_debugalloc OFF)
    set(need_nanosleep OFF)
elseif(host MATCHES ".*-cygwin.*")
    set(default_build_heap_checker OFF)
    set(default_build_cpu_profiler OFF)
elseif(host MATCHES ".*-freebsd.*")
    set(default_build_heap_checker OFF)
elseif(host MATCHES ".*-darwin.*")
    set(default_build_heap_checker OFF)
endif()

include(CheckCSourceCompiles)
include(CheckFunctionExists)
include(CheckIncludeFile)
include(CheckTypeSize)
include(CMakeDependentOption)

check_c_source_compiles("int main() { return __s390__; }" s390)
if(s390)
    set(default_enable_libunwind OFF)
    set(default_enable_backtrace ON)
else()
    set(default_enable_libunwind ON)
    set(default_enable_backtrace OFF)
endif()

check_c_source_compiles("int main() { return __PPC64__; }" PPC64)
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
if(gperftools_tcmalloc_alignment NOT STREQUAL "8" AND
   gperftools_tcmalloc_alignment NOT STREQUAL "32" AND
   gperftools_tcmalloc_alignment NOT STREQUAL "64")
    message(WARNING
        "Invalid gperftools_tcmalloc_pagesize (${gperftools_tcmalloc_pagesize}),"
        "setting to default value (${default_tcmalloc_pagesize})")
    set(gperftools_tcmalloc_pagesize ${default_tcmalloc_pagesize})
endif()

set(gperftools_tcmalloc_alignment ${default_tcmalloc_alignment}
    CACHE STRING "Set the tcmalloc allocation alignment")
set_property(CACHE gperftools_tcmalloc_alignment PROPERTY STRINGS "8" "16")
if(gperftools_tcmalloc_alignment NOT STREQUAL "8" AND
   gperftools_tcmalloc_alignment NOT STREQUAL "16")
    message(WARNING
        "Invalid gperftools_tcmalloc_alignment (${gperftools_tcmalloc_alignment}), "
        "setting to default value (${default_tcmalloc_alignment})")
    set(gperftools_tcmalloc_alignment ${default_tcmalloc_alignment})
endif()

# TODO: `target_add_definitions` for above (i.e. `TCMALLOC_32K_PAGES`,
#       `TCMALLOC_ALIGN_8BYTES`)

# TODO: Find `objcopy` with `--weaken` support

check_type_size("__int64" __INT64)
set(CMAKE_EXTRA_INCLUDE_FILES "malloc.h")
check_type_size("struct mallinfo" struct_mallinfo)
set(CMAKE_EXTRA_INCLUDE_FILES "elf.h")
check_type_size("Elf32_Versym")
check_function_exists("sbrk" HAS_SBRK)
check_function_exists("geteuid" HAS_GETEUID)
check_function_exists("fork" HAS_FORK)
check_include_file("features.h" HAS_FEATURES)
check_include_file("malloc.h" HAS_MALLOC)
check_include_file("glob.h" HAS_GLOB)
check_include_file("execinfo.h" HAS_EXECINFO)
