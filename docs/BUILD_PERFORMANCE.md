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

ngtcp2 is pinned to upstream `main` commit
`48228939787d4b5bea94c7795b8c7fe0781d918e` from 2026-10-11, newer than stable
tag `v1.25.0`. Linux `dev`, `server` and strict `ci` builds passed with this
pin. Strict headless suites passed 723/723, and server CTest passed 43/43.

The [first native CI run][cold-platform-run] had cold compiler caches. Its
largest cumulative edge group was shaderc: 2,285 seconds on Windows and 1,295
seconds on macOS. Luau
followed at 1,084 and 434 seconds. These are overlapping work totals, not build
wall times. The AccessKit Cargo step took 141 seconds on Windows and 34 seconds
on macOS. Windows completed; the macOS log stops at the compile failure below.
The existing CI compiler cache was saved for subsequent runs. The [next Windows
run][cache-run] had 1,175 hits and two misses for changed translation units, after
subtracting the cold run's counts from the retained stats log. Its fresh Ninja
log spanned 135 seconds versus 1,483 seconds cold. This demonstrates existing
cache reuse, not a separate speedup introduced by these changes. No linker
speedup is claimed for either native platform.

The platform fixes make CMake invoke `assetc` by target name so Wine runs it,
build AccessKit for `x86_64-pc-windows-gnu` into Rust's target-specific artifact
path and link its reported native libraries, and rename the graph lookup to
`FindResourceDesc` to avoid the Win32 macro. Apple AccessKit links AppKit,
Foundation and objc from the Rust static-library probe.

The full `windows-cross` build passed with MinGW GCC 13 posix, including all
products, tools and staging; its null build took 0.083 s. Under Wine 11,
`server`, `cdn` and `launcher --version` passed, as did a three-tick server
run. Unmodified `client --version` and `studio --version` failed in the built-in
`dbghelp` DWARF `dwarf2_locate_cu` parser. Copies with only debug sections
removed passed both commands; the original artifacts and build options were
unchanged. No GPU runtime was tested.

Rust-only AccessKit static-library probes passed for Windows GNU, Windows MSVC
and macOS ARM64 on Linux. The macOS probe used no Apple SDK and did not link a
native application. The [final native platform run][platform-run] passed the
Windows and macOS release builds and packaging. Both passed the 10 build-profiler tests,
and macOS passed the floating-point parser suite using its own standard library.
The macOS fixes route Studio number parsing through `core::FromChars` and name
an image-composer lambda capture explicitly for Apple Clang. The portable parser
now rejects overflow and zero underflow while preserving valid subnormals.

After those fixes, Linux `dev`, `server` and strict `ci` builds passed again.
The complete core CTest check passed; focused checks passed 203 parser
assertions, 146 UI-authoring assertions and 354 image-composer assertions.
Formatting, prose checks, architecture and source checks, the linker fixture,
and the build-profiler tests passed. No GPU or macOS application runtime was
tested.

[platform-run]: https://github.com/SPOOKEXE/atomic-game-engine/actions/runs/38848123921
[cold-platform-run]: https://github.com/SPOOKEXE/atomic-game-engine/actions/runs/38725290645
[cache-run]: https://github.com/SPOOKEXE/atomic-game-engine/actions/runs/38847518322

