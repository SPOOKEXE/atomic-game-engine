# Build performance

Measured on 2026-10-15 with the `dev` preset, GCC 13.3, LLD 21, full debug
information and 24 available CPU threads. Other processes were running, so these
are local observations rather than portable timing guarantees.

| Work | System linker (GNU BFD) | LLD, four workers |
|---|---:|---:|
| Full dev relink and resulting asset staging | 52.807 s | 10.667 s |
| Studio test executable, isolated relink | 9.70 s | 1.74 s |

The full comparison used the same existing objects and two concurrent link
jobs in both runs. Neither run invoked the compiler. It includes shader
translation and asset baking triggered by relinking their tools. An earlier
LLD pass also rebuilt the Studio archive and took 13.090 s.

This measures relinking, not a clean source build. PCH and unity settings stay
as they were. A null dev build took about 0.13 s. Rebuilding the same cached
Studio source twice took 0.09 s each, with direct cache hits on both calls.
Historical cache misses therefore did not establish a current cache defect.

`MONO_LINKER=auto` now probes LLD for native GCC/Linux builds and falls back to
the system linker. `MONO_LINKER=system` disables the choice; `MONO_LINKER=lld`
requires it. Compiler options and generated objects are unaffected. Four LLD
workers reduced CPU work on the Studio test link from 11.09 to 5.54 CPU-seconds
versus unrestricted LLD, at the cost of 1.74 s rather than 1.32 s elapsed time.

That large link peaked around 6.4 GB RSS with LLD versus 4.8 GB with BFD.
`MONO_LINK_JOBS=2` bounds simultaneous first-party Ninja links. Compiles can
still overlap links, so this is not a whole-build memory bound. Caller-supplied
job pools are preserved.

Use the existing profiler to repeat the comparison:

```sh
cmake --preset dev -DMONO_LINKER=system
just build-profile
cmake --preset dev -DMONO_LINKER=auto
just build-profile
```

Each linker change forces relinking. The profiler now reports only the current
build command's new Ninja edges and ccache events. Its wall time excludes any
initial configure performed by the recipe. Standalone inspection remains
historical:

```sh
python3 scripts/build-profile.py --build-dir .cache/build/dev
just build-profile-check
just linker-check
```

The original linker change also passed the Linux `server` and strict `ci`
builds, plus all 43 server CTest checks.

The platform fixes make CMake invoke `assetc` by target name so Wine runs it,
build AccessKit for `x86_64-pc-windows-gnu` into Rust's target-specific artifact
path and link its reported native libraries, and rename the graph lookup to
`FindResourceDesc` to avoid the Win32 macro. Apple AccessKit links AppKit,
Foundation and objc from the Rust static-library probe.

After these fixes, the full Linux `dev` build passed. CTest passed the graph,
game, examples and architecture suites; headless render (830 cases), client
(322) and Studio (723) checks also passed. Formatting, prose checks, the 10
build-profiler tests and the linker fixture passed.

The full `windows-cross` build passed with MinGW GCC 13 posix, including all
products, tools and staging; its null build took 0.083 s. Under Wine 11,
`server`, `cdn` and `launcher --version` passed, as did a three-tick server
run. Unmodified `client --version` and `studio --version` failed in the built-in
`dbghelp` DWARF `dwarf2_locate_cu` parser. Copies with only debug sections
removed passed both commands; the original artifacts and build options were
unchanged. No GPU runtime was tested.

Rust-only AccessKit static-library probes passed for Windows GNU, Windows MSVC
and macOS ARM64 on Linux. The macOS probe used no Apple SDK and did not link a
native application. In [the GitHub Actions run][platform-run], the Windows and
macOS release jobs failed at checkout before compilation:
upstream did not contain pinned ngtcp2 commit
`751c3402549f5918deb253db5b8abfd1055e8c82`. The workflow reported overall
success because those platform jobs are optional. `.gitmodules` was unchanged.

[platform-run]: https://github.com/SPOOKEXE/atomic-game-engine/actions/runs/38720005737

The driver search option is applied only while linking, because GCC also uses
[`-B` to select compiler tools and search paths](https://gcc.gnu.org/onlinedocs/gcc/Directory-Options.html).
Link concurrency uses [CMake's Ninja job pools](https://cmake.org/cmake/help/v3.28/prop_gbl/JOB_POOLS.html).
