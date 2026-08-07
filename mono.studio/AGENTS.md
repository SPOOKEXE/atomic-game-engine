# mono.studio — program invariants

The editor. `client` tier, with a named escape to `Mono::server`.

## Nothing in this program is world state

Selection, expansion, scroll, splitter positions, which panels are open, where
the camera is. None of it enters a store, crosses a bus, or reaches a snapshot.

That is not tidiness — it is what makes Stop work. Stop throws the entire
universe away and restores a snapshot, and the editor has to still be looking at
the same thing afterwards.

The corollary bites more often: **a panel must never cache something the store
owns.** Read it every frame. A cached instance name is wrong for one frame after
a rename, and one frame is enough to see.

## Every panel draws from inside `Universe::Enter` and acts from outside it

`Enter` aborts on re-entry rather than allowing it, because the affinity check
is the only thing between a scoped store reference and a data race. So a panel
that offers an action records what was asked for and `ApplyPendingActions`
applies it once, outside.

Adding an action means adding a `Pending*` field. It does not mean calling
`Enter` from inside a tree node, however tempting the call site looks.

## A world is furnished, and furnishing it twice does nothing

`scene::InstallServices` puts `Workspace`, `Lighting` and the rest into a world,
and it runs on every world this program **makes** *and* every world it
**loads**. That is only safe because it is idempotent: a game saved before
services existed has none and gets them, one saved after has them all and gets
nothing back.

Branching on the file's format version instead would be a version test that has
to stay right forever. Finding each service before creating it is a lookup that
cannot go stale.

The services are per world rather than per universe, which is Roblox's
arrangement: an entity is a row in one `ecs::Store` and a store is a world.

## Moving an instance between worlds is not a reparent

Within a world it is `SetParent` and the instance keeps its handle. Across two
it cannot: an `ecs::Entity` is an index into one store. So
`MoveInstanceToWorld` writes the subtree out with
`game::WriteInstanceDocument`, rebuilds it on the far side, and destroys the
original **last** — a move that deleted first and then failed to rebuild would
be a delete nobody asked for, and there is no undo to reach for.

The handle changes, so anything holding one has to be told: the selection is
repointed and script tabs inside the moved subtree are closed. A single code
path covering both operations would make that "sometimes".

## A Play run admits N clients, and each is a whole world

`WorldRun::Links` is a vector because one replica cannot show a disagreement.
The bugs a play test exists for — an entity that arrives on one client and not
another, a value that replicates late to the second joiner — need two replicas to
disagree *with each other*, and two views of one store never can.

Each link is an independent `PlayLink` with its own replica world, its own
`Replica` and its own `LinkReport`. The worlds are named apart deliberately:
rule 4 makes a world's name its identity to the universe, the worlds panel and
any recording, so two clients sharing one would be two worlds nothing could tell
apart.

**Only the first is pinned to a viewport.** A panel per client is three pictures
nobody asked for, each a slice of the centre pane and a turn in `PresentWorld`'s
round robin; the rest are ordinary worlds reachable from the scene selector.

The count is fixed at `BeginRun`, so the menu that sets it is disabled while
anything runs — a number that changed under a live run would describe something
that is not there.

## `RunService:IsStudio()` is set here and nowhere else

`script::HostRole::Studio` defaults to false and `script/Runtime.hpp` says why:
editor-only behaviour must never appear in a shipped game because a default was
optimistic. `BeginRun` is the one place in the repository that sets it true.

## Undo is a command log, and Stop clears it

Every edit is recorded as a `Command` carrying what it did and what it undid.
Two rules keep it honest, and both are in `Commands.hpp`:

- **`Clear` on `BeginRun`**, because Stop restores a snapshot taken before the
  run and a stack spanning that boundary would offer to reverse edits the
  restore has already discarded.
- **`Forget(world)` on `EndRun`**, because a scene *can* be edited while it runs
  — the gizmo and the explorer both work during a play test, deliberately — and
  those commands are not merely stale but unreversible, since the restore gives
  every instance a new handle.

The History panel is a reader over the two stacks. Walking to a point in it is
repeated `Undo`/`Redo` rather than a second way to move the log.

## Play snapshots before it runs, and Stop restores

Roblox's model, including the part people forget. An author's scene must not be
whatever their scripts left behind.

**Script buffers are flushed into their worlds *before* the snapshot is taken.**
Get that order wrong and Stop restores a universe from before the author's code
was filed — which deletes whatever they had just typed, unrecoverably. It was
the first thing `BeginRun` got wrong.

