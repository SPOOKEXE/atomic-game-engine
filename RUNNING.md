# Build and run

This is the command index for this checkout: use `just` from the repository root, and use each program's `--help` for its runtime options.

`just` defaults to `preset=dev`. Override it before the recipe, for example
`just preset=release build` or `just preset=server test`. Build products live in
`.cache/build/<preset>/`. Build and test outputs live under `.cache/`;
documentation generators can update checked-in reference files.

Every Linux preset builds with GCC, and configure refuses any other compiler.
Only the fuzz recipes use `clang++-21`, because libFuzzer needs it. If a build
directory picked up another compiler, run `cmake --preset <name> --fresh`.
This does not change the compiler: `MONO_LINKER=auto` uses LLVM LLD on native
GCC/Linux when its probe succeeds, and otherwise uses the system linker. To
use the system linker, configure with
`cmake --preset dev -DMONO_LINKER=system`;
`MONO_LINK_JOBS` (default `2`) limits concurrent first-party Ninja links. Set
`MONO_LLD` to an `ld.lld` path when CMake cannot find it automatically.
See [build performance](docs/BUILD_PERFORMANCE.md) for measurements and comparisons.

## Start here

```sh
just setup
just build
just run --stats
```

For a specific product, use `just client`, `just server`, `just cdn`,
`just studio`, or `just launcher`. The executables are staged at:

| Program | Path |
|---|---|
| Client | `.cache/build/<preset>/client/client` |
| Server | `.cache/build/<preset>/server/server` |
| CDN | `.cache/build/<preset>/cdn/cdn` |
| Studio | `.cache/build/<preset>/studio/studio` |
| Launcher | `.cache/build/<preset>/launcher/launcher` |

## Presets

| Preset | Use |
|---|---|
| `dev` | Default development build. |
| `release` | Optimized shipping-cost build. |
| `profile` | Instrumented profiling build. |
| `server` | Headless server-only build. |
| `cdn` | Content-origin build. |
| `ci` | Strict CI build. |
| `bench` | Optimized benchmark build. |
| `bench-o0`, `bench-o1`, `bench-o2`, `bench-o3` | Benchmark compiler-optimization comparisons. |
| `dist-dev` | Development distribution build. |
| `windows-cross` | Windows cross-build. |

Configure or build a preset directly:

```sh
just preset=release configure
just preset=release build
just preset=server test
```

For `windows-cross` from Linux, install MinGW and Wine with
`sudo apt install mingw-w64 wine`, then add the Rust target with
`rustup target add x86_64-pc-windows-gnu`. The preset uses native `glslc` at
`.cache/build/release/mono.vendor/shaderc/glslc/glslc`; prepare it with
`just preset=release configure` and `just preset=release build glslc_exe`.
Then run `just preset=windows-cross configure` and
`just preset=windows-cross build`.

## Setup and build

| Recipe | Purpose |
|---|---|
| `setup` | Initialize submodules, shader dependencies, and hooks. |
| `install-hooks` | Install the repository pre-push hook. |
| `ccache [size]` | Show compiler-cache status, or set its size. |
| `configure` | Configure the selected preset. |
| `build [target]` | Build the selected preset, optionally one target. |
| `build-profile [target]` | Build and report current-invocation edge, compiler-cache, and wall-clock timing. |
| `build-profile-check` | Test the build-profile tool. |
| `linker-check` | Check automatic, system, and LLD linker selection in a standalone CMake fixture. |
| `compiler-cache-check` | Verify GCC PCH regeneration and compiler-cache hits. |
| `client` | Build the client. |
| `server` | Build the server. |
| `cdn` | Build the content origin. |
| `studio` | Build Studio. |
| `launcher` | Build the launcher. |
| `build-prune [apply]` | Preview reclaimable preset build trees, or delete them with `yes`. Default: `no`. |
| `clean` | Remove build directories. |
| `clean-all` | Remove all derived files, including test caches. |

