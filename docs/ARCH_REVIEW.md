# Architecture review, v0.19

**What a full pass over the tree found, and what was done about it.** This is a
findings register, not a reference: `docs/CODE_ARCH.md` is where the rules live,
and anything here that gets fixed should leave this file and appear there or in
the relevant `AGENTS.md`.

Every finding carries a `file:line`. Findings marked **[verified]** were
reproduced directly - the file was read, the number measured, or the check run.
Findings marked **[reported]** came from the survey pass and are recorded with
their citation but were not independently reproduced. Do not act on a
**[reported]** finding without checking it first; root `AGENTS.md` is explicit
that a plausible explanation which happens to be wrong costs more than none.

---

## A · Correctness

### A1. `core::FrameGraph`'s owner and dropped counter were racy **[verified, fixed]**

`State` is a single `static` instance (`core/src/FrameGraph.cpp:145`). Its
`std::thread::id Owner` was written by the recording thread and read by every
job worker, and `DroppedThisFrame` was incremented concurrently by workers.
`ecs::Store` hit the identical problem and made its owner atomic, with the
argument written at `ecs/include/engine/ecs/Store.hpp:125-135`; `FrameGraph` did
not follow. Reached in a real frame by any `ENGINE_PROFILE` inside an
`EachParallel` body.

Both are now `std::atomic`, relaxed on every access. `core` and `ecs` suites
pass.

### A2. `graph::NodeCatalogue` hands out pointers the lock does not protect **[verified, open]**

`Find` returns `&table.Specs[index]` and `All` returns a span into `Specs`, both
after releasing the mutex (`graph/src/PipelineCatalogue.cpp:151-163`). A later
`Register` does `push_back` **and** `std::sort`, so the pointer can dangle or
silently refer to a different kind.

The lifetime contract is documented for `All`
(`PipelineCatalogue.hpp:283`, "Valid until the next `Register`") and not for
`Find`. That contract is only safe single-threaded, and the mutex makes the type
look stronger than it is.

**Not urgent, and the reason matters.** Registration is init-only in production:
`mono.client/src/Scene.cpp:1625` and `mono.studio/src/RenderPipelineGraph.cpp:238`
call `RegisterRenderNodeKinds` once each, and nothing else registers outside
tests. The hazard is the documented intent that "a game may correct a built-in"
(`PipelineCatalogue.hpp:263`), which would put a `Register` after a `Find`.

**Do:** state the same lifetime contract on `Find` that `All` carries, and say
in it that the mutex does not extend past the return. If runtime registration
ever becomes real, the table has to stop being a sorted vector first.

### A3. Unbounded microtask loop in the JavaScript host **[reported]**

`script/src/JavaScriptRuntime.cpp:226-235` drains `JS_ExecutePendingJob` without
a bound, so a self-re-enqueuing microtask hangs the host inside one tick. The
step budget does not catch it because both interrupt handlers zero their counter
on trip (`JavaScriptRuntime.cpp:38`, `LuauRuntime.cpp:91`), giving every job a
fresh budget. The same reset makes `StepsTaken()` report `0` for the script that
used the most, which is a second bug in the same three lines.

### A4. `scene::InputState` mouse behaviour is last-write-wins across worlds **[reported]**

`mono.client/include/client/Client.hpp:826-832` keeps a copy of
`scene::InputState::Behaviour` and `MouseIconEnabled`. It is written once per
world per frame (`Client.cpp:1490-1491`), so with `--worlds 2` or `--connect` a
script in world 0 setting `MouseBehavior` is overwritten by whichever world was
entered last. **This is root `AGENTS.md` rule 2 exactly**: the ECS owns the fact,
the copy cannot represent "per world", and therefore cannot be right.

### A5. `CommandQueue::Post`'s result is discarded at every call site **[reported]**

`mono.client/src/Sounds.cpp` discards the return at all fifteen sites while
recording the state as landed. A dropped `Open` burst is a permanently silent
voice that is never repaired and never logged.

---

## B · Documented invariants that were false

The pattern is the finding. Five module documents asserted invariants the code
had outgrown, and each was stated absolutely enough that a reader would have
trusted it.

