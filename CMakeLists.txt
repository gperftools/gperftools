project(gperftools VERSION 2.6.1 LANGUAGES CXX)


set(tcmalloc_so_version 8.5.4)
set(profiler_so_version 4.14.4)

set(default_build_cpu_profiler ON)
set(default_build_heap_profiler ON)
set(default_build_heap_checker ON)
set(default_build_debugalloc ON)
set(default_build_minimal OFF)

set(tcmalloc_default_alignment 16)
set(need_nanosleep ON)

if(MINGW)
    set(default_build_minimal ON)
    set(default_build_debugalloc OFF)
    set(need_nanosleep OFF)
elseif(CYGWIN)
    set(default_build_heap_checker OFF)
    set(default_build_cpu_profiler OFF)
elseif(FREEBSD)
    set(default_build_heap_checker OFF)
elseif(DARWIN)
    set(default_build_heap_checker OFF)
endif()

option(gperftools_build_cpu_profiler "Build cpu-profiler" ${default_build_cpu_profiler})
option(gperftools_build_heap_profiler "Build heap-profiler" ${default_build_heap_profiler})
option(gperftools_build_heap_checker "Build heap-checker" ${default_build_heap_checker})
option(gperftools_build_debugalloc "Build debugalloc" ${default_build_debugalloc})
option(gperftools_build_minimal "Build fully minimal" ${default_build_minimal})
