package ci

// The list of build configs, i.e. what CI covers. Each config is
// identified by its set of tags. CI runs one job per
// (config, runner); ci/run.rb <tags...> runs, in order, every config
// that has all the given tags (see workflow.cue for the rules).

_autotoolsChecks: [
	// A number of features in gperftools is optional. Make sure that
	// on common Linux platforms all the optional pieces are actually
	// configured and built. See e1f821e56f80ad63d37407267e0022b9ca07a458
	// for the curious example of how things could be subtly broken.
	//
	// getpc_test is a proxy for libprofiler being enabled.
	"getpc_test",
	// Ensure that debugallocator is present as well.
	"tcmalloc_minimal_debug_unittest",
	"tcmalloc_debug_unittest",
	// Verify that heap profiler is built and works.
	"heap-profiler_unittest",
]

// Tags that a config only matches when the query names them; for
// variants needing a special host/toolchain. See optInTags in
// workflow.cue.
optInTags: ["i386", "msvc"]

configs: [
	{
		tags: ["autotools"]
		sys: "autotools"
		// The -arm runners are Linux/aarch64. They matter for
		// aw-backtrace, which has a separate aarch64 implementation
		// (see https://github.com/gperftools/gperftools/issues/1653).
		runners: ["ubuntu-latest", "ubuntu-24.04-arm", "ubuntu-26.04-arm"]
		apt: ["libunwind-dev"]
		expect: _autotoolsChecks
	},
	{
		tags: ["autotools", "i386"]
		sys: "autotools"
		runners: ["ubuntu-latest"]
		// GNU/Linux i386 currently fails with libgcc backtracer due to
		// wrong unwind info in memcpy-ia32.S. So we force frame pointers.
		configure: ["--host=i686-linux-gnu", "--enable-frame-pointers"]
		apt: ["g++-i686-linux-gnu", "libc6-i386", "lib32stdc++6"]
		ccache: true
		// ccache only wraps compilers it has symlinks for.
		ccacheCompilers: ["i686-linux-gnu-gcc", "i686-linux-gnu-g++"]
		expect: _autotoolsChecks
	},
	for o in ["opt", "dbg"] {
		tags: ["cmake", o]
		sys: "cmake"
		runners: ["macos-latest", "ubuntu-latest", "ubuntu-24.04-arm"]
		configure: ["-DCMAKE_BUILD_TYPE=\(_buildType[o])"]
		ccache: true
	},
	for o in ["opt", "dbg"] {
		tags: ["cmake", "msvc", o]
		sys:  "cmake"
		msvc: true
		runners: ["windows-latest"]
		configure: [
			"-DCMAKE_BUILD_TYPE=\(_buildType[o])",
			"-DGPERFTOOLS_BUILD_HEAP_PROFILER=OFF",
			"-Dgperftools_build_minimal=ON",
		]
	},
]

_buildType: {opt: "RelWithDebInfo", dbg: "Debug"}