| Where | Claimed | Actually | Status |
|---|---|---|---|
| `game/AGENTS.md:137`, `game/CMakeLists.txt:77` | "Nothing here opens a file or a socket" | `Game.hpp` takes a `std::filesystem::path` in six places; `Game.cpp:17` includes `<fstream>` | **fixed** - narrowed to "nothing here fetches", which is the load-bearing half |
| `scene/AGENTS.md:600` | "No systems" | `RegisterGravitySystem` and `RegisterOwnershipSystem` register two | **fixed** - narrowed to "no simulation systems", with the line between them stated |
| `render/AGENTS.md:5` | "No SDL GPU type in a public header" | true of `Renderer.hpp`; false for `MeshTable.hpp:22-23` and `TextureTable.hpp` | **fixed** - split into the absolute claim (no SDL *header*) and the two named exceptions |
| `render/AGENTS.md:194-232` | six named passes, `PassRecorder`, `PassOrder`, `render::Pass`, `graph::StandardPipeline`, `Pipeline::Validate` | **none of the five names exists.** The render-graph system arrived and replaced them; `render/tests/Passes.cpp:3` says so | **fixed** - rewritten to describe `graph::NodeRunner` and `render::GraphRunner` |
| `render/AGENTS.md:309` | geometry lives in `Primitives.hpp`, checked by `tests/Primitives.cpp` | neither exists; the shapes are inline in `src/Renderer.cpp` and `src/InterfacePass.cpp`, unchecked | **fixed** - recorded as a gap |
| root `AGENTS.md:77-80` vs `docs/CODE_QUALITY.md:60-62` | one said the build enforces layer heights, the other said check by hand | the second was right | **fixed** - the build now enforces it, and both say so |

**Still open and [reported]:**

- `parallel/AGENTS.md` claims `process/` and `ipc/` do not exist yet. They have
  since v0.2, and the file misidentifies the join's serialisation point as
  `Jobs.cpp:103` when it is `:219`.
- `launcher/AGENTS.md` says "do not move a decision into `Interface.cpp`". Six
  are already there: `Interface.cpp:42-46`, `:186-197`, `:383-412`, `:543`.
- `mono.client/AGENTS.md:5-9` says the directory holds attachments and the test
  is whether a second program would want them. `mono.studio` already includes
  four of its headers.
- `script/AGENTS.md:10` carries one stale claim.
- `mono.server/AGENTS.md` is stale on rewind and silent on both networking and
  content.
- ~~`docs/QUIC.md:55` calls `net` L2. It is L11.~~ Fixed at v0.19, with the
  same line gaining `Vendor::ngtcp2`.
- `README.md:34` says "four rules"; there are six.
- `docgen/pages/Modules.md` lists ten of the engine's twenty-nine modules.

---

## C · Things in the wrong place

### C1. `nodegraph` is in the wrong monorepo member **[verified]**

It is `client`-tier, sits in `mono.engine/`, links nothing first-party, uses
`std::thread`, `std::atomic`, `std::condition_variable` and `imgui.h`, and
**exactly one thing in the repository links it: `studio`**. It is an editor
widget library.

**Do:** move it to `mono.studio/nodegraph/`. It is a rename plus a row in
`expected_graph.json`, and it takes an imgui-carrying target out of
`mono.engine/`.

### C2. `Renderer::RenderView` is 5,485 lines **[verified]**

`render/src/Renderer.cpp:8032-13516`, inside a 13,517-line translation unit -
two fifths of the module in one function. It holds its node handlers as lambdas
and calls `SDL_BeginGPURenderPass` inline in eighteen places, which is what makes
those passes invisible to the render graph.

**It is simultaneously the module's build cost and its architecture problem**,
because one enormous translation unit cannot be split across cores. Splitting
`RenderView` by node family, into files that each implement `GraphRunner` for
one family, fixes both with one change. That is the argument for doing it as one
change rather than two.

### C3. `mono.client` uses three modules it does not declare **[reported]**

`Engine::assets`, `Engine::graph` and `Engine::script` are included from
**public** headers - `client/Client.hpp:5`, `:14`, `:30`; `client/Scene.hpp:26`;
`client/Replicated.hpp:78` - and appear nowhere in
`mono.client/CMakeLists.txt`'s `DEPS`. They arrive transitively today. The same
file already documents this exact failure happening once before, at
`CMakeLists.txt:40-45`.

**This is the highest-value small fix in the list**: three lines, and it stops a
future break landing in `mono.client` rather than where the change was made.

### C4. `mono.client` is a de-facto shared library **[reported]**

`mono.studio/include/studio/Editor.hpp:71-73` and `src/Editor.cpp:38` include
four `client/` headers. Either the charter in `mono.client/AGENTS.md` is wrong,
or those four belong in `mono.engine/`. `EditableMeshes.cpp:67-151` is the
clearest case: a pure `EditableMesh -> MeshData` transform with no device in it.

