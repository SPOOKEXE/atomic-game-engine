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

Everything is built from the `release` preset: `RelWithDebInfo`, first-party
targets optimised (`MONO_OPTIMISE=ON`), tests off, Tracy compiled in.

| Platform | Files |
|---|---|
| Linux x86_64 | `atomic-<version>-linux-x86_64.tar.gz`, `atomic-client-<version>-linux-x86_64.AppImage`, `atomic-studio-<version>-linux-x86_64.AppImage` |
| Windows x86_64 | `atomic-<version>-windows-x86_64.zip` |
| macOS arm64 | `atomic-<version>-macos-arm64.tar.gz` |

Each archive holds the staged directory of all four programs - `client/`,
`studio/`, `server/`, `cdn/` - beside `LICENSE`, `THIRD_PARTY_NOTICES.md`,
`README.md` and a `VERSION` file. The trees are not merged into a shared
`bin/lib` layout on purpose: `mono_add_program` builds each one to be runnable
where it sits, and flattening them would put four copies of `libSDL3` and four
disagreeing `shaders/render/` directories in one place.

`unified_server_client` is not shipped. It is a diagnostic harness, and
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

### AppImage

Only `client` and `studio` get one. `server` and `cdn` are daemons, and an
AppImage of a daemon is a tarball with a mount step in front of it.

They work without a wrapper because `engine::core::Paths::Base()` resolves from
the running executable rather than the working directory, and each program is
linked with `INSTALL_RPATH "$ORIGIN"` - so the staged tree runs unchanged from a
squashfs mount at a path nothing could have predicted.

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
