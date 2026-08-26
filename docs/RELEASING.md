# Releasing

How a version number is chosen, where it is written down, and what happens when
a tag is pushed.

---

## The version number

`v<major>.<minor>.<patch>`, and every published tag has all three parts.
`v0.18` is a branch name, not a release; `v0.18.0` is a release.

While the major is `0` the engine is pre-release and every artifact is marked as
such on GitHub. What the three numbers mean here:

| Part | Bumped when |
|---|---|
| **major** | `0` until the engine is something a person can ship a game on. After that, a break in the scripting API, the save format or the network protocol. |
| **minor** | A milestone lands. This is what `ROADMAP.md` counts, and what the working branch is named after. |
| **patch** | A fix, a packaging change or a build repair on top of a released minor. No new milestone work. |

Pre-1.0 there is no compatibility promise: the `.agame` save format, the wire
protocol and the Luau surface all still move, and a minor bump is allowed to
break any of them. `ROADMAP.md` is the record of what changed.

The milestone headings in `ROADMAP.md` are development labels and do not have to
line up with a published tag. `v0.18` in the roadmap is a body of work; `v0.18.0`
is the first build cut from it, and `v0.18.1` is the next one, with no roadmap
heading of its own.

---

## Where the version lives

**`VERSION`, in the repository root.** One line, `major.minor.patch`, and it is
the only place the number is written down. Everything else derives from it:

| Derives | How |
|---|---|
| `project(atomic VERSION ...)` | `CMakeLists.txt` reads `VERSION` before `project()`, and refuses a value that is not `major.minor.patch` |
| `engine::core::Version()` | `MONO_VERSION`, set from `PROJECT_VERSION` on `mono.engine/core/src/Version.cpp` alone |
| `<program> --version` | `Arguments::VersionLine()`, so all seventeen programs print the same shape |
| The artifact names | the `version` job reads `VERSION` |
| The `VERSION` file inside each archive | `scripts/package-release.sh` writes it |

Editing `VERSION` re-runs the configure - `CMAKE_CONFIGURE_DEPENDS` names it -
and rebuilds one translation unit. It is not a whole-engine recompile, so there
is no reason to put off bumping it.

Two things do *not* derive and have to be kept in step by hand:

- The git tag - `vx.y.z`, on the commit carrying the matching `VERSION`.
- `README.md`, which names the current version for people who are reading rather
  than building. Prose, not a source of truth.

The `version` job in `.github/workflows/build.yml` compares the tag against
`VERSION` and stops the run if they differ, before any platform starts
compiling. A tag pointing at a tree that still says `0.4.0` would otherwise
produce an hour of binaries reporting the wrong version.

```console
$ client --version
client 0.18.0
```

The program's own name, then the number. The name is there because seventeen
programs share one version and a pasted line has to say which one wrote it.

---

## Cutting a release

```sh
# 1. Bump the declared version.
echo 0.18.1 > VERSION
#    and README.md:  Current Version: **v0.18.1**

# 2. Check it the way CI will, from a clean tree.
#    `bun install` (or npm install) first: without node_modules/.bin/tsc the
#    build skips the two TypeScript example scenes and says so in one STATUS
#    line nobody reads.
bun install
just check

# 3. Commit, tag, push.
git commit -am "v0.18.1"
git tag v0.18.1
git push origin <branch> --follow-tags
```

The tag is what triggers the build. Pushing the commit alone builds nothing
unless the branch is `main`.

To see what the workflow produces without publishing anything, run it from the
Actions tab - a `workflow_dispatch` run builds and uploads artifacts and never
creates a release.

---

## What comes out

Two flavours of the same tree, per platform.

**The shipped one**, from the `release` preset: `RelWithDebInfo`, first-party
targets at `-O3` (`MONO_OPTIMISE=ON`), no heap profiler, tests off, Tracy
compiled in. This is what a player downloads.

**The dev one**, from the `dist-dev` preset: the same code at `-O1`, frame
pointer kept, and `MONO_HEAP_PROFILE=ON`. `-dev` on the end of the file name.

