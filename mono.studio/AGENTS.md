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

## `RunService:IsStudio()` is set here and nowhere else

`script::HostRole::Studio` defaults to false and `script/Runtime.hpp` says why:
editor-only behaviour must never appear in a shipped game because a default was
optimistic. `BeginRun` is the one place in the repository that sets it true.

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
- **No undo.** Every action is applied straight to the store. Undo needs a
  command log, and a half-undo that covered property edits and not deletions
  would be worse than none.
- **No reference picker.** A `PropertyType::Reference` shows its target's name
  and cannot be changed. Picking one means a modal over the tree.
- **No viewport picking or gizmos.** Selection is the explorer's. A click in
  the world does nothing.
- **No syntax highlighting and no line-number gutter.** The script editor is a
  plain multiline field in imgui's default proportional font. A monospace font
  is a file this repository does not have and a licence somebody has to choose.
