# Code architecture

**Where a thing goes, and what it is allowed to see.** `docs/CODE_FORMAT.md`
says how code looks and `docs/CODE_QUALITY.md` is the checklist a change is
reviewed against. This is the third one: the shape the modules make, the rule
that orders them, and the reasoning that produced it.

Read it when adding a module, moving a type between modules, adding a
dependency edge, or arguing about which module a thing belongs to. Nothing here
is about a single function.

## Its relationship to `repo_layout.md`

Sixty-two comments in this repository cite `repo_layout.md` by section number,
from CMake, from public headers, from `.gitmodules` and from
`THIRD_PARTY_NOTICES.md`. That document is not in this repository and never has
been. It lives with `CDN.md`, `RENDER_PIPELINE.md`, `MCP.md` and
`DATATYPES_LIBRARIES.md` in the sibling `atomic-game-engine-hidden-docs`, which
`docs/retired/ROADMAP.md` records and nothing else does.

**This file is the in-tree half of it, and it is deliberately not a copy.**
`repo_layout.md` is a hundred and seventeen kilobytes of design written before
most of this engine existed; it argues positions and records rejected
alternatives. What follows is the part a contributor needs in front of them,
reconciled against the tree as it is at v0.19, with the drift between the two
named rather than smoothed over. Section numbers below are this file's own. Where
a rule comes from a `repo_layout.md` section, the section is cited so the
argument behind it can still be followed.

---

## 1 · The trust model, because it decides more than it looks like it does

**Anyone can run a server.** That is the single most-cited line in the design
notes - `repo_layout.md` §1, fifteen of the sixty-two citations - and it is the
reason a dozen unrelated-looking pieces of this engine are built the way they
are.

It means there is no trusted direction on a wire. A client's packet is hostile
because a hostile client is trivial to write; a *server's* packet is hostile
because anybody can stand one up and point a player at it. So:

- Every field of an inbound packet is validated before it reaches simulation.
  `mono.engine/net/include/engine/net/Packet.hpp:9` says so at the definition.
- A frame header may claim any length, so a length is bounded before it is
  believed. `mono.engine/delivery/include/engine/delivery/GroupCodec.hpp:25`.
- A manifest, a WAV, a content directory and a source list are all content an
  origin served, not data the engine produced.
  `mono.engine/assets/include/engine/assets/Manifest.hpp:265`,
  `mono.engine/audio/include/engine/audio/Wav.hpp:14`,
  `mono.studio/src/ContentSources.cpp:80`.
- A client that is told to fetch from an arbitrary host is a vulnerability, not
  a feature. `mono.engine/delivery/AGENTS.md:96`.

**Decoding is the largest attack surface in a game engine and this engine
decodes untrusted bytes in six modules.** If a change adds a seventh, the
question to answer in the pull request is not "does it parse" but "what does it
do with a length field that is a lie".

---

## 2 · The shape of the tree

```
mono.build/            the tier system, MonoLibrary.cmake, the shared test main
mono.engine/           libraries, and nothing else. No main.cpp lives here
mono.client/           the client library and its thin main
mono.server/           the server library and its thin main
mono.studio/           the editor
mono.launcher/         the launcher
mono.cdn/              the content origin
mono.network/          finding a peer, and being findable
mono.discord/          rich presence
mono.tools/            build-time and developer tools, one directory each
mono.unified_tests/    the cross-module suites
mono.vendor/           submodules, and the CMake that tames them
```

**`mono.engine/` is never a product.** No `main.cpp` lives under it and nothing
ships from it alone. Every program picks the subset of modules it needs, and §7
below is the table of which.

**Tools do not live in a central `tools/` directory of their own invention.**
`mono.tools/AGENTS.md:88` records the rule from `repo_layout.md` §3: each tool
arrives with the thing it serves, in its own directory, with its own tests.

---

## 3 · Inside a module

Every directory under `mono.engine/` has the same shape, and the shape is
enforced by `mono_add_library` rather than trusted:

```
mono.engine/ecs/
├─ CMakeLists.txt
├─ AGENTS.md                    module-local invariants and what not to do
├─ include/engine/ecs/          public headers, and only public headers
├─ src/                         sources and private headers
├─ shaders/                     GLSL owned by this module, optional
├─ tests/                       one file per public header
└─ benchmarks/                  optional
```

