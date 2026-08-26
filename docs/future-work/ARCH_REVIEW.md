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

### A2. `graph::NodeCatalogue` hands out pointers the lock does not protect **[verified, fixed]**

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

**Done at v0.19.** `Find` carries the contract `All` already had, and both now
say the mutex does not extend past the return. The half worth writing down was
the *second* failure mode rather than the first: because `Specs` is sorted, a
later `Register` can leave an old pointer naming a **different kind without
dangling**, which looks entirely correct at the use site. `engine.graph.pipelinecatalogue`
gained "a later Register moves what an earlier Find named", which demonstrates
the shift by index rather than by pointer, because a case that read a stale
pointer to prove the point would be the same mistake wearing a `CHECK`.

### A6. `parallel::Jobs`' pool destroys its condition variables with waiters on them **[verified, open]**

`Jobs::Get()` is `static Pool pool;` (`parallel/src/Jobs.cpp:78`), so the pool is
destroyed by an exit handler. Its four `std::condition_variable`s
(`Jobs.cpp:55-60`) are destroyed while every worker is still parked on
`Available`, and glibc's `pthread_cond_destroy` **waits for the last waiter**.
Any process that calls `Jobs::Start` and does not call `Jobs::Stop` therefore
runs its whole program, returns zero from `main`, and hangs for ever in `exit`.

Reproduced from a core: the main thread is in `futex_wait` under
`__GI___pthread_cond_destroy` under `Pool::~Pool` under `__run_exit_handlers`,
with twenty-three workers still in `Available.wait`.

**It is invisible because every existing caller happens to pair the two.** Six
test suites and six benchmark suites own their pool through a `struct Pool {
Start; ~Pool { Stop; } }` at namespace scope, and `mono.client` and
`mono.server` stop theirs on the way out. The first suite in this pass that
started a pool and did not stop it hung `test_replication` at exit and, through
`just test-all`, the shared build lock behind it - which reads as a hung test
rather than as a clean run with a stuck teardown.

**Do:** the same thing `ecs::Components`, `ecs::ChunkPool` and `core::Log`
already do for their own exit-order problems - `static Pool *pool = new Pool();`
and never destroy it. The process reclaims the memory, `Jobs::Stop` still joins
whoever asks, and a caller that forgets it gets a clean exit instead of a hang.
The reasoning is already written out three times in this tree at
`ecs/src/Components.cpp:41` and `ecs/src/ChunkPool.cpp:47`.

Not applied this pass: `parallel` was not this task's module, and the header
touch would have rebuilt the tree under four other agents.

### A3. Unbounded microtask loop in the JavaScript host **[verified, fixed]**

`script/src/JavaScriptRuntime.cpp:226-235` drains `JS_ExecutePendingJob` without
a bound, so a self-re-enqueuing microtask hangs the host inside one tick. The
step budget does not catch it because both interrupt handlers zero their counter
on trip (`JavaScriptRuntime.cpp:38`, `LuauRuntime.cpp:91`), giving every job a
fresh budget. The same reset makes `StepsTaken()` report `0` for the script that
used the most, which is a second bug in the same three lines.

**Reproduced, then fixed at v0.19, and the reproduction is the interesting
half.** A probe against the vendored QuickJS with the shipped interrupt handler
drained **52,000,000 jobs and polled the handler 31,201 times in 120 seconds**,
because QuickJS polls once per ten thousand safepoints. Against the default
200,000,000-step budget that is roughly twenty years to trip, so the step budget
was never going to catch it whatever the reset did.

`RuntimeLimits::JobBudget` bounds the drain by a **count**, not a deadline, so
`just determinism` and `just replay-check` stay byte-identical. Both interrupt
handlers stopped zeroing on trip: the counter is monotonic and every host entry
(`Run`, `Heartbeat`, `Invoke`, `Surface`) moves a mark instead, which fixes the
fresh-budget-per-job bug and `StepsTaken()` reporting zero for the script that
spent the most. `Surface` is included because a tripped runtime stays tripped and
would otherwise hand back an empty completion list after a runaway script.

The citations were one off: the drain is `:226-236`, the JavaScript reset was
`:37` and the Luau reset `:91`. The files are now `scriptjs/` and `scriptluau/`
(see C5).

### A4. `scene::InputState` mouse behaviour was last-write-wins across worlds **[verified, fixed]**

`Client.hpp` kept a copy of `scene::InputState::Behaviour` and
`MouseIconEnabled`, written by `Client::WriteInput` - which runs once per
simulated world per frame plus once for the replica - so the window obeyed
whichever world was entered last. Root `AGENTS.md` rule 2 exactly.

**Reproduced with a real `--worlds 2` run**, against a script whose two VMs draw
different randoms and therefore ask for different modes. Before: world 0 asked
`LockCenter` and world 1 asked `Default`, and the window stayed `Default`; with
the two swapped the window took world 1's `LockCenter`. After: the window takes
world 0's answer in both.

**The fix is that the client stops keeping the copy.** `Client::PumpEvents`
reads both fields out of `InterfaceWorld()`'s store at the point the window call
is made - the same world the interface layout, the router and `gui::Type`
already use, and the one that owns the input focus. There is exactly one answer
per frame and `Client::InterfaceWorld` now says so. `AppliedPointerMode` and
`AppliedPointerIcon` remain and carry an `arch-waiver ecs-copy`: they are the
record of a system call, no store holds what SDL was last told, and SDL has no
getter for it.

A `pointer:` log line now names the world that asked, because a client obeying a
world the player is not standing in is what this used to do and nothing said so.

**Found beside it, and now fixed - A4b.** `client::InstallControls` created a
world's `scene::InputState` *after* the scene script had run, so a top-level
`UserInputService.MouseBehavior = ...` in a `--script` scene was silently
dropped and only a write from a `Heartbeat` took. Reproduced exactly:
`--headless --script`, a top-level write reading back `Default` and a tick-three
write reading back `LockCenter`.

Three properties were losing writes, not one - `MouseBehavior`,
`MouseIconEnabled` and `MouseDeltaSensitivity`, the last onto
`scene::CameraController`. Both resources are now created by
`client::InstallInputResources` before `examples::LoadScene` runs the chunk, and
`InstallControls` calls the same function; it is idempotent under
`HasResource`, so nothing is created twice and a second `SetResource` cannot
throw the script's write away.

**And the drop is loud now, which is the half that mattered.** The setters still
refuse to mint an `InputState` - the presence of that resource is how a world
says somebody is looking at it, and a script on a dedicated server must not be
able to declare a window - so the write is still dropped there. It now says so
through `script::ReportDroppedWindowWrite`, at warning level and throttled to
one line every five seconds with a suppressed count, because the honest failure
shape is a script setting the pointer mode every `Heartbeat` on a server. Case:
`client.scene.tick`, "a top-level pointer-mode write survives the install".

### A5. `CommandQueue::Post`'s result was discarded at every call site **[verified, fixed]**

Counted: fifteen sites in `mono.client/src/Sounds.cpp`, all discarding the
return while recording the state as landed. One transient full queue became
three different permanent faults, and "check the return" is not one policy for
them.

`audio/Commands.hpp` now states the contract in three classes, per command kind:

- **Coalescable** (`SetGain`, `SetPan`, `SetMuted`, `SetLooping`,
  `SetPlacement`, `SetListener`) - the value is stated rather than the edge, so
  the repair is the next pass noticing the difference again. The producer must
  record what it *posted*, never what it meant to post.
- **Repairable** (`AddNode`, `Connect`, `SetSound`, `Play`, `Rewind`) - half a
  voice is a player wired to nothing and the undo would itself be commands, into
  the queue that just refused. `CommandQueue::Free()` is new and lets a producer
  reserve the burst before it starts.
- **Terminal** (`Stop`, `Disconnect`, `RemoveNode`) - nothing later reposts
  these, because the row that would have noticed is the one being torn down.

`SoundStage` meets all three. `Open` reserves five commands or seven and returns
nothing when it cannot, so the row keeps no voice and the next pass builds it.
`Close` reserves too and a refusal puts the voice in `PendingCloses()` to be
retried at the top of the next pass. Every last-posted value is written only
after `Post` returned true, `Voice::Started` repairs a dropped `Play` against a
fresh deadline, and `Refused()` counts what this world's sounds lost. The drop
is reported through `ENGINE_WARN_EVERY(5.0, ...)`, which is the right tool for a
per-frame failure.

`SetListener` was also being posted unconditionally every pass, which was one
command per world per frame saying where the ear already was - the one place in
that file that broke the rule the rest of it is built around. It now has a
last-posted value like everything else.

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

- ~~`parallel/AGENTS.md` claims `process/` and `ipc/` do not exist yet.~~
  **Fixed at v0.19, and understated.** Both have existed since v0.2 and
  **neither was ever a directory**; nor was `jobs/`. All four rows of that
  table were wrong about the layout. The second half of the finding is wrong
  in a different way: **`Jobs.cpp:103` never appeared in the file at all**, nor
  any other `file:line`. The prose pointed at `Retire`, describing a pool the
  join rewrite ended. `:219` is right, and the sharper answer is that the
  mutex is the point rather than the line: `Pool::Guard`, taken twice per woken
  worker per batch at `:198` and `:219`. Measured at about **1.1 us a woken
  worker against 44 ns a range**, which makes `Jobs.hpp`'s claim that cost is
  "linear in the ranges now and not in the pool" measurably false. That file is
  now 363 lines and audits all 24 primitives.
- ~~`launcher/AGENTS.md` says "do not move a decision into `Interface.cpp`". Six
  are already there.~~ **Fixed at v0.19, and it undercounts: there are nine**,
  three of them duplicating a filter loop the tab beneath ran again. They moved
  rather than the rule narrowing, because unlike the `game` and `scene`
  precedents above the launcher already had a home for them (`Plan.hpp`, with
  `tests/Plan.cpp`). `Interface.cpp` went 803 to 730 lines and the launcher's
  suites 38 to 45 cases. The rule keeps its sentence, gains the definition of
  "decision" it never drew, and says out loud that nothing checks it.
- ~~`mono.client/AGENTS.md:5-9` says the directory holds attachments and the test
  is whether a second program would want them.~~ **Fixed at v0.19**, with the
  count corrected to five studio headers plus three in `mono.unified_tests` and
  the test restated as the one actually being applied. The same pass added the
  account of `Client::PumpContent` the file never had. **One half of the finding
  was wrong**: `PumpContent` is 323 lines and is not the largest piece of logic
  in the directory - `Client::Step` is 1,177 lines and was already described in
  three sections. What was true is that the content pump had no account at all.
- ~~`script/AGENTS.md:10` carries one stale claim.~~ **Fixed at v0.19.** The
  stale claim was the whole sentence: it said `Runtime.hpp` forward-declares
  `lua_State` and holds a pointer. `Runtime.hpp` contains no such declaration
  and the base class holds no VM pointer at all; the forward declarations live
  in the private `LuauRuntime.hpp` and `JavaScriptRuntime.hpp`.
  `script/CMakeLists.txt:13-14` carried the same sentence and is also fixed.
- `mono.server/AGENTS.md` is stale on rewind and silent on both networking and
  content.
- ~~the QUIC survey called `net` L2. It is L11.~~ Fixed at v0.19, with the
  same line gaining `Vendor::ngtcp2`.