### C5. `script` is 32,981 lines and should be four modules **[reported]**

The survey bucketed all 88 files and the totals sum exactly. It is **not** an
object model problem - the object model is 1,465 lines, because `ecs::Classes`
already owns it. It is binding glue: **17,342 lines, 52.6%**, split Luau 9,954
and JavaScript 7,388.

The seam already exists: `ScriptCall.hpp` and `ServiceSurface.hpp` are a VM-free
port with two adapters behind it, and there are **zero VM types in any `script`
public header** (verified by the survey: `grep -c lua_State
include/engine/script/Runtime.hpp` is 0).

Proposed split: `script` (L9 shared, ~15.6k, **no vendor at all**),
`script.luau` (L10, ~11k), `script.js` (L10, ~8.2k), `script.host` (L11, ~200
lines of factory). That also deletes the bespoke 60-line CMake closure walk
`mono_check_script_vm_naming`, which currently enforces the rule by filename.

### C6. Smaller misplacements **[reported]**

- `scene/src/SurfaceCameras.cpp` is 4,108 lines of portal and mirror maths, 36%
  of `scene`'s code budget. Lifts out cleanly as its own L7 module.
- `spatial::CollisionGroups` is a naming registry and a policy matrix, tagged
  `@tier L2` in a module that is L6.
- `ecs` carries a second bounded context - `Classes`, `Instance`, `Attributes`,
  the Roblox object model - that its `AGENTS.md` never acknowledges. That is why
  `Store.hpp` is 2,318 lines.
- `HttpService.cpp:80-660` is a 580-line hand-written JSON parser while
  nlohmann is vendored and used forty lines away in `SourceMap.cpp:5`.
- `input::Action`'s thirteen members are all profiler and HUD intents, in an
  engine module.

### C7. `mono.libraries/` should **not** be created yet **[verified]**

Decision 22 reserves it for leaves. Measured, there are two: `collision` (STL
plus `core/types` only) and `spatial` (the same, plus `core::Name`). `msl`
carries SPIRV-Cross and `nodegraph` carries imgui and four concurrency headers,
so neither qualifies. `repo_layout.md` §4.1 sets the bar at three. This entry
exists so the next person to notice `collision` is a leaf does not create the
directory either.

---

## D · ECS components

`docs/ECS_COMPONENTS.md` is now generated from the registry by `just
components`, and `just components-check` fails if a component has no purpose
line. **134 engine-registered components, all documented** - 129 when the tool
landed, plus the five that had no name to be found under until D1 gave them one.
What the generated table immediately showed:

- **Zero components are saved, raw-serialised and padded together.** That trio
  leaks uninitialised padding into `.agame` files. Clean today, and now checked
  every time the catalogue regenerates.
- **Three of 134 have a compact wire form**: `scene.Transform` (10 of 28 bytes),
  `scene.Motion` (12 of 24), `ecs.Hierarchy` (8 of 40). Everything else
  replicates at full width.
- **Three tags exist**, `scene.Simulated`, `script.Disabled` and
  `ecs.NotArchivable`. The zero-cost query marker is barely used.
- The three largest rows are `effects.ParticleEmitter` (1264 B),
  `physics.PhysicsWorld` (1240 B) and `effects.Trail` (1152 B).

### D1. Six components had no name, and the table was never closed **[verified, fixed]**

Two roadmap entries that turned out to be one job in a fixed order.

**The names.** `WorldTime`, `NotArchivable` and `DirtyBits` (`ecs`),
`PortalProxy` (`scene`), `PoppercamState` (`physics`) and `Sun` (`scene`) were
all registered by `Components::Of<T>()` at first use, under the compiler's
spelling of the type. All six reach a `.agame`. Decision 21 and rule 4 both say
a name that crosses a file is a string somebody chose, and
`__PRETTY_FUNCTION__`'s output is stable within one build and nothing wider.

Order was the whole difficulty. `Adopt` aborts on an explicit registration that
arrives *after* an automatic one - a type has one name - so each had to be
registered before its own first use. `Store`'s constructor calls
`RegisterInstanceComponents` and then immediately does `SetResource(WorldTime{})`,
which is how close some of these were.

