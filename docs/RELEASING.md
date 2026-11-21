# Releasing

How a version number is chosen, where it is written down, and what happens when
a tag is pushed.

---

## The version number

`v<major>.<minor>.<patch>`, and every published tag has all three parts.
`v0.18` is a branch name, not a release; `v0.18.0` is a release.

| Part | Bumped when |
|---|---|
| **major** | `0` until the engine is something a person can ship a game on. After that, a break in the scripting API, the save format or the network protocol. |
| **minor** | A milestone lands. This is what `ROADMAP.md` counts, and what the working branch is named after. |
| **patch** | A fix, a packaging change or a build repair on top of a released minor. No new milestone work. |

While the major is `0` every release is marked pre-release on GitHub, and there
is no compatibility promise: the `.agame` save format, the wire protocol and the
Luau surface all still move, and a minor bump may break any of them.

Roadmap milestone headings are development labels. `v0.18` in `ROADMAP.md` is a
body of work; `v0.18.0` is the first build cut from it, and `v0.18.1` is the
next one, with no roadmap heading of its own.

---

## Where the version lives

**`VERSION`, in the repository root.** One line, `major.minor.patch`, and the
only place the number is written down. Everything else derives from it:

| Derives | How |
|---|---|
| `project(atomic VERSION ...)` | `CMakeLists.txt` reads `VERSION` before `project()`, and refuses a value that is not `major.minor.patch` |
| `engine::core::Version()` | `MONO_VERSION`, set from `PROJECT_VERSION` on `mono.engine/core/src/Version.cpp` alone |
| `<program> --version` | `Arguments::VersionLine()`, so every program prints the same shape |
| Artifact names | the `version` job in `.github/workflows/build.yml` reads `VERSION` |
| The `VERSION` file inside each archive | `scripts/package-release.sh` writes it |

Editing `VERSION` re-runs the configure (`CMAKE_CONFIGURE_DEPENDS` names it) and
rebuilds one translation unit, so there is no reason to put off bumping it.

Two things do *not* derive and are kept in step by hand:

- The git tag, `vx.y.z`, on the commit carrying the matching `VERSION`.
- `README.md`'s `Current Version:` line. Prose, not a source of truth.

The `version` job compares the tag against `VERSION` and stops the run before
any platform compiles if they differ.

```console
$ client --version
client 0.18.0
```

The program's name comes first because every program shares one version and a
pasted line has to say which one wrote it.

---

## Cutting a release

```sh
# 1. Bump the declared version, and README.md's "Current Version: **v0.18.1**".
echo 0.18.1 > VERSION

# 2. Check everything from a clean tree: the pull-request gate plus every [slow]
#    case. Install node_modules first: without node_modules/.bin/tsc the build
#    skips the TypeScript example scenes and says so in one STATUS line.
bun install        # or: npm install
just check

# 3. Commit, tag, push. The pre-push hook runs `just preset=ci check-pr`.
git commit -am "v0.18.1"
git tag v0.18.1
git push origin <branch> --follow-tags
```

The tag is what triggers the build. Branch pushes start no workflow, so
`just check` and the pre-push hook are the only verification a release tree
gets before the tag.

To see what the workflow produces without publishing, run `build` from the
Actions tab. A `workflow_dispatch` run builds and uploads artifacts, names them
`<version>+<short sha>`, and never creates a release. Its `domain_benchmarks`
input also builds the `bench` preset and runs
`scripts/verify-domain-benchmarks.py`.

---

## What the workflow does

`.github/workflows/build.yml`, three jobs:

1. **`version`** reads `VERSION`, checks it against the tag, decides
   pre-release (`0.*`), and emits the build matrix.
2. **`build`**, one entry per platform and flavour: configure and build
   `client studio server cdn launcher`, build `release-tests` and run
   `test_testrunner "[process]"` (shipped flavour only), then
   `scripts/package-release.sh` and upload `dist-<platform>-<flavour>`. ccache
   and the AccessKit Cargo build are cached between runs with
   `actions/cache`; `scripts/build-profile.py` reports cache behaviour and the
   raw logs upload as `build-profile-*`.
3. **`publish`**, tags only: downloads every `dist-*` artifact, adds
   `docs/schema.toml` and `docs/schema-data.toml`, and runs
   `gh release create --generate-notes` (or `gh release upload --clobber` if the
   release already exists).

`.github/workflows/version-impact.yml` is separate: on pull requests touching
engine or build paths it compares demo benchmarks against a baseline branch. It
does not build release artifacts.

---

## What comes out

Two flavours of the same tree, per platform.

**Shipped**, from the `release` preset: `RelWithDebInfo` with line tables,
first-party code at `-O3` (`MONO_OPTIMISE=ON`), asserts off, no heap profiler,
tests off, Tracy compiled in. This is what a player downloads.

**Dev**, from the `dist-dev` preset: the same code at `-O1`
(`MONO_OPTIMISE_LEVEL=1`), asserts on, full debug info, and
`MONO_HEAP_PROFILE=ON`. `-dev` on the end of the file name.

The dev archive exists because the allocator hooks behind `--heap-report` are a
compile-time decision for the whole program: a block allocated without the
profiler header and freed through the tracking `operator delete` would read
somebody else's memory. The shipped build answers `--heap-report` with "this
build has no allocator hooks". `-O1` rather than `-O0` because a build nobody
can play is a build nobody reports from.

