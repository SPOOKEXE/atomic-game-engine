# mono.studio - program invariants

The editor. `client` tier, with a named escape to `Mono::server`.

## Nothing in this program is world state

Selection, expansion, scroll, splitter positions, which panels are open, where
the camera is. None of it enters a store, crosses a bus, or reaches a snapshot.

That is not tidiness - it is what makes Stop work. Stop throws the entire
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
original **last** - a move that deleted first and then failed to rebuild would
be a delete nobody asked for, and there is no undo to reach for.

The handle changes, so anything holding one has to be told: the selection is
repointed and script tabs inside the moved subtree are closed. A single code
path covering both operations would make that "sometimes".

## A Play run admits N clients, and each is a whole world

`WorldRun::Links` is a vector because one replica cannot show a disagreement.
The bugs a play test exists for - an entity that arrives on one client and not
another, a value that replicates late to the second joiner - need two replicas to
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
anything runs - a number that changed under a live run would describe something
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
  - the gizmo and the explorer both work during a play test, deliberately - and
  those commands are not merely stale but unreversible, since the restore gives
  every instance a new handle.

The History panel is a reader over the two stacks. Walking to a point in it is
repeated `Undo`/`Redo` rather than a second way to move the log.

## Play snapshots before it runs, and Stop restores

Roblox's model, including the part people forget. An author's scene must not be
whatever their scripts left behind.

**Script buffers are flushed into their worlds *before* the snapshot is taken.**
Get that order wrong and Stop restores a universe from before the author's code
was filed - which deletes whatever they had just typed, unrecoverably. It was
the first thing `BeginRun` got wrong.

## The class picker never hard-codes a class

It walks `ecs::Classes` and filters to descendants of `Instance`. The only
names written out are the abstract bases nobody can instantiate, and that list
lives in one function.

A class added by any module appears in the palette with nothing here changing.
That is the same property the properties panel has, for the same reason.

## The viewport is a texture in a panel, not a hole in the dockspace

The cheap version - draw the world to the swapchain and put a
background-less imgui window over it - works right up until that window is
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

## There is no maximum number of viewports

`Extras` is a vector and "New Viewport" grows it. Anything indexed by panel -
the viewer cameras, the overlay slots, the compiled GUI lists and their routers
- is grown in `Editor::ResizeViewports` and nowhere else, because they are one
thing indexed five ways and growing four of five is a subscript past the end on
the next frame.

Two things a new panel needs that a fixed array gave for free. Its **title** is
owned by the `ViewportState` rather than pointed at a literal, because imgui
keys a window - and the ini keys its dock node - on that string; it is derived
from the index in `ResizeViewports` so panel 5 is "Viewport 6" in every session.
And its **renderer slot** is its panel index, so the asset preview's slot is
`Editor::PreviewSlot()` - computed as one past the last panel rather than a
constant, because a constant is a cap.

Closing a panel clears `Open` and leaves it in the vector; `AddViewport` hands a
closed one back before minting a new index. Removing entries would renumber the
panels above it, and a renumbered panel is one the saved layout has lost.

The cost is honest and unchanged: `PresentWorld` round-robins one panel per
frame, so N open panels each refresh at a Nth of the frame rate. That is the
person opening them deciding to pay it, which is why the count is theirs.

## Every panel is closable, and the View menu is the only way back

A closable panel with no way back is a panel somebody loses permanently.
Anything added to the layout goes in `DrawViewMenu` in the same change.

## Bump the dockspace version when you add a panel

imgui owns the layout once it has written its ini, which is right - an editor
that threw away where somebody dragged a panel would be unusable. The cost is
that a panel added later is a window the saved layout has never heard of, and it
opens floating in a corner.

`DOCKSPACE` carries a version for that reason. Bump it when a panel is added or
the default arrangement changes, and **not otherwise** - every bump costs
everybody their layout.

## imgui's "last item" is whatever was submitted most recently

`BeginDragDropSource`, `IsItemClicked`, `BeginPopupContextItem` and friends all
refer to it. A `SameLine()` and a `Text()` between the tree node and its drag
source makes the *text* the last item - which asserts rather than misbehaving,
and that is the good version of this mistake.