- ~~`README.md:34` says "four rules"; there are six.~~ Fixed at v0.19.
- ~~`docgen/pages/Modules.md` lists ten of the engine's twenty-nine modules.~~
  **Fixed at v0.19 by making the page generated rather than by correcting it.**
  `mono.tools/architecture/WriteModulePages.cmake` walks the tree for every
  `AGENTS.md` and orders them by layer; `just docs-pages` writes it and
  `just docs-pages-check` is in `just check`. **41 pages**, against the sixteen
  the hand-written file actually carried.

---

## C · Things in the wrong place

### C1. `nodegraph` is in the wrong monorepo member **[verified, fixed]**

It was `client`-tier, sat in `mono.engine/`, linked nothing first-party, used
`std::thread`, `std::atomic`, `std::condition_variable` and `imgui.h`, and
**exactly one thing in the repository linked it: `studio`**. Every part of that
was re-checked before the move: the only first-party include anywhere under the
module was `engine/testing/Suite.hpp` in its own `tests/`, `imgui.h` appeared in
`src/Inspectors.cpp` and `src/Editor.cpp` and no public header, and `studio` was
the sole `links` entry naming it in `expected_graph.json`. It is an editor
widget library.

Moved to `mono.studio/nodegraph/` at v0.19. `Engine::nodegraph` is
`Mono::nodegraph`, `<engine/nodegraph/X.hpp>` is `<nodegraph/X.hpp>` and
`engine::nodegraph` is `nodegraph`, which is the convention every other
non-engine library here already follows. **Its row lost its `layer` rather than
keeping L11**, which is the part that is worth more than the directory: the
program band has no layer and `CheckTargetGraph.cmake` refuses an edge from
anything that has one to anything that has not, so an engine module linking this
now fails `just test-architecture` by name. At L11 it would have been legal for
`ui` at L12 to pick it up and nothing would have said so.

### C2. `Renderer::RenderView` is 5,485 lines **[verified, fixed]**

`render/src/Renderer.cpp:8032-13516`, inside a 13,517-line translation unit -
two fifths of the module in one function. It holds its node handlers as lambdas
and calls `SDL_BeginGPURenderPass` inline in eighteen places, which is what makes
those passes invisible to the render graph.

**It is simultaneously the module's build cost and its architecture problem**,
because one enormous translation unit cannot be split across cores. Splitting
`RenderView` by node family, into files that each implement `GraphRunner` for
one family, fixes both with one change. That is the argument for doing it as one
change rather than two.

**Split at v0.19, and two of this entry's numbers were wrong.**

`RenderView` is 60 lines now: build a `ViewRequest`, `Begin`, register eight node
families, `Finish`. `Renderer.cpp` went 13,680 to **1,702**. The state the
handlers closed over is `ViewRecording`, whose members *are* the locals: `Begin`
binds a reference per name and writes through it and each handler binds the same
names back, so nothing is published from a local into a member and there is no
second copy to drift. That also kept the moved code textually identical, which is
what allowed the split to be verified mechanically rather than by reading.

**Three of the eighteen `SDL_BeginGPURenderPass` "calls" were `ENGINE_ERROR`
strings naming the function.** There were fifteen. Thirteen are now inside a node
family's runner or inside a shared operation called only from one. Two cannot
move, and the reasons are the design: the host chrome pass is recorded *after*
`output-image` precisely so graph previews and authored captures hold only the
game image, and the clear of a window nothing touched has no node to live in
because its whole condition is that none ran.

**The 31.2 s was measured under 420% of stolen CPU and before E1 landed.** The
same translation unit measures **10.6 s** today, confirmed three ways, so the
critical path available here was about 7 s rather than 22. Compiling the module
at `-j24` with the cache off and unity off went **11.02 s wall over 22 units to
6.46 s over 38**, and the slowest unit in it from 10.56 s to 3.83 s. CPU seconds
went the other way, 34.9 to 76.3, which is sixteen more files parsing the shared
private headers; wall clock is what a developer waits on.

Public header widening is one line, `friend class ViewRecording;`, so
`Renderer::Impl` stays in `src/`. No SDL header and no third exception. Five
deterministic scenes capture byte-identically before and after.

### C3. `mono.client` used three modules it did not declare **[verified, fixed]**

`Engine::assets`, `Engine::graph` and `Engine::script` were included from public
headers - `client/Client.hpp`, `client/Scene.hpp`, `client/Replicated.hpp` - and
appeared nowhere in `mono.client/CMakeLists.txt`'s `DEPS`. All three are now
declared, on the library and - for `Engine::assets`, which `app/main.cpp` names
itself - on the program too.

`Engine::script` and not `Engine::scripthost`: nothing in this directory calls
`MakeRuntime`, and the worlds are opened through `Engine::examples` and
`Engine::game`, which sit above the adapters and name the factory themselves.
The row says so, because the next person to see `script::Runtime` in a header
will otherwise reach for the wrong one.

`just test-architecture` still passes and `expected_graph.json` did not change:
all three were already in the client row's closure, which is exactly why nobody
noticed. Two stale layer labels in the same file's comments were corrected while
there - `examples` and `game` moved to L12 with the `script` split.

### C4. `mono.client` is a de-facto shared library **[verified, charter corrected]**

Counted rather than estimated: `mono.studio` includes **five** `client/` headers
across `Editor.hpp`, `Editor.cpp`, `Worlds.cpp`, `Network.cpp` and
`PlayLink.cpp` - `Scene.hpp`, `Replicated.hpp`, `ContentDemand.hpp`,
`EditableImages.hpp`, `EditableMeshes.hpp` - and `mono.unified_tests` includes
`Scene.hpp`, `Replicated.hpp` and `ContentLink.hpp`. So a second program wants
most of this directory.

**The charter was what was wrong.** `mono.client/AGENTS.md` asked "would a
second program want it", which was already false when it was written and which
read literally would empty the directory. It now states the test that is
actually being applied: does it need a device, a window or a swapchain, and is
it the *program*'s decision or the *presentation*'s. Two programs drawing one
world through two collectors is the failure that arrangement prevents, and it
has already happened once inside this directory between `Scene.cpp` and
`Replicated.cpp`.

**`BuildMeshData` is the clearest case and it stays**, with the argument written
down beside it. It is device-free - checked, it names `scene` and `assets` and
nothing else - but `scene` is L7 and cannot host it, `assets` is the file
formats and giving it a `scene` edge to host one function inverts a clean
module, and a module of its own is a row in `expected_graph.json`, a layer, a
tier, an `AGENTS.md` and a suite for eighty-five lines with two callers that
both link `Mono::client` already. What would change it is a third caller that
cannot.

### C5. `script` is 32,981 lines and should be four modules **[verified, fixed]**

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

**Done at v0.19.** Four rows: `script` at L9 with **no vendor at all** (55
files, 13,925 lines), `scriptluau` at L10 (10,851), `scriptjs` at L10 (8,686) and
`scripthost` at L11, which is 64 lines of factory plus every suite that has to
see both VMs to check they agree. `game` and `examples` moved to L12, because
both call `MakeRuntime` and the factory sits above the adapters; that cost two
`layer` fields and no new edge.

The naming is `scriptluau` rather than `script.luau`. Dotted target names do
configure, and it was tried; every engine module here is one lowercase word
including `bakegraph` and `nodegraph`, and `mono_add_library` derives the
directory, the header prefix and the test target from the name.

**`mono_check_script_vm_naming` is deleted**, which was the point. It was sixty
lines of CMake closure walk re-deriving the VM boundary from filenames on every
configure; the tier and layer checks say the same thing for free.

**The split immediately found what the filename rule had allowed.** Five neutral
functions were defined inside Luau-named files and called by the JavaScript
adapter, legal under the old rule and a link error under the new one; they are
`script/src/Datatypes.cpp` and `PlayerSignals.cpp` now. And
`script/tests/SourceMap.cpp` had **no `TEST_SUITE_ID`**, so its eight cases had
never once been selected by the runner.

**One violation survived the split and only the unity build found it.**
`Vocabulary.cpp` called `LuauInstanceSignalNames`, declared at L9 and defined at
L10 - an upward edge invisible to both checks, because it is a bare symbol rather
than a link edge. It was harmless only because no header declared its *caller*,
so the archive never pulled the object; `UNITY_BUILD` made it live and stopped
`test_script` and `tools/bindings` linking. The caller was a second, unreachable
path to a list the editor already gets through `ScriptSurface::InstanceMembers`,
so it was deleted rather than rerouted.

**The build effect is small and is reported as such**: clean compile CPU across
the family went 40.9 s to 45.3 s with ccache off, all of it the five translation
units the split created. The payoff is architectural.

The survey's line count measures `src/` plus `include/` only: 88 files and
33,157 lines today, against 111 files and 49,316 for the whole module.

### C6. Smaller misplacements **[all five worked through, v0.19]**

- ~~`scene/src/SurfaceCameras.cpp` is 4,108 lines of portal and mirror maths, 36%
  of `scene`'s code budget. Lifts out cleanly as its own L7 module.~~ **Measured
  at v0.19 and refused: both numbers are wrong and the edges run both ways.** The
  file was 2,909 lines when measured and has never been 4,108 (its four commits
  read 2,607, 2,684, 2,895, 2,910); 4,108 is nearest to the file plus its header,
  4,343. The
  share is 22.7% of `scene/src`, 18.9% of `src` and `include` together, 22.0% of
  the module with tests. No reading of `just linecount` gives 36%. "Lifts out
  cleanly" is the reverse of what the graph says: five places in `scene` call
  into it, four sources and a *public header signature* -
  `ActiveCamera.hpp` declares `SeamMatrix(const SeamTransform &)`, which
  `render::ShadowNodes` calls through that header - while `SurfaceCameras.cpp`
  reads back down into `Character`, `Humanoid`, `SunOf`, `ActiveCamera`,
  `DrawInstance`, `SurfaceLens` and eight more. That is a cycle, so above L7
  those five call upward, below L7 the module cannot name what it is written in
  terms of, and lateral is one-directional by definition. Splitting out only the
  ~370 lines that take no `ecs::Store` does not help either: those types still
  carry `ecs::Entity` and `SurfaceLens`, so the leaf is not a leaf, and C7 and
  decision 22 already say the bar is three. The reasoning is written up in
  `mono.engine/scene/AGENTS.md`, together with the portal seam survey the read
  produced.
- ~~`spatial::CollisionGroups` is a naming registry and a policy matrix, tagged
  `@tier L2` in a module that is L6.~~ Reproduced at
  `spatial/include/engine/spatial/CollisionGroups.hpp:32`, and the tag was the
  error rather than the placement. The header includes
  `engine/spatial/LayerMask.hpp`, so it could not sit at L2 whatever the tag
  said, and its own comment argues at length that a name for a bit belongs
  beside the bits. Fixed to `@tier L6 · shared` at v0.19, matching the module's
  other four headers.
- ~~`ecs` carries a second bounded context - `Classes`, `Instance`, `Attributes`,
  the Roblox object model - that its `AGENTS.md` never acknowledges. That is why
  `Store.hpp` is 2,318 lines.~~ **The context is real; the causal half is
  wrong.** Measured as found, over the module's 27,126 lines of `.hpp` and
  `.cpp`, the object model is **6,586 of them, 24.3%** - and 507 of
  `Store.hpp`'s 2,317. The header is long because it is **1,591 lines of comment
  against 519 of code**, not because of what it declares. `ecs/AGENTS.md` now
  opens with the split, the argument for keeping the two in one module (a class
  *is* a `ComponentSet`, so there is no interface to draw), and the header rule
  that already keeps a column consumer out of the class tree: `Store.hpp` does
  not include `Classes.hpp`. Turning `Store`'s instance API into free functions
  was costed at about two thousand call sites in seven modules and not done.
  What changed is the cost rather than the length - `Store.hpp` preprocesses
  from 84,150 lines to **64,030** and `Classes.hpp` from 91,271 to **55,529**,
  on 216 and 179 translation units. See §E4.