Headers are included as `<engine/ecs/Column.hpp>`. The `engine/` prefix names a
role and stops two modules colliding on a common file name.
`mono_add_library` puts `include/` on the public include path and `src/` on the
private one, so **a header in `src/` is unreachable from another module.** That
is the point: it gives every module a surface smaller than its contents, which
is what keeps a monorepo from becoming one program with folders.

**The public/private split is the cheapest architectural tool in the repository
and the most under-used.** Before adding a header to `include/`, ask whether any
other module will include it. If the answer is no, it belongs in `src/`, and a
test that needs it can link the module's `src/` directly - that is what the
private include directory is for.

**The module `AGENTS.md` carries invariants; the root one carries policy.** The
per-module file is where "change detection works like this here, do not add a
second dirty flag" belongs, and it is the file most often skipped and most often
would have helped.

---

## 4 · The layer stack

> **A layer may see every layer below it and none above it.**

"Layer" is a height, not a directory, and it is a *ceiling on what a module may
see* rather than a measurement of what it happens to see today. `collision` sits
at L5 and includes nothing above `core/types`; the layer says what it would be
allowed to reach for, which is what makes it a design statement rather than a
derived number.

The reading order is the comment column in `mono.engine/CMakeLists.txt`. The
machine-readable copy is the `layer` field on every module in
`mono.tools/architecture/expected_graph.json`, and §8 below describes what
checks it.

### 4.1 · The stack as built

| L | Modules | Tier | What the layer is |
|---|---|---|---|
| 13 | `control` | shared | the control surface |
| 12 | `render` `ui` `input` `audio` | client | presentation |
| 12 | `replication` `network` | shared | what the wire means, and finding a peer |
| 11 | `net` `delivery` `discord` | shared | transport, fetching, presence |
| 11 | `resources` `msl` `nodegraph` | client | built-in GLSL, SPIR-V to MSL, a node canvas |
| 10 | `game` `examples` | shared | what a game file is |
| 9 | `script` `graph` `bake` `bakegraph` | shared | running a script, what is drawn, importing |
| 8 | `assets` `physics` `effects` | shared | content addressing, simulation |
| 7 | `scene` `gui` | shared | what a thing in a world is, what a 2D thing is |
| 6 | `spatial` | shared | where things are |
| 5 | `collision` | shared | hulls and triangle soups |
| 4 | `world` | shared | universe, worlds, ticking |
| 3 | `ecs` | shared | columns, sets, change channels |
| 2 | `parallel` | shared | jobs, threads, processes, ipc |
| 1 | `core` | shared | platform and values |

Above L13 is the **program band**: `client`, `server`, `studio`, `launcher`,
`cdn`, `loadtest`, `unified_tests`, and the tool libraries `assetc`, `docgen`,
`linecount`, `shadercheck`, `testrunner`. These have no layer, and that absence
is itself checked: **nothing that has a layer may link something that has not.**
A program-band entry is the thing that links modules and that no module links.

### 4.2 · Where the built stack and the designed one disagree

`repo_layout.md` §5.1 draws a different stack, and the difference is worth
knowing before somebody "corrects" one of them.

| Designed | Built | What happened |
|---|---|---|
| L13 `script` | L9 `script` | `script` binds the object model and the simulation, and both sit below it. It never needed the top of the stack, so it stopped claiming it |
| L12 `vfx` | L8 `effects` | as designed, `render` and `vfx` were siblings and `render` needed `effects`, which is a lateral edge. Demoting `effects` made the edge downward, which is the better fix |
| L10 `physics` | L8 `physics` | same reason: `script` and `graph` both read it from L9 |
| L6 `ledger` **[server]** | not built | no global commit log exists |
| L5 `persistence` | not built | no storage layer exists |

**The two unbuilt layers are the finding.** L5 and L6 were reserved in the
design for `persistence` and `ledger`, and in the built tree those numbers are
occupied by `collision` and `spatial`. Decision 7 in §9 below still says
`persistence` is shared and the datastore surface is server-only, and the
roadmap still schedules datastores. **When that module arrives it has nowhere to
land**, and the choice at that point is a renumber of eleven modules or a
fractional layer. Deciding now costs a paragraph; deciding then costs a
migration.

