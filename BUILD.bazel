# NOTE: as of this writing Bazel support is highly experimental. It is
# also not entirely complete. It lacks most tests, for example.

config_setting(
    name = "is_gcc",
    flag_values = {"@bazel_tools//tools/cpp:compiler": "gcc"},
)

config_setting(
    name = "is_clang",
    flag_values = {"@bazel_tools//tools/cpp:compiler": "clang"},
)

config_setting(
    name = "is_msvc",
    flag_values = {"@bazel_tools//tools/cpp:compiler": "msvc"},
)

CFLAGS_FOR_GCC = ["-Wall", "-Wwrite-strings", "-Wno-sign-compare", "-DTCMALLOC_DISABLE_HIDDEN_VISIBILITY"]
CXXFLAGS_FOR_GCC = CFLAGS_FOR_GCC + ["-Woverloaded-virtual", "-std=gnu++17", "-fsized-deallocation"]

CXXFLAGS = select({
    ":is_msvc": ["/std:c++17 /D_WIN32_WINNT=0x0602"],
    "//conditions:default": ["/std:c++17 /D_WIN32_WINNT=0x0602"], # the above doesn't work!
    ":is_gcc": CXXFLAGS_FOR_GCC,
    ":is_clang": CXXFLAGS_FOR_GCC + ["-Wthread-safety"]
})

CFLAGS = select({
    ":is_msvc": ["/D_WIN32_WINNT=0x0602"],
    "//conditions:default": ["/D_WIN32_WINNT=0x0602"],
    ":is_gcc": CFLAGS_FOR_GCC,
    ":is_clang": CFLAGS_FOR_GCC + ["-Wthread-safety"]
})

NON_WINDOWS = select({
    "@platforms//os:osx": [],
    "@platforms//os:linux": [],
    "//conditions:default": ["@platforms//:incompatible"],
})

cc_library(
    name = "trivialre",
    hdrs = ["benchmark/trivialre.h"],
    copts = CXXFLAGS,
)

cc_library(
    name = "all_headers",
    hdrs = glob(["src/*h", "src/base/*h", "generic-config/*h", "src/gperftools/*h"]),
    copts = CXXFLAGS,
)

cc_library(
    name = "run_benchmark",
    srcs = ["benchmark/run_benchmark.cc"],
    hdrs = ["benchmark/run_benchmark.h"],
    includes = ["generic-config", "src"],
    deps = [":trivialre", ":all_headers"],
    copts = CXXFLAGS,
)

cc_binary(
    name = "basic_benchmark",
    srcs = ["benchmark/malloc_bench.cc"],
    deps = [":run_benchmark"],
    copts = CXXFLAGS,
)

cc_library(
    name = "common",
    includes = ["generic-config", "src", "src/base"],
    copts = CXXFLAGS,
    srcs = [
        "src/base/logging.cc",
        "src/base/sysinfo.cc",
        "src/base/dynamic_annotations.cc",
        "src/base/spinlock.cc",
        "src/base/spinlock_internal.cc",
        "src/safe_strerror.cc",
        "src/base/generic_writer.cc",
        "src/base/proc_maps_iterator.cc",
    ] +
    select({"@platforms//os:windows": ["src/windows/port.cc",
                                       "src/windows/ia32_modrm_map.cc",
                                       "src/windows/ia32_opcode_map.cc",
                                       "src/windows/mini_disassembler.cc",
                                       "src/windows/preamble_patcher.cc",
                                       "src/windows/preamble_patcher_with_stub.cc"],
            "//conditions:default": []}),
    linkopts = select({"@platforms//os:windows": ["psapi.lib", "synchronization.lib", "shlwapi.lib"],
                       "//conditions:default": []}),
    deps = [":all_headers"],
)

cc_library(
    name = "tcmalloc_minimal",
    visibility = ["//visibility:public"],
    hdrs = [
        "src/gperftools/malloc_extension.h",
        "src/gperftools/malloc_extension_c.h",
        "src/gperftools/malloc_hook.h",
        "src/gperftools/malloc_hook_c.h",
        "src/gperftools/nallocx.h",
        "src/gperftools/tcmalloc.h",
    ],
    # note, bazel thingy is passing NDEBUG automagically in -c opt builds. So we're okay with that.
    local_defines = ["NO_TCMALLOC_SAMPLES"],
    includes = ["generic-config", "src", "src/base"],
    copts = CXXFLAGS,
    srcs = [
        "src/common.cc",
        "src/internal_logging.cc",
        "src/memfs_malloc.cc",
        "src/stack_trace_table.cc",
        "src/central_freelist.cc",
        "src/page_heap.cc",
        "src/sampler.cc",
        "src/span.cc",
        "src/static_vars.cc",
        "src/thread_cache.cc",
        "src/thread_cache_ptr.cc",
        "src/malloc_hook.cc",
        "src/malloc_extension.cc"] +
    select({"@platforms//os:windows": ["src/windows/patch_functions.cc", "src/windows/system-alloc.cc"],
            "//conditions:default": ["src/tcmalloc.cc", "src/system-alloc.cc"]}),
    alwayslink = 1,
    deps = [":all_headers", ":common"],
)