- ~~`HttpService.cpp:80-660` is a 580-line hand-written JSON parser while
  nlohmann is vendored and used forty lines away in `SourceMap.cpp:5`.~~
  **The size is close and the comparison is not available.** The reader and
  writer are `:80-643`, 564 lines. But `SourceMap.cpp` is in `scriptjs` now, a
  different module one layer up, and `scriptjs` carries `Vendor::json` while
  `script` carries **no vendor at all** (C5) - so nlohmann is not forty lines
  away, it is unreachable, and "replace it with nlohmann" was never on the
  table. The header comment says so now.

  That left only the question of whether the hand-written half is *correct*, and
  it was measured one failure mode at a time rather than argued about. **Seven of
  eight were already right**: number round-tripping (sixteen awkward doubles
  including `5e-324`, `DBL_MAX` and `2^53+2`, all exact), surrogate pairs joined,
  lone surrogates refused in all four arrangements, raw control bytes refused
  and `\u0000` surviving as a byte, NaN and infinity refused both directions,
  and reader and encoder agreeing exactly on depth at 16 accepted and 17
  refused.

  **The number grammar was `strtod`'s, not JSON's.** Taking the maximal run of
  number-ish bytes and handing it to `from_chars` accepted `.5`, `-.5`, `1.`,
  `01`, `-01`, `00.5`, `1.e5` and `0100`; none of those is JSON. `ReadNumber`
  scans RFC 8259 itself now. In the same three lines, `1e-400` was refused as
  out of range, because `from_chars` reports underflow and overflow with **one
  error code** and only the overflow case had been argued; underflow to a signed
  zero is a value the writer accepts happily.

  And `{ [1] = "a", ["1"] = "b" }` encoded to `{"1":"a","1":"b"}` - two Lua keys,
  one JSON name, in an order `std::sort` does not promise. That is the
  determinism `Codec.hpp` §1 exists to protect, reached through the one case the
  sort alone does not cover, and **the message bus encoder had it too**. Both
  now share `Codec::SortEntries` and refuse with `CodecStatus::DuplicateKey`.
- ~~`input::Action`'s thirteen members are all profiler and HUD intents, in an
  engine module.~~ **Right, and moved at v0.19.** Thirteen *enumerators* and
  twelve actions (`Count` is the thirteenth); of the twelve, ten are profiler
  and HUD panel intents, `Quit` is program lifecycle and `ToggleWireframe` is a
  renderer debug toggle. The fact that decided it is one the finding did not
  state: **`mono.client/CMakeLists.txt:16` is the only line in the repository
  that links `Engine::input`**, and `Client.cpp` is the only consumer of
  `Action`. So `Actions.hpp`, its source, its suite and its benchmark are
  `client::Action` now, beside the profiler and the HUD they drive, and
  `mono.client` changed by three lines. The key-naming invariant survives and is
  restated as what it protects: a binding is one table, and there are still
  exactly two places an `SDLK_` appears. What is left in `input` is `Translate`,
  which is the half a game actually reads.

### C7. `mono.libraries/` should **not** be created yet **[verified]**

Decision 22 reserves it for leaves. Measured, there are two: `collision` (STL
plus `core/types` only) and `spatial` (the same, plus `core::Name`). `msl`
carries SPIRV-Cross and `nodegraph` carries imgui and four concurrency headers,
so neither qualifies. The bar is three leaf libraries, not one
(`docs/CODE_ARCH.md` §4.3). This entry
exists so the next person to notice `collision` is a leaf does not create the
directory either.

**Recounted after C1 and C6 landed, and the answer is unchanged.** Moving
`nodegraph` to `mono.studio` removed a candidate that was never a leaf, and
`CollisionGroups` was a tag fix rather than a module, so `spatial` still holds
it and is still a leaf with `core::Name`. Two, and the bar is three.

---

## D · ECS components

`docs/ECS_COMPONENTS.md` is now generated from the registry by `just
components`, and `just components-check` fails if a component has no purpose
line. **134 engine-registered components, all documented** - 129 when the tool
landed, plus the five that had no name to be found under until D1 gave them one.
What the generated table immediately showed:

- **Zero components are saved, raw-serialised and padded together.** That trio
  leaks uninitialised padding into `.agame` files. Clean today, and **checked
  since v0.19** - this bullet claimed it was checked and it was not. The tool
  printed the two columns and compared neither, so the rule was a paragraph.
  `componentdoc` now refuses the trio in both modes and names the component;
  proved by deleting `RigidBody::Reserved` and watching it refuse.
- **Three of 134 have a compact `ecs::WireFormat`**: `scene.Transform` (10 of 28
  bytes), `scene.Motion` (12 of 24), `ecs.Hierarchy` (8 of 40). Those are the
  three components a *system* writes every tick; everything else is
  signature-detected, so it costs bytes on the tick an author edits it and none
  on any other. `ecs.Hierarchy` is the exception that proves it: a signed
  component with a wire form, because a reparent is rare and a parent link is
  five entity handles. See D3.11 and D5 for what the widths actually looked like
  once measured.
- **Four tags exist** since v0.19: `scene.Simulated`, `script.Disabled`,
  `ecs.NotArchivable` and `scene.Transient`. A sweep of every registered struct
  found exactly one whose only member was a `Reserved` run, and that was it.
- The three largest rows are `effects.ParticleEmitter` (1264 B),
  `physics.PhysicsWorld` (1240 B) and `effects.Trail` (1152 B), and **none of
  the three replicates or saves at anything near its width** - which is what
  D3.3 and D3.4 turned out to be about. The widest row that *did* cross at full
  width was `gui.Gradient`: 672 bytes, of which 504 are the eighteen unused
  keypoint slots a default two-stop ramp leaves in each of its two sequences. It
  writes 77 now, measured.

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

### D3. Component changes worth making **[worked through, v0.19]**

The original survey reported 35 misplaced fields, 6 merges, 12 splits and 10
renames, and its ten clearest arguments are below with what came of each. **Four
of the ten were wrong**, and each was wrong in the same way: a component's
`sizeof` was read as what it costs a file or a wire, when the serialiser beside
it had already drawn the line. The list itself was redone from the registry
rather than taken on trust, and is D5 - it comes out at 35 fields, 7 merges, 14
splits and 12 renames.

1. **`scene.PortalTransitSeen` and `scene.RenderedSignature` to
   `LocalToTheClient`. Done, and the first was the live bug it was said to be.**
   `scene::SnapPortalTransit` collapses a replica's interpolation onto where a
   body arrived and takes the transit serial after it snaps; the authority runs
   the same pass and takes the same serial, so a replicated row arrived on the
   client in the very snapshot carrying the crossing it described. The client's
   own pass found `seen->Serial == went.Serial` on its first look and returned
   without snapping, and the body smeared from the room it left to the room it
   arrived in. `scene.RenderedSignature` is the same shape one layer over: the
   stamp means "the walk has already run against a tree that folds to this", and
   a client handed the authority's stamp skips a walk it has never run. Its own
   serialiser already refuses to persist it for that exact reason; the wire was
   the second door into the room and it was open. Both proved red first in
   `engine.replication.defaults`.
2. **`scene.TextContent` and `scene.ShaderSource` to `CannotBeSigned`. Done, and
   the drop is proved rather than described.** Both carry hand-written
   `Write`/`Read` pairs written expressly so the text could cross; both hold a
   `std::string`; and the gate is `Trivial`, not `Serialisable`. A new stand-in
   in the same suite - non-trivial, *with* a serialiser, not on the list - is
   dropped from the table, which is the mechanism reproduced without touching a
   real name. The gate is at `Defaults.cpp:406`, not `:404`.
3. **Split `physics::PhysicsWorld`. Declined - the premise is false.** The claim
   was "about forty members, about thirty-four of them per-step scratch, **all
   serialised**". `WritePhysicsWorlds` writes the cell size and the flag saying
   whether that size was measured: **five bytes**, and the same five for a world
   whose pair list, manifolds, rows and impulse caches hold four thousand entries
   each. `save: yes` in the catalogue means the type has a pair, not that the
   pair writes the type. `engine.physics.physicsworld` now measures it, so the
   next reader gets the number rather than the inference.
4. **Split `TrailHistory` out of `effects::Trail`. Declined - the premise is
   false and the split buys nothing.** "448 hot bytes inside a 1068-byte saved
   row": the saved row is **82 bytes**, measured. `WriteTrails` walks the
   authored fields and never touches the ring, and `ReadTrails` puts it back at
   empty, because a record of where something has been is a fact about a run.
   `engine.effects.ribbon` asserts a full ring and an empty one write byte for
   byte the same thing. The cache argument fails separately: both passes that
   touch a trail - `RecordTrails` and `BuildTrail` - read both halves.
5. **`effects.` into `SHARED_PREFIXES`. Declined, and it would have broken
   joining.** "Particles, beams and trails never reach a client" is true and is
   the design. `client::BuildReplicatedWorld` registers `scene`, `gui`, `script`,
   `client` and `replication` and nothing else - effects reach a `--game` client
   because the *world loader* registers them - and `ecs::LoadSnapshot` refuses a
   snapshot naming a component the build does not have. One `ParticleEmitter` in
   a server world would fail every join. Two more reasons behind that one: the
   handles inside `Beam`, `Trail` and `EmitterSlot` are dropped by their own
   writers, so the rows would arrive with no endpoints; and all three are
   trivially copyable, so all three would be hashed per row per tick, at 102,400
   emitters in `examples/StressParticles.luau`. The refusal is written down in
   `Defaults.cpp` beside the prefix list and in `effects/AGENTS.md`.
6. **Delete `scene::QuickHash`. Done.** No reader, no writer, no property, no
   binding, repo-wide - only its own registration, a name in a list, two trait
   assertions and four comments citing it as a pattern. Registered from v0.4, a
   column in every archetype and a row in every `.agame` schema, describing a
   comparison nothing ever made. The four citations now name `gui::Compiled` and
   `studio::HierarchyView`, which fold their own hashes and are what the pattern
   actually looks like in this tree. It is `Components.hpp:1591`, not `:1569`.
7. **Move `scene::Visual::Locked` to a studio tag. Declined - the finding is
   wrong twice.** Its reader is `Overlay.cpp:1459`, not `:1264`, and it is not
   the only one: `Locked` is a **public, writable `BasePart` property** in both
   the Luau and the TypeScript surfaces, declared four times in
   `bindings/manifest.json`, and `Tools.cpp` reads it back through the property
   system to label the ribbon button. `Part.cpp:2026`'s own comment already
   answers the proposal - "declared here rather than kept in the editor because
   it is authoring data that has to survive a save, which an editor-side set
   could not". It also costs nothing: it sits in `Visual`'s named padding.
8. **Extract a shared `RenderMaterial`. Declined - the fields do not line up.**
   The seven carriers are `effects::ParticleEmitter`, `effects::Beam`,
   `effects::Trail`, `effects::RibbonRun`, `gui::Surface`, `gui::Billboard` and
   `gui::SpatialCanvas`, with `render::ParticleBatch` an eighth. Only
   `ParticleEmitter` has all five. The subsets nest rather than repeat -
   {all five} then {LightInfluence, Brightness, ZOffset} then
   {LightInfluence, Brightness}, and separately {ZOffset, Additive} then
   {Additive} - so no subset is shared by more than two of the six components,
   and one flat struct would add unused columns to five of six. `Ribbon.hpp:96`
   also records the absence of `LightEmission` and `LightInfluence` on a beam as
   a decision, not an omission: "absent rather than declared and ignored, because
   the pass is unlit". A shared struct reverses it.