**Naming `scene.PortalProxy` switched a live rule back on.**
`replication/src/Defaults.cpp:212` has tested for that exact string in
`LocalToTheClient` since portals landed, and the string never matched, so every
proxy was replicated. The comment beside the test says what that costs: a proxy
is made and unmade inside one tick, so replicating one is a create and a destroy
per proxy per tick on the wire, describing geometry the client already has on
the other side of the pane. **That branch was dead code and is now live.**

**The seal.** `Components::Seal()` closes the table; registering afterwards is
refused. Its only caller was a test, so the determinism guarantee
`just determinism` and `just replay-check` rest on was switched on in no shipped
binary. `mono.client` and `mono.server` now seal after `Initialise`.

Three programs deliberately do not, each for its own reason:

- **`mono.studio`** calls `LoadPlugins()` every time a game file is opened
  (`Editor.cpp:2737`), and a plugin registers a schema. Sealing would break
  opening a second file.
- **`mono.launcher`** never touches `ecs`. The call would do nothing.
- **`mono.unified_tests`** is a harness; a test binary registering its own types
  is the point of one.

**Sealing found two more bugs on its first run, which is the argument for it.**
`physics::PoppercamState` was registered mid-tick by the camera pass installing
its own resource. And `mono.client` **never called `RegisterPhysicsComponents`
at all** - the server does it from `Simulation.cpp:251` and the studio from
`Editor.cpp:611`, and the client alone relied on `physics::Prepare` reaching it
when the first world was given physics, during the run. Both are fixed.
`RegisterClientComponents`'s own comment already described this failure for
`client::DrawList` at v0.7; it was the same bug twice more.

**Evidence.** All 43 staged example scenes run headless to exit 0 with the seal
and the abort live, and report no late registration. The server runs clean plain
and with `--worlds-per-host 3 --chatter`. `just determinism` and
`just replay-check` are byte-identical. 43/43 suites pass. The catalogue is 134
components, all documented, and the `(unprefixed)` section it used to carry is
gone.

**A script that declares a component after the seal gets a clean refusal, not a
crash.** `Schemas::Register` checks `Components::Sealed()` and returns
`Status::Sealed`, whose own comment is the reason. That path was designed for
this and had nothing switching it on. No shipped script calls
`World.DefineComponent`; a game that wants one must declare it before the first
tick, which is now a stated constraint rather than an accident.

**The rule is checked.** `just determinism` and `just replay-check` already ran
the server, so that half was covered. `just client-smoke` has joined
`just check` for the client half, because five of the six late registrations
lived there and no suite can find them: a lazily-registered resource only
appears when the pass that uses it runs against a real scene.

### D3. Component changes worth making **[reported]**

The survey produced 35 specific misplaced fields, 6 merges, 12 splits and 10
renames. The ones with the clearest arguments:

1. Add `scene.PortalTransitSeen` and `scene.RenderedSignature` to
   `LocalToTheClient` (`replication/src/Defaults.cpp:190`). The first is a live
   bug: the authority's transit serial defeats the client's own portal snap at
   `Interpolation.cpp:55`.
2. Add `scene.TextContent` and `scene.ShaderSource` to `CannotBeSigned`
   (`Defaults.cpp:172`). Both carry hand-written `Write`/`Read` pairs so they
   can cross, and both are silently dropped by the `!Trivial` gate at `:404`.
3. Split `physics::PhysicsWorld` (`PhysicsWorld.hpp:535`): about forty members,
   about thirty-four of them per-step scratch, **all serialised**.
4. Split `TrailHistory` out of `effects::Trail`: 448 hot bytes inside a
   1068-byte saved row.
5. Add `effects.` to `SHARED_PREFIXES`. Particles, beams and trails never reach
   a client.
6. Delete `scene::QuickHash` (`Components.hpp:1569`): no readers, no writers,
   repo-wide.
7. Move `scene::Visual::Locked` to a studio tag; its only reader is
   `Overlay.cpp:1264`.
8. Extract a shared `RenderMaterial`. `LightEmission`, `LightInfluence`,
   `Brightness`, `ZOffset` and `Additive` are duplicated across seven
   components.
9. Split `Humanoid`: `Height` and `Radius` to `Collider`, `Health` to its own
   component. `ROADMAP.md` v0.23 asks for this anyway.
10. Replace `SpawnLocation::TeamColour` with an entity handle. `PlayerTeam`
    already does it correctly.

### D4. Components the roadmap needs and that do not exist **[reported]**

No `Skeleton`, `Bone`, `Animator`, `Constraint`, `Terrain`, `LevelOfDetail`,
`Fog` or `Atmosphere` type exists anywhere. v0.21 and v0.23 both need several.