Eleven modules that exist today appear nowhere in the designed stack:
`collision`, `spatial`, `gui`, `bakegraph`, `examples`, `delivery`,
`replication`, `control`, `resources`, `msl` and `nodegraph`. Their placement
above is this file's, not `repo_layout.md`'s.

### 4.3 · The leaf band, and why it does not exist yet

`repo_layout.md` §4.1 and decision 22 reserve `mono.libraries/` for leaves: a
library that may depend on the STL and `core/types` and on other leaves, and on
nothing else. No platform layer, no logging, no `ecs`. The section also sets the
bar for creating the directory: **"create it at three libraries, not one"**,
because a constraint written against zero examples is a guess.

Counting what each candidate actually includes:

| Module | Includes from `core/` | Vendor | Leaf? |
|---|---|---|---|
| `collision` | `types/AABB`, `types/Vector3` | none | yes |
| `spatial` | `types/{AABB,Ray,Vector3}`, `Name` | none | yes, with `core::Name` |
| `msl` | nothing | SPIRV-Cross | no, it carries a vendor |
| `nodegraph` | nothing | imgui | no, and see below |
| `resources` | `Paths` | none | no, it reads the filesystem |

Two leaves is not three. **`mono.libraries/` should not be created yet**, and
this paragraph exists so that the next person to notice `collision` is a leaf
does not create it either. The trigger is a third one.

`nodegraph` is a different problem and a real one. It uses `std::thread`,
`std::atomic`, `std::condition_variable` and `imgui.h`, and the only thing in
the repository that links it is `studio`. It is a studio widget library living
in `mono.engine/`. See §6.3.

---

## 5 · Tiers

Three tiers, checked at configure time by `mono_check_all_tiers` in
`mono.build/MonoLibrary.cmake:42`, which fails the build with the offending edge
named.

| Tier | May link | Means |
|---|---|---|
| `shared` | `shared` | in every program |
| `client` | `shared` `client` | presentation and input |
| `server` | `shared` `server` | authority and hosting |

**A tier is not a layer.** The tier decides which binaries a module can appear
in; the layer decides what it may see. `render` and `script` are both reachable
from the client, but `script` is `shared` and `render` is `client`, which is why
`script` cannot bind a renderer type directly and why the server contains no
graphics stack at all.

That last property is proved rather than asserted: the `server` preset
configures with `MONO_BUILD_CLIENT` off so SDL and the shader compiler are never
configured, and `just check-server-is-headless` fails if the staged `server/`
directory grows a `shaders/` folder.

**`ALLOW_TIER_ESCAPE` is almost never the fix.** If a change needs an edge the
tier check refuses, the design is saying something.

---

## 6 · The dependency rule

### 6.1 · Downward, or sideways by name

An edge from a module at L*n* may go to any module at a layer strictly below
*n*. An edge to a module at L*n* itself is a **lateral edge**, and lateral edges
are allowed only where the module's `lateral` array in `expected_graph.json`
names the target. Three exist:

| Edge | Why |
|---|---|
| `bake` → `bakegraph` L9 | `bake` runs the pipeline `bakegraph` describes. The split is that `bakegraph` carries no decoders, so a game file's bake pipelines travel without them |
| `delivery` → `net` L11 | `delivery` frames its fetches with `net::Packet` rather than inventing a second framing |
| `ui` → `render` L12 | `ui` implements `render::FrameOverlayHook`. Both are `client`, and `mono.engine/ui/CMakeLists.txt:23` argues the case at the edge |

Adding a fourth means editing a checked-in file, which puts it in front of a
reviewer. That is the entire mechanism.

### 6.2 · Three edges that are load-bearing and easy to break

**`assets` must not depend on `scene`.** The natural way to write an asset layer
is to let it hand back instances, which makes it depend on the object model,
which drags the class registry and eventually the script VM into anything that
reads a manifest. `assets` links `core` and nothing else
(`mono.engine/assets/AGENTS.md:9`), which is why `mono.cdn` can serve content
without linking the engine, and why the `cdn` preset configures on a machine
with no Vulkan SDK.