9. **Split `Humanoid`. Partly done, and the roadmap does not ask for it.**
   `ROADMAP.md` v0.23 is five plugin and universe bullets; there is no Humanoid
   entry in it or anywhere else in the file. What was real is one field:
   **`Humanoid::Radius` had two writers and no reader anywhere** - not in
   `physics`, `scene`, `client` or `studio`, and it was not a property in either
   language. The character sweep takes its width from `Collider::Extent`. Deleted,
   which took the four-byte `Reserved` run with it: `sizeof(Humanoid)` is 48.
   `Height` stays, because the ground ray's origin is `Position - Height/2` and no
   collider field says where the feet are; moving it to `Collider` beside
   `Extent` would be the second answer to one question that rule 2 refuses.
   `Health` stays on the row, because every pass that reads it - `IsDead`,
   `StepCharacters`, the sweep, `UpdateRespawns` - reads a movement field in the
   same loop body.
10. **Replace `SpawnLocation::TeamColour` with an entity handle. Declined.**
    "`PlayerTeam` already does it correctly" compares two different questions.
    Roblox's `Player.Team` **is** an `Instance` and Roblox's
    `SpawnLocation.TeamColor` **is** a `BrickColor`; this engine matches both
    exactly, and `Teams.hpp:21-26` already records the one place it has to
    differ - there is no palette here, so the compare is `Color3` against
    `Color3` within `TEAM_COLOUR_TOLERANCE`. Changing it would change the
    script-facing type of a public property from `Color3` to `Instance` and
    break four manifest entries and both declaration files, to remove a
    tolerance compare that is documented and tested.
11. **The survey, redone.** D5 is the full list, walked from the registry rather
    than taken on trust. Four items from it were applied beyond the ten above,
    and two of them are bugs the ten did not name:

    - **`scene::Visual::Surface` was written and read as eight bits.** The field
      was widened to `int16_t` at v0.17 expressly to lift a ceiling of a hundred
      and twenty-seven mirrors, and `WriteVisuals` went on calling `WriteInt8`
      until v0.19 - so every slot index from 128 up was truncated into every
      `.agame` and came back as a different pane or as -1. The case beside it
      never caught it because it used slot 3. `engine.scene.registration` now
      walks the whole width; four of its seven values failed before the fix.
    - **`gui.Canvas` crossed the wire and is derived locally.** `gui::Layout`
      writes it and `gui.Resolved` on the same collector in the same block, off
      the same local viewport, and only one of the two was excluded. The
      authority's screen rectangle reached every client and was overwritten by
      that client's next layout pass, which is verbatim the failure `gui.Resolved`
      is excluded for. `client.replica.arrival`'s exhaustiveness case did not
      catch it because its expected list was written from the same oversight.
    - **`gui.Gradient` replicated 672 bytes to carry 56.** A `core::ColorSequence`
      is twenty keypoint slots and a count, a `core::NumberSequence` is twenty
      more, and the component had no hand-written pair - so the generated writer
      copied all forty slots into every save and every delta whatever `Count`
      said, and a default two-stop ramp uses four of them. A hand-written pair
      honouring `Count` puts that default at **77 bytes**, measured and pinned by
      `engine.gui.registration`. That is the compact form the table was short of:
      an 88% cut on the widest row that actually crossed.
    - **`scene.Transient` is a tag.** Its only member was a four-byte `Reserved`
      that existed so the struct had a body. A snapshot still carries it -
      `ecs::WriteComponents` names the entity and writes nothing for a zero-sized
      component, which is how `scene.Simulated` already crosses a file.

### D4. Components the roadmap needs and that do not exist **[verified, fixed]**

~~No `Skeleton`, `Bone`, `Animator`, `Constraint`, `Terrain`, `LevelOfDetail`,
`Fog` or `Atmosphere` type exists anywhere. v0.21 and v0.23 both need several.~~

**Seven of the eight were missing. `Fog` was not, and adding one would have been
rule 2.** The engine has had fog since v0.16:
`LightingServiceComponent::FogColor`, `LightingServiceComponent::FogStart` and
`LightingServiceComponent::FogEnd` are authored on the `Lighting` service
(`scene/Services.hpp:109-134`), `LightingOf` resolves them into
`WorldLighting` (`scene/src/Sunlight.cpp:53-55`), and
`render/src/ViewRecording.cpp:1662-1668` puts them in a uniform. A `scene::Fog`
component would be a second answer to what a world's distance fade is, which is
exactly what `Humanoid::Radius` was deleted for in D3. What was genuinely
missing is `Atmosphere`, because linear fog cannot express scattering.

**The version numbers here are also stale against the current roadmap.**
Skinning, animation, humanoid and the bone controller are **v0.24**; ambient
occlusion, fog, atmosphere and clouds are **v0.22**; terrain, constraints and
level of detail are **FUTURE**. v0.21 is UI and particles and v0.23 is plugins
and universe work, so neither of the two versions this entry names needs any of
them.

**Ten types declared at v0.19 with nothing reading them**, which is decision 16
rather than a gap, and which had to be done now rather than later for a
mechanical reason: `Components::Seal()` runs at start-up on both programs, so a
type that registers lazily aborts the first host that meets a game file carrying
one. `scene.Skeleton`, `scene.Bone`, `scene.AnimationClip`, `scene.Animator`,
`scene.AnimationTrack`, `scene.Constraint`, `scene.LevelOfDetail`,
`scene.Atmosphere`, `scene.Clouds` and `scene.Terrain`, all under explicit names,
all with a replication decision argued beside the registration line, none leaking
padding.

The design judgement is in `docs/FUTURE_COMPONENTS.md`. The three worth naming
here: `Constraint` is **one generic six-degree-of-freedom joint** with seven
classes differing only by the prototype row `Instance.new` copies, and
deliberately has no `Kind` field, because one would make a `HingeConstraint`
whose axes are all free expressible; `LevelOfDetail` stores pixels of projected
area per triangle rather than a distance, because decision 19 says selection
targets quad utilization and a distance ladder answers differently for a tower
and a coffee cup at one range; and `Bone` derives from `Instance` rather than
Roblox's `Attachment`, because inheriting it would put two world frames on one
row resolved by two passes against two different parents.

---

### D5. The survey itself, redone from the registry **[verified, v0.19]**

Every struct registered by a `Components::Register<T>` call in `mono.engine`,
read against every reader and writer in `mono.engine`, `mono.client`,
`mono.server`, `mono.studio`, `mono.unified_tests` and the three script binding
artefacts. **35 field findings, 7 merge candidates, 14 split candidates, 12
renames.** Applied items are marked; the rest carry the reason they were not.

#### Misplaced fields (35)

Bugs, fixed:

1. `scene::Visual::Surface` written and read at eight bits, field is `int16_t`. **Fixed.**
2. `gui.Canvas` replicated while its twin `gui.Resolved` did not. **Fixed.**

Dead, removed:

3. `scene::QuickHash` - no reader, no writer, no property, repo-wide. **Deleted.**
4. `scene::Humanoid::Radius` - two writers, no reader, not a property. **Deleted.**
5. `scene::TransientComponent::Reserved` - the struct's only member. **Now a tag.**

Dead or near-dead, **not** removed and each for a stated reason:

6. `scene::RigidBody::LinearDamping` - no reader. It is a **writable public
   property** in Luau and TypeScript and `Components.hpp` claims physics applies
   it. Removing it breaks a published API; the honest fix is a drag term in
   `IntegrateMotion`, which is a physics feature and wants its own change.
7. `scene::RigidBody::AngularDamping` - the same, exactly.
8. `scene::SurfaceAppearance::AlphaCutoff` - written by a property and by the
   deserialiser, read only by the serialiser. `MakeDrawInstance` omits it. Public
   property; same argument as 6.
9. `scene::SurfaceAppearance::Mode` - reaches `DrawInstance::Alpha`, whose only
   consumer is the change-detection hash. No draw path branches on it. `render`
   is another agent's this pass.
10. `scene::ActiveCamera::Matrices` - **192 of the row's 208 bytes**, written only
    by `ResolveActiveCamera`, which had no production caller anywhere; every
    consumer recomputes from `Camera` and `Transform`. **Both deleted, v0.19.**
    Re-verified across `mono.engine`, `mono.client`, `mono.server`, `mono.studio`,
    `mono.tools` and both binding artefacts: the only callers of
    `ResolveActiveCamera` and the only readers of `Matrices` were four cases in
    `engine.scene.activecamera`. The deciding argument was not the bytes, which
    nothing was paying for, but that a single cached set cannot serve the
    consumers: `render::ViewRecording` needs matrices against the swapchain and
    against a near plane it has already shrunk for the nearest portal pane,
    `render::ResolveSpatialPointer` needs them against the `gui::Screen`, and
    `studio::Overlay` needs them against a viewport panel - and the studio
    round-robins two panels of different sizes through one world. So this is not
    a decision-16 surface waiting to be wired; it is a copy the renderer already
    replaced, and root `AGENTS.md` says to delete the thing you replaced.
    `ResolveCamera` stays and is the shared part. The resource is now 16 bytes.
    **Exposed one thing**: `scene::NearestSeamDistance` had `ResolveActiveCamera`
    as its only production caller and now has none. It is a pure query with its
    own case in `engine.scene.surfacecameras`, and `SurfaceCameras.cpp` was being
    lifted into another module while this landed, so it was left alone rather
    than deleted out from under that change. **Decided at v0.19: kept, as a
    decision-16 surface, with the reason now in the header.** Three arguments,
    none of them "it might be useful". It is the store half of a pair whose other
    half `render::ViewRecording` calls every frame - `PortalNearPlane` takes a
    distance, and the renderer produces its own from the panes the frame handed
    it *on purpose*, because that is the set the pass draws through and a
    renderer has no store. It is the third rung of a deliberately complete ladder
    (`RectangleDistance` asks a rectangle, `SeamDistance` a gathered seam, this a
    world), and the missing rung is the only one a caller outside
    `SurfaceCameras.cpp` has. And deleting it leaves the next host with a world
    to compose `GatherPortalSeams` and a min loop by hand, which is the second
    way to do one job the root `AGENTS.md` refuses.
11. `scene::PortalProxy::Owner` - no production reader; the self-collision guard
    its comment claims is an identity compare at creation. Kept: the physics
    suite identifies a proxy by it, and a proxy is a row that exists for one tick.
12. `scene::Sun` - no production writer since `LightingService` took over. Kept:
    `Sunlight.cpp` reads it as an explicit override and a case covers that.
13. `scene::Visual::Fitted` - a memo about `Bounds::HalfExtent`, living on the
    draw component, read only by `mono.studio`. Belongs on `Bounds`.
14. `scene::Visual::Locked` - D3.7. Public property, zero bytes. **Declined.**
15. `scene::PlayerIdentity::RespawnTime` duplicates
    `PlayersServiceComponent::RespawnTime`, copied once at join. Both are
    script-visible **under the same property name** on two classes, so writing
    `Players.RespawnTime` after anyone joined changes nothing for them.
    `Services.cpp` states this is Roblox parity, so it stays; the identical
    spelling is the hazard, not the duplication.