---

## E · Build time

Measured on this machine, `release` preset, 24 cores, GCC 13.3. Another job was
stealing about 420% CPU throughout, so every wall-clock number below is a
pessimistic bound.

| | |
|---|---|
| clean build from empty | **172.6 s** wall, 2912 CPU-s, 8.5 GB build directory |
| configure, first / re-run | 17.6 s / 1.06 s |
| null build | 0.09 to 0.15 s |
| touch a leaf header, one dependant | 7.3 s, almost all archive and link |
| touch `ecs/Store.hpp`, 206 objects | **50.6 s** |
| touch `core/Log.hpp`, 252 objects | **57.0 s** |
| touch `core/Name.hpp`, 342 objects | **65.3 s** |
| studio link, cold / warm | 43.3 s / 2.87 s |

Vendor is 2373 of 4206 CPU-s. Of first-party cost, **72% is the frontend** -
which is to say parsing headers, which is why the include fixes below are worth
more than they look. The slowest four translation units are
`render/src/Renderer.cpp` at **31.2 s**, `ecs/src/Schema.cpp` at 22.2,
`studio/src/RojoSync.cpp` at 20.0 and `studio/src/Editor.cpp` at 19.6. The last
13 s of a build is near-serial: studio objects, then `libstudio_lib.a`, then
`studio`.

**No ccache or sccache is installed and no preset enables one.**

### E1. Two unused includes in headers everything sees **[verified, fixed]**

Measured with the real compile flags, as preprocessed lines. This number is
independent of machine load, unlike wall clock.

| Header | Before | After | Reaches |
|---|---|---|---|
| `ecs/Store.hpp` - included `core/Log.hpp`, used nothing from it | 120,696 | **82,358** | 253 TUs |
| `core/Clock.hpp` - included `<chrono>`, declares no chrono type | 82,779 | **422** | 264 TUs |

`Clock.hpp` is the striking one: a 99.5% cut on a header two thirds of the tree
includes. `<chrono>` was dragging `std::vformat_to` instantiation into hundreds
of translation units that never format anything, measured at **86 CPU-s**.

Four files were relying on a transitive include and now include it themselves:
`physics/src/Portals.cpp`, `script/src/JsEcs.cpp`,
`studio/include/studio/Editor.hpp` (all for `Log.hpp`) and `core/src/Clock.cpp`
(for `<chrono>`, which its implementation genuinely uses). **All 43 suites
pass.**

The clean-build effect was not re-measured; the machine was loaded and the noise
band was wider than the expected effect. Someone on a quiet machine should time
`touch mono.engine/ecs/include/engine/ecs/Store.hpp` before and after, against
the 50.6 s above.

### E2. The ranked list of what is left