The dev archive exists because the diagnostics that answer a bug report cannot
be switched on at runtime. A block allocated with no profiler header and freed
through the tracking `operator delete` would read four words of somebody else's
memory, so the allocator hooks are a compile-time decision for the whole
program - which is why the shipped build answers `--heap-report` with "this
build has no allocator hooks" rather than with a report. Before this there was
one answer to "it leaks": build the engine yourself. `-O1` rather than `-O0`
because a build nobody can play is a build nobody reports from, and rather than
`-O3` because a stack a report can name functions from is worth more here than
the last of the speed.

| Platform | Files |
|---|---|
| Linux x86_64 | `atomic-<version>-linux-x86_64.tar.gz`, `atomic-<version>-linux-x86_64-dev.tar.gz`, `atomic-client-<version>-linux-x86_64.AppImage`, `atomic-studio-<version>-linux-x86_64.AppImage`, `atomic-launcher-<version>-linux-x86_64.AppImage` |
| Windows x86_64 | `atomic-<version>-windows-x86_64.zip`, `atomic-<version>-windows-x86_64-dev.zip` |
| macOS arm64 | `atomic-<version>-macos-arm64.tar.gz`, `atomic-<version>-macos-arm64-dev.tar.gz` |

**AppImages are built for the shipped flavour only.** An AppImage is named for
its program and its version and nothing else, so a second flavour would collide
with the first or need a naming scheme invented for a download nobody
double-clicks. The dev flavour ships as the archive, which is what somebody
chasing a report unpacks anyway.

**The dev flavour is built for a tag and not for a branch push.** It doubles an
hour-long matrix, and on a branch push nothing publishes it. The `version` job
decides that by emitting a shorter matrix, because `jobs.<id>.if` cannot read
the `matrix` context.

Each archive holds the staged directory of all five programs - `client/`,
`studio/`, `server/`, `cdn/`, `launcher/` - beside `LICENSE`,
`THIRD_PARTY_NOTICES.md`, `README.md` and a `VERSION` file. The trees are not
merged into a shared `bin/lib` layout on purpose: `mono_add_program` builds each
one to be runnable where it sits, and flattening them would put five copies of
`libSDL3` and four disagreeing `shaders/render/` directories in one place.

**The launcher depends on that layout being exactly this.**
`launcher::StageRoot` takes the parent of the directory the running binary is
in and looks for `<root>/client/client` beneath it, so the archive root is the
stage root and an unpacked tarball is a working front door. A launcher shipped
on its own would start, find nothing beside it, and grey out every mode.

`unified_tests` is not shipped. It is a diagnostic harness, and
`RUNNING.md` is where it is described.

### Two things packaging adds that the build does not

**The demo scenes are copied in.** This is the one place the build's layout and
the runtime's disagree. Shaders stage into each program's own directory and
`Paths::Assets()` defaults to that directory, so those line up. The example
scenes stage into `<build>/assets/examples`, a *sibling* of every program
directory, and `examples::ExamplePath` reaches them through a
`Base().parent_path()` fallback whose own comment calls it a mismatch. That
fallback holds in a build tree and nowhere else: a `client/` directory copied
somewhere on its own starts, opens Vulkan, and dies with `could not open
.../Rings.luau`.

So `scripts/package-release.sh` copies them into `<program>/examples`, which is
where `ExamplePath` looks *first*. Into `client`, `studio` and `server` only -
the three that link `Engine::examples`. `cdn` does not, and a copy of the scenes
in its directory would be a claim that it can run one.

`assets/panels/` is staged beside them and is deliberately not copied, because
nothing loads it yet. Add it to the script when something does.

**Debug information is stripped.** `--strip-debug`, so `.symtab` survives and a
backtrace still names functions. It is not a small saving: `client` is 478 MB
out of the `release` preset and 27 MB after, and the Linux tarball goes from 519
MB to 40 MB. Nothing is published in its place - the tag rebuilds the same
binary with the same symbols, and a debug archive nobody downloads is half a
gigabyte per platform per release.

### Platform status

Linux is the platform the engine is developed on, and a failure there fails the
release. Windows compiles and has a supported build path
(`scripts/build-windows.bat`) but has never shipped. macOS has never been run at
all - `CMakeLists.txt` warns when a client configures on Darwin and
`docs/DEFERRED.md` D00001 is the entry.

So both are `continue-on-error` in the workflow: they upload whatever they
manage to build, and neither holds up a Linux release. When one of them has been
run by a person and works, drop its `optional: true` from the matrix.