Examples: `just build engine_ecs`, `just ccache 20G`, and
`just preset=ci build`.

## Test and check

| Recipe | Purpose |
|---|---|
| `test [args...]` | Run suites affected by the current change. |
| `test-all [args...]` | Run every test suite. |
| `test-list` | Show selected test suites without running them. |
| `render-check [filter] [backend]` | Run GPU render tests. Defaults: `[render][gpu]`, `vulkan`. |
| `check` | Run the standard local verification set. |
| `test-architecture` | Check declared dependency layers. |
| `source-check` | Check source-level architecture rules. |
| `deps-check` | Check dependency records. |
| `orphan-check` | Find generated files with no owner. |
| `orphan-clean` | Remove generated orphan files. |
| `shader-check` | Check shader resource bindings. |
| `components` | Generate component documentation. |
| `components-check` | Check component documentation. |
| `bindings` | Generate script-binding documentation. |
| `bindings-check` | Check script-binding documentation. |
| `typecheck` | Type-check staged scripts. |
| `typecheck-editor` | Type-check scripts through the Luau language server. |
| `check-server-is-headless` | Confirm the server stages no presentation assets. |
| `check-cdn-is-bare` | Confirm the CDN build has no unwanted runtime payload. |
| `check-one-node-graph` | Check the node-graph boundary. |
| `format` | Format first-party C++ sources. |
| `format-check` | Check formatting without modifying files. |
| `em-dash-check` | Check first-party prose for em dashes. |

Examples: `just test engine.ecs`, `just test-all`, and
`just preset=ci check`.

## Run products and tools

| Recipe | Purpose |
|---|---|
| `launch [args...]` | Run the launcher. |
| `run [args...]` | Run the client. |
| `demo` | Run the client with statistics and frame graph enabled. |
| `edit [args...]` | Run Studio. |
| `host [args...]` | Run the headless server. |
| `serve [args...]` | Run the content origin. |
| `materials [count]` | Fetch and publish PBR materials. Default: `100`. |
| `demo-check [frames]` | Run staged scripts and worlds headlessly, then validate BMP captures. Default: `120`. |
| `mcp [port] [args...]` | Run Studio's MCP bridge. Defaults: `8738`, `--width 1600`. |
| `luau-lsp` | Build the vendored Luau language server. |
| `unified [args...]` | Run unified client/server arrangements. |

Examples: `just run --stats`, `just host --ticks 300`,
`just serve --root ./content`, and `just mcp 8738 --width 1600`.

## Documentation and schemas

| Recipe | Purpose |
|---|---|
| `docs` | Generate the API and project documentation site. |
| `docs-serve [port]` | Generate and serve the site. Default port: `8000`. |
| `docs-check` | Check generated documentation and public API coverage. |
| `docs-pages` | Generate module pages. |
| `docs-pages-check` | Check generated module pages. |
| `schema-dump` | Write `docs/schema.toml` and `docs/schema-data.toml`. |
| `linecount [args...]` | Run the source line-count tool. |

Documentation is written to `.cache/build/<preset>/docs/`; use
`just docs-serve` to browse it locally.

## Benchmarks and profiling

Benchmark output is printed to the terminal unless a recipe names an output
directory. Use the `bench` preset for comparable measurements.

Select one benchmark by its exact original name with `--benchmark`; it combines
with `--suite`, and missing, empty or unmatched selections fail before any
benchmark runs or report is printed. For example:

```sh
.cache/build/bench/bench/bench_client --suite client.bench.update-cadence.native --benchmark 'Native complex authored cadence, whole harness' --samples 1 --warmup-samples 0
```

Frame Graph's `250 ms` setting is the aggregation window, not a frame duration.
Structural averages can place CPU spans at overlapping mean positions; separate
rows preserve those values without claiming parallel execution. GPU queries use
a separate duration scale because their results can arrive several frames late
and have no correlated CPU start. `submit.scene SDL handoff` identifies time in
the backend submission call separately from engine submission bookkeeping.