1. ~~**Install ccache and set `CMAKE_CXX_COMPILER_LAUNCHER`.**~~ **Done at
   v0.19, and measured.** `MONO_CCACHE` is on by default, finds `ccache` or
   `sccache`, and sets both launchers **before the first `add_subdirectory`** so
   the vendor tree is covered - that is the 2373 of 4206 CPU-seconds that
   matters most. A missing cache is reported at configure time and is not an
   error, because nobody should have to install a tool to build this repository.

   Measured on this machine, `dev` preset, same build directory both times, at a
   load average of 18 to 24 - so the wall figures are pessimistic bounds and the
   CPU figures are the robust ones:

   | | wall | CPU | hits | load |
   |---|---|---|---|---|
   | clean, empty cache | 186.0 s | 3508 CPU-s | 0 of 1565 | 18-24 |
   | clean, warm, PCH not cached | 28.6 s | 310 CPU-s | 1565 of 1565 | 18-24 |
   | clean, warm, PCH cached | **21.5 s** | **197 CPU-s** | **1894 of 1894** | 8-15 |

   **A factor of eighteen in CPU**, 3508 down to 197. The wall figures fell from
   186 s to 21.5 s but the machine was differently loaded across those runs, so
   the CPU column is the one to quote; the survey's estimate of "about 150 s"
   off a clean build was right either way. Cache footprint is 0.7 GiB for one
   preset.

   **All 43 suites pass on a build where every object came out of the cache**,
   which is the check the sloppiness change below needed and not a formality.

   **Two things a reader should know before trusting that number.**

   `hash_dir` is on by default, so a debug build in a *different* directory does
   not share entries with this one - the measurement is deliberately
   same-directory for that reason, and a cross-preset figure would have been
   measuring `hash_dir` rather than ccache. With six presets at roughly 0.6 GiB
   each and a 5 GiB default `max_size`, the cache will begin evicting and the
   hit rate will quietly fall. `ccache --max-size 20G` is the cheap insurance,
   and it is a machine setting rather than something this repository should set.

   **A sixth of the build was being skipped silently.** ccache reported 331 of
   1896 calls as uncacheable, every one of them "Could not use precompiled
   header": SDL and glslang both call `target_precompile_headers` in their own
   CMake, and ccache declines a PCH compile unless `sloppiness` permits it. The
   launcher now passes `CCACHE_SLOPPINESS=pch_defines,time_macros` through
   `cmake -E env`, so the setting travels with the build rather than depending
   on each developer's `ccache.conf` - a cache that behaves differently
   depending on who ran it is exactly the third category rule 6 refuses.
   `time_macros` is the half worth justifying and it is safe here for a
   checkable reason: **no `__DATE__` or `__TIME__` appears anywhere in this
   repository**, first-party or in the vendored sources of the two targets that
   carry a PCH.

   Setting it took the uncacheable count from 331 to **zero** and a warm build
   from 310 CPU-seconds to 197. Two calls still report as errors and always
   will: they are the two PCH *generation* steps, where ccache cannot parse a
   header as a source and passes the call to the compiler untouched. Correct,
   and 0.11% of the build.

   **The lookup uses `NO_CACHE`, and that line is what stops the feature being a
   trap.** `find_program` normally writes its answer into `CMakeCache.txt` and
   never looks again, so the obvious sequence - clone, build, read the message
   saying to install ccache, install it, build again - would hand back the
   cached "not found" and no speedup, with nothing to say why. Verified: the
   entry no longer appears in `CMakeCache.txt` and a re-configure picks up a
   newly installed cache with no wipe.

   **CI keeps a cache between runs**, `.github/workflows/build.yml`. A runner is
   a fresh machine every time, so before this every CI build was the 3508
   CPU-second column. `actions/cache` rather than a marketplace ccache action,
   for the reason that workflow already gives about vcvars: a first-party
   mechanism whose behaviour is written down in the file beats one whose is
   written down elsewhere. `CCACHE_DIR` is pinned to `runner.temp` because
   ccache's default differs between Linux and macOS and a `path:` naming the
   wrong one would restore nothing and look exactly like a cold cache. Stats are
   printed on every run, not only on failure, because a hit rate that fell to
   zero and stayed there is a regression worth seeing in a green build.

   **Linux and macOS only.** ccache works with MSVC but only where debug info is
   `/Z7` rather than `/Zi`, and that workflow's own comments record that the
   Windows build compiles and has never shipped. Adding a caching layer with a
   compiler-flag precondition to the one platform nobody runs is how an optional
   build becomes a flaky one.

2. **`UNITY_BUILD` for `release` and `ci`**, at `MonoLibrary.cmake:315`.
   Measured at **51 to 73%** of first-party compile CPU. Blocked by about nine
   anonymous-namespace collisions, which is a morning's work.
3. **`-g1` instead of full debug info** at `CMakeLists.txt:149`. Measured 36%
   off the heaviest translation units, and objects 4 to 5 times smaller.