**`graph` must not depend on `render`.** `graph` is the node runtime; `render`
is one of the node sets that runs on it. The edge running the other way is what
makes decision 12 possible - one graph library, N node sets, audio among them -
and `mono.engine/audio/include/engine/audio/Graph.hpp:10` is where somebody
already had to explain why the audio graph is not `engine::graph` yet.

**No layer reaches up, even to report a number.** `net` does not include
`script` to report a statistic. That is what `core::Metrics` is for: the
reporter never names the reader.

### 6.3 · What is allowed to cross

Four mechanisms cross layers, and each is a named mechanism rather than a call.

| Mechanism | Lives at | Crosses | Why it is safe |
|---|---|---|---|
| ECS columns and change channels | L3 | between siblings, and upward to any reader | data, not calls. No sibling names another |
| `core::Metrics` sink | L1 | any layer reports; a tool reads | the reporter never names the reader |
| The ordered bus | L4 | between worlds | copies at tick boundaries. No pointers |
| The commit stream | L6 | *not built* | reserved for `ledger` |

And four that must not:

- **No pointer crosses a world boundary.** This is what keeps thread-per-world
  and process-per-world interchangeable. One shared pointer added because "it is
  only threads today" ends the process option permanently, and the thing that
  ended it is invisible.
- **No shared change list between systems.** A colour write must not wake the
  broadphase.
- **No lateral include that is not named in §6.1.**
- **No layer reaches up.**

The first and last are the two that are cheap to break and expensive to fix,
because breaking either compiles, passes tests, and shows up as an architecture
that can no longer be changed.

---

## 7 · What each program links

`O` is a direct or transitive first-party link. Generated from
`expected_graph.json`; if it disagrees with the build, the build is right and
this table is stale.

| module | L | tier | client | server | studio | launcher | cdn | loadtest | tests |
|---|---|---|---|---|---|---|---|---|---|
| `core` | 1 | shared | O | O | O | O | O | O | O |
| `parallel` | 2 | shared | O | O | O | O | O | O | O |
| `ecs` | 3 | shared | O | O | O | O | . | O | O |
| `world` | 4 | shared | O | O | O | . | . | O | O |
| `collision` | 5 | shared | O | O | O | O | . | O | O |
| `spatial` | 6 | shared | O | O | O | O | . | O | O |
| `gui` | 7 | shared | O | O | O | O | . | O | O |
| `scene` | 7 | shared | O | O | O | O | . | O | O |
| `assets` | 8 | shared | O | O | O | O | O | O | O |
| `effects` | 8 | shared | O | O | O | O | . | . | O |
| `physics` | 8 | shared | O | O | O | . | . | O | O |
| `bake` | 9 | shared | . | . | O | . | . | . | . |
| `bakegraph` | 9 | shared | O | O | O | . | . | O | O |
| `graph` | 9 | shared | O | O | O | O | . | O | O |
| `script` | 9 | shared | O | O | O | . | . | O | O |
| `examples` | 10 | shared | O | O | O | . | . | . | O |
| `game` | 10 | shared | O | O | O | . | . | O | O |
| `delivery` | 11 | shared | O | O | O | . | O | . | O |
| `discord` | 11 | shared | O | O | O | . | O | . | O |
| `msl` | 11 | client | O | . | O | O | . | . | O |
| `net` | 11 | shared | O | O | O | . | O | O | O |
| `nodegraph` | 11 | client | . | . | O | . | . | . | . |
| `resources` | 11 | client | O | . | O | O | . | . | O |
| `audio` | 12 | client | O | . | O | . | . | . | O |
| `input` | 12 | client | O | . | O | . | . | . | O |
| `network` | 12 | shared | O | O | O | . | O | . | O |
| `render` | 12 | client | O | . | O | O | . | . | O |
| `replication` | 12 | shared | O | O | O | . | . | O | O |
| `ui` | 12 | client | . | . | O | O | . | . | . |
| `control` | 13 | shared | . | O | O | . | . | . | O |

Three rows are worth reading twice.

**The `server` column is the headless proof.** It has no `render`, `ui`,
`input`, `audio`, `msl`, `resources` or `nodegraph`. Any change that puts an `O`
in one of those cells has made the server contain a graphics stack, and
`just check-server-is-headless` is the recipe that notices.