Everything that asks about an item goes immediately after it.

## A widget's id comes from its label

Appending a modified marker to a tab title makes it a different tab, which
closes the old one and drops keyboard focus. Use `###` to pin an id to something
stable - the instance, not the text.

## What is deferred, said out loud

- **One VM per world, and it is Luau.** A world whose scripts are JavaScript
  runs nothing. `script::LanguageOf` picks per file and the runtime is per
  world; reconciling those is a design decision, not an oversight to patch.
- **Headless runs no interface backends.** `--headless` starts the imgui
  context but not its platform or renderer backends, so panel *drawing* is
  skipped entirely - what a headless run exercises is the universe, the scripts,
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
| `X.luau`, `init.luau` | `ModuleScript` | never - `require` evaluates it |

**`ModuleScript` is a sibling of `Script`, not a kind of one.** `ScriptsIn`
collects `IsA(Script)` and `IsA(LocalScript)`; a module is neither, so the run
loop never visits one and nothing had to learn to skip it. Derived from `Script`
it would have run on every server by default, which is the opposite of what a
module is for - and a synced project would execute every library it contains.

**A module is keyed by instance, not by path.** Two copies in two places are two
modules with two states, exactly as two copies of a `Script` are two scripts.
That is what makes a module a thing in the tree rather than a thing on disk, and
it is why `require` takes an instance and never a string.

`Players.LocalPlayer` is a **computed property**, not a script-side special case.
`LuauInstances.cpp` switches on `PropertyType` and nothing else, so declaring it in
`scene` made it readable from Luau, from JavaScript and in the properties panel
with none of them changing. It is read-only and nil on a server: a `Script`
reaching for it gets nothing rather than somebody else's player, which is the one
thing a shared codebase must not get wrong.

## A model file is read in `bake` and only mapped here

The Rojo sync builds both of Roblox's model containers as of v0.15, and the
split is the point: `bake::ReadRobloxModel` and `bake::ReadRobloxModelXml` turn
bytes into a tree of class names, names and values, and `RojoSync.cpp` turns
that tree into instances. A parser for a foreign format is the largest attack
surface a content pipeline has and `bake/AGENTS.md` is where that rule lives;
putting one in an editor would put it in a shipped program's dependency graph the
first time somebody linked the two.

**The extension picks the reader and nothing else differs.** Both readers hand
back the same `RobloxModel`, so there is one mapping below them rather than one
each - an `.rbxmx` gets the same class lookup, the same property conversion and
the same source staging as an `.rbxm`, because every one of those is a decision
about a tree and not about the bytes it came out of. Sniffing instead of reading
the extension would be a second answer to a question Rojo's table has already
answered.

**With `.rbxmx` closed, no row of Rojo's table is unbuilt**, and `UnbuiltKind` -
the function that named the ones that were - is gone rather than left returning
nothing. Anything that now falls past `BuildMapped` really is a file the table
says nothing about, so "not a script" is finally the whole truth.

Three decisions are this program's rather than the reader's, and each is the
kind that is easy to take differently by accident:

- **One instance, named after the file.** Both containers allow any number at
  their top level; Rojo's table maps a model file to one. A file with several is
  refused by name rather than wrapped in a folder nobody wrote.
- **A class this engine does not have becomes a `Folder` and says so** - the
  same answer `$className` already gets. Two answers to one question is how a
  sync starts having a dialect.
- **A script's `Source` is staged into the world's `SourceCache`.** Roblox keeps
  the text on the instance and this engine keeps a path, so an import that made
  only the instance would produce a `Script` that never runs, which looks exactly
  like a script with a bug in it.

**A property this engine has no declaration for is counted, not listed.** Studio
writes every property of every class, so the `.meta.json` rule - name the key,
because an author typed it - would be a hundred lines here saying this engine is
smaller than Roblox, and would bury the notes that are about the file.

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
then carries on - or ends that one script with an ordinary error, which the host
already knows how to report. A stepping debugger wants the VM on its own thread
and is a different program.