4. ~~**`spdlog/spdlog.h` out of `core/Log.hpp:9`.**~~ **Done, and the survey's
   estimate was five times too big.** `Log.hpp` preprocessed from **118,383
   lines to 22,842**, an 81% cut, and `<string_view>` rather than fmt is now its
   floor - `spdlog/fmt/bundled/base.h` is 7,373 lines on its own.

   Measured rather than extrapolated: 23 real translation units that include it,
   compiled `-fsyntax-only` with ccache disabled, minimum of three runs, against
   the old header restored from git.

   | | CPU |
   |---|---|
   | old, `spdlog.h` in the header | 16.88 CPU-s |
   | new, `fmt/base.h` | **12.22 CPU-s** |

   **27.6% off any translation unit that logs**, or 0.20 CPU-s each. 152 objects
   record `Log.hpp` in their dependencies, so about **31 CPU-s off a cold
   build** - not the 151 predicted here before. Two reasons for the gap: E1
   already removed the largest transitive path, taking the includer count from
   252 to 152, and the 0.60 s per object was not the marginal cost.

   **The inner-loop framing is the one that matters.** 31 CPU-s is under 2% of a
   clean build and ccache erases it on any rebuild. What ccache cannot erase is
   the first compile after a header changes, and there this is 27.6% off all
   152.

   The shape: `Log.hpp` includes only fmt's `base.h`, which declares
   `format_string`, `format_args` and `make_format_args` and nothing that
   formats. `Log::Emit` packs the arguments and hands them to `Log::Write`,
   which formats in `Log.cpp`. Compile-time format checking is kept -
   `fmt::format_string<Ts...>` still rejects a mismatched `{}` at the call site
   - and all 707 call sites still type-check with no change to any of them.

   `spdlog::logger` is forward-declared so `Log::Logger()` can still be offered
   to the one thing that installs a sink. Two files complete the type
   themselves, `mono.studio/src/Editor.cpp` and `core/tests/Log.cpp`, and that
   they have to is the invariant working rather than a gap in it.

   **`Log::Enabled` is new and the macros deliberately do not use it.** They
   evaluate their arguments whether the level is on or not, exactly as they did
   when they called spdlog directly. Guarding them closes G1's "a disabled
   statement still evaluates its arguments" and is one line, but it silently
   stops running any argument with a side effect - so it is its own change with
   its own review rather than a rider on this one.
5. **Split `render/src/Renderer.cpp`.** About 22 s off wall clock, because one
   31.2 s translation unit cannot be split across cores. See C2 - the same
   change fixes the architecture problem.
6. Precompiled headers at `MonoLibrary.cmake:353`. Measured at only **11%**, so
   this ranks below unity builds rather than above them, which is the opposite
   of the usual intuition and the reason it was measured.
7. Mark vendor includes `SYSTEM` at `MonoLibrary.cmake:344-348`. Unmeasured.
8. Cache the vendored `glslc` output. About 600 CPU-s.
9. `Justfile:68` reconfigures every build, and `_stage_shaders` has no declared
   output so it is always dirty. That is the whole of the 0.09 s null build, so
   it costs nothing today, but it hides a real null build behind a fake one.

### E3. Two more headers of E1's kind **[verified, not applied]**

- `core/types/AABB.hpp:41` includes `CFrame.hpp` for one function,
  `FromOrientedBox` at `:96`, with three call sites repo-wide. `AABB.hpp`
  preprocesses to 74,013 lines and `CFrame.hpp` to 73,891, so **`AABB` is
  essentially all `CFrame`**, and every consumer pays for
  `<glm/gtc/quaternion.hpp>`. Moving `FromOrientedBox` to a free function beside
  `CFrame` would make `AABB.hpp` nearly free, the way `Clock.hpp` now is.
- `mono.client/include/client/Scene.hpp:27` pulls `render/Renderer.hpp` (1,823
  lines, plus glm) into a header with direct fan-in 18, which
  `studio/Editor.hpp:73` includes at fan-in 43. Paid about sixty times.
- `mono.client/include/client/Settings.hpp:35` includes the whole 1,194-line
  `Client.hpp` and its 186-header closure solely to name `Options`. `Options`
  should be its own header.

## F · Parallelism

**The engine is far less concurrent than a reader would guess, and that is
mostly correct.** Networking has 17 concurrency primitives across 5 files.
`scene`, `world`, `game`, `assets`, `bake` and `bakegraph` contain **no thread,
mutex, future or condition variable at all**. The two real defects found are A1
and A2 above.

Loops worth changing, all **[reported]**:

1. `mono.client/src/Client.cpp:495-498` calls `RequestWantedContent`
   unconditionally every frame, which runs **eight full store walks per world**
   (`ContentDemand.cpp:27-70`) and discards essentially all of it in the steady
   state. Live on every default run. **This is work that should not happen, not
   work to parallelise.**
2. `replication::Authority::Publish`'s per-client loop
   (`Authority.cpp:1750`, N = 64 to 200 clients x 10k entities) is blocked first
   by a process-wide mutex taken at `Authority.cpp:1493`.
3. `mono.client/src/Replicated.cpp:127-170` is the serial twin of a loop already
   parallelised: `CollectInstances` uses `EachBatchParallel` with grain 1024 at
   `Scene.cpp:281-345`. The blocker is the per-entity `Sample` and three `Get`s,
   not the loop shape.
4. `physics/src/BroadPhase.cpp:80-131`, N about 4,000, safely parallelisable
   because the pair set is sorted and uniqued afterwards.