**The `cdn` column is the "partial engine" proof.** Seven modules, none of them
above L12, and no `ecs`. A content origin does not know what an entity is. The
moment somebody adds a convenience function that needs one, `mono.cdn` starts
linking the engine and the claim stops being true.

**Three modules are absent from the `tests` column**: `bake`, `nodegraph` and
`ui`. They are covered only by their own `tests/` directories and by nothing
cross-module. That is a gap rather than a policy.


## 8 · Domain-driven design, as this engine actually does it

**A module is a bounded context.** That is the whole mapping, and everything
else follows from it. The layer is the context's ceiling, the `include/`
directory is its published language, `src/` is its internal model, and
`AGENTS.md` is where its invariants are written down.

### 8.1 · The ubiquitous language is Roblox's, deliberately

`docs/CODE_FORMAT.md` puts public data members in Pascal case and spells out
why: this engine is for Roblox developers and the scripting surface is Pascal on
both sides of the binding. That is a domain decision wearing a formatting rule's
clothes. `CFrame`, `Instance`, `Humanoid`, `SurfaceGui` and `BillboardGui` are
not names this engine chose; they are the language its users already speak, and
matching it is worth more than any internal consistency it costs.

**The consequence is that "what should this be called" is usually not an open
question.** If Roblox has a name for the concept, that is the name, and a
divergence needs a reason written next to it.

### 8.2 · Where two contexts claim the same noun

This is the DDD question worth actually answering, and the tree has four cases.

| Noun | Claimants | The real split |
|---|---|---|
| world | `world` L4, `scene` L7, `game` L10 | `world` owns the tick and the universe of worlds. `scene` owns what a thing *in* a world is - the instance tree, the class registry, serialisation. `game` owns what a *file* is. Three different questions that all say "world" in English |
| graph | `graph` L9, `bakegraph` L9, `nodegraph` L11, `audio::Graph` | `graph` is the node runtime. `bakegraph` is a pipeline *description* carrying no decoders, which is what lets a game file travel with its bake pipelines and without glTF. `nodegraph` is an editor canvas. `audio::Graph` is a fourth one that decision 12 says should eventually be `graph` and is not yet |
| ui | `gui` L7 shared, `ui` L12 client | `gui` is the widget tree: retained, serialisable, replicable, headless-testable. `ui` is the drawing of it. See §9.2 - this is the cleanest port in the repository |
| net | `net` L11, `network` L12, `replication` L12 | `net` is transport and framing. `network` is discovery - finding a peer and being findable. `replication` is what the bytes mean |

**None of these four is an accident and none should be merged.** They are
recorded here because each looks like duplication until you know the split, and
a contributor who does not know it will put a type in the wrong one.

### 8.3 · The anti-corruption layers, and the one that matters most

**`assets` refuses to know what an instance is.** It links `core` and nothing
else. It handles GUIDs, content addressing, chunking, manifests and the virtual
filesystem: bytes, and only bytes. `bake` is the layer that knows about bytes
*and* the object model, and it is a separate module for exactly this reason.

Without the split, the content origin links the whole engine. With it,
`mono.cdn` links seven modules, none above L12, and the `cdn` preset configures
on a machine with no Vulkan SDK. **That is a bounded context boundary paying for
itself in a build configuration**, which is the most concrete form the argument
ever takes.

The same shape appears twice more: `bakegraph` is the description without the
decoders, and `msl` translates SPIR-V without knowing whether a build tool or a
running engine asked.

### 8.4 · Where a new thing goes

In order. Stop at the first one that answers.

1. **Does an existing module already own this noun?** Put it there. Check §8.2
   first, because the noun may be claimed by a module whose name does not
   suggest it.
2. **Does it need to be seen outside the module that will hold it?** If not, it
   is a header in `src/` and this decision is over.
3. **What is the highest layer it must see?** Its own layer is at least one
   above that. If that answer is above the module you wanted to put it in, the
   module was wrong.
4. **Which programs must contain it?** That fixes the tier. If the answer
   includes the server and it draws, something has gone wrong at step 1.