**Breakpoints live on the `Editor`, not on a runtime.** Stop destroys every
runtime, and re-typing line numbers after each Play is how a debugger stops being
used, so `BeginRun` hands the list to each new one - *before* its scripts run,
because a script's top level has already executed by the time `StartWorldScripts`
returns. Hits stay per-run: a hit describes one execution and showing an old one
against a new run would be a lie about when it happened.

**Stepping compiles the chunk at `-O0`.** At the usual optimisation level the
compiler folds constants and drops the instructions their lines would have
produced, so a breakpoint on `local x = 1` would sit there never firing while the
script plainly ran past it. That is the worst thing a debugger can do. It applies
only while something is armed, so nothing else in the world pays for it - and
keeping that true is the property to protect in any change here.

## A panel that answers a question is closed by default

The v0.10 additions - History, Find Instances, Bus, Changes, Debugger, Script
Profile - are all off until somebody opens them from the View menu. They answer a
question occasionally rather than earning permanent space, and each returns
immediately when closed, which is what makes a long list of them cost nothing.

Three of them are readers over data that already existed, and that is why they
were cheap: the Bus panel reads `Outbox` and `Inbox` because rule 3 already
forces every crossing to be a copy; Script Profile reads the interrupt counter
the step budget already maintains; History reads `CommandLog`'s two stacks.
**None of them caches what it reads** - the corollary at the top of this file
bites hardest on the Bus panel, because `Inbox` is replaced wholesale every
barrier.

## The testable half of a panel goes in a free function

`MatchesQuery`, `DiffText` and `ParseRojoProject` are not methods, and that is
deliberate. Each is the half that can be **silently wrong** - a filter that
matches nothing looks exactly like a scene containing nothing, and a comparator
that finds no changes looks exactly like a clean tree - while everything around
them needs a window, a device and an imgui frame.

`tests/Operators.cpp` records what it costs to get this wrong: the suite there
cannot reach `Editor::RegisterOperators`, so the join it claims to check is not
actually checked. Put the logic where a test can call it.

**"An imgui frame" was on that list and does not belong on it.** A window and a
device genuinely cannot be had in a suite; an imgui *context* can - `CreateContext`
allocates a style table and a font atlas description and touches no driver, which
`engine.ui.theme` has said in one line since v0.7. A frame can be submitted, a
mouse can be moved and clicked, and everything that is not a rasteriser answers.

That was not free to learn. The asset picker's row rewound the cursor with
`SetCursorPos` and submitted nothing after it, which is an `IM_ASSERT_USER_ERROR`
inside `EndChild` - so the editor did not misdraw, it called `abort()` on the
frame the picker opened, and choosing a material was impossible. Nothing could
have caught it, because nothing here had ever run a frame.

`studio/AssetRow.hpp` is the shape that cannot do it - one item per row,
everything else painted - and `tests/AssetRow.cpp` is the suite, including a case
that provokes the fault on purpose with imgui's assert switched off, so the
detector is known to work rather than merely known to pass. **Reach for that
harness before extracting a free function**: a panel's imgui behaviour is now
testable, and moving logic out to avoid testing it is no longer the only option.

## The node graph is vendored, and the panel is all that is left here

`studio/NodeGraph.hpp` was this program's own implementation of a design that
already existed as a standalone library. Two of one thing is the debt
`AGENTS.md` calls the most expensive kind, and `D00113` closed by taking the
further-along copy - this one - upstream. `mono.vendor/nodegraph` is the result
and `src/NodeDemo.cpp` is what stayed: the panel, and nothing else.

The split is worth stating, because the next node editor has to know which side
it is adding to. **The library owns everything that is true of any node graph** -
the registry, the model, folding, layout, evaluation, the canvas, the save
format, and pictures as pixels. **This program owns what only an engine has**: a
texture for a picture, a theme, an undo stack, a file dialog and a dockable
window.

Two seams, both in `studio/Editor.hpp` as free functions so that
`tests/NodeGraph.cpp` can reach them, and both silent when they break:

- **`NodePreviewTexture`** copies a `nodegraph::PreviewImage` into an
  `assets::TextureData`. It is a `memcpy` because both sides mean red first and
  the top row first - and the day one of them stops meaning that, the editor
  keeps drawing thumbnails with their channels swapped.