| Recipe | Purpose |
|---|---|
| `bench [args...]` | Run selected affected benchmarks. |
| `bench-all [args...]` | Run every benchmark. |
| `bench-accept [args...]` | Accept benchmark baselines. |
| `domain-runtime-memory-bench [build_preset]` | Measure prepared image evaluation and audio mixing heap payload, per-call peaks, and full output checks. Default: `bench`, with heap hooks required. Diagnostic timings are invalid; output stays on stdout. |
| `weather-bench [samples] [build_preset]` | Measure seeded weather, packing, clocks, assemblies, sparse residency, events and the stateful solver without graphics. Defaults: `5`, `bench`; use `bench-o0` for unoptimized costs. |
| `weather-descriptor-bench [samples] [build_preset]` | Measure device-free weather GPU descriptor preparation. Defaults: `5`, `bench`. |
| `weather-residency-bench [samples] [build_preset]` | Measure actual sparse cloud page selection, retained maintenance and teleports without graphics. Defaults: `5`, `bench`. |
| `weather-temporal-bench [samples] [build_preset]` | Measure temporal invalidation, periodic displacement and cloud composition without graphics. Defaults: `5`, `bench`. |
| `weather-shadow-bench [samples] [build_preset]` | Measure shadow caster bounds, cascades, low-sun fades and retained revisions without graphics. Defaults: `5`, `bench`. |
| `weather-effects-bench [samples] [build_preset]` | Measure weather emission and roof/ground visibility against published geometry without graphics. Defaults: `5`, `bench`. |
| `weather-gpu-bench [samples] [build_preset] [width] [height] [shadow_resolution] [scene] [frames]` | Open a GPU device and measure sparse weather generation, reuse, integration and tracked memory. Scenes: `inactive`, `empty`, `simple`, `busy`, `complex`. Defaults: `3`, `bench`, `1920`, `1080`, `256`, `simple`, `256`. Shadow resolution must be a power of two in `32..2048`. |
| `ray-gpu-bench [samples] [build_preset] [width] [height] [family] [scene] [frames] [projection]` | Measure real renderer ray consumers. Families: `all`, `trace`, `volume`, `environment`, `fallback`, `god-rays`, `lens`, `capture`, `mirror`. Scenes: `all`, `inactive`, `empty`, `simple`, `busy`, `complex`. Projections: `perspective`, `orthographic`, `oblique`, one per invocation. Defaults: `3`, `bench`, `1920`, `1080`, `all`, `all`, `256`, `perspective`. Reports print the configured work and output oracles. |
| `weather-fuzz [runs] [compiler]` | Fuzz complete weather checkpoint decoding with AddressSanitizer and UBSan. Defaults: `10000`, `clang++-21`. |
| `procedural-fuzz [runs] [compiler]` | Fuzz portable procedural inputs, PRD1 descriptions, and script state snapshots with libFuzzer, AddressSanitizer, and UBSan. Defaults: `10000`, `clang++-21`. |
| `imagecodec-bench [samples]` | Measure CPU image imports at 1080p and print results to stdout. Default: `5`. |
| `ui-headless-bench [samples]` | Measure ImGui layout and frozen draw signatures without a GPU. Default: `5`. |
| `studio-interface-bench [samples] [build_preset]` | Measure toolbar, Preferences, retained 250 ms Frame Graph, and idle content scans without a window or GPU. Defaults: `5`, `bench`; use `bench-o0` for unoptimized engine costs. |
| `studio-replication-stress-bench [samples] [build_preset]` | Measure the actual Studio link with 20,000 moving Parts, replica delivery, and acknowledgements. Defaults: `3`, `bench`. |
| `client-frame-bench [samples] [build_preset]` | Measure idle content demand, visibility and actual PreRender systems. Defaults: `5`, `bench`; use `bench-o0` for unoptimized costs. |
| `view-graph-lookup-bench [samples] [build_preset]` | Compare pipeline node scans and indexed lookup. Defaults: `5`, `bench`; use `bench-o0` for unoptimized costs. |
| `instance-packing-bench [samples] [build_preset]` | Measure instance conversion and residency with centered and off-center meshes. Defaults: `5`, `bench`; inline conversion is optimized in the benchmark translation unit. |
| `surface-capture-plan-bench [samples] [build_preset]` | Measure CPU planning for 320 visible mirror roots with one bounce and retained buffers. Defaults: `3`, `bench`; no GPU work is included. |
| `frame-plan-bench [samples]` | Measure CPU frame planning, including the default PBR pipeline. Default: `5`. |
| `frame-plan-cache-bench [samples]` | Compare installed-pipeline diagnostic cache misses and hits. Default: `5`. |
| `render-preparation-bench [samples]` | Measure render-preparation work. Default: `5`. |
| `render-recording-bench [samples]` | Measure moving 128x128 views with zero or 1,024 resident meshes. Default: `3`. The zero-mesh row always has no authored compute, even with `MONO_RENDER_RECORDING_COMPUTE=1`; that option applies to the 1,024-mesh rows. |
| `keep-loaded-filter-bench [samples]` | Compare copying 100,000 default Parts with retaining their draw span, plus resident and missing named meshes. Default: `5`. |
| `ordinary-particle-bench [samples]` | Measure ordinary emitter preparation, actual upload bytes, dispatch lanes, and delayed Vulkan pass timings. Default: `1`; waits are outside CPU recording spans. |
| `volume-light-stress [frames]` | Check lighting stress scene, selection costs, and Vulkan fog stress. Default: `120`. |
| `lighting-stress-scene [frames]` | Run the 256 point, 256 spot, 256 fog volume scene. Default: `720`. |
| `data-capture-hook-bench [samples]` | Measure renderer data-capture hooks. Default: `5`. |
| `gpu-texture-atlas-bench [samples]` | Measure GPU texture-atlas work. Default: `1`. |
| `gpu-particle-field-bench [samples]` | Measure GPU particle-field presets. Default: `1`. |
| `check-bench-render-shaders` | Confirm benchmark shader staging. |
| `medium-render-profile [seconds]` | Capture a medium render profile. Default: `15`. |
| `shader-fuzz [runs] [compiler]` | Fuzz cooked shader parsing. Defaults: `10000`, `clang++-21`. |
| `presentation-fuzz [runs] [compiler]` | Fuzz presentation-message parsing. Defaults: `10000`, `clang++-21`. |
| `fuzz-ui [runs] [compiler]` | Fuzz GUI documents, text, bindings, and bake SVGs. Defaults: `1000`, `clang++-21`. |
| `bakegraph-pipeline-set-bench [samples]` | Measure lookup across a large pipeline set. Default: `5`. |
| `portal-exchange-bench [samples]` | Measure portal exchange. Default: `5`. |
| `portal-ambient-bench [samples]` | Measure portal ambient codec work. Default: `5`. |
| `portal-directional-bench [samples]` | Measure portal directional codec work. Default: `5`. |
| `portal-ambient-profile [samples]` | Capture portal ambient profiling. Default: `5`. |
| `script-binding-bench [samples]` | Measure Luau script bindings. Default: `5`. |
| `particle-emit-bench [samples]` | Measure particle emission. Default: `5`. |
| `spatial-hashgrid-bench [samples]` | Measure spatial hash-grid work. Default: `5`. |
| `parallel-grid-bench [samples]` | Measure parallel grid work. Default: `5`. |
| `kinematic-broadphase-bench [samples]` | Measure kinematic broad-phase work. Default: `5`. |
| `persistent-island-bench [samples]` | Measure persistent physics islands. Default: `5`. |
| `persistent-manifold-bench [samples]` | Measure persistent contact manifolds. Default: `5`. |
| `speculative-contact-bench [samples]` | Measure speculative contacts. Default: `5`. |
| `triangle-bvh-bench [samples]` | Measure triangle BVH work. Default: `5`. |
| `terrain-collision-build-bench [samples]` | Measure terrain collision rebuilding. Default: `5`. |
| `continuous-collision-bench [samples]` | Measure continuous collision. Default: `5`. |
| `dynamic-bvh-bench [samples]` | Measure dynamic BVH work. Default: `5`. |
| `simulation-sweep [samples]` | Measure simulation configurations. Default: `5`. |
| `simulation-publish-sweep [samples]` | Measure simulation publication. Default: `3`. |
| `priority-refinement-bench [samples]` | Measure refined multi-row replication publishing. Default: `5`. |
| `recovery-rows-bench [samples]` | Measure recovery-row re-offer work. Default: `5`. |
| `bench-mesh-lod [samples]` | Measure mesh LOD generation. Default: `5`. |