The driver search option is applied only while linking, because GCC also uses
[`-B` to select compiler tools and search paths](https://gcc.gnu.org/onlinedocs/gcc/Directory-Options.html).
Link concurrency uses [CMake's Ninja job pools](https://cmake.org/cmake/help/v3.28/prop_gbl/JOB_POOLS.html).

## Native Cargo cache cold and warm runs

At exact source head `50a9c492`, native cold run
[39014980981](https://github.com/SPOOKEXE/atomic-game-engine/actions/runs/39014980981)
and warm run
[39016026769](https://github.com/SPOOKEXE/atomic-game-engine/actions/runs/39016026769)
passed on Linux x86_64, Windows x86_64, and macOS ARM64. Warm Cargo had an exact
cache hit on every platform, ran freshness checks, and compiled only `accesskit-c`.

| Platform | AccessKit Ninja edge, cold/warm (s) | Ninja span, cold/warm (s) | Whole job, cold/warm (s) | Rust crate compilations, cold/warm |
|---|---:|---:|---:|---:|
| Linux x86_64 | 143.499 / 31.419 s | 431.090 / 53.558 s | 637 / 287 s | 90 / 1 |
| Windows x86_64 | 111.140 / 30.643 s | 654.638 / 100.025 s | 976 / 489 s | 23 / 1 |
| macOS ARM64 | 54.595 / 19.305 s | 360.945 / 53.975 s | 638 / 345 s | 13 / 1 |

The whole-job change cannot be attributed to Cargo alone. The source changes
include a public core header. Cold C/C++ cache misses were 87 / 87 / 293, and
warm misses were 0 / 0 / 206, ordered Linux, Windows, macOS. The warm macOS misses
span release Build and process-check. These misses led to the PCH investigation
below. They do not establish broad engine speed gains or statistical performance
acceptance. Shared-host timing remains unaccepted.

## Native Clang PCH cache reuse

At exact code head `da132daf75bc920a01a196145414ef3df405819e`, strict pre-push
passed 774 suites with no skips or failures. Two native runs
[39017676705](https://github.com/SPOOKEXE/atomic-game-engine/actions/runs/39017676705)
and [39018193934](https://github.com/SPOOKEXE/atomic-game-engine/actions/runs/39018193934)
passed on Linux x86_64, Windows x86_64, and macOS ARM64. After 248 first-run
macOS cache misses, the next run had 1,435 hits / 1,435 calls / 0 misses. The
earlier `50a9c492` warm run had 206 repeated macOS C/C++ misses. This reproduces
cache reuse on a fresh checkout.

This is evidence of cache reuse, not a whole-build speedup. macOS Ninja spans
were 80.131 s on the first fixed run and 58.288 s on the second, compared with
53.975 s in the earlier warm run. SDL compile-edge sums fell from 77.682 s to
54.658 s, but overlap and do not represent CPU or critical-path time. Actionlint
1.7.12 validated the later macOS-only condition. No further native run was
performed after that workflow-only edit. Product build flags were unchanged.
Shared-host timing remains unaccepted.

## Remaining hot build edges

In the second fixed native run at `da132daf`, these were the largest observed
edges after Cargo cache reuse:

| Platform | AccessKit Rust edge | Other largest observed edges |
|---|---:|---|
| Linux x86_64 | 35.268 s | `weather-clouds.comp.spv` 10.534 s; `libSPIRV-Tools-opt.a` archive 9.416 s |
| macOS ARM64 | 20.181 s | SPIRV-Tools-opt archive 17.783 s; `weather-clouds` SPIR-V 14.079 s |
| Windows x86_64 | 30.809 s | `weather-clouds` SPIR-V 22.054 s; `client.exe` link 11.695 s |

These edges overlap and cannot be summed into build wall time or critical path.
This is one observational run, not a statistical speed claim. Ccache hits do not
eliminate archive assembly or shader translation.

## LLD text section placement measurements

LLD links keep hot, unlikely and startup text inputs in their matching output
sections with [`-z keep-text-section-prefix`](https://lld.llvm.org/ELF/linker_script.html).
This leaves those groups together instead of interleaving them by object. It
does not enable garbage collection or identical-code folding, and removes no
code. One local GCC 13.3 / LLD 21 server-alone Rings workload, measured for 15
seconds of serial compute, used the same objects and a no-GC, one-flag control:
final executable mapping fell from 15,848 KiB to 13,956 KiB, and total RSS from
34,860 KiB to 32,968 KiB. A same-object joined ServerRings paired GDB check
using the same software-Vulkan viewer at 960x540 and a 60 Hz cap for 15 seconds
of serial compute measured executable RSS at 17,176 KiB to 15,180 KiB and total
RSS at 38,092 KiB to 36,084 KiB. Both are local diagnostic memory observations,
with no timing improvement or hosted CI result claimed.