16. `scene::SpawnLocation::TeamColour` duplicates `Team::Colour`. D3.10.
    **Declined** - both spellings match Roblox exactly.
17. `scene::Collider::Extent` and `Bounds::HalfExtent` hold the same number in
    every non-test site in the tree. Observation only: `SurfaceCameras.cpp` says
    in as many words that a collider may differ from the drawn box, and the
    physics query path genuinely reads `Extent` alone.
18. `scene::CameraController::SeenTransit` and `PortalTransitSeen::Serial` latch
    the same fact for two passes with different lifetimes. Not a duplicate:
    `SnapPortalTransit` writes its latch unconditionally and would break the
    camera's if merged.
19. `scene::Attachment::WorldFrame` is derived, replicated, and **no authority
    derived it** - `ResolveAttachments` was scheduled by `mono.client`'s
    scripted and presented paths only, so neither `mono.server` nor the replica
    world ever ran it. **Fixed, v0.19**, and the stated symptom was wrong.
    `Attachment.WorldCFrame` is a *computed* property (`scene/src/Part.cpp:1014`)
    that calls `ResolveAttachment` on the spot, so a script on either side always
    read the right value; reproduced with a scene script on a dedicated server,
    which reported `WorldCFrame.X 20.00` correctly on tick 20. What was actually
    lost is the *signal*. The pass writes the rows that moved through
    `Store::GetMutable`, and that reported write is what makes `WorldCFrame` and
    `WorldPosition` fire `.Changed` when the part underneath moves - D00043 in
    `docs/retired/DEFERRED.md` is that fix, half-delivered. The same run counted
    **0 change signals over 20 ticks** of a part moving one stud each before, and
    18 after. The other half was `client::CollectLights`, which reads the cached
    field to place a lamp parented to an attachment: in a replica the field stayed
    at the identity for the whole session, so such a lamp lit the world origin.
    Registered in `server::PrepareSimulation` at `PostSimulation` (the tick's
    transforms are final, signals flush immediately after, extraction reads the
    bits after that) and in `client::BuildReplicatedWorld` at `PreRender`, first
    in the phase. `studio::PlayLink` was already covered:
    `client::InstallPresentation` installs the pass and every `--game`, studio and
    imported world comes through it. Cases in `server.host` and
    `client.replicated`; both fail with the registration removed.
20. `gui::Button::Modal` - no reader, no writer, no property. **Not a defect**:
    the field's own comment declares it reserved for mouse-capture policy "until
    the input router has a modal route to apply it to", which is the same shape
    as `scene::Light::Shadows`. Listed so the next survey does not re-find it.
21. `gui::Button::Selected` - the same, and its comment carries the sharper
    reason: `GuiService.SelectedObject` owns selection, so exposing a second
    writable answer would let the two disagree. Both are one byte inside a
    three-byte row, so neither is costing anything to keep declared.
22. `gui::GuiServiceState::MenuIsOpen` - script-writable, folded into the
    draw-list signature, no functional reader. A script assigning it forces a
    full draw-list rebuild for a value that draws nothing.
23. `examples.Orbit` - **nothing in the repo adds the component.** The query
    always matches zero rows. `Scene.hpp` claims the C++ path iterates it;
    `Rings.luau` says there is no such component.
24. `examples.Spin` - the same.
25. `ecs::WorldTime::FrameDelta` - a drawing host's own frame fact, in a raw
    serialiser, so it reaches every snapshot. Every reader is in `mono.client`.
26. `ecs::WorldTime::Alpha` - the same, and `engine.ecs.snapshot` asserts a
    restored world reads back an interpolation fraction of whichever machine took
    the snapshot.
27. `physics::PhysicsClock::Steps` - read only by its own suite.
28. `physics::PhysicsClock::DroppedSteps` - the same, and `Clock.hpp` says it is
    "worth surfacing". Nothing surfaces it.
29. `world::BusBudget::PerTick` - copied from `world::UniverseSettings`. Weaker
    than the rest, because `UniverseSettings` is not a component.
30. `core::NumberKeypoint::Envelope` as carried by `gui.Gradient::Transparency` -
    round-tripped and folded into the compile signature, sampled by nothing.
    A `core` decision rather than a `gui` one.
31-35. `gui::SpatialCanvas::MaxDistance`, `gui::SpatialCanvas::AlwaysOnTop`,
    `gui::SpatialCanvas::Interactive`, `gui::SpatialCanvas::LightInfluence` and
    `gui::SpatialCanvas::Brightness` are verbatim or clamped copies of the
    same fields on `gui::Surface` and `gui::Billboard`. Kept, and the flattening
    is argued in `Components.hpp`: the draw pass must not have to ask which class
    a collector is.

**What was deliberately left standing, and why each.** Fields 6, 7, 8 and 22 are
dead or near-dead and all four are **published script properties** - four
manifest entries and a declaration line each for `LinearDamping`,
`AngularDamping` and `AlphaCutoff`, and one apiece for `MenuIsOpen`. Deleting a property is
an API break a game outside this repository feels, and this pass was chartered to
clean the component set rather than to shrink the scripting surface, so each is
recorded here with its evidence and none was removed. The two damping fields have
a better answer than deletion anyway: `Components.hpp` claims physics applies
them, so a drag term in `IntegrateMotion` would make the comment true rather than
the field go, and that is a physics feature wanting its own change.

**Fields 20 and 21 are not the same case and are deliberately kept.**
`gui::Button::Modal` and `gui::Button::Selected` have no reader and no script
surface, and
each carries a comment declaring itself reserved with the reason - a modal route
the input router does not have yet, and a second writable answer to selection
that would fight `GuiService.SelectedObject`. A declared placeholder with an
argument beside it is not dead code, and both are one byte inside a three-byte
row. Fields 23 to 30 are engine-internal and were left for their owning modules:
`examples` is a demo target, `ecs::WorldTime` is split 6, and `PhysicsClock`'s
counters are split 7.

#### Merges (7 examined, 0 applied)

1. `effects.ParticleEmitter` + `effects.EmitterSlot` - meets both halves of the
   test: one add site, and all three queries read both. **Declined**, because
   `EmitterSlot::Index` is a process-local pool index its writer deliberately
   drops, and merging puts it inside the largest raw-serialised row in the engine.
2. `scene.PlayerIdentity` + `scene.PlayerNetworkComponent` - always present
   together, one add site, but **read apart in two places**, so it fails the
   second half of the test. **Declined** on that: what is left is a cost argument
   (one component id, one registration, one wire entry) for four bytes on a row
   there is one of per player, and `PlayerNetworkComponent` holds a *testing*
   knob - artificial latency - that has no business inside a player's identity.
3. `gui.Layer` + `gui.Canvas` - rejected: `CanvasFor` reads `Layer` alone, and
   `Canvas` should go rather than merge.
4. `scene.Surface` + `scene.SurfaceAppearance` - rejected: read apart, by the
   physics material lookup and by the draw row.
5. `scene.Bounds` + `scene.Visual` - rejected: `Bounds` is queried alone by three
   passes.
6. `scene.SurfaceCamera` + `scene.SurfaceLens` - rejected by the wire: one
   crosses and the other must not.
7. `scene.CollisionShapes` + `scene.EditableMeshCollision` - rejected:
   `CollisionShapes` is read alone by physics.

#### Splits (14 examined, 0 applied)

1. `physics::PhysicsWorld` - D3.3. **Premise false**, five bytes are written.
2. `effects::Trail` - D3.4. **Premise false**, 82 bytes are written, and both
   passes read both halves.
3. `scene::ActiveCamera` - real, and the answer was delete rather than split.
   **Applied**, see field 10: the resource went from 208 bytes to 16 and
   `ResolveActiveCamera` went with it.
4. `scene::Rendered` - `Mark` is one pass's scratch and `Visibility.hpp` says so;
   moving it out makes the component a tag and saves four bytes per drawable.
   **Declined**: the byte is what buys the sweep an O(1) membership test, and the
   replacement is a sorted vector and a binary search per row. 48 KB at twelve
   thousand parts is not worth an O(n log n) sweep without a measurement asking
   for it.
5. `scene::Attachment` - `ResolveAttachments` writes only `WorldFrame`, the
   emitter and light placement read only `WorldFrame`, and three property getters
   read only `Frame`. Real, and still **not applied**, but the reason has moved
   on: field 19 is fixed, so the question of who derives the derived half is
   settled - every host does. What now argues against the split is the signal.
   `WorldCFrame`'s `.Changed` is delivered by observing the `Attachment`
   component, so splitting would move the reported write onto a second component
   and the property's `Reads` set would have to follow it. That is a change to the
   property surface for 28 bytes on a row a scene holds hundreds of, and it wants
   a measurement asking for it.
6. `ecs::WorldTime` - two writers already separate and named, and passes that read
   only one half on each side. **Not applied**: `Store::Time()` returns the
   aggregate by value, so this is an API change at every call site in `ecs`,
   `scene`, `physics`, `client`, `server` and `studio` at once, for a resource
   that costs 32 bytes per world. It wants its own change with its own gate.
7. `physics::PhysicsClock` - the header states the split and the serialiser
   already implements it. **Declined**: the gain is clarity and the cost is a
   second resource lookup in every pass that reads a clock. 48 bytes, one per
   world, and the line is already drawn where it matters.
8. `gui::ScrollMotion` - `Overshoot` has two readers that touch nothing else on
   the row. **Declined**: neither half is authored, so this is gesture scratch
   against resolved output, and both halves are local to the machine looking and
   already excluded from the wire together. Splitting moves 48 bytes between two
   rows on the same entity.
9. `script::SourceCache` - `MirrorSourcePrograms` reads only `Generation` on a
   steady tick. **Declined**: a per-world singleton, so the split buys a second
   resource lookup per tick and saves nothing a cache line cares about.
10-14. `gui.Resolved`, `gui.ScrollState`, `gui.SpatialCanvas`, `gui.PageMotion`
    and `scene::Humanoid`/`CameraController`/`InputState` were each checked and
    **have no split**: no pass reads one half without the other. Recorded so the
    next survey does not re-derive them.

#### Renames (12 found, 3 applied)

Applied, all three prose rather than symbols:

1. `effects::EmitterSlot::Index` was called `Row` in `replication/Defaults.cpp`
   and `effects/AGENTS.md` - two of its three references named a field that does
   not exist. **Fixed.**
2. `gui::Scrolling::Elastic`'s comment said "declared, pinned and not yet
   animated". It has been animated since v0.18 and three cases cover it.
   **Fixed.**
3. `gui.Canvas`'s description said `ScreenGui`; it is written for every
   collector, and its one reader exists for a case a screen gui can never be in.
   **Fixed**, along with the wire note.

Found and **not** applied. Each is a symbol rename with no behavioural gain,
across files three other agents were editing in the same pass, and two of them
are `.agame` format breaks:

4. `scene::Humanoid::Height` is a full capsule height; its property is
   `HipHeight`.
5. `scene::LightingServiceComponent::FogColor` is the only American colour
   spelling among nine British ones, and `Services.cpp` states the rule it
   breaks. `WorldLighting::FogColor` copies it.
6. `"scene.PlayerNetworkComponent"` keeps a suffix every sibling drops -
   `TransientComponent` registers as `scene.Transient`, `ServiceComponent` as
   `scene.Service`. Format break.
7. "Surface" carries three unrelated meanings in one module: a physical material,
   a texture set, and a mirror render-target slot. `Part.cpp` names the collision
   itself. The third reads better as `Pane`.