5. `scene/src/Visibility.cpp:112` uses a single hash chain over every row every
   frame, when `DrawInstance.cpp:64-79` already measured that exact problem and
   fixed it with four lanes.

Three fetch-path costs sit inside the tick barrier: `delivery/src/Cache.cpp:189`
rescans the whole cache directory per stored asset; `assets/src/Manifest.cpp:221`
and `:247` make bundle splitting O(M squared x A), and the comment there
predicts it and says "that is the moment to add an index";
`assets/src/ChunkStore.cpp:143` and `:158` hash every chunk twice.

**Measure in `release` before acting on any of these.** The `dev` preset is
`-O0` and a timing from it means nothing.

---

## G · Observability

### G1. Logging is 101 lines and thirteen modules never use it **[reported]**

`core/Log.hpp` plus `Log.cpp` is the whole facility: four levels, four macros,
one hard-coded stdout sink. No compile-time filtering (`SPDLOG_ACTIVE_LEVEL` is
unset), no categories, no thread id in the pattern (`Log.cpp:23`), no source
location, no correlation id, no throttling, and **a disabled statement still
evaluates its arguments**.

707 call sites, of which eight are `ENGINE_TRACE`. `net`, `network`, `scene`,
`spatial`, `collision`, `resources`, `graph`, `nodegraph`, `bake`, `msl`,
`input`, `gui` and `effects` log nothing at all.

No assert facility exists, despite `core/AGENTS.md:12`.

### G2. `core::Metrics` is write-only **[reported]**

Sum-only, with no `Get`, no gauge and no histogram. Four separate counter
implementations exist across the tree and nothing exports out of process. The
headless server never drains its counters.

`core::FrameGraph` by contrast is solid - 65k spans, five seconds of history in
a 2 MiB ring, snapshot percentiles - but it is single-threaded by design, so
parallel compute drops every worker span. That is what A1's counter exists to
report, which is why A1 mattered.

**The shape of the fix, in order:** give a disabled log statement zero argument
evaluation; add a category to the macro; make the level dynamic per category;
give `Metrics` a read side; drain it on the server.

### G3. MCP **[partly fixed]**

`mono.tools/mcpbridge` is a 183-line byte pump over `engine::control`, which
speaks five JSON-RPC methods with no `resources/*` and no `prompts/*`. Ten
shared tools plus five editor tools; the server registers nothing server-shaped.

**Added this pass:** `engine_components`, which answers what storage the engine
actually has. `component_list` deliberately covers only what a *game* declared
(`Tools.cpp:869`), so a model could not previously ask that question at all.

Still missing, in rough value order: the module graph and layer table; a test
runner; script tools; a log tool outside the editor. Three port mismatches are
**[reported]**, including `studio --mcp-port` bare defaulting to 8720 while its
own help says 8738, and the bridge has no tests.

---

## H · What changed this pass

- `docs/CODE_ARCH.md`, new. The layer stack, tiers, the dependency rule,
  domain-driven design and ports-and-adapters as this engine does them, the
  transport-per-feature table, and what is checked by what.
- **The layer rule is now enforced.** `expected_graph.json` carries a `layer` on
  all 30 layered modules, and `CheckTargetGraph.cmake` refuses an edge that does
  not run downward, a same-layer edge not named in a `lateral` array, and any
  edge from a layered module into the program band. Zero violations at HEAD.
- **The architecture check is itself checked**, by six fixtures under
  `mono.tools/architecture/tests/` - five that must fail with a named message
  and one that must pass. It is the one check in the repository that could go
  green by parsing nothing.
- `docs/ECS_COMPONENTS.md`, new and generated. `mono.tools/componentdoc`, `just
  components`, `just components-check`, folded into `just check`.
- `mono.engine/AGENTS.md` was a one-line stub and is now the module table and
  the invariants common to all of them.
- `mono.engine/control/AGENTS.md`, new. It was the only module of 29 without
  one.
- `game`, `scene` and `render` `AGENTS.md` corrected where they were false.
- Root `AGENTS.md` and `docs/CODE_QUALITY.md` reconciled on layer enforcement.
- `core::FrameGraph`'s data races fixed.
- `ecs/Store.hpp` stopped including `core/Log.hpp` (120,696 preprocessed lines to
  82,358, across 253 translation units) and `core/Clock.hpp` stopped including
  `<chrono>` (82,779 to **422**, across 264). All 43 suites pass.
- `engine_components` added to the MCP surface.
- The compiler cache is wired, on by default, and reported when absent.