| Platform | Files |
|---|---|
| Linux x86_64 | `atomic-<version>-linux-x86_64.tar.gz`, `atomic-<version>-linux-x86_64-dev.tar.gz`, `atomic-client-<version>-linux-x86_64.AppImage`, `atomic-studio-<version>-linux-x86_64.AppImage`, `atomic-launcher-<version>-linux-x86_64.AppImage` |
| Windows x86_64 | `atomic-<version>-windows-x86_64.zip`, `atomic-<version>-windows-x86_64-dev.zip` |
| macOS arm64 | `atomic-<version>-macos-arm64.tar.gz`, `atomic-<version>-macos-arm64-dev.tar.gz` |
| All | `schema.toml`, `schema-data.toml` |

**The dev flavour is built for a tag only.** It doubles an hour-long matrix.
The `version` job emits a shorter matrix on a manual run, because
`jobs.<id>.if` cannot read the `matrix` context.

Each archive holds the staged directory of all five programs (`client/`,
`studio/`, `server/`, `cdn/`, `launcher/`) beside `LICENSE`,
`THIRD_PARTY_NOTICES.md`, `README.md` and `VERSION`. The trees are not merged
into a shared `bin/lib` layout: `mono_add_program` builds each one to run where
it sits, and flattening them would put several copies of `libSDL3` and
disagreeing `shaders/render/` directories in one place.

**The launcher depends on that layout.** `launcher::StageRoot` takes the parent
of the directory the running binary is in and looks for `<root>/client/client`
beneath it, so the archive root is the stage root. A launcher shipped on its
own would grey out every mode.

`unified_tests` is not shipped. `RUNNING.md` describes it.

### What packaging adds that the build does not

**Example tree and audio.** The build stages examples into
`<build>/assets/examples` and audio into `<build>/assets/audio`, siblings of
the program directories. `examples::DemosLoader` reaches them through a
`Base().parent_path()` fallback that only holds in a build tree, so a `client/`
copied on its own dies with `could not open .../Rings.luau`.
`scripts/package-release.sh` copies the whole example tree (with its
`scripts/` and `worlds/`) into `<program>/examples` for `client`, `studio` and
`server`, the programs that link `Engine::examples`, and `audio/` into
`client` and `studio`. `assets/panels/` is deliberately not copied because
nothing loads it yet.

**Debug information is stripped.** `--strip-debug` (`-S` on macOS; a
PE-aware `x86_64-w64-mingw32-strip` for a cross-built Windows tree), so
`.symtab` survives and a backtrace still names functions. No debug archive is
published: the tag rebuilds the same binary with the same symbols.

### Platform status

Linux is the development platform, and a failure there fails the release.
Windows has a supported build path (`scripts/build-windows.bat`, MSVC on
`windows-2022`) but has never shipped. macOS has never been run:
`CMakeLists.txt` warns when a client configures on Darwin, and
`docs/DEFERRED.md` D00001 is the entry.

Both are `optional: true` in the matrix, so the `build` job is
`continue-on-error` for them: they upload what they build and do not hold up a
Linux release. Drop `optional: true` when a person has run one and it works.

### Building for Windows from Linux

The `windows-cross` preset (`mono.build/Toolchain-mingw64.cmake`) cross-compiles
all five programs with mingw-w64. It is not the supported path, but it is the
way to find out on Linux whether a change compiles and links for Windows.

```sh
# The native glslc first: the cross build's own shaderc cannot run here, so the
# preset's MONO_GLSLC points at the release preset's glslc.
cmake --preset release
cmake --build --preset release --target glslc_exe

cmake --preset windows-cross
cmake --build --preset windows-cross
scripts/package-release.sh .cache/build/windows-cross <version> windows-x86_64 dist
```

Needs `mingw-w64` and `wine`. Wine runs the Windows builds of the build-time
tools `shadercross` and `assetc`; it tests nothing. The toolchain file's header
has the rest. The cross-built binaries are otherwise unexercised: `--version`
under wine is all that has been run.

### AppImage

`client`, `studio` and `launcher` get one, shipped flavour only (a second
flavour would collide on the name). `server` and `cdn` are flag-driven daemons;
they are in the tarball and inside the launcher image. `scripts/package-release.sh`
calls `scripts/package-appimage.sh` on the stripped staged copy, downloading
`appimagetool` unless `APPIMAGETOOL` names one.

No wrapper is needed: `engine::core::Paths::Base()` resolves from the running
executable, each program links with `INSTALL_RPATH "$ORIGIN"`, and `AppRun`
forwards `"$@"`.

**The launcher's image carries the other four.** Each stage goes to
`usr/<program>/`, so `usr` is the stage root, the same shape as the tarball.
That makes it the single-file way to get everything. Stages are never merged
into one `usr/bin`.

Known limit: the mount is read only. `client --profile-snapshot` writes
`frame-graph-snapshot.txt` beside the binary and fails inside an AppImage; use
the tarball for profiling. The studio is unaffected, because its configuration
root is `~/Documents/atomic-game-engine/studio`.

---

## What the workflow does not do

- **It does not run the test suites.** The `release` preset sets
  `MONO_BUILD_TESTS=OFF`; only the `[process]` isolation case of
  `test_testrunner` runs. The pre-push hook runs `just preset=ci check-pr`,
  which skips `[slow]` cases. `just check` adds them, so it is the gate before
  tagging.
- **It does not build on pull requests or branch pushes.** A cold clone
  compiles shaderc, SDL, Luau, QuickJS and Crypto++ from source.
- **It does not sign or notarise anything.** A macOS build, if one ever
  succeeds, is unsigned and Gatekeeper will say so.
