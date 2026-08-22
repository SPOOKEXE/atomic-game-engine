# mono.engine - module invariants

**This directory holds libraries and nothing else, and is never a product.** No
`main.cpp` lives under here and nothing ships from it on its own. Every program
selects the subset of modules it needs, and `docs/CODE_ARCH.md` §7 is the table
of which.

This file carries what is true of *every* module here. The invariants that
actually catch mistakes are in each module's own `AGENTS.md`, and the root
`AGENTS.md` carries policy. **Read the module's file before changing anything in
it.** Skipping that is the most common way to produce a change that compiles,
passes, and is wrong.

Architecture questions - where a new thing goes, what it may see, why `scene`
and `world` and `game` are three modules - are `docs/CODE_ARCH.md`. That
document also carries the layer stack, which used to live only in the comment
column of `CMakeLists.txt` beside this file.

---

## The modules, and what each one owns

The order is the layer order. `CMakeLists.txt` beside this file carries the same
list as its reading order; `mono.tools/architecture/expected_graph.json` carries
the machine-readable copy that `just test-architecture` enforces.

| L | Module | Tier | Owns |
|---|---|---|---|
| 1 | `core` | shared | the platform and the value types. `CFrame`, `Vector3`, `Colour`, the clock, the log, `Name`, `FrameGraph` |
| 2 | `parallel` | shared | jobs, threads, processes and the pipe between them |
| 3 | `ecs` | shared | the storage. Columns, sets, change channels, the scheduler, thread affinity |
| 4 | `world` | shared | the universe of worlds, the tick, and the ordered bus between them |
| 5 | `collision` | shared | hulls and triangle soups. Geometry, with no notion of an entity |
| 6 | `spatial` | shared | where things are. Hash grids, chunk maps, layer masks, queries |
| 7 | `scene` | shared | what a thing *in* a world is. Row shapes, the class registry, per-world name tables, serialisation |
| 7 | `gui` | shared | what a 2D thing is. The **game's** authored interface, as components whose enum order is the save format |
| 8 | `assets` | shared | content addressing. GUIDs, chunking, manifests, the virtual filesystem. Bytes, and only bytes |
| 8 | `physics` | shared | shapes, integration, pairs, the solver |
| 8 | `effects` | shared | particles, beams, trails |
| 9 | `script` | shared | running a script. Two VMs, one binding set, the sandbox, the debugger |
| 9 | `graph` | shared | the node runtime. One graph library, N node sets |
| 9 | `bakegraph` | shared | what a bake pipeline *is*, carrying no decoders |
| 9 | `bake` | shared | importing and baking content. The glTF, OBJ, PMX, PNG and WAV readers |
| 10 | `game` | shared | what a game file is. Reads and writes a `.agame` |
| 10 | `examples` | shared | the example scene every program can load |
| 11 | `net` | shared | transport and framing. Split by trust direction, not by feature |
| 11 | `delivery` | shared | fetching content, and the cache it lands in |
| 11 | `discord` | shared | what a person's Discord profile says while a program runs |
| 11 | `resources` | client | the engine's own built-in GLSL |
| 11 | `msl` | client | SPIR-V to Metal Shading Language |
| 12 | `replication` | shared | what the wire means. Snapshots, deltas, ownership, priority |
| 12 | `network` | shared | finding a peer, and being findable |
| 12 | `render` | client | the device. Render nodes, the RHI, the frame |
| 12 | `ui` | client | the **editor's** toolkit. Dear ImGui lives here and nowhere else |
| 12 | `audio` | client | the mixer graph and spatialisation |
| 12 | `input` | client | devices and action mapping |
| 13 | `control` | shared | the control surface a tool drives a running program through |

Two pairs are routinely confused and are not the same thing:

- **`gui` is the game's interface; `ui` is the editor's.** `gui` is L7 `shared`,
  links `core` and `ecs`, carries no vendor and runs on the headless server.
  `ui` is L12 `client` and is the only target that makes Dear ImGui public.
  `mono.client/CMakeLists.txt:18` says it outright: "`Engine::ui` is the
  editor's toolkit and is deliberately absent."
- **`graph`, `bakegraph` and `nodegraph` are three different things.** `graph`
  is the runtime that executes nodes. `bakegraph` is a pipeline *description*
  with no decoders in it, which is what lets a game file carry its bake
  pipelines without carrying a glTF reader. `nodegraph` is an editor canvas and
  is **not here** - it was `mono.engine/nodegraph` until v0.19 and is
  `mono.studio/nodegraph` now, because the editor was the only thing that ever
  linked it.

---

## What is true of every module here

**The shape is the same everywhere, and `mono_add_library` enforces it.**
`include/engine/<module>/` is public and `src/` is private, so a header in
`src/` is unreachable from another module. Before adding a header to `include/`,
ask whether another module will include it. If not, it belongs in `src/`, and a
test that needs it can link `src/` directly.

**A component belongs to the ECS, not to a module's private vector.** Two copies
of one fact drift the first time one of them is updated inside a branch. Every
component also needs a line in `mono.tools/componentdoc/purposes.md`, or `just
components-check` fails.

**A name crosses boundaries; a number does not.** Register a component with an
explicit string. `Components::Of<T>()` mints one from the compiler's spelling of
the type, which is stable within one build and nothing wider - and a `.agame`
outlives a compiler. `WorldTime` is currently in that state and is a defect.

**Nothing crossing a world boundary is a pointer.** It is a message carrying a
copy. This is what keeps thread-per-world and process-per-world interchangeable.

**Work inside a tick may be parallel; work across ticks may not.** And parallel
is not free: below a crossover it is slower, the crossover is higher than it
looks, and the number belongs in a comment. Measure in `release` - the `dev`
preset is `-O0` and a timing from it means nothing.

**Nothing in `mono.engine` may depend on a program.** `client`, `server`,
`studio`, `launcher` and `cdn` are the program band; the layer check fails an
edge from a layered module into it, by name.

**A vendor type in a public header widens what the whole engine can see.**
`VENDOR_PUBLIC` exists for the cases where a type genuinely has to propagate,
and every use of it is a decision to argue in the pull request. There are few:
`SDL_Event` in `input`, imgui in `ui`, and the SDL GPU handles `render`'s
resident-resource tables hand out.

---

## The four things a module may not do, restated because they compile

A change that breaks any of these builds, passes its tests, and is wrong.

1. **Reach up a layer.** Checked as of v0.19. `net` does not include `script`,
   even to report a number - that is what `core::Metrics` is for.
2. **Include a sibling at its own layer** without naming the edge in its
   `lateral` array. Checked as of v0.19.
3. **Keep a second copy of data the ECS owns.** Not checked. A review question,
   and the one most often answered wrong.
4. **Put a pointer in something that crosses a world boundary.** Not checked. A
   review question, and the most expensive of the four to undo.