8. `SurfaceLimit::Panes` is the property `MaxSurfaces`; `SurfaceBounces::Levels`
   is the property `SurfaceBounces`. Two adjacent resources of identical shape,
   one matching its struct name and one not.
9. `InputState::Behaviour` is British and its type `MouseBehavior` is American,
   on one line. The type is fixed by the Roblox-facing property.
10. `Collider::Extent` holds a half-extent and its own header says so. 153 call
    sites.
11. `WorldBounds::HalfExtent` is a scalar radius; `Bounds::HalfExtent` is a
    per-part vector. Different shapes under one name.
12. `script.LuaSourceContainer` spells "Lua" where the enum, the sibling
    component and the declarations all spell "Luau", and the same string is a
    registered *class* meaning something wider. Format break, and the class name
    should stay for Roblox parity.

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

### E3. Two more headers of E1's kind **[verified, two applied, one refused]**

- `core/types/AABB.hpp:41` includes `CFrame.hpp` for one function,
  `FromOrientedBox` at `:96`, with three call sites repo-wide. `AABB.hpp`
  preprocesses to 74,013 lines and `CFrame.hpp` to 73,891, so **`AABB` is
  essentially all `CFrame`**, and every consumer pays for
  `<glm/gtc/quaternion.hpp>`. Moving `FromOrientedBox` to a free function beside
  `CFrame` would make `AABB.hpp` nearly free, the way `Clock.hpp` now is.
- ~~`mono.client/include/client/Settings.hpp:35` includes the whole 1,194-line
  `Client.hpp` and its 186-header closure solely to name `Options`.~~ **Done.**
  `Options` is `client/Options.hpp` now. Measured with the real `release` flags,
  as preprocessed lines: `client/Settings.hpp` **174,863 to 65,853, a 62% cut**.
  Two thirds of that came from a second split it forced: `Options` needs
  `render::ProfilerTab` complete for a default member initialiser, and reaching
  it through `render/DebugPanels.hpp` cost 92,484 lines of `core::FrameGraph`,
  `core::HeapProfile` and `core::Metrics` for one `uint8_t` enum, so the enum is
  `render/ProfilerTab.hpp` now and costs 19,636. What is left is `<filesystem>`,
  which is 61,881 of the remaining 65,853 - the honest floor for a struct that
  holds four paths.
- ~~`mono.client/include/client/Scene.hpp:27` pulls `render/Renderer.hpp` (1,823
  lines, plus glm) into a header with direct fan-in 18, which
  `studio/Editor.hpp:73` includes at fan-in 43. Paid about sixty times.~~
  **Measured and declined, and the entry was wrong twice.** Removing
  `render/Renderer.hpp` from `client/Scene.hpp` takes it from 134,105
  preprocessed lines to 121,122 - **12,983 lines, 9.7%**, not the 131,000 the
  entry implies, because `engine/examples/Scene.hpp` (118,317) and
  `scene/DrawInstance.hpp` (92,345) already pull glm, `ecs` and `world`. And it
  is not paid sixty times through this header: of the 18 direct includers, five
  include `render/Renderer.hpp` on a line of their own - **including both hubs**,
  `client/Client.hpp` and `studio/Editor.hpp:59`. So the studio's fan-in of 43
  saves nothing. Against that, the fix is lifting `SurfaceView`, `PortalView`,
  `SceneLight`, `ParticleBatch` and `ParticleSeam` out of a 1,823-line header
  another agent is actively splitting. Not worth it at 9.7%; worth revisiting if
  `Renderer.hpp` is being split anyway.

**Worked through at v0.19. One applied here, one applied by the client, one
measured and refused.**

`AABB.hpp` is done and the entry undercounts its call sites: there are **eleven**
(six in `src/`, five in tests), not three. `FromOrientedBox` is the free
`core::OrientedBoxBounds` beside `CFrame` now, and `AABB.hpp` fell from **73,835
preprocessed lines to 25,368** across the 152 objects that include it. It does
**not** become "nearly free" the way `Clock.hpp` did: what is left is `<cmath>`
reached through `Vector3.hpp`, which is 25,272 lines on its own. That is an
untouched finding of E1's exact shape.

`client/Settings.hpp` is done: `Options` is its own header, and the file went
**174,863 preprocessed lines to 65,853**, whose floor is `<filesystem>` at
61,881. Splitting it needed `render::ProfilerTab` out of the debug panels.

**`client/Scene.hpp` is refused, and both halves of the claim are wrong.**
Removing `render/Renderer.hpp` saves **12,983 lines, 9.7%**, not the ~131k the
entry implies, because `examples/Scene.hpp` (118,317) and `scene/DrawInstance.hpp`
(92,345) already pull glm, `ecs` and `world`. And it is **not paid about sixty
times**: of the 18 direct includers, five include `render/Renderer.hpp`
themselves, **including both hubs** - `client/Client.hpp` and
`studio/Editor.hpp:59`. A forward declaration cannot do it either, because
`Scene.hpp` needs `SurfaceView`, `PortalView`, `ParticleBatch`, `ParticleSeam`,
`SceneLight` and `MAX_SCENE_LIGHTS` **by definition** (vector members and default
arguments) and only `Renderer` itself by reference. Doing it properly means
splitting `render/Renderer.hpp` into a value-types header and the renderer, which
belongs with C2 rather than here.

### E4. `ecs`'s public headers, cut the same way **[verified, fixed]**

E1's method, applied to the module §C6 sent somebody to look at. Preprocessed
lines with the real `release` flags; translation-unit counts are `release`, and
`dev` adds the test binaries.

| Header | Before | After | Cut | TUs (release / dev) |
|---|---:|---:|---:|---|
| `ecs/Store.hpp` | 84,150 | **64,030** | 23.9% | 216 / 377 |
| `ecs/Classes.hpp` | 91,271 | **55,529** | 39.2% | 179 / 282 |
| `ecs/Scheduler.hpp` | 84,245 | **64,125** | 23.9% | 126 / 195 |
| `ecs/Property.hpp` | 118,185 | **69,332** | 41.3% | 12 |
| `ecs/Schema.hpp` | 91,446 | **55,708** | 39.1% | 5 |

`Classes.hpp` included eight `core/types` headers to *name* the ten values
`PropertyType` covers. `std::is_same_v` answers for an incomplete type and every
caller that instantiates the template holds the complete one, so they are
forward-declared: 35,742 lines off a header 179 objects record, nearly all of it
`CFrame.hpp` reaching glm. `Attributes.hpp` keeps its includes because
`AttributeValue` holds all ten as members, which is the difference the rule now
turns on.

`Store.hpp` gave up `<thread>` and `<memory>`. They overlap so heavily that
dropping either alone is worth 6.6% and dropping both is worth 24%, so it took
both: the affinity check compares the address of a thread-local sentinel instead
of a `std::thread::id`, and `StoreState` is an owning raw pointer deleted by
`~Store` rather than a `std::unique_ptr` - copy and move were already deleted
and the destructor was already out of line, which is what a `unique_ptr` over an
incomplete type would have needed anyway.

**One file in the tree was relying on the transitive include**,
`control/src/Tools.cpp`, and now includes `core/types` itself. All 43 suites
pass, `determinism` and `replay-check` are byte-identical.

Three things measured and *not* done, so they are not re-measured:

- **`<functional>` out of `ecs/Entity.hpp`.** It is 43,345 of that header's
  43,536 lines and it is there for `std::hash<Entity>`. Removing it saves
  **one** line in a real closure, because `core::Name` pulls `<functional>` into
  every consumer that has an entity. Exactly one type in the tree hashes an
  `Entity`.
- **Forward-declaring `Store` in `ecs/Scheduler.hpp`.** Zero of the 126
  translation units that include it lack `Store.hpp` already.
- **Splitting `ecs/src/Schema.cpp`.** §E called it the second-slowest
  translation unit at 22.2 s; on a quiet machine it is **7.60 s**, of which 6.6
  is the 2048-slot thunk table - 1.75 s at 256 slots, 2.60 at 512, 4.20 at 1024,
  about 3.4 ms a slot. Four translation units of 512 would put 2.6 s on the
  critical path instead of 7.6, at the price of four near-identical files.

## F · Parallelism

**The engine is far less concurrent than a reader would guess, and that is
mostly correct.** `scene`, `world`, `game`, `assets`, `bake` and `bakegraph`
contain **no thread, mutex, future or condition variable at all**. The two real
defects found are A1 and A2 above.

**"Networking has 17 concurrency primitives across 5 files" is wrong, and by a
lot [verified].** Over `net`, `replication` and `mono.network`, at this commit
and at `3c1471a` where the survey was written, a grep for every `std::` mutex,
lock, atomic, thread, condition variable, future, promise, latch, barrier,
semaphore and `once_flag` finds **one primitive in one file**: the `std::mutex`
guarding a mailbox in `net/src/LoopbackTransport.cpp:65`, which the datagram
loopback uses and no socket does. Widened to every module a networking path
reaches into - `delivery`, `mono.cdn` and `control` - it is five more objects
across four more files. The audit of all of them, and of the process-wide
primitives the networking path *takes* without owning, is in F3 below.

Loops worth changing, all **[reported]**:

1. ~~`mono.client/src/Client.cpp:495-498` calls `RequestWantedContent`
   unconditionally every frame~~ **[verified, fixed]**. Reproduced and measured
   in `release` against a static 20,000-part scene, headless, 400 frames, read
   off the `content.demand` frame-graph span:

   | | mean | p50 | p99 | max |
   |---|---|---|---|---|
   | before | 0.028 ms | 0.024 | 0.055 | 0.113 |
   | after | **0.001 ms** | **0.000** | 0.025 | 0.041 |

   **A 96% cut on the span's mean**, and it ran on 18 of 400 frames instead of
   399. Frame mean moved 1.117 ms to 0.766 ms across the two runs but those runs
   owed different tick counts, so the span is the robust number and the frame
   mean is not.

   `Client::ScanWantedContent` gates the collection on `ecs::WorldTime::Tick`:
   nothing can put a new content name into a world without a tick advancing.
   **The ECS change channel cannot serve this consumer and all three reasons
   were checked**, which is worth recording because rule 2 points straight at
   it: `Store::ClearChanges` runs at the *start* of a tick, so a once-per-frame
   reader loses every tick but the last of a frame that owed several; a write
   made outside a tick has its bits cleared before `FlushSignals` ever runs, so
   `Store::OnChanged` misses a snapshot apply and a world build; and
   `Store::ChangeVersion` moves for any write in an archetype carrying
   `DirtyBits`, and `physics` observes `scene::Transform`, which shares an
   archetype with every `Visual` - one moving part falsifies it for the whole
   scene. Verified the content still arrives: `MeshGrid.luau` registers 7 meshes
   and 16 textures, which is what `just studio-smoke`'s own comment expects.
