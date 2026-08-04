# Running

Every way to build and run something in this repository, and what each one is
for.

For getting a first build working on a fresh machine, see
[CONTRIBUTING.md](CONTRIBUTING.md). This file assumes the clone configures.

---

## The short version

```sh
git clone --recurse-submodules <url>
cd atomic-game-engine

just setup                  # submodules, and shaderc's own DEPS
just build                  # everything, dev preset
just run --stats --graph    # the client, both debug panels open
just edit                   # the editor, on a new game
just test                   # only the suites your change could have affected
```

Needs CMake 3.24+, Ninja and a C++20 compiler — nothing else. `glslc` is built
from the vendored shaderc, so it is not a prerequisite. Everything derived lands
in `.cache/`.

---

# Building

## Presets

```sh
cmake --preset dev       # everything, tests, Tracy, unoptimised first-party code
cmake --preset release   # optimised — the only preset that turns -O0 off
cmake --preset server    # no client at all, and therefore no graphics stack
cmake --preset cdn       # the content origin alone, on a machine with nothing installed
cmake --preset ci        # dev, with warnings fatal
cmake --preset bench     # release, plus the benchmark binaries
```

| Preset | Client | Server | CDN | Tests | First-party `-O` |
|---|---|---|---|---|---|
| `dev` | yes | yes | yes | yes | `-O0` |
| `release` | yes | yes | yes | no | optimised |
| `server` | **no** | yes | no | yes | `-O0` |
| `cdn` | **no** | **no** | yes | yes | `-O0` |
| `ci` | yes | yes | yes | yes | `-O0`, warnings fatal |

`server` and `cdn` are the two presets that prove something rather than build
something. `server` proves the client tier is absent; `cdn` proves the content
origin needs no graphics stack at all — it configures where there is no Vulkan
SDK, no SDL and no shader compiler.

The first configure of a client preset builds glslang, SPIRV-Tools and shaderc,
so that the `glslc` compiling the engine's shaders is pinned rather than
whatever is on your PATH. It is the slowest part of a fresh build and it happens
once. `-DMONO_VENDORED_GLSLC=OFF` skips it and uses a system `glslc`.

`just` works against `dev` unless told otherwise. **One override, written before
the recipe name, and it applies to every recipe:**

```sh
just preset=release build
just preset=release run --stats --uncapped
just preset=server test
```

It is not a recipe argument. `build` is derived from `preset` when the Justfile
is read, and an argument cannot reach a derived variable — which is what made
the older `just test server` configure one preset and then run another's
binaries. The override is applied before anything is derived, so the configure,
the build and the directory the binary is run from cannot disagree.

## Building all the monos

```sh
just build                     # every target the preset configures
just preset=release build      # the same, optimised
cmake --build --preset dev     # without just
```

## Building one mono

Every module is one CMake target, so building one is `just build <target>`.
Its dependencies come with it; nothing above it does.

```sh
just build engine_ecs              # mono.engine/ecs and what it needs
just build client_lib              # mono.client's library, not the program
just build test_render             # one test binary
just preset=release build engine_ecs   # any of the above, other preset
```

| Folder | Library target | Alias | Test binary | Tier |
|---|---|---|---|---|
| `mono.engine/core` | `engine_core` | `Engine::core` | `test_core` | shared |
| `mono.engine/parallel` | `engine_parallel` | `Engine::parallel` | `test_parallel` | shared |
| `mono.engine/ecs` | `engine_ecs` | `Engine::ecs` | `test_ecs` | shared |
| `mono.engine/assets` | `engine_assets` | `Engine::assets` | `test_assets` | shared |
| `mono.engine/net` | `engine_net` | `Engine::net` | `test_net` | shared |
| `mono.engine/input` | `engine_input` | `Engine::input` | `test_input` | client |
| `mono.engine/render` | `engine_render` | `Engine::render` | `test_render` | client |
| `mono.client` | `client_lib` | `Mono::client` | `test_client` | client |
| `mono.server` | `server_lib` | `Mono::server` | `test_server` | server |
| `mono.unified_server_client` | `unified_server_client_lib` | `Mono::unified_server_client` | `test_unified_server_client` | client |
| `mono.cdn` | `cdn_lib` | `Mono::cdn` | `test_cdn` | shared |
| `mono.tools/testrunner` | `testrunner_lib` | `Tool::testrunner` | `test_testrunner` | shared |
| `mono.tools/linecount` | `linecount_lib` | `Tool::linecount` | `test_linecount` | shared |

The names are derived, not chosen per module: `mono_add_library` makes
`engine_<name>` for an `Engine::` module and `<name>_lib` for a product's own
library, and `mono_add_tests` makes `test_<name>`. See
`mono.build/MonoLibrary.cmake`.