## The class picker never hard-codes a class

It walks `ecs::Classes` and filters to descendants of `Instance`. The only
names written out are the abstract bases nobody can instantiate, and that list
lives in one function.

A class added by any module appears in the palette with nothing here changing.
That is the same property the properties panel has, for the same reason.

## The viewport is a texture in a panel, not a hole in the dockspace

The cheap version — draw the world to the swapchain and put a
background-less imgui window over it — works right up until that window is
*docked*, which is the only way anybody uses it. `imgui.cpp`'s
`central_node_hole` is only punched while the central node is **empty**, so
docking a panel into it makes the dockspace fill its whole rectangle with
`ImGuiCol_WindowBg` and paint over the frame.

So the world goes into a `render::SceneTarget` and the panel shows it with
`ImGui::Image`. Do not reintroduce `ImGuiDockNodeFlags_PassthruCentralNode`;
there is nothing to see through any more, and it would silently bring the bug
back the moment somebody undocked the viewport to test it.

The image is **last frame's texture**, because imgui records its draw lists
before the renderer runs. That one-frame lag is the same trade
`world::ViewChannel` and `SurfaceView` already make, and it is what removes the
cycle between "how big is the panel" and "what is in the texture".

## Every panel is closable, and the View menu is the only way back

A closable panel with no way back is a panel somebody loses permanently.
Anything added to the layout goes in `DrawViewMenu` in the same change.

## Bump the dockspace version when you add a panel

imgui owns the layout once it has written its ini, which is right — an editor
that threw away where somebody dragged a panel would be unusable. The cost is
that a panel added later is a window the saved layout has never heard of, and it
opens floating in a corner.

`DOCKSPACE` carries a version for that reason. Bump it when a panel is added or
the default arrangement changes, and **not otherwise** — every bump costs
everybody their layout.

## imgui's "last item" is whatever was submitted most recently

`BeginDragDropSource`, `IsItemClicked`, `BeginPopupContextItem` and friends all
refer to it. A `SameLine()` and a `Text()` between the tree node and its drag
source makes the *text* the last item — which asserts rather than misbehaving,
and that is the good version of this mistake.

Everything that asks about an item goes immediately after it.

## A widget's id comes from its label

Appending a modified marker to a tab title makes it a different tab, which
closes the old one and drops keyboard focus. Use `###` to pin an id to something
stable — the instance, not the text.

## What is deferred, said out loud

- **One VM per world, and it is Luau.** A world whose scripts are JavaScript
  runs nothing. `script::LanguageOf` picks per file and the runtime is per
  world; reconciling those is a design decision, not an oversight to patch.
- **Headless runs no interface backends.** `--headless` starts the imgui
  context but not its platform or renderer backends, so panel *drawing* is
  skipped entirely — what a headless run exercises is the universe, the scripts,
  the presentation phase and the render. Driving the panels themselves without a
  window needs the backends replaced rather than skipped, and that is the shape
  a scripted or agent-driven editor will need.
- **No reference picker.** A `PropertyType::Reference` shows its target's name
  and cannot be changed. Picking one means a modal over the tree.
- **No syntax highlighting and no line-number gutter.** The script editor is a
  plain multiline field in imgui's default proportional font. A monospace font
  is a file this repository does not have and a licence somebody has to choose.
- **No two-way sync.** A Rojo project is read, never written back. Conflict
  semantics for everything the editor can touch is a real piece of work rather
  than a flag, and `docs/retired/v07v08.md` files it under v0.10.

## A file's shape decides its class, and its class decides its context

The three script classes are three contexts, and the Rojo sync is where a file
becomes one of them:

| On disk | Class | Runs |
|---|---|---|
| `X.server.luau` | `Script` | where `IsServer()` |
| `X.client.luau` | `LocalScript` | where `IsClient()` |
| `X.luau`, `init.luau` | `ModuleScript` | never — `require` evaluates it |

**`ModuleScript` is a sibling of `Script`, not a kind of one.** `ScriptsIn`
collects `IsA(Script)` and `IsA(LocalScript)`; a module is neither, so the run
loop never visits one and nothing had to learn to skip it. Derived from `Script`
it would have run on every server by default, which is the opposite of what a
module is for — and a synced project would execute every library it contains.