2. `replication::Authority::Publish`'s per-client loop **[verified with
   corrections, fixed]**. The two line numbers had moved and the word "blocked"
   was too strong, but the finding is real and the diagnosis order was right.

   What is at `Authority.cpp:1493` after the QUIC rewire is `Dispute`, not a
   lock. The process-wide mutex is `ecs::Components`' registry guard
   (`ecs/src/Components.cpp:29`), taken by the `Components::Find` and
   `Components::Describe` that `BuildComponents` called **per declared slot per
   client per tick** - about nine thousand acquisitions of one lock on a
   two-hundred-client tick, on the thread every other world in the process is
   also publishing from. It was never *held* across the loop; it was taken and
   released inside it, which is the same serialisation point for a parallel loop
   and none at all for a serial one.

   Two more of the same kind were beside it and cost more: `BuildComponents`
   called `Store::EachChangedRuns` per slot per client, which is an archetype
   walk whose answer does not depend on the client, and `Publish` ran the
   interest walk for every client including the ones part way through a join,
   which throws the answer away.

   All of it is resolved once per tick by `Survey` now, into `Authority::Crossing`
   - so the per-client pass takes the registry lock **zero** times and reaches
   the store only through `Alive`, `HasComponent` and `GetComponent`, which are
   `const` and take no affinity. That is what made the loop parallelisable at
   all: `EachChangedRuns` calls `Store::RequireOwningThread`, which aborts.

   The loop is then spread across `parallel::Jobs` above
   `AuthoritySettings::ParallelClientThreshold` steady-state clients, one `Lane`
   of scratch each, with the join half left on the owning thread because
   `Capture` builds a world. `engine.replication.bench.publish` is the ladder
   the threshold comes from and `engine.replication.publishlanes` is the case
   that requires the two to send identical bytes.

   **And a bigger one was found by trying to reproduce it.** Nothing bounded how
   many join snapshots one `Publish` built, and `Capture` is about 113 ms for ten
   thousand entities - so `mono.tools/loadtest` against a ten-thousand-entity
   world could not get past thirty-two clients at all: sixteen arriving together
   spent 1.8 seconds in one tick, answered no handshakes while they did, and every
   client behind them gave up. `AuthoritySettings::JoinsPerTick` is the bound.
3. `mono.client/src/Replicated.cpp`'s `CollectReplicated` **[verified, stays
   serial, measured]**. It had no profiling span at all, which is why nobody
   could read the number; it has one now (`collect-replicated`), matching
   `collect-instances`. Measured in `release` against a server holding 20,000
   replicated rows, over a 12-second connected run: **840 frames, mean 1.692 ms,
   p50 1.542 ms, p99 3.466 ms - about 77 ns a row.**

   **The crossover is low, not high**, which is the opposite of the usual
   answer. A pool handover is 7.74 us for eight ranges (`parallel/Jobs.hpp`,
   re-measured at `-O3`), so at 77 ns a row the handover is a quarter of the
   serial work at about **400 rows**. This loop clears that by fifty times and a
   perfect split across 23 workers would be roughly 75 us against 1.542 ms.

   It stays serial anyway, and the reason is two things outside the loop.
   `SnapshotBuffer::Sample` is not thread-safe and its header says it never will
   be quietly - it counts `Statistics::Held` against `Statistics::Interpolated`
   on every call and refuses to hide them behind a `mutable`. And the output is
   a *filtered* `push_back`: `visual.Visible` is a field test rather than a
   query term, so a row's position in the walk does not decide its position in
   the draw list, which is exactly what lets `CollectInstances`' workers write
   `out[base + first + row]` with no atomic. Making the list dense needs
   `scene::Rendered` in the query and a visibility system in a world
   `mono.client/AGENTS.md` says advances nothing. The four optional joins are
   **not** a third reason: they rule out `EachBatchParallel`, but
   `Store::EachParallel` hands out entities and could express them. The number
   and the argument are in a comment beside the loop.
4. `physics/src/BroadPhase.cpp:80-131`, N about 4,000, safely parallelisable
   because the pair set is sorted and uniqued afterwards.
5. `scene/src/Visibility.cpp:112` uses a single hash chain over every row every
   frame, when `DrawInstance.cpp:64-79` already measured that exact problem and
   fixed it with four lanes.

Three fetch-path costs sat inside the tick barrier, all three reproduced at the
named lines and all three fixed **[verified, fixed]**. Measured in `bench`, which
optimises; two new suites hold the numbers, `engine.assets.bench.manifest` and
`engine.delivery.bench.cache`.

1. `delivery/src/Cache.cpp:189` walked the whole cache directory on every store -
   three syscalls per file already there, so caching N assets cost N squared
   stats. **1.17 ms per store** over 1024 four-kilobyte assets, against 16.3 us
   for the same stores into an uncapped cache, which does no eviction work.
   `ContentCache` now keeps a running total that decides *whether* to walk;
   eviction still chooses from the walk, so nothing it does not know about can
   escape being evicted. Eviction goes an eighth of the ceiling below it, so a
   cache at its ceiling does not walk on every store. **16.4 us**, and 36 us for
   a cache a quarter the size of its working set against 745 us.
2. `assets/src/Manifest.cpp:221` and `:247`. `FindByRoot` was a scan and
   `SliceOf` calls it per member it walks past, so cutting a group up was
   O(M squared x A). **11.96 us to locate one member** of a 32-member group in a
   4096-asset manifest. `Manifest::RootOrder` is the index the comment predicted:
   positions into the asset list ordered by root, so it holds no fact the list
   does not already hold. **319 ns**, and a lookup on its own 669 ns to 78 ns.
   Parsing a manifest does the same join and went 1.90 us to 1.40 us per asset;
   `AddAsset` pays 2.70 us to 3.27 us for maintaining it.
3. `assets/src/ChunkStore.cpp:143` and `:158` hashed every chunk twice - once in
   `Read` against its own name, once in `VerifyAsset` over the concatenation.
   `VerifyAsset` is now defined as a content half and a shape half, and
   `ReadAsset` calls the shape half because its reads already made the other.
   **14.76 us per chunk to 9.17 us** over a 4 MiB asset in 64 chunks.

Two more of the same shape were found beside them. `delivery::Client` and
`delivery::Relay` each held a linear scan for a bundle by root over a list the
format has always kept in root order; both call `Manifest::FindBundle` now, 18 ns
to 10 ns. And `Client::AnyWaiting` and `Client::Abandon` asked `BundleFor` per
pending request per pump, which is a walk of the bundle list inside a walk of the
requests; `Resolve` records the carrier it already found. `BundleFor` itself is
still a walk and was left one: an asset-to-bundle index is a key the format does
not have, unlike `RootOrder`, and the repeated lookup was the actual cost.

---

### F3. Every concurrency primitive the networking path owns or takes **[verified]**

The roadmap item is "check all asynchronous and parallel points", so this is the
whole set rather than the ones that looked suspicious. **Owned** means the module
declares it; **taken** means a networking call reaches it in a process-wide
singleton it does not own, which is where the cost that started F2 actually was.

| # | Primitive | Where | Protects | Complete? | Deadlock? | Held across a blocking call? |
|---|---|---|---|---|---|---|
| 1 | `std::mutex Mailbox::Guard` | `net/src/LoopbackTransport.cpp:65` | one loopback end's inbound deque, its byte count and its attached flag | Yes. Every read and write of all three is inside it, and `Mailbox::Address` is the one field outside it - written at construction and never again | No. `Send` takes `Self->Guard` in `Open()`, releases it, then takes `target->Guard` in `Deliver`. Never two at once, so there is no order to get wrong. Broadcast is the same lock taken and released per mailbox | No |
| 2 | `std::mutex Registry::Guard` | `ecs/src/Components.cpp:29` | the descriptor deque, the name table and the sealed flag | Yes, with one deliberate exception: `Describe` returns a reference the caller reads after the lock is dropped. That is A2's shape and it is safe here for a reason A2's table did not have - a `std::deque` that only ever grows, whose elements are never erased, so no `Adopt` can invalidate a reference already handed out. The deque is documented as being chosen for exactly this | No. Never taken with another held | No |
| 3 | `std::mutex ComponentSetTable::Guard` | `ecs/src/ComponentSet.cpp:104` | the interned set table | Yes, and `Intern` returns a reference out from under it for #2's reason: `std::deque<std::unique_ptr<ComponentSet>>`, never erased | No | No |
| 4 | `std::mutex Registry::Guard` and `std::array<std::atomic<const Schema *>, MAX_SCHEMAS>` | `ecs/src/Schema.cpp:276` and `:304` | the schema deque and name maps; the array is the lock-free read side a generated destructor hook uses | Yes, and the split is the point: a lock in `Destruct` would be a lock per row, so the pointers are published once under the mutex and read atomically for ever after | No | No |
| 5 | `std::shared_mutex Table::Guard` | `ecs/src/Classes.cpp:62` | the class table and its lazily merged property lists | Yes. `Describe` is not a pure read - it merges - and takes the shared lock first and the exclusive one only on the pass that has merging to do | No | No |
| 6 | `std::mutex Table::Guard` | `ecs/src/EnumTable.cpp:23` | the enum deque, its name map and the order list | Yes. `MembersOf` and `Names` return **by value** precisely because the vectors behind them reallocate on a later registration, and the header says so. **One stale comment**: `EnumTable.cpp:27` still says "so `Names` can hand back a span", which it has not done since it started returning a vector | No | No |
| 7 | `std::mutex PoolState::Guard` | `ecs/src/ChunkPool.cpp:41` | the free lists and the retained total | Yes | No | No |
| 8 | `std::atomic<std::thread::id> Store::Owner` | `ecs/include/engine/ecs/Store.hpp` | which thread may mutate a world | Yes, and it is the reason F2's fix works: every run walk calls `RequireOwningThread`, and `Alive`, `HasComponent` and `GetComponent` deliberately do not | No | No |
| 9 | `std::shared_mutex Registry::Guard` | `core/src/Name.cpp:29` | the intern deque, the id map and the slot vector | Yes. `Text()` hands out a `string_view` into a deque that only grows and is never erased - #2 again - and the miss path drops the shared lock, takes the exclusive one and **re-checks**, which is the half a shared/exclusive upgrade usually gets wrong | No | No |
| 10 | `std::mutex Sink::Guard` | `core/src/Metrics.cpp:38` | the counter, gauge and histogram tables | Yes. Every reader returns by value - `Get`, `GetGauge`, `GetHistogram`, `Snapshot`, `Drain` - so nothing escapes the lock | No | No, and it is the leaf: `Metrics` calls nothing that locks |
| 11 | `std::atomic<std::thread::id> Owner`, `std::atomic<size_t> DroppedThisFrame` | `core/src/FrameGraph.cpp:77`, `:108` | A1, fixed | Yes | No | No |
| 12 | `std::mutex Registry::Guard` plus a relaxed byte per category | `core/src/Log.cpp:64`, `:36-50` | the category deque; the per-category threshold is read by every log statement without the lock | Yes. The threshold is written and read with `__atomic_store_n`/`__atomic_load_n` at relaxed order, not as a plain byte, so it is not A1's bug. It is written with builtins rather than `std::atomic` because `<atomic>` costs 7,452 preprocessed lines in a header with a stated budget, and the reason is at the code. The MSVC fallback is `volatile`, which is not a C++ atomic and is relied on for MSVC's own acquire/release volatile semantics | No | No. spdlog's sink mutex is taken **after** this one is released |
| 13 | `std::mutex Contents::Lock` | `core/src/Flags.cpp:51` | the flag table | Yes, and the table is frozen before a server ticks | No | No |
| 14 | The job pool: `Guard`, `Available`, `Finished`, `Drained`, `ReadyCondition`, `Claimed`, and the batch's `Outstanding`, `Next`, `Inside`, `Generation` | `parallel/src/Jobs.cpp` | one batch at a time and the ranges inside it | Yes. A second dispatch - nested, or from another thread - does not wait: `Claimed.exchange` sends it inline. Every range retires even when the body throws, so the join cannot hang on a failure | No, by construction: there is no second batch to order against | The caller blocks *in* `For` by design. It holds no other lock while it does, which is what F2's fix had to preserve - `Publish` dispatches with nothing held |
| 15 | `base_sink<std::mutex>` | `control/src/DiagnosticTools.cpp:79` | the log ring the MCP surface reads | Yes, spdlog's own | No | No |
| 16 | `std::mutex Impl::Guard` | `mono.cdn/src/PreparedCache.cpp:33` | the prepared-frame map, its recency list and the held total | Yes, and every accessor returns a `PreparedFrame` by value | **No, but it is the one place with two locks nested.** `Find` and `Insert` call `core::Metrics::Count` while holding this, so the order is cache then sink. Nothing anywhere takes the sink first and then a cache, and `Metrics` calls out to nothing, so the order cannot invert - the hazard is contention, not deadlock | No |