5. **Is it a leaf?** STL and `core/types` only, no logging, no filesystem, no
   component reads. If so see §4.3, which currently says to leave it in
   `mono.engine/` and note it.
6. **Only then, is it a new module?** A new module is an architectural change:
   it needs a row in `expected_graph.json` with a `layer`, an `AGENTS.md`, a
   `tests/`, and a paragraph in the pull request saying what it owns that no
   existing module does. `/new-module` scaffolds the mechanical part.

---

## 9 · Ports and adapters, as this engine actually does it

Hexagonal architecture asks that the domain not name its infrastructure. This
engine honours that, and mostly **not** with abstract base classes.

### 9.1 · The primary port is a column, not an interface

Root `AGENTS.md` rule 2: the ECS owns the storage. `repo_layout.md` §7 says the
same thing from the other side: *when two subsystems need to share data, the
data is an ECS column and the coupling is a change channel.*

That is ports and adapters implemented with data. `physics` writes a column;
`render` reads it; neither includes the other's headers and neither could name
the other if it wanted to. The seam is a component type at L3, which both can
see because both are above it.

**This is why the ECS is the most important architectural object in the
repository and why widening its public surface is expensive.** Every component
type in `include/engine/ecs/` that the bindings expose is a shipped API that
users write against. Keep the bound subset small and deliberate; everything else
goes in `src/`.

### 9.2 · The cleanest classical port in the tree

`gui` is `shared`, sits at L7, links `core`, `ecs` and `parallel`, and knows
nothing about drawing. It produces a flat `gui::DrawList`. `ui` is `client`,
sits at L12, and turns that list into draw calls.

`mono.engine/ui/CMakeLists.txt:31` states the property that makes it a port:
*"`gui` is the engine's own widget tree and knows nothing about this module,
exactly as `render` knows nothing about it. What arrives here is a flat
`gui::DrawList` - no store, no tree, no class table - which is what lets a
second backend draw the same list without either learning about the other."*

**A widget tree that can be laid out, serialised, replicated and asserted on
with no GPU in the process is the payoff**, and it is why `gui` is in the
`server` column of §7 and `ui` is not.

### 9.3 · The other ports, and their adapters

| Port | Adapters | Where |
|---|---|---|
| `net::Transport` | loopback, socket | `mono.engine/net/include/engine/net/Transport.hpp` |
| `render::FrameOverlayHook` | `ui`, and the studio's own overlays | `mono.engine/render` |
| `core` platform backends | per-OS, behind one interface (decision 8) | `mono.engine/core` |
| `graph` node sets | render nodes, bake nodes, and audio eventually (decision 12) | `mono.engine/graph` |

`net::Transport` is the one to study, because it is the port that decision 6
depends on. Single-player runs a loopback transport **with real encoding**, so
the bytes a solo session moves are the bytes a networked one moves. That makes
`repo_layout.md` §16.6 honest rather than aspirational, and
`mono.engine/net/tests/Transport.cpp:176` is the test that keeps it so.

### 9.4 · The test of a hexagon is whether the domain runs headless

Here it is not an argument, it is a preset. The `server` preset configures with
`MONO_BUILD_CLIENT` off, so SDL, the shader compiler and every `client` module
are never configured at all. Everything that builds in it is domain; everything
excluded is adapter.

`docs/CODE_QUALITY.md` asks for both presets to configure and build, and both
suites to pass, for exactly this reason: **a `client`-tier dependency that a
`shared` module picked up by accident only fails in the second one.**


## 10 · Transports, and which one each feature should be on

**Every wire in this engine is TCP or UDP today, and the split is deliberate
rather than accidental.** This section records what each feature actually needs,
so that a QUIC transport lands where it earns something instead of everywhere.