- **`ApplyNodeChrome`** hands the library the four values it draws chrome with.
  Called every frame the panel draws, because the theme and the interface scale
  are both settings somebody can change while it is open.

**A third implementation is what must not happen**, and it would arrive as a
file rather than as a decision - the render pipeline as a node editor is still
to be written, and it could start its own registry and its own canvas without
anybody noticing until the cycle guard or the hash diverged.
`just check-one-node-graph` is what stops it: no first-party file may open
`namespace nodegraph`. Extend the library where it lives.

## Configuration lives in the person's folder, not beside the binary

`studio-layout.ini`, `studio-content.ini` and `studio-keybinds.ini` used to sit
in `Paths::Base()`, which is `.cache/build/<preset>/` for anybody working on the
engine. Every `just build` against another preset therefore read as a fresh
install, and deleting the build directory threw away somebody's keybinds and
their origin list.

`studio/Config.hpp` is the one place that decides where these go -
`~/Documents/atomic-game-engine/studio`, beside the content store that already
lives there. Three conventions the build cannot check:

- **Every document goes through `ReadConfigDocument`/`WriteConfigDocument`.** A
  second path that spelled its own filename would be a second place to change
  when the folder moves, and the first one to be forgotten.
- **A flag beats a preference and is never written back.** `Options` is what
  somebody asked for on one run and `Preferences` is what they configured;
  `app/main.cpp` reconciles the two because it is the only code that can tell a
  flag that was given from one left at its default. `--headless` once must not
  make an editor headless for ever.
- **A missing file is not an error.** Every `Load` answers `false` and leaves
  the caller's defaults alone, so nothing has to tell "not configured yet" from
  "configured to the default".

## A locked part is left out of the pick, not filtered out of the hit

`scene::Visual::Locked` is authoring data - it survives a save, which an
editor-side set of "instances I am ignoring" could not. What the editor does
with it is the part that is easy to get subtly wrong: `PickInViewport` omits a
locked entity from the proxy list **before** the raycast.

Filtering the *hit* afterwards passes every simple test and is wrong: the locked
proxy still swallows the ray, so a locked wall makes everything behind it
unclickable too - which is the opposite of what locking a wall is for.
`studio.tools` has the case, and it is the one that needs something *behind* the
locked thing to fail at all.

## A plugin is a script, and the editor is not in its vocabulary

`studio::PluginHost` runs a plugin as an ordinary `script::Runtime` against the
world an author is editing. That is the whole design: the engine already has a
sandbox, a step budget, a memory ceiling and two languages, and a second
scripting model for tools would be two of each to keep safe.

Four rules the build cannot check:

- **One runtime each.** Two plugins sharing one would share a global table, a
  step budget and a memory ceiling - a plugin that looped would stop the others
  and a plugin that set a global would be read by them. `studio.plugins` asserts
  the second by putting the check inside the plugin that runs second.
- **One failure each.** A plugin that will not start, or that raises on
  `PLUGIN_FAULT_LIMIT` consecutive beats, is switched off with its reason kept
  and the rest keep running. The limit exists because the failure is per frame:
  a plugin whose heartbeat raises does it again next frame and every frame
  after.
- **The selection is a component, not a call.** `studio.Selected` is published
  into the world, so a plugin, a C++ system and the properties panel are three
  readers of one fact - `ecs/AGENTS.md` rule 2, applied to the editor's own
  state. `PublishSelection` writes **only when the selection changed**, and that
  is load-bearing rather than tidy: a tag written every frame moves
  `Store::ChangeVersion` every frame, and `physics::SyncBroadphase` reads that
  counter to decide whether static geometry moved. Publishing unconditionally
  rebuilds the static index every tick, forever.
- **Plugins are reloaded whenever the active world is replaced.** Every plugin
  holds a `Store &` from the universe, so carrying them across an `OpenGame` is
  a reference into storage that has gone.

There is deliberately **no toolbar or editor-command API**, and the absence is
stated in `studio/Plugins.hpp` rather than left to be discovered. `D00105`
carries what adding one would take, and the obstacle is `script/AGENTS.md`'s
first rule rather than an opinion.