The prototype's 12 ms combined weather GPU budget was met by this one-layer,
1920x1080 Vulkan fixture on an RTX 4090 with one 256x256 shadow cascade.
Concurrent GPU residency affected timing variance; this measurement does not
establish a universal frame guarantee or a quality ranking. The default matches
authored shadow resolution. Compare lower-quality maps with
`just weather-gpu-bench 3 bench 1920 1080 64` and
`just weather-gpu-bench 3 bench 1920 1080 128`.
`MONO_WEATHER_GPU_BENCH_SHADOW_RESOLUTION` sets the same value when running the
benchmark executable directly; invalid values are refused before SDL
initialization. Set `MONO_WEATHER_GPU_BENCH_TICK` to choose its initial
canonical tick, for example `MONO_WEATHER_GPU_BENCH_TICK=128`; the benchmark
prints `initial_tick` and refuses values that leave its eight-tick phase open to
overflow. Its default is `0`.

Profile and soak outputs are retained under `.cache/`, including
`.cache/medium-render-profile/`, `.cache/heap-*.txt`, and
`.cache/build/<preset>/`.

## Runtime and integration checks

| Recipe | Purpose |
|---|---|
| `studio-smoke [game] [out] [meshes]` | Headlessly run Studio and capture images. Defaults: empty game, `.cache/studio-smoke.bmp`, `.cache/studio-meshes.bmp`. |
| `client-smoke` | Headlessly exercise a client UI interaction. |
| `packaged-tornado-audio` | Check packaged TornadoSim audio resolves beside the client. |
| `ui-check` | Capture and compare client UI output. |
| `client-exit` | Check headless and windowed client shutdown. |
| `heap-soak [seconds] [limit] [warmup] [scenes]` | Detect client heap growth. Defaults: `60`, `8192`, `15`, `Rings Particles Meshes Interface StressPhysics`. |
| `studio-resize` | Check Studio window resizing. |
| `studio-viewport-isolation` | Check Studio viewport isolation. |
| `unified-soak [seconds] [limit] [warmup] [entities]` | Detect unified-arrangement heap growth. Defaults: `25`, `8192`, `8`, `64`. |
| `determinism [entities] [ticks]` | Check deterministic server simulation. Defaults: `512`, `200`. |
| `replay-check [entities] [ticks]` | Check server replay behavior. Defaults: `256`, `120`. |
| `stress [label] [clients] [seconds] [port]` | Run a server load test. Defaults: `baseline`, `200`, `45`, `45100`. |
| `stress-motion [label] [clients] [seconds] [port] [window]` | Run motion load testing. Defaults: `motion-baseline`, `1`, `20`, `45200`, `30`. |
| `stress-random-motion [label] [clients] [seconds] [port] [window] [seed] [every]` | Run fixed-seed random-motion load testing. Defaults: `random-motion`, `200`, `45`, `45300`, `30`, `1`, `30`. |

Use positional arguments for recipe parameters: `just heap-soak 300 8192 15
"StressPhysics"` and `just determinism 1024 400`.
