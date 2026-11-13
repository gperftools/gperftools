# gperftools CI

`.github/workflows/ci.yml` is generated. Don't edit it by hand: edit
the files here and regenerate.

## How it works

- `configs.cue` is the list of build configs, i.e. what CI covers.
  Each config says how to build (autotools or cmake, configure args,
  env, extra packages, ...) and which GitHub runners to run it on.
- `workflow.cue` turns every (config, runner) pair into one GitHub
  Actions job, and checks the result against a GitHub Actions
  workflow schema.
- `gen.rb` runs [CUE](https://cuelang.org) to produce
  `.github/workflows/ci.yml` and `configs.json`. Both are committed.
- `run.rb` does the actual building and testing of a config, reading
  `configs.json`. Each CI job just runs `ruby ci/run.rb <tags>`, so
  you can run the very same thing locally.

### Tags

A config is identified by its set of tags, e.g. `autotools`,
`autotools i386`, `cmake dbg`, `cmake msvc opt`. Order doesn't matter
for matching; job names and build directories use the tags in the
order they're written.

`ruby ci/run.rb <tag>...` selects every config that has all the given
tags, except that *opt-in* tags (listed in `optInTags` in
`configs.cue`, currently `i386` and `msvc`) must be named explicitly.
They mark variants that need a special host or toolchain. So:

| Command                      | Runs                                  |
|------------------------------|---------------------------------------|
| `ruby ci/run.rb autotools`   | `autotools` (not `autotools i386`)    |
| `ruby ci/run.rb i386`        | `autotools i386`                      |
| `ruby ci/run.rb cmake`       | `cmake opt`, `cmake dbg`              |
| `ruby ci/run.rb cmake msvc`  | `cmake msvc opt`, `cmake msvc dbg`    |
| `ruby ci/run.rb dbg`         | `cmake dbg`                           |

A config's full tag set must select only that config. `gen.rb`
refuses to generate anything otherwise, with an error naming the two
configs involved.

## Running CI configs locally

```
ruby ci/run.rb                 # list configs
ruby ci/run.rb autotools       # build and test the autotools config
ruby ci/run.rb cmake           # both cmake configs, one after another
```

Matching configs run in order and the run stops at the first failure.
Each config builds out of tree in its own `ci-build/<tags>/`
directory (git-ignored), so configs don't interfere with each other or
with your own builds. Test logs end up there too.

Things to know:

- You need what the config needs: autotools or cmake + ninja, a
  compiler, and for `i386` the i686 cross compiler. See the config's
  `apt` list and the setup steps in `workflow.cue`. Some tests
  (pprof-based ones) are only built if `pprof` is in PATH
  (`go install github.com/google/pprof@latest`).
- Autotools refuses to build out of tree if the source directory
  itself has been configured. Run `make distclean` in the source
  directory first (or use a separate checkout/worktree).
- `run.rb` runs `./autogen.sh` only if `configure` doesn't exist yet.
- The runner isn't passed to `run.rb`: picking configs that make sense
  for your machine is up to you. On Windows, from a shell where ruby,
  cmake and ninja are available: `ruby ci\run.rb cmake msvc`.

## Making changes

You need ruby and the `cue` tool in PATH, at the version CI uses
(see `cue.mod/module.cue` and the `ci-gen-check` job in
`workflow.cue`):

```
go install cuelang.org/go/cmd/cue@v0.17.1
```

After editing any `.cue` file:

```
cue fmt ./ci/...
ruby ci/gen.rb
```

and commit the regenerated `ci.yml` and `configs.json` along with
your change. The `ci-gen-check` job fails if they are out of date.
The first `gen.rb` run fetches the GitHub Actions schema module from
the CUE registry, so it needs network access.

### Adding a config

Usually just a new entry in `configs` in `configs.cue`, e.g.:

```cue
{
	tags: ["autotools", "minimal"]
	sys: "autotools"
	runners: ["ubuntu-latest"]
	configure: ["--enable-minimal"]
},
```

The available fields and their defaults are in `#Config` in
`workflow.cue`.

Mind the "full tag set selects only that config" rule. Here the plain
config's tags (`autotools`) would also select `autotools minimal`, so
`gen.rb` rejects it as is. Variants of an existing config like this
one usually want their distinguishing tag to be opt-in: add
`"minimal"` to `optInTags`. Then `ruby ci/run.rb autotools` keeps
running just the plain config, and `ruby ci/run.rb minimal` runs the
new one. A tag that should be picked up implicitly (e.g. `dbg`) stays
ordinary, and then no other config's tags may be a subset of the new
config's tags.

Plain CUE `for` loops work inside the list for families of configs
(see the cmake `opt`/`dbg` entries).

### Adding a new kind of setting

If a config needs something `#Config` doesn't have yet:

1. add the field (with a default) to `#Config` in `workflow.cue`;
2. use it in `_job` in `workflow.cue` if it affects the GitHub job
   (setup steps, packages, ...), and/or in `run.rb` if it affects
   how the build and tests run;
3. regenerate.

Anything that is only about GitHub runners (installing packages,
caches, artifacts) belongs in `workflow.cue`; anything about building
and testing belongs in `run.rb`, so that it also works locally.

### Changing action versions or runners

Action versions are in `_uses` in `workflow.cue`. Runner labels are
per config in `configs.cue`.