| Feature | Today | What it needs | Should be |
|---|---|---|---|
| Replication deltas | UDP, unreliable | unordered, loss-tolerant, latency-critical, many small messages | **QUIC DATAGRAM** (RFC 9221). The gain is per-datagram acknowledgement, which the hand-rolled window does not give |
| Structure, inputs, RPC, join snapshot | UDP, **one shared reliable window** | ordered and reliable, but a megabyte snapshot must not stall a door opening | **a QUIC stream each.** This is the one that matters: they share a window today, so head-of-line blocking is a property of the design |
| Audit | UDP, unreliable, deliberately | loss is fine | unchanged |
| Studio live sync | the replication link | occasional small messages, mostly keep-alives | unchanged - it rides whatever replication rides |
| Discovery and rendezvous | UDP broadcast, and a borrowed socket | tiny, loss-tolerant, and bound to the port NAT punched | **unchanged.** QUIC rides the punched port; the discovery itself has no reason to move |
| Asset fetch | TCP HTTP/1.1, **one connection per fetch**, capped at 16 | few large transfers, and one slow group must not hold up the others | **HTTP/3**, and read `net/http/Client.hpp:18-23` before touching it. One connection per fetch is deliberate: multiplexing onto one HTTP/1.1 socket would reintroduce the head-of-line blocking N connections exist to avoid. HTTP/3 is right precisely because its streams keep that property without N connections; the comment leaves the question open |
| Relayed content | the replication reliable channel | bulk, on a latency-critical wire | **its own stream.** `ContentMode::Redirect` already exists as the escape |
| CDN serving | TCP | few large transfers | HTTP/3, as the sibling of asset fetch |
| Control surface (MCP) | loopback TCP | one local client | unchanged. A loopback socket has no congestion problem to solve |
| Discord presence | a local unix socket | a desktop application on the same box | unchanged, and `discord` deliberately does not link `net` |

**The through-line: sixteen TCP connections and one shared UDP reliable window
are both workarounds for not having streams.** Everything in the "should be"
column is one mechanism replacing two workarounds, which is why the QUIC entry
on the roadmap is worth its cost and why doing it feature-by-feature would not
be.

**What does not change is the send rate.** `D00014` recorded this and v0.15
proved it: congestion control shipped without QUIC, because the algorithm is a
property of the send rate and the send rate is ours whatever carries the bytes.
A QUIC transport inherits Copa rather than replacing it.

### 10.1 · The seam it has to arrive through

`net::Transport` is a real port: six virtual functions, every consumer takes it
by reference, and the asio types stay inside three files. **`replication::Session`
already holds a `net::Transport *` rather than a socket**, so which transport a
session runs on is a question the type can already be asked. That is further
along than it looks.

**What is concrete is the layer above the transport, not the transport.**
`Session` owns a `net::Link`, a `ReliableSender`, a `ReliableReceiver` and its
`Sealer`/`Opener` pair as members. Those are exactly the pieces QUIC would
supply itself - streams, acknowledgement, and TLS 1.3 - so a QUIC session is not
"a `Transport` with a different `Send`", it is a session that skips four of its
own members. Deciding whether that is a second `Session` implementation behind a
new interface, or a `Session` whose reliability pieces become optional, is the
first design question of the work rather than the last.

**Answered at v0.19, and it is the first of the two.**
`replication::SessionPort` is the interface; `replication::Session` and
`replication::QuicSession` are the two implementations, and
`ListenerSettings::Wire` chooses. The argument against the second option is in
`SessionPort.hpp`: a `Session` whose four members become optional is one class
with two modes, and every method on it grows a branch that only one
configuration exercises - which is `docs/QUIC.md` §8's "two overlapping
reliability stacks is worse than either" applied inside a single type. Those
members are not incidental state, they *are* the design, and a QUIC session does
not have a different version of them, it has none.

What the interface deliberately excludes is as much of the answer as what it
holds. No `AdoptKeys`, because QUIC's keys are the handshake's own and one
implementation would refuse it for ever. No `net::Link`, because reaching
through a session to a link is being coupled to one reliability design - the
three things every consumer actually wanted from it (the round trip, the send
allowance, whether the link is up) are named on the port instead, and
`Connector::Link()` is now a pointer that is null under QUIC so the absence is
something a caller looks at rather than walks into.

`net::http::Client` is the other port, and it already has a non-socket
implementation in the content relay - so HTTP/3 fits behind it after one
widening, for stream identity.

---

## 11 · What is checked, and by what

Root `AGENTS.md` rule 6: *a rule the build does not check is documentation.*
This is the current honest accounting.