cc_library(
    name = "libbacktrace",
    copts = CFLAGS,
    includes = ["vendor/libbacktrace-integration", "vendor/libbacktrace"],
    hdrs = ["vendor/libbacktrace/elf.c", "vendor/libbacktrace/macho.c"], # yes, elf.c is included by file-format.c below and bazel makes us do this
    srcs = [
        "vendor/libbacktrace-integration/file-format.c",
        "vendor/libbacktrace/dwarf.c",
        "vendor/libbacktrace/fileline.c",
        "vendor/libbacktrace/posix.c",
        "vendor/libbacktrace/sort.c",
        "vendor/libbacktrace/state.c",
        "vendor/libbacktrace/read.c"] +
    glob(["vendor/libbacktrace-integration/*.h", "vendor/libbacktrace/*.h"]),
    target_compatible_with = NON_WINDOWS,
)

cc_library(
    name = "symbolize",
    includes = ["generic-config", "src", "src/base"],
    copts = CXXFLAGS,
    srcs = ["src/symbolize.cc",
            "vendor/libbacktrace-integration/backtrace-alloc.cc"],
    deps = [":all_headers", ":libbacktrace"],
    target_compatible_with = NON_WINDOWS,
)

cc_library(
    name = "low_level_alloc",
    includes = ["generic-config", "src", "src/base"],
    copts = CXXFLAGS,
    srcs = ["src/base/low_level_alloc.cc"],
    deps = [":all_headers"],
)

cc_library(
    name = "tcmalloc_minimal_debug",
    visibility = ["//visibility:public"],
    hdrs = ["src/tcmalloc.cc",
            "src/gperftools/malloc_extension.h",
            "src/gperftools/malloc_extension_c.h",
            "src/gperftools/malloc_hook.h",
            "src/gperftools/malloc_hook_c.h",
            "src/gperftools/nallocx.h",
            "src/gperftools/tcmalloc.h",
            ],
    # note, bazel thingy is passing NDEBUG automagically in -c opt builds. So we're okay with that.
    local_defines = ["NO_TCMALLOC_SAMPLES"],
    includes = ["generic-config", "src", "src/base"],
    copts = CXXFLAGS,
    srcs = [
        "src/common.cc",
        "src/internal_logging.cc",
        "src/memfs_malloc.cc",
        "src/stack_trace_table.cc",
        "src/central_freelist.cc",
        "src/page_heap.cc",
        "src/sampler.cc",
        "src/span.cc",
        "src/static_vars.cc",
        "src/thread_cache.cc",
        "src/thread_cache_ptr.cc",
        "src/malloc_hook.cc",
        "src/malloc_extension.cc",
        "src/debugallocation.cc", "src/system-alloc.cc"],
    alwayslink = 1,
    deps = [":all_headers", ":common", ":symbolize", ":low_level_alloc"],
    target_compatible_with = NON_WINDOWS,
)

cc_library(
    name = "stacktrace",
    visibility = ["//visibility:public"],
    hdrs = ["src/gperftools/stacktrace.h"],
    includes = ["generic-config", "src", "src/base"],
    copts = CXXFLAGS,
    srcs = ["src/stacktrace.cc", "src/base/elf_mem_image.cc", "src/base/vdso_support.cc"],
    deps = [":all_headers", ":common"],
)

cc_binary(
    name = "tcmalloc_bench",
    copts = CXXFLAGS,
    srcs = ["benchmark/malloc_bench.cc"],
    deps = [":run_benchmark", ":tcmalloc_minimal"])

cc_binary(
    name = "tcmalloc_debug_bench",
    copts = CXXFLAGS,
    srcs = ["benchmark/malloc_bench.cc"],
    deps = [":run_benchmark", ":tcmalloc_minimal_debug"])

cc_library(
    name = "tcmalloc",
    visibility = ["//visibility:public"],
    hdrs = [
        "src/gperftools/heap-profiler.h",
        "src/gperftools/malloc_extension.h",
        "src/gperftools/malloc_extension_c.h",
        "src/gperftools/malloc_hook.h",
        "src/gperftools/malloc_hook_c.h",
        "src/gperftools/nallocx.h",
        "src/gperftools/tcmalloc.h",
    ],
    # note, bazel thingy is passing NDEBUG automagically in -c opt builds. So we're okay with that.
    local_defines = ["ENABLE_EMERGENCY_MALLOC"],
    includes = ["generic-config", "src", "src/base"],
    copts = CXXFLAGS,
    srcs = [
        "src/common.cc",
        "src/internal_logging.cc",
        "src/memfs_malloc.cc",
        "src/stack_trace_table.cc",
        "src/central_freelist.cc",
        "src/page_heap.cc",
        "src/sampler.cc",
        "src/span.cc",
        "src/static_vars.cc",
        "src/thread_cache.cc",
        "src/thread_cache_ptr.cc",
        "src/malloc_hook.cc",
        "src/malloc_extension.cc",
        "src/tcmalloc.cc",
        "src/system-alloc.cc",
        "src/emergency_malloc.cc",
        "src/heap-profile-table.cc",
        "src/heap-profiler.cc",
        "src/malloc_backtrace.cc",
        "src/heap-checker-stub.cc",
    ],
    alwayslink = 1,
    deps = [":all_headers", ":common", ":low_level_alloc", ":stacktrace"],
    target_compatible_with = NON_WINDOWS,
)

