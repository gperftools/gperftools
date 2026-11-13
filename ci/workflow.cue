package ci

import (
	"list"
	"strings"

	"cue.dev/x/githubactions"
)

#Config: {
	// Identifies the config. Order doesn't matter for matching; names
	// (job names, build dirs) use the order as written.
	tags: [...string] & [_, ...] & list.UniqueItems()
	sys: "autotools" | "cmake"
	runners: [...string] & [_, ...]
	configure: [...string] | *[]
	env: [string]: string
	apt: [...string] | *[]
	// Binaries (relative to the build dir) that must exist and pass
	// after the test suite has run.
	expect: [...string] | *[]
	ccache: bool | *false
	// Extra compiler names to symlink into /usr/lib/ccache.
	ccacheCompilers: [...string] | *[]
	// Windows/MSVC: build commands are wrapped with msvc-env.rb.
	msvc: bool | *false

	// Out-of-tree build dir, relative to the source dir. Each config
	// gets its own, so several configs can run one after another.
	builddir: "ci-build/" + strings.Join(tags, "_")

	// Autotools builds verify PACKAGE_VERSION matches tcmalloc.h.
	if sys == "autotools" {
		env: GPERFTOOLS_ENSURE_PACKAGE_VERSION: "1"
	}
}

configs: [...#Config]

// Opt-in tags: a config carrying one of these only matches a query
// that names it (see tagCheck below). E.g. `ci/run.rb autotools` runs
// the plain autotools config but not the i386 one.
optInTags: [...string]

// Exported as ci/configs.json for ci/run.rb.
runData: {"optInTags": optInTags, "configs": configs}

// A config's full tag set must select only that config (this also
// rules out duplicates). A violation shows up as a conflict naming
// the configs involved.
//
// Query q selects config c iff c has all tags of q and every opt-in
// tag of c is in q. So a's full tag set selects b iff b has all of
// a's tags and b's extra tags are all ordinary (not opt-in).
tagCheck: {
	for i, a in configs for j, b in configs if i != j {
		let missing = [for t in a.tags if !list.Contains(b.tags, t) {t}]
		let extraOptIn = [for t in b.tags if !list.Contains(a.tags, t) && list.Contains(optInTags, t) {t}]
		if len(missing) == 0 && len(extraOptIn) == 0 {
			"tags [\(strings.Join(a.tags, " "))] also select [\(strings.Join(b.tags, " "))]": "ok" & "ambiguous"
		}
	}
}

// Action versions, shared by all jobs.
_uses: {
	checkout: "actions/checkout@v7"
	setupGo:  "actions/setup-go@v7"
	ccache:   "hendrikmuhs/ccache-action@v1.2"
	upload:   "actions/upload-artifact@v7"
}

// The runner images already have the autotools/compiler basics; ruby
// is listed for ci/run.rb in case an image lacks it.
_autotoolsDeps: ["ruby"]

// _job turns (config, runner) into a GitHub Actions job.
_job: {
	c:      #Config
	runner: string
	_linux: strings.HasPrefix(runner, "ubuntu")

	out: githubactions.#Job & {
		name:      "\(strings.Join(c.tags, " ")) (\(runner))"
		"runs-on": runner
		steps: [
			{name: "Clone", uses: _uses.checkout},
			if c.sys == "autotools" {
				name: "Install dependencies"
				run:  """
					sudo apt-get update
					sudo apt-get install -y --no-install-recommends \(strings.Join(list.Concat([_autotoolsDeps, c.apt]), " "))
					"""
			},
			// cmake jobs skip apt unless something is missing; the
			// arm64 runner images aren't guaranteed to have ninja.
			if c.sys == "cmake" && _linux {
				name: "Ensure ninja and ruby"
				run: """
					command -v ninja && command -v ruby || { sudo apt-get update && sudo apt-get install -y ninja-build ruby; }
					"""
			},
			if c.ccache {name: "Configure ccache action", uses: _uses.ccache, with: key: "${{ github.job }}"},
			if c.ccache {
				name: "Add ccache to PATH"
				run: strings.Join(list.Concat([
					["echo \"PATH=/usr/lib/ccache:$PATH\" >> $GITHUB_ENV"],
					[for cc in c.ccacheCompilers {"sudo ln -s /usr/bin/ccache /usr/lib/ccache/\(cc) || true"}],
				]), "\n")
			},
			if !c.msvc {name: "Install Go", uses: _uses.setupGo, with: "go-version": "stable"},
			if !c.msvc {
				name: "Install pprof"
				run: """
					go install github.com/google/pprof@latest
					echo "PATH=$HOME/go/bin:$PATH" >> $GITHUB_ENV
					PATH=$HOME/go/bin:$PATH which -a pprof
					"""
			},
			{
				"name": "Build and test"
				run:    "ruby ci/run.rb \(strings.Join(c.tags, " "))"
				if c.msvc {shell: "bash"}
			},
			{
				"name": "Upload Test Logs"
				uses:   _uses.upload
				if:     "${{ always() }}"
				with: {
					"name":           "test-logs-${{ github.job }}"
					"retention-days": 7
					if c.sys == "autotools" {path: "\(c.builddir)/*.log"}
					if c.sys == "cmake" {path: "\(c.builddir)/Testing/Temporary/LastTest.log"}
				}
			},
		]
	}
}

// Cheap check that ci.yml and configs.json are up to date with ci/*.cue.
_genCheck: githubactions.#Job & {
	"runs-on": "ubuntu-latest"
	steps: [
		{name: "Clone", uses: _uses.checkout},
		{name: "Install Go", uses: _uses.setupGo, with: "go-version": "stable"},
		{
			name: "Install cue"
			run: """
				go install cuelang.org/go/cmd/cue@v0.17.1
				echo "PATH=$HOME/go/bin:$PATH" >> $GITHUB_ENV
				"""
		},
		{
			name: "Check generated files are up to date"
			run: """
				ruby ci/gen.rb
				git diff --exit-code
				test -z "$(git status --porcelain)" || { git status; exit 1; }
				"""
		},
	]
}

workflow: githubactions.#Workflow & {
	name: "CI"
	on: {
		push: {
			branches: ["*"]
			tags: ["*"]
		}
		pull_request: {}
	}
	// A new push to the same branch (or PR) cancels the runs still in
	// progress for the previous one.
	concurrency: {
		group:                "${{ github.workflow }}-${{ github.ref }}"
		"cancel-in-progress": true
	}
	jobs: {
		"ci-gen-check": _genCheck
		// Job ids only need to be unique and valid (letters, digits,
		// '-', '_'); the UI shows the job's name.
		for c in configs for r in c.runners {
			"\(strings.Join(c.tags, "_"))_\(strings.Replace(r, ".", "_", -1))": (_job & {"c": c, runner: r}).out
		}
	}
}
