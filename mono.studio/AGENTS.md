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