**What the audit found, in order of how much it matters.**

1. **F2's registry lock**, above. Fixed.
2. **`EnumTable.cpp:27`'s comment is stale.** It describes a span the function stopped returning; the code is right and the comment is not. Not this pass's module to edit.
3. **`PreparedCache` nests two process-wide locks** and is correct only because the order cannot invert. Worth knowing before a third counter is added to a locked region.
4. **`net`, `replication` and `mono.network` own exactly one primitive between them**, and it belongs to the loopback transport that only tests and single-player use. The real sockets, the QUIC stack, `Listener`, `Connector`, `Authority` and `Replica` are all single-threaded by construction, which is what `net/AGENTS.md`'s "one owner, one thread" already says.

**ThreadSanitizer was not run, and the reason is a build fact rather than a
choice.** `CMakePresets.json` carries no sanitizer preset and
`mono.build/MonoLibrary.cmake` has no `-fsanitize` path, so there is nothing to
turn on; adding one is a build change of its own and would want its own review.
`replication`'s new `engine.replication.publishlanes` suite is the substitute
that does exist: it publishes the same world through a serial authority and a
lane-spread one and requires the two to produce identical bytes, which is the
property TSan would have been run to protect.

---

## G · Observability

### G1. Logging is 101 lines and thirteen modules never use it **[verified, fixed]**

`core/Log.hpp` plus `Log.cpp` is the whole facility: four levels, four macros,
one hard-coded stdout sink. No compile-time filtering (`SPDLOG_ACTIVE_LEVEL` is
unset), no categories, no thread id in the pattern (`Log.cpp:23`), no source
location, no correlation id, no throttling, and **a disabled statement still
evaluates its arguments**.

707 call sites, of which eight are `ENGINE_TRACE`. `net`, `network`, `scene`,
`spatial`, `collision`, `resources`, `graph`, `nodegraph`, `bake`, `msl`,
`input`, `gui` and `effects` log nothing at all.

No assert facility exists, despite `core/AGENTS.md:12`.

**Rebuilt at v0.19, and the sweep that preceded it is the part worth
recording.** The guard was the one change that could silently alter behaviour, so
all **711** call sites (the count is 711 today, not 707) were swept twice and
independently for arguments with side effects: a balanced-paren scan over all 198
multi-line sites plus every distinct function named in an argument list, each
checked const or pure. **None had one, so the guard landed with no call site
edited.**

Categories are `core::Name` and are supplied by `mono_add_library` as the
module's own name, so all 711 sites gained one with **zero call-site edits** and
there is one family of macros rather than two. Levels are per category and
settable from a config file, `ATOMIC_ENGINE_LOG_LEVEL`, a flag, or
`--log net=trace,physics=off`. A compiled floor removes a statement below it by
the preprocessor, which `release` sets to `debug`; that was verified by
**looking** rather than trusting, `ENGINE_TRACE`'s literals being absent from the
`release` objects while `ENGINE_WARN`'s from the same file are in both. The
pattern gained a thread id and a source location, and `*_EVERY` variants count
what they suppressed.

Measured at `preset=bench`: a disabled statement is under a nanosecond with or
without an argument, an enabled one into a null sink is 84 ns, a throttled one
while quiet 20 ns. `Log.hpp` went 22,842 preprocessed lines to **23,016**, all of
them declarations in the file: it includes neither `<atomic>` (7,452) nor
`Name.hpp` (51,820), reading a category's level with the compiler's own atomic
builtin to keep it that way.

**The assert half of the finding understates it.** No facility existed, and the
tree carried exactly **one** C++ `assert()`, at `render/src/ResourcePreview.hpp:34`.
Every other module had no invariant check at all. `ENGINE_ASSERT`,
`ENGINE_ASSERT_MSG`, `ENGINE_UNREACHABLE` and `ENGINE_ENSURE` exist now, compiled
out in `release` through a `sizeof` that keeps the condition type-checked, and
routed through the log sink in one write so an assert on a job worker is a line
rather than interleaved fragments.

**All thirteen silent modules were instrumented**, at 178 statements, one
assertion and 30 metric writes, placed at decisions that are hard to reconstruct
and failures that were silent rather than at function entry. The whole sweep adds
**240 lines to a full `test-all`** at the default level. `nodegraph` deliberately
gained none: it links nothing and has no `Log` to write, so the gap in its own
vocabulary was closed instead, and a node it cannot evaluate now reports through
`NodeStatus::Note`, which the canvas already draws.

### G2. `core::Metrics` is write-only **[verified, fixed]**

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

**Fixed at v0.19, and the "four counter implementations" resolved differently
from what this entry implies.** They are `core::Metrics`, `core::FrameGraph`,
`core::HeapProfile` and `ecs::Scheduler`'s per-system timings. Three of the four
have reasons that survive review: `FrameGraph` needs tree structure and per-frame
identity, `HeapProfile` runs inside `operator new` and would recurse through
`Metrics`' mutex, and the scheduler's timings are drained in system order by a
panel that draws them in that order. **What was missing was not one write side
but one read side.** Merging the rest would have broken `cdn::Dashboard`
outright.

`Metrics` has `Get`, gauges, and histograms with nearest-rank percentiles over a
bounded window, plus a `Snapshot` that resets nothing. `FrameGraph` counts its
dropped worker spans into it, so a partial flamegraph on a headless host is a
line in a report rather than a number only the F5 overlay could reach - which is
what A1's counter was for. The server drains and reports at shutdown and, on
request, on an interval. `ecs::ChunkPool`'s `Allocated`/`Reused` atomics, the one
genuine duplicate left, are `ecs.chunkpool.allocated` and `.reused` now, and the
reuse half has coverage it never had.

### G3. MCP **[fixed]**

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

**Closed at v0.19.** 24 tools on a server and 25 in the editor, **44
resources**, **5 prompts**, and `mcpbridge`'s first suite.

`layer_table`, `module_get` and `module_may_link` answer out of
`expected_graph.json` compiled in at configure time, and `MayLink` reimplements
the two rules while the *data* stays single-sourced - so the verdict a model gets
on "may `render` link `script`" is the one `just test-architecture` enforces
rather than a second description of it. `module_get` also carries the reverse
index, which is in no file.

`test_run` and `test_result` had to be asynchronous for a measured reason: a full
run is 125 s wall and 193 s of suite time. The tool spawns `testrunner` with an
argument list the module assembles, with no shell, no `just` and no
client-influenced path. `class_list`, `class_get` and `script_check` cover the
script side; **checking is offered and evaluating is not, and the reason is the
tick rather than security** - this surface already writes properties and starts
worlds, but a Luau chunk with a `while true` in it would hang the program from a
thread there is nothing to interrupt from. `log_tail`, `log_level` and
`metrics_read` ask the process what only the editor's panel could previously be
asked. The server registers three tools of its own where it had none.

**The port finding is wrong as stated and the real one is worse.**
`core::Arguments::Value` requires a value, so a bare `--mcp-port` is a parse
error and no default is ever reached. What was real: `studio/Config.hpp:268` had
`ControlPort = 8720` and `Config.cpp:643` seeds the panel from it, so **the
editor's saved preferences offered 8720** while its help, `.mcp.json` and
`RUNNING.md` all said 8738. The v0.19 note claiming that mismatch was fixed
changed `Editor.hpp:5344` and not the line that overwrites it. The other two were
`mcpbridge`'s usage example still showing the dead 8730 and `mono.server` writing
8734 twice with nothing tying them together. `mcpbridge`'s CMakeLists now reads
`DEFAULT_PORT` out of the header at configure time and **fails the configure**
when `.mcp.json` or the `just mcp` recipe disagrees; both guards were proved by
breaking each in turn.

**Writing the first suite found three bugs**, including a well-formed JSON array
that escaped `Surface::Answer` as a `type_error.306` exception into the frame
loop over a message a client could send by accident, and a report reader looking
for a lowercase `fail` in a report that writes `FAIL` - it had been calling a run
with five red suites green.

## H · What changed in the first pass

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

---

## I · What changed in the closing pass

The first pass above was a survey that fixed what it could reach. This one
worked the register itself, and its most useful output is not the fixes: it is
that **nine of the entries above were wrong**, each in a way that would have cost
somebody a day. They are corrected in place rather than deleted, with the
measurement that corrected them, because the point of a `[reported]` tag is that
the next reader checks before acting and that is what happened.

**What was wrong.** `physics::PhysicsWorld` writes 5 bytes and not forty
serialised members; `effects::Trail` writes 82 and not 1068; adding `effects.` to
`SHARED_PREFIXES` would have failed every join outright; `Visual::Locked` is a
public `BasePart` property in both languages rather than an editor flag;
`SurfaceCameras.cpp` is 2,909 lines and not 4,108, and its edges are a cycle
rather than a clean lift; `Renderer.cpp` is 10.6 s and not 31.2; `Store.hpp`
reaches 216 translation units in `release` and not 253; `Fog` has existed since
v0.16; the "17 concurrency primitives across 5 files" are **one**, in
`LoopbackTransport.cpp`; `Jobs.cpp:103` never existed; the "nine
anonymous-namespace collisions" are 81; `Interface.cpp` holds nine decisions and
not six; and three of §C2's eighteen `SDL_BeginGPURenderPass` calls were error
strings naming the function.

**Four rules that were convention are now checked.** `mono.tools/sourcecheck`
reads first-party C++ as text and decides what the target graph cannot see: a
long-lived object holding data the ECS owns, a pointer inside a type marked as
crossing a world boundary, a `core::Name` reaching a serialiser as its `Id()`,
and a header under `include/` nothing outside its module includes. Three gate and
`public-header` reports. Thirteen fixture trees run before the repository does,
and the runner holds exit status to the gating table.

**`just em-dash-check`** greps 1,722 first-party files, with fixtures in both
directions and three guards against going green on an empty scan.

**The transport is decided by the server.** `--transport quic|datagram|both`,
defaulting to `quic`, with no client flag at all; one UDP port carries either;
both refusal directions are explicit, so a fallback costs one round trip
(measured at 30 ms) rather than a deadline.

**Bugs found that the register did not know about**, each by work aimed at
something else: `scene::Visual::Surface` written and read at 8 bits while
`int16_t` since v0.17, truncating every mirror slot past 127 into every save;
`gui.Canvas` crossing the wire while derived locally three lines from
`gui.Resolved`; `Attachment::WorldFrame` resolved on no authority, so
`WorldCFrame.Changed` never fired at all and a light parented to an attachment in
a replica lit the origin; nothing bounding join snapshots per publish, so a
10k-entity world could not admit 32 clients; `parallel::Jobs` hanging `exit` for
ever in any binary that started a pool without stopping it; `script` holding an
upward edge into `scriptluau` that only linked because no header declared its
caller; a JSON number scanner accepting eight documents that are not JSON; and
the padding rule §D claimed was checked, which was not checked.