| Rule | Checked by | When |
|---|---|---|
| Tier edges | `mono_check_all_tiers`, `mono.build/MonoLibrary.cmake:701` | configure |
| Module set, tiers, exact link sets | `mono.tools/architecture/CheckTargetGraph.cmake` | `just test-architecture` |
| **Layer edges: downward, or lateral by name** | the same file, `_check_layers` | `just test-architecture` |
| **A layered module linking the program band** | the same | `just test-architecture` |
| That the architecture check still bites | `mono.tools/architecture/tests/`, six fixtures | `just test-architecture` |
| The server stages no shaders | `just check-server-is-headless` | `just check` |
| The cdn stays bare | `just check-cdn-is-bare` | `just check` |
| Every object records its headers | `just deps-check` | manual |
| Public headers are documented | `just docs-check` | `just check` |
| Formatting | `just format-check` | `just check` |

The layer rows are new at v0.19. Before then `docs/CODE_QUALITY.md` asked a
reviewer to *"check the layer heights by hand"*, which is exactly the third
category rule 6 refuses to allow.

**What is still not checked, and is therefore convention:**

- That a public header is actually public. §3's rule is judgement.
- That a module does not keep a private copy of data the ECS owns. Root
  `AGENTS.md` rule 2 is a review question.
- That nothing crossing a world boundary is a pointer. Rule 3 is a review
  question.
- That a `Name` is serialized as its string and never as its `Id()`. Rule 4 is a
  review question, and decision 21 is the reasoning.

Those four are the highest-value checks that do not exist. Anyone with an
appetite for tooling should take them in that order.

---

## 12 · The decisions

From `repo_layout.md` §16. A decision is settled until its revisit condition
fires; nothing is reopened because somebody new dislikes it. The full argument
for each, with the rejected alternatives, is in that document.

| # | Decision | Reopen when |
|---|---|---|
| 1 | One repository | the delivery service gets its own on-call |
| 2 | The engine is libraries. Programs are thin mains | never |
| 3 | Tiers are checked at configure time | never |
| 4 | Two scripting languages, native semantics, one binding set. Behavioural parity is a non-goal; capability parity is CI-enforced | never |
| 5 | World boundaries are process boundaries by design | never |
| 6 | Single-player uses a loopback transport with real encoding | the encode cost is measured and material |
| 7 | `persistence` is shared. The datastore surface is server-only | never |
| 8 | Platform backends live in `core`, behind one interface | a closed platform lands |
| 9 | Scene identifiers are random 128-bit, allocated at creation | never |
| 10 | Studio links for authoring and drives a child process for play | never |
| 11 | User shaders compile at cook time. No shader compiler ships on the client | every target platform permits runtime compilation *and* a game needs generated shaders |
| 12 | `graph` is one library, N node sets, audio included | physics needs a `PipelineKind` enum |
| 13 | Ledger reads are projections. Worlds never block on it | never |
| 14 | Determinism is strict IEEE `f32`/`f64`, simulation confined to fixed-width 128-bit lanes | a target platform cannot honour strict IEEE |
| 15 | Hand-authored configuration is TOML. JSON is interchange only | an external consumer requires JSON on a file a human edits |
| 16 | A surface may ship complete and frozen with its implementation deliberately unwired | never - it is a state, not a stage |
| 17 | The renderer is native-first. WebGPU is a declared lower tier | WebGPU gains custom MSAA resolve and programmable sample positions |
| 18 | Deferred shading, a partial depth prepass, MSAA over a correct edge stencil | a target platform cannot express sample-frequency shading |
| 19 | LOD selection targets quad utilization. No virtualized geometry | measurement on this engine's own content contradicts it |
| 20 | Render graphs may vary per platform. Anything reaching a simulation input may not | never - decision 14 restated where it gets violated |
| 21 | A name crosses boundaries. A number does not | never |
| 22 | Leaf libraries live in `mono.libraries/`. `mono.engine/` holds subsystems | a leaf needs the engine, which means it was not one |
| 23 | Work inside a tick may be parallel. Work across ticks may not | a system cannot fit in a tick even parallel |
| 24 | First-party code is unoptimised by default. `release` is the one preset that opts in | never |

Decisions 7 and 22 are the two with no implementation behind them today. §4.2
and §4.3 record what that costs.