**A module is keyed by instance, not by path.** Two copies in two places are two
modules with two states, exactly as two copies of a `Script` are two scripts.
That is what makes a module a thing in the tree rather than a thing on disk, and
it is why `require` takes an instance and never a string.

`Players.LocalPlayer` is a **computed property**, not a script-side special case.
`Instances.cpp` switches on `PropertyType` and nothing else, so declaring it in
`scene` made it readable from Luau, from JavaScript and in the properties panel
with none of them changing. It is read-only and nil on a server: a `Script`
reaching for it gets nothing rather than somebody else's player, which is the one
thing a shared codebase must not get wrong.

## The debugger captures, it does not pause

Two reasons, and both outrank the convenience of a stepping debugger:

- **A paused world is not a replayable one.** Rule 5 is that work across ticks
  may not be parallel; a tick held open while somebody reads a variable is the
  largest possible violation, and a recording made through the session would
  stop replaying a long way from the cause.
- **The editor's frame loop is the thread the VM runs on.** Blocking in the hook
  blocks the loop that would draw the panel showing what was caught, so a
  stop-the-world breakpoint here is a frozen window with the answer inside it.

So a breakpoint records the stack and every local at the moment the line ran and
then carries on — or ends that one script with an ordinary error, which the host
already knows how to report. A stepping debugger wants the VM on its own thread
and is a different program.

**Breakpoints live on the `Editor`, not on a runtime.** Stop destroys every
runtime, and re-typing line numbers after each Play is how a debugger stops being
used, so `BeginRun` hands the list to each new one — *before* its scripts run,
because a script's top level has already executed by the time `StartWorldScripts`
returns. Hits stay per-run: a hit describes one execution and showing an old one
against a new run would be a lie about when it happened.

**Stepping compiles the chunk at `-O0`.** At the usual optimisation level the
compiler folds constants and drops the instructions their lines would have
produced, so a breakpoint on `local x = 1` would sit there never firing while the
script plainly ran past it. That is the worst thing a debugger can do. It applies
only while something is armed, so nothing else in the world pays for it — and
keeping that true is the property to protect in any change here.

## A panel that answers a question is closed by default

The v0.10 additions — History, Find Instances, Bus, Changes, Debugger, Script
Profile — are all off until somebody opens them from the View menu. They answer a
question occasionally rather than earning permanent space, and each returns
immediately when closed, which is what makes a long list of them cost nothing.

Three of them are readers over data that already existed, and that is why they
were cheap: the Bus panel reads `Outbox` and `Inbox` because rule 3 already
forces every crossing to be a copy; Script Profile reads the interrupt counter
the step budget already maintains; History reads `CommandLog`'s two stacks.
**None of them caches what it reads** — the corollary at the top of this file
bites hardest on the Bus panel, because `Inbox` is replaced wholesale every
barrier.

## The testable half of a panel goes in a free function

`MatchesQuery`, `DiffText` and `ParseRojoProject` are not methods, and that is
deliberate. Each is the half that can be **silently wrong** — a filter that
matches nothing looks exactly like a scene containing nothing, and a comparator
that finds no changes looks exactly like a clean tree — while everything around
them needs a window, a device and an imgui frame.

`tests/Operators.cpp` records what it costs to get this wrong: the suite there
cannot reach `Editor::RegisterOperators`, so the join it claims to check is not
actually checked. Put the logic where a test can call it.

**"An imgui frame" was on that list and does not belong on it.** A window and a
device genuinely cannot be had in a suite; an imgui *context* can — `CreateContext`
allocates a style table and a font atlas description and touches no driver, which
`engine.ui.theme` has said in one line since v0.7. A frame can be submitted, a
mouse can be moved and clicked, and everything that is not a rasteriser answers.

That was not free to learn. The asset picker's row rewound the cursor with
`SetCursorPos` and submitted nothing after it, which is an `IM_ASSERT_USER_ERROR`
inside `EndChild` — so the editor did not misdraw, it called `abort()` on the
frame the picker opened, and choosing a material was impossible. Nothing could
have caught it, because nothing here had ever run a frame.

`studio/AssetRow.hpp` is the shape that cannot do it — one item per row,
everything else painted — and `tests/AssetRow.cpp` is the suite, including a case
that provokes the fault on purpose with imgui's assert switched off, so the
detector is known to work rather than merely known to pass. **Reach for that
harness before extracting a free function**: a panel's imgui behaviour is now
testable, and moving logic out to avoid testing it is no longer the only option.