cc_binary(
    name = "tcmalloc_full_bench",
    copts = CXXFLAGS,
    srcs = ["benchmark/malloc_bench.cc"],
    deps = [":run_benchmark", ":tcmalloc"])

cc_library(
    name = "tcmalloc_debug",
    visibility = ["//visibility:public"],
    hdrs = ["src/tcmalloc.cc", # tcmalloc.cc gets included by debugallocation.cc
            "src/gperftools/heap-profiler.h",
            "src/gperftools/malloc_extension.h",
            "src/gperftools/malloc_extension_c.h",
            "src/gperftools/malloc_hook.h",
            "src/gperftools/malloc_hook_c.h",
            "src/gperftools/nallocx.h",
            "src/gperftools/tcmalloc.h",
            ],
    # note, bazel thingy is passing NDEBUG automagically in -c opt builds. So we're okay with that.
    local_defines = ["ENABLE_EMERGENCY_MALLOC"],
    includes = ["generic-config", "src", "src/base"],
    copts = CXXFLAGS,
    srcs = [
        "src/common.cc",
        "src/internal_logging.cc",
        "src/memfs_malloc.cc",
        "src/stack_trace_table.cc",
        "src/central_freelist.cc",
        "src/page_heap.cc",
        "src/sampler.cc",
        "src/span.cc",
        "src/static_vars.cc",
        "src/thread_cache.cc",
        "src/thread_cache_ptr.cc",
        "src/malloc_hook.cc",
        "src/malloc_extension.cc",
        "src/debugallocation.cc",
        "src/system-alloc.cc",
        "src/emergency_malloc.cc",
        "src/heap-profile-table.cc",
        "src/heap-profiler.cc",
        "src/malloc_backtrace.cc",
        "src/heap-checker-stub.cc",
    ],
    alwayslink = 1,
    deps = [":all_headers", ":common", ":low_level_alloc", ":symbolize", ":stacktrace"],
    target_compatible_with = NON_WINDOWS,
)

cc_binary(
    name = "tcmalloc_full_debug_bench",
    copts = CXXFLAGS,
    srcs = ["benchmark/malloc_bench.cc"],
    deps = [":run_benchmark", ":tcmalloc_debug"])

cc_test(
    name = "tcmalloc_minimal_test",
    copts = CXXFLAGS,
    srcs = ["src/tests/tcmalloc_unittest.cc", "src/tests/testutil.h"],
    deps = [":all_headers", ":tcmalloc_minimal", "@googletest//:gtest_main"],)

cc_test(
    name = "tcmalloc_minimal_debug_test",
    copts = CXXFLAGS,
    srcs = ["src/tests/tcmalloc_unittest.cc", "src/tests/testutil.h"],
    deps = [":all_headers", ":tcmalloc_minimal_debug", "@googletest//:gtest_main"],)

cc_test(
    name = "tcmalloc_test",
    copts = CXXFLAGS,
    srcs = ["src/tests/tcmalloc_unittest.cc", "src/tests/testutil.h"],
    deps = [":all_headers", ":tcmalloc", "@googletest//:gtest_main"],)

cc_test(
    name = "tcmalloc_debug_test",
    copts = CXXFLAGS,
    srcs = ["src/tests/tcmalloc_unittest.cc", "src/tests/testutil.h"],
    deps = [":all_headers", ":tcmalloc_debug", "@googletest//:gtest_main"],)

cc_test(
    name = "debugallocation_test",
    copts = CXXFLAGS,
    srcs = ["src/tests/debugallocation_test.cc", "src/tests/testutil.h"],
    deps = [":all_headers", ":tcmalloc_debug", "@googletest//:gtest_main"],)

cc_library(
    name = "cpu_profiler",
    visibility = ["//visibility:public"],
    hdrs = ["src/gperftools/profiler.h"],
    copts = CXXFLAGS,
    srcs = [
        "src/profiler.cc",
        "src/profile-handler.cc",
        "src/profiledata.cc",
    ],
    alwayslink = 1,
    deps = [":all_headers", ":stacktrace", ":common"],
    target_compatible_with = NON_WINDOWS,
)

cc_binary(
    name = "tcmalloc_full_bench_with_profiler",
    copts = CXXFLAGS,
    srcs = ["benchmark/malloc_bench.cc"],
    deps = [":run_benchmark", ":tcmalloc", ":cpu_profiler"])