**`engine_input` and `engine_render` do not exist under the `server` preset.**
`mono.engine/CMakeLists.txt` only adds them when `MONO_BUILD_CLIENT` is on, so a
server configure never compiles SDL and never needs a shader compiler. That is
what makes "the server contains no graphics stack" a property of the build
rather than a claim about a link line — and it is why
`just preset=server build engine_render` fails with `unknown target` rather than
quietly building it.

## Building one program

```sh
just client                # the client program and only its dependencies
just server                # the server program and only its dependencies
just cdn                   # the content origin and only its dependencies
just build testrunner      # the test runner
just preset=release client # optimised
```

`mono.engine` is libraries and never a product, so there is no target that
"builds the engine" on its own. Building a program builds the subset of modules
it links.

## Where things land

Each preset writes to `.cache/build/<preset>/`:

```
.cache/build/dev/
├─ client/       the client — binary, SDL3, shaders/render/
├─ server/       the server — no shaders, no SDL
├─ cdn/          the content origin — no shaders, no SDL
├─ tools/        testrunner
├─ tests/        every test binary
├─ lib/          static libraries, intermediate and never shipped
└─ shaderstage/  compiled SPIR-V, staged into each program that links it
```

A program's directory is runnable as it stands — see
[Running from somewhere else](#running-from-somewhere-else). Libraries are
intermediate and ship nowhere.

## Cleaning

```sh
just clean       # .cache/build — every preset's build tree
just clean-all   # .cache — including the smart-test cache
```

## Formatting

```sh
just format        # clang-format -i over every first-party .cpp and .hpp
just format-check  # fails if anything is misformatted, changes nothing
```

Both skip `mono.vendor/` by construction: the directory list is explicit rather
than `find .`, because reformatting a submodule turns every future update into a
conflict.

---

# What there is to run

`mono.engine/` is libraries. No `main.cpp` lives under it and nothing ships from
it alone, so there is no single binary that "runs the engine" — there are thin
mains over the parts of it they need.

| You want to | Run | Adds, over the engine |
|---|---|---|
| Run a script with nothing else in the way | `atomic` *(v0.6)* | nothing |
| Build a game | `studio` *(v0.7)* | the editor: explorer, properties, script editor, run and play |
| See something on screen | `client` | window, input, renderer, and the client half of networking |
| Host a simulation with no window | `server` | the tick loop, and the hosting half of networking |
| Serve a game's content | `cdn` | a content root, and later the manifest and the origin's HTTP layer |
| Exercise one module | its test binary | Catch2 |
| Re-run only what your change affected | `testrunner` | — |
| Check the architecture held | a CMake script | — |

**The client and the server are shims, not the engine.** What they add is the
part that has to exist between two machines — name resolution, peer-to-peer,
hosting, the session — plus, on the client, presentation. A script that needs
none of that does not need either program, which is what the standalone runtime
is for.

`client --help`, `server --help` and `studio --help` print their own options,
generated from the option declarations, so they cannot drift from what the
program accepts.

---

# The editor *(v0.7)*

```sh
just edit                       # a new game, one world, a baseplate
just edit --game My.agame       # open one
```

The one program where `RunService:IsStudio()` is true. It writes a `.agame`,
which is the file the other two programs read:

```sh
just host --game My.agame       # a dedicated server hosting every world in it
just run  --game My.agame       # single-player, both roles in one process
```

### The window

A menu bar, a toolbar, and three docked panels around a hole in the middle that
the world is drawn into. The layout is yours after the first run — it is written
to `studio-layout.ini` beside the binary, and the built-in arrangement is only
used when that file does not exist.

| Panel | What it is |
|---|---|
| Explorer | the universe, its worlds, and every instance in the active one |
| Properties | whatever is selected, grouped by which class declares each property |
| Script Editor | a tab per open script |
| Output | the engine log, including what your scripts `print` |

**The tree is the mapping `game` and `workspace` already had.** The root is the
universe, which is what a script calls `game`; each world under it is a scene,
and the active one is what a script running in it calls `workspace`. That is not
a resemblance — `script/src/Services.cpp` established the mapping at v0.6 and the
explorer draws the same objects.

### Getting around

| | |
|---|---|
| Right mouse, held | look |
| `W` `A` `S` `D`, while looking | move |
| `Q` `E`, while looking | down and up |
| Wheel, while looking | how fast, not how far |

The camera is not an entity in any world, which is why it is not saved, not
replicated and not reset by Stop.

### Editing

Insert Object on the toolbar, or right-click in the tree. The list is every
class registered under `Instance` — it is read from `ecs::Classes` rather than
written down, so a class added by any module appears in it with nothing in the
editor changing. The same is true of the properties panel: it walks the class's
declared properties, and an `Enum` property is a list of its registered members
rather than a text field, so a mistyped material is impossible rather than
caught.

Drag a row onto another to reparent it. `Ctrl+D` duplicates, `Del` deletes,
`Ctrl+S` saves.

### Run, Play and Stop

| | |
|---|---|
| **Run** (`F6`) | the server's scripts — `Script`, not `LocalScript` |
| **Play** (`F5`) | both, which is what single-player is |
| **Stop** (`Shift+F5`) | ends it, and *puts the scene back* |

Stop is a snapshot restore, exactly as it is in Roblox: what your scripts did to
the world is undone, and what you authored before pressing Play is not. Edits
made to a script while the game is running apply on the next run, and the editor
says so rather than leaving you to find out.

**Nothing ticks in edit mode.** A world that simulated while you were building it
would settle physics under your hands — a part placed in the air would be on the
floor by the time you looked away.

### Options

| Option | Default | What it does |
|---|---|---|
| `--game PATH` | — | open a game file at startup |
| `--width`, `--height` | 1600×900 | window size |
| `--scale FACTOR` | 1.0 | multiplies every font and padding |
| `--tick-rate HZ` | 60 | simulation rate while running |
| `--frames N` | — | exit after N frames, for a script or a screenshot |
| `--override-assets-directory DIR` | — | read staged data from here |

### The file it writes

A `.agame` is one XML document holding the universe, every world, every
instance and every script's text. It is meant to be read:

```xml
<Game format="1" name="Untitled">
	<Universe mode="WorldParallel" catchUp="8" busBudget="64" />
	<World name="Start" tickRate="60" idleTickRate="2" faultLimit="3">
		<Sources>
			<Source path="Scripts/Script.luau"><![CDATA[print("hello")]]></Source>
		</Sources>
		<Item class="Part" name="Baseplate" id="1">
			<Property name="Size" type="Vector3">128, 1, 128</Property>
		</Item>
	</World>
</Game>
```

Only properties that differ from their class default are written, which is what
keeps a scene of a thousand parts a file you can read a diff of. A single world
exports on its own as a `.aworld` — File ▸ Export Active World — and imports
back into any universe.

**It is not a snapshot and does not replace one.** `--record` still writes the
binary format, which carries a *running* universe including tick counters and
bus state and is same-build only. A game file is content: it survives an engine
version, and that is what it is for.

---

# Running a script file *(v0.6)*

The engine runs `.luau` and `.ts` files directly. A game is a script, not a
compiled artefact, and all three programs take a path to one:

```sh
atomic game.luau                    # the engine, and nothing else
client game.luau --stats            # with a window, a renderer and input
server game.ts --tick-rate 60       # headless, hosting
```

A bare path is the documented form. The explicit spellings are synonyms, kept
because they are what the programs declare today:

```sh
client --script game.luau           # identical to `client game.luau`
server --game world.ts              # identical to `server world.ts`
```

Both extensions load the same way. `.ts` is TypeScript/JavaScript and `.luau` is
Luau; which one a file is comes from its extension, and a game may mix them.
`mono.engine/examples/` holds the demo scenes, each written twice — once in each
language, doing the same thing — so that the binding surface is exercised from
both.

### What happens today

**Nothing executes yet.** In v0.1:

- `--script` and `--game` are parsed and *warn*. `client` says the scripting
  layer has not landed; `server` says the game file format has not landed and
  names the placeholder world it is hosting instead.
- A bare positional path is collected by the parser (`core::Arguments` keeps
  positionals) and then ignored by both mains, with no message. Prefer the
  explicit spelling until the runtime lands, because it at least tells you it
  did nothing.
- `atomic` is not a target. It arrives with the runtime.
- The files in `mono.engine/examples/` are placeholders.

The VM, the bindings and the standalone runtime are v0.5–v0.6 on
[ROADMAP.md](ROADMAP.md). A flag that is silently ignored is worse than one that
says so, which is why the warnings are there rather than removed —
[docs/DEFERRED.md](docs/DEFERRED.md).

---

# The client

```sh
just run                          # dev preset, no panels
just run --stats --graph          # both debug panels open
just demo                         # the same, spelled shorter
scripts/run-demo.sh               # the same again, without just
./.cache/build/dev/client/client  # directly, no just and no build
```

`just run` passes everything after it straight through, and builds the client
first.

### Without `just`

`scripts/run-demo.sh` and `scripts/run-demo.bat` are `just demo` for a machine
that does not have `just` — which on Windows is most of them. They build the
client and run the demo scene with both panels open, and everything after the
script name reaches the client:

```sh
scripts/run-demo.sh --frames 600 --uncapped     # Linux, macOS
scripts\run-demo.bat --frames 600 --uncapped    # Windows
PRESET=release scripts/run-demo.sh              # any preset but `server`
```

They call CMake rather than `just`, so the two halves cannot drift apart, and
they resolve the repository from their own path — run them from anywhere. The
`.bat` wants a Developer Command Prompt, because the presets generate Ninja and
a plain `cmd` window has no compiler in it.

Both carry the note for what replaces them: the demo scene is C++ today
(`mono.client/src/Demo.cpp`) and becomes
`mono.engine/examples/Mirrors-1-world.luau` once the script runtime lands at
v0.6.

### Options

```
--stats                          Open the F3 statistics panel at startup
--graph                          Open the F5 frame graph at startup
--uncapped                       Present without waiting for vblank
--verbose                        Log at trace level
--worlds N                       Worlds to simulate and composite (default 1)
--view-spacing UNITS             World units between composited views (default 40)
--entities N                     Cubes in the demo scene, per world (default 2048)
--frames N                       Exit after N presented frames
--width PX                       Window width (default 1280)
--height PX                      Window height (default 720)
--profiler-tab NAME              frame, categories, systems or counters
--script PATH                    Luau or TypeScript script to run at startup (v0.6)
--game PATH                      Game file to play single-player (.agame) (v0.7)
--enable-profiler SECONDS        Wait for a Tracy profiler before starting
--profile-seconds SECONDS        Run for this long, then exit
--override-assets-directory DIR  Read shaders and data from here
--help                           Show this text
```

Naming a `--profiler-tab` opens the graph, and `--profile-seconds` turns
collection on — asking to see something is not a separate flag from showing it.

`--game` plays a `.agame` written by the editor: every world in it is
simulated, its scripts run with both roles true, and there is no socket and no
server library involved — single-player is the format and a VM, not a server
hosted in this process. Given both `--game` and `--script`, the game file wins
and the client says so rather than choosing quietly.

### Benchmarks

```sh
just bench                  # only what a change could have affected
just bench-all              # every suite
just bench-accept           # make what was measured the new baseline
just bench --filter ecs     # one area
```

The same selection as `just test`, over `bench/` instead of `tests/`: a
benchmark declares `TEST_SUITE_ID` and gets the same cascading signature, so a
change at the bottom of the stack re-measures everything above it and nothing
else. That matters more here than for tests — a test suite costs milliseconds
and a benchmark suite costs seconds by design.

Always against the `bench` preset, which optimises. A debug build measures the
debug build, and the danger is not that it is slower: it is that the *ratios*
between two implementations invert.

```
engine.ecs.bench.iteration
  Each · 500k entities                     198.84 us      -1.8%
  EachParallel · 500k entities              90.46 us     +33.6%  slower
```

**A number is reported, never enforced.** `just bench` does not fail on a slow
figure and it should not: a laptop on battery, a CI runner with a noisy
neighbour and a desktop with a compile going all swing further than most real
regressions — the `+33.6%` above is one of those, not a change to the code. Read
the number, then decide. `docs/CODE_QUALITY.md`'s rule about attaching a
measurement to an algorithm change is what this exists to serve.

Take a baseline on a quiet machine, and say so in the commit. One taken while
something else was compiling makes every later run look like an improvement.

### Compositing several worlds

```sh
just run --worlds 3 --entities 512
```

Three whole worlds, each with its own clock, its own store and its own view
channel, drawn into one frame side by side. They are *placed* rather than
overlaid because two worlds' coordinates do not mean the same thing — nothing
says they should, and drawing two scenes inside each other and calling it one is
the mistake `--view-spacing` exists to avoid.

The renderer draws what the compositor took off the channels rather than
reaching into a store. Between a world at its tick rate and a frame at the
display's sit three slots and an atomic index, so a slow frame **drops rather
than throttles a simulation** — and a producer that stalls keeps its last frame
instead of vanishing for one.

`--name=value` works everywhere `--name value` does. Everything after a bare
`--` is a path, including something that looks like an option.

### While it is running

| Key | Does |
|---|---|
| **F3** | frame counter — FPS now, and min/avg/max over the last twenty seconds |
| **F5** | frame graph — last frame's scope tree |
| **F6** / **F7** | next / previous frame-graph view |
| **PgUp** / **PgDn** | scroll a graph taller than the panel |
| **-** / **=** | shallower / deeper flamegraph |
| **F8** | write `frame-graph-snapshot.txt` beside the binary |
| **Esc** | quit |

The four frame-graph views are the flamegraph, time by category, per-system cost
from the scheduler, and whatever was written to the metrics sink.

#### Reading the flamegraph

One row per span, indented by nesting, with a colour chip for its category and a
timeline strip on the right showing *when* in the frame it ran — which the
numbers cannot say, because two systems costing 2 ms each read identically
whether they ran back to back or with the GPU wait between them.

| Column | Is |
|---|---|
| `MS` | this frame |
| `RMAX` | the worst single reading in the last 300 frames |
| `SHARE` | of the frame |

**`RMAX` is the column to read first.** A span costing 0.2 ms now and 14 ms
every fortieth frame shows 0.2 in `MS` on a panel repainted at any rate a person
can watch, and the fortieth frame is the one worth knowing about. The header
says how much history that maximum is over, because a worst case over a fifth of
a second and one over five seconds are different claims.

Rows deeper than the depth limit are hidden and their nearest visible ancestor
is marked `+`, so a collapsed subtree never looks like a leaf.

#### F8, for what the panel cannot show

The panel shows one frame. `F8` writes the retained window — five seconds or
20,000 frames, whichever comes first — to `frame-graph-snapshot.txt`: mean, p50,
p99 and max per span, then the forty worst frames and the biggest spans in each.
That last section is what separates "this span spikes" from "the whole frame
went with it".

Collection only runs while the panel is open, so `F8` with `F5` closed has
nothing to write and says so rather than leaving a zero-byte file.

### Measuring rather than watching

```sh
just run --stats --graph --uncapped
```

**Always pass `--uncapped` when you care about the numbers.** Without it the
frame is paced by your display, every frame reads as 16.7 ms, and the flamegraph
is dominated by the swapchain wait. With it you see what the frame actually
costs.

Remember that `dev` builds first-party code at `-O0` on purpose — a profile
there measures what the engine does rather than what the optimiser rescued. Use
the `release` preset when you want the shipped number instead:

```sh
just preset=release run --stats --graph --uncapped
```

### Repeatable runs

```sh
just run --frames 600 --entities 4000 --uncapped
```

`--frames` makes the client usable from a script or a CI job. The demo scene is
a function of elapsed time and is deterministic, so two runs with the same
arguments simulate the same thing — which is what makes comparing them mean
anything.

---

# The server

Headless. No window, and no renderer in the binary at all.

```sh
just host                                   # 30 Hz until you stop it
just host --ticks 300 --entities 20000
./.cache/build/dev/server/server --help
```

### Options

```
--unpaced                        Tick back to back instead of pacing to the tick rate
--graph                          Collect the frame graph (for a Tracy capture)
--verbose                        Log at trace level
--tick-rate HZ                   Ticks per second (default 30)
--entities N                     Entities in the placeholder world (default 4096)
--ticks N                        Exit after N ticks
--seconds N                      Exit after N seconds
--game PATH                      Scene script, or a .agame universe to host (v0.7)
--record PATH                    Write a recording of this run
--replay PATH                    Replay a recording instead of simulating
--override-assets-directory DIR  Read staged data from here
--chatter                        Make every world publish on a shared topic
--host NAME                      Run as a supervised host under a driver
--world NAME                     A world this host was granted (repeatable)
--remote-world NAME              Place this world in a host process (repeatable)
--worlds-per-host N              Shared worlds per host process (default 8)
--host-program PATH              The program a host runs (default: this one)
--processes N                    How many processes share this machine
--help                           Show this text
```

### Recording and replaying

A recording is one snapshot plus every bus envelope applied since, which is
complete because a world is deterministic given its state and its inbox.

```sh
./server --entities 512 --ticks 200 --unpaced --record run.rec
./server --replay run.rec                       # the same run, again
./server --replay run.rec --record again.rec    # and record what it replayed
```

The third form is what `just replay-check` runs: `again.rec` has to come back
byte-identical to `run.rec`, which says the snapshot, the frame times and every
envelope all reproduced — not merely that the replay survived. `just
determinism` makes the weaker but broader claim, that two live runs of one scene
produce identical files. Both are same-binary, same-machine; cross-machine
agreement is deliberately not promised, because floating point differs between
compilers and chips.

### Host mode

A host is not a different program. It is this one, holding some of a universe's
worlds in its own address space so that a hard fault in one of them takes that
process rather than the server — which is what makes the grouping a deployment
decision instead of an engine one.

You do not run the host side by hand. You run the *driver* side, and it spawns
what it needs:

```sh
./server --seconds 10 --chatter --remote-world lobby --remote-world arena
```

That starts one host holding both worlds, links it, and routes every bus
operation through the driver's own MessagingService, MemoryStore and DataStore —
there is one router, so a world behaves the same wherever it is held. `--chatter`
exists because there is no game file yet and therefore no traffic: it makes every
world publish its name and tick on a shared topic, which is the only thing that
crosses a link until v0.5 gives worlds something real to say.

Running the host side directly is refused:

```sh
./server --host host.one --world lobby --world arena    # refuses: no driver
```

Deliberately. A host with no driver would tick worlds nobody asked for and answer
to nobody, and accepting it would make `--host` look like a way to run the server
under a nicer name.

`--processes` is the worker budget. Every process calling for one worker per core
is the bug it prevents: a driver and seven hosts on a twenty-four core machine
would run a hundred and ninety threads over twenty-four cores. The driver works
it out from the hosts it is about to spawn and passes the answer down, so you
should not normally set it.

### Stopping it

Ctrl-C, or `SIGTERM`. The handler sets one flag that the loop reads between
ticks, so the current tick finishes and the run summary still prints:

```
30 tick(s) over 0.97s · mean 0.336 ms · slowest 0.540 ms · 0 overrun(s)
```

**`overruns` is the number worth reading.** It counts ticks that took longer
than their budget. Zero means the tick rate was actually held; anything else
means the server is behind, and `slowest` tells you by how much.

### Measuring a tick

```sh
just host --unpaced --ticks 1000 --entities 50000
```

`--unpaced` removes the sleep between ticks, so `mean` measures the simulation
rather than the pacing. Use it for any comparison; leave it off to check that a
given entity count actually holds a given tick rate.

---

# The content origin

Serves a game's content out of a directory. Two deployments, one program: beside
the game for single-player, LAN and split-screen, or on its own for a large
content collection. Nothing inside the program tells them apart — the difference
is which directory it mounts and who can reach it.

```sh
just serve                            # the staged directory, beside the binary
just serve --root ./content           # a directory of your own
./.cache/build/dev/cdn/cdn --help
```

### Options

```
--root DIR   Directory to serve content from (default: beside the binary)
--verbose    Log at trace level
--help       Show this text
```

### What happens today

**Nothing is served.** The program mounts the root, reports it and says so:

```
[info]    cdn: content root /home/you/game/content
[warning] cdn: nothing is served — the manifest is Engine::assets and the
          origin's HTTP layer is Engine::net, and neither has landed.
```

A root that is missing or is not a directory is refused at start-up with exit
code 1, rather than being accepted and failing one request at a time.

What exists is `cdn::ContentRoot` — the boundary between a content name and the
filesystem. It refuses traversal by default, and both of its checks are
load-bearing: components are checked before the disk is touched, which catches
`..` and absolute names but cannot see a symlink, and the resolved path is then
checked for containment, which catches the symlink. A symlink that stays inside
the root is served, because an atomically swapped `current` is the deployment
pattern this is for.

The design — content addressing, the hierarchical hash, grants, and how groups
are streamed so a game builds progressively — is `CDN.md` in the design notes.
`ROADMAP.md` puts the rest at v0.8.

### Proving it needs no graphics stack

```sh
just check-cdn-is-bare
```

Configures with no client and no server, builds, and fails if the staged `cdn/`
directory has grown a `shaders/` folder or if another program was built into a
cdn-only preset. The tier rule is what actually enforces this — `mono.cdn` is
`shared` tier, and a `shared` target may link only `shared` — so a presentation
module reaching this link line fails the configure with the edge named. The
recipe is the version of that anybody can see without a graph query.

---

# The tools

## The test runner

```sh
just test                                     # only what your change affected
just test-all                                 # everything
just test-list                                # what it would run, and why
./.cache/build/dev/tools/testrunner --help
```

```
--build DIR   A configured build directory
--cache PATH  Cache file (default .cache/smart-tests.txt)
--report DIR  Where test-output.md/.html go (default .cache)
--no-report   Write no documents
--all         Run every suite, cache or not
--list        List suites and signatures, run nothing
--verbose     Name every skipped suite
```

It prints what it skipped, and warns when it had to narrow — no header closure
for a suite, an unknown `TEST_DEPENDS`, a dependency cycle. Those warnings mean
the cascade is covering less than it claims to, so do not ignore them.

To force a full re-run, either pass `--all` or delete `.cache/smart-tests.txt`.
That file is text: one tab-separated line per suite, so you can also delete a
single line to re-run one thing.

`just preset=server test` runs the server preset's suites with the server
preset's runner — the preset override reaches `--build` too, so the binaries and
the build directory cannot come from different presets.

## The report

Every run writes `.cache/test-output.md` and `.cache/test-output.html`, and
prints both paths. `--list` writes nothing, because it ran nothing.

Sections are the suite identifiers themselves: `engine.core.arguments` sits
under `engine`, then under `engine.core`, and is a row of its own. There is no
second taxonomy to keep in step with the first.

Both documents cover **every** suite, not only the ones this invocation ran. A
suite the cascade skipped shows its last known numbers and says `pass (cached)`
in its result column — a green row you cannot tell from a row nothing
re-checked is a green row that lies by omission. The counts and durations behind
those rows live in `smart-tests.txt`, which is why it is at `v2`; a `v1` cache is
discarded on sight rather than reported as a tree of suites holding no tests and
costing no time.

The numbers come from the test binaries. `mono.build/testmain` registers a
Catch2 reporter named `mono` that writes one tab-separated line per test case,
and the runner asks for it alongside the console reporter — so the output you
read when something goes red is unchanged, and the report is a record of the run
rather than a parse of its console text. You can ask a binary for it directly:

```sh
./.cache/build/dev/tests/test_core --reporter "mono::out=-"
```

Neither document carries a timestamp. Two runs that learned the same thing
produce the same bytes, so a diff shows what moved rather than that it was
written again.

`ctest` writes no report. It never consults the cache either — see below.

### Timings

The runner prints what each suite cost as it goes, and both documents carry the
same numbers — `Time` and `Slowest case` columns in the Markdown tables, and a
flamegraph in the HTML.

**Two clocks, measuring two different things.** A suite is timed by the runner,
from the outside, so its number includes starting the process and running its
static initialisers. A case is timed by the reporter, from the inside. A suite
is therefore always wider than its cases add up to, and that gap is real: it is
the fixed price of a suite existing, paid once per suite per run.

That gap is the first thing the report is good for. If `engine.core.arguments`
costs 300 ms and its slowest case costs 29 µs, none of that 300 ms is a test.

`Time` beside `Slowest case` is the other diagnosis. A slow suite whose slowest
case is most of it is one pathological test; a slow suite whose slowest case is a
fraction of it is a lot of ordinary ones. Those want different fixes.

### The flamegraph

`test-output.html` opens with one. Width is wall-clock, depth is the identifier
one component at a time — everything, then `engine`, then `engine.core`, then the
suite, then its cases. Hover a box for its name and what it cost. Widest first,
so what is worth looking at is where you are already looking.

It is nested `div`s with percentage widths and no script, so it stays
proportional when you resize the window and renders on a machine with no network.

A suite the cascade skipped is a **leaf** — washed out, with no cases under it.
`smart-tests.txt` is one line per suite, so it keeps a suite's total and not its
breakdown; one line per *case* would be a different file with a different
contract. Run `just test-all` for a graph that goes all the way down.

## Everything, always

```sh
cd .cache/build/dev && ctest --output-on-failure
ctest -N            # list without running
ctest -R ecs        # only suites matching a pattern
```

`ctest` never consults the cache. Use it in CI and before a pull request; use
`just test` in the inner loop.

## One suite, directly

Test binaries are ordinary Catch2 executables:

```sh
./.cache/build/dev/tests/test_ecs                        # all of it
./.cache/build/dev/tests/test_ecs "a component round-trips"
./.cache/build/dev/tests/test_ecs "[scheduler]"          # by tag
./.cache/build/dev/tests/test_ecs -# "[#Store]"          # by source file
./.cache/build/dev/tests/test_ecs --help                 # Catch2's own options
```

`-# "[#File]"` is how the runner re-runs exactly one suite, since a suite is one
file.

They also answer one option Catch2 does not know about:

```sh
./.cache/build/dev/tests/test_ecs --mono-suites
```

which prints `id <tab> source <tab> dependencies` for each suite it contains.
That listing is how the runner discovers what exists, rather than scanning
sources and hoping a regex holds.

## The architecture check

```sh
just test-architecture
```

It needs a *configure*, not a build — its input is the `target-graph.json` that
CMake emits. Directly:

```sh
cmake -DGRAPH=.cache/build/dev/target-graph.json \
      -DEXPECTED=mono.tools/architecture/expected_graph.json \
      -P mono.tools/architecture/CheckTargetGraph.cmake
```

This is not what enforces the tier rule — `mono_check_all_tiers` does that at
configure time and fails the build with the offending edge named. This checks
the graph against the checked-in expectation, so that an architectural change
shows up as a diff somebody reviews.

## Proving the server is headless

```sh
just check-server-is-headless
```

Configures with no client, builds, and fails if the staged `server/` directory
has grown a `shaders/` folder, or if a client got built into a server-only
preset. That is a link-line mistake anybody can see without a graph query.

## The API reference

```sh
just docs          # build it
just docs-serve    # build it and serve it on http://localhost:8000
just docs-check    # fail if a public entity is undocumented
```

`docs/CODE_DOCUMENTING.md` is how to write the comments that end up in it —
where each one lands, and the tags that are available.

The site lands in `.cache/build/<preset>/docs/html/`, so `just clean` takes it
with everything else derived. `just docs-serve 9000` picks another port.

**Needs `doxygen` on PATH, and `graphviz` for the diagrams.** Neither is a build
prerequisite — a machine without them builds and tests exactly as before, and
`just docs` says which one is missing and how to install it rather than failing
as a missing target.

```sh
sudo apt install doxygen graphviz   # Debian, Ubuntu
brew install doxygen graphviz       # macOS
```

Nothing in the sources carries a documentation marker. `mono.tools/docgen`
rewrites plain `//` into the `///` Doxygen reads as the file goes past, so the
comments stay prose and the reference is generated from them. Run it by hand to
see what Doxygen is being shown:

```sh
.cache/build/dev/tools/docgen mono.engine/core/include/engine/core/Name.hpp
```

`mono.tools/docgen/AGENTS.md` is the reasoning, including why the line count is
an invariant and why the filter is scoped to `*.hpp`.

## Counting the lines

```sh
just linecount                                  # the whole repository
just linecount mono.engine/render               # one module
just linecount mono.engine --files              # a row per file as well
just linecount > LINECOUNT.md                   # keep it
```

Empty, comment and code across every `.cpp`, `.hpp`, `.h`, `.inl` and their
relatives, as a markdown table. Directory rows are grouped two path segments
deep — so a walk of the root reports per module — and `--depth 3` splits each
one into its `src/`, `include/` and `tests/`.

**`mono.vendor` and every dot-directory are excluded.** The vendored libraries
are an order of magnitude more code than the engine, so counting them answers a
question about somebody else's project; `.cache/build/<preset>/` holds generated
headers, so counting it makes the number depend on which presets happen to be
configured. `--vendor` includes the first, and `--exclude TEXT` drops anything
else by substring.

The classification is a scan of the whole file rather than a test on each line,
because two things outlive a line — an open `/* */` and an unterminated raw
string. A blank line is empty wherever it is, including inside a comment block;
a line with code and a comment after it is code; and the GLSL in a `R"(...)"`
is code, not comment. `mono.tools/linecount/include/linecount/Counter.hpp`
states the rules, and `tests/Counter.cpp` is where each one is pinned.

---

# Tracy

The Tracy client is compiled into every build and is **on-demand**: it collects
nothing until a profiler attaches, so leaving it in costs a predictable branch
and nothing else. It listens on localhost only.

The profiler UI is not built by our CMake. Build it from the vendored source
once:

```sh
cmake -S mono.vendor/tracy/profiler -B .cache/tracy -DCMAKE_BUILD_TYPE=Release
cmake --build .cache/tracy -j
```

Then run it, and start the program you want to capture:

```sh
just run --stats --uncapped
```

Because collection only starts on attach, a short run that ends before you
connect records nothing. When that matters, make the program wait:

```sh
just run --enable-profiler 30 --frames 600 --uncapped
just host --graph --unpaced --ticks 5000
```

`--enable-profiler SECONDS` blocks until a profiler connects or the timeout
passes, and says which happened. It needs a value: bare `--enable-profiler` is
an error, because keeping "takes a value" and "does not" as separate
declarations is what makes a missing value an error rather than a silently
swallowed next argument.

Tracy and the F5 overlay are fed by the same macros. Tracy is the real one —
every thread, full history, a second process. The overlay exists for the case
Tracy cannot cover: seeing a frame breakdown inside the running game, with
nothing attached, on a machine that is not yours.

---

# Exit codes

The same for every program here:

| Code | Means |
|---|---|
| `0` | it worked |
| `1` | it started and then failed — a missing shader, a bad tick rate, a failing suite |
| `2` | the command line was wrong — unknown option, missing value |

`2` is worth distinguishing in a script: it means the invocation is wrong, so
retrying will not help.

---

# Running from somewhere else

The staged directories are self-contained. This works:

```sh
cp -r .cache/build/dev/client /somewhere/else
/somewhere/else/client --stats
```

The binary finds `libSDL3.so.0` and `shaders/render/` beside itself, not
relative to the working directory. If you need it to read data from elsewhere —
pointing a release build at a working tree, or a test at a fixture directory —
use `--override-assets-directory`.

That flag is read once at startup, before anything loads a file. Setting it
later would leave whatever had already loaded pointing at the old tree, so there
is no way to change it while running.