### Building for Windows without a Windows machine

`mono.build/Toolchain-mingw64.cmake` cross-compiles all five programs to
Windows x86_64 from Linux with mingw-w64. It is not the supported path -
`scripts/build-windows.bat` with MSVC is, and the `windows-2022` runner is what
publishes - but it is the only way to find out on a Linux machine whether a
change compiles and links for Windows at all.

```sh
# The native glslc first. A cross build's own shaderc is built for the target
# and cannot run on the machine doing the building, so the preset borrows this
# one; it is where the preset's default MONO_GLSLC points.
cmake --preset release
cmake --build --preset release --target glslc_exe

cmake --preset windows-cross
cmake --build --preset windows-cross
scripts/package-release.sh .cache/build/windows-cross <version> windows-x86_64 dist
```

Needs `mingw-w64` and `wine` installed. Wine is not there to test anything: it
runs `shadercross`, which is a build-time tool of ours that a cross build
produces as a `.exe`. See the toolchain file's own header for the rest.

**Two portability bugs were found by trying this, and both were real on MSVC's
side of the fence too.** `constinit std::mutex` in `HeapProfile.cpp`, which
compiles under libstdc++ on glibc only because `pthread_mutex_t` happens to have
a constant initialiser, and which was a latent violation of that file's own
"nothing here may have a constructor" invariant. And asio's `AcceptEx`,
requested through `#pragma comment(lib, "mswsock")` - an MSVC extension that GCC
parses and ignores - which the MSVC link had only ever found by luck of the
compiler.

The binaries are otherwise unexercised: `--version` on each is all that has been
run, under wine. Nothing has opened a window on Windows.

### AppImage

Three of the five get one: `client`, `studio` and `launcher`. `server` and `cdn`
are daemons driven entirely by command-line flags, and an AppImage of a daemon
is a tarball with a mount step in front of it. They are in the tarball, and they
are inside the launcher image described below.

They work without a wrapper because `engine::core::Paths::Base()` resolves from
the running executable rather than the working directory, and each program is
linked with `INSTALL_RPATH "$ORIGIN"` - so the staged tree runs unchanged from a
squashfs mount at a path nothing could have predicted. Command-line arguments
reach the program in every case, because `AppRun` forwards `"$@"`.

**The launcher's image carries the other four, and has to.** Its whole job is
starting programs beside it, so an image holding only the launcher would open a
window with every mode greyed out. Inside the AppDir each stage goes to
`usr/<program>/`, which puts the launcher at `usr/launcher/launcher` and makes
`usr` the stage root its siblings are found under - the same shape as the
tarball, for the same reason. That makes it the single-file way to get
everything: about 36 MB against 14 MB for the client alone.

Note that the stages are never merged into one `usr/bin`. Four of them carry
their own `libSDL3.so` and their own `shaders/render/`, compiled from different
modules, and flattening them would leave one of each.

One known limit: the mount is read only. `client --profile-snapshot` writes
`frame-graph-snapshot.txt` beside the binary and that write fails inside an
AppImage; use the tarball for profiling. The studio is unaffected, because its
configuration root is `~/Documents/atomic-game-engine/studio` and only the
pre-v0.15 fallback files it still *reads* live beside the binary.

---

## What the workflow does not do

- **It does not run the tests.** The `release` preset sets
  `MONO_BUILD_TESTS=OFF`, and a second full configure of the `ci` preset would
  roughly double an already hour-long job. `.githooks/pre-push` builds `ci`
  before anything is pushed, and `just check` is the gate named in
  `docs/CODE_QUALITY.md`. Run it before tagging.
- **It does not build on pull requests.** Same reason: a cold clone compiles
  shaderc, SDL, Luau, QuickJS and Crypto++ from source. Cheap per-pull-request
  checks belong in a workflow that does not configure a client.
- **It does not cache.** Nothing is reused between runs, so every build is from
  scratch and every build is reproducible from the tag alone. If release builds
  become frequent enough for that to hurt, `ccache` on the two Unix platforms is
  the first thing to add.
- **It does not sign or notarise anything.** A macOS build, if one ever
  succeeds, is unsigned and Gatekeeper will say so.
