# game - module invariants

L10, `shared` tier. The game file format, and nothing about what reads one.

## A game file is not a snapshot, and neither replaces the other

`world::Universe::Save` writes a running universe - entity ids, tick counters,
bus state - and is same-build only. This writes *authored content*: it has to
survive an engine version, read in a diff, merge between two people, and be
repairable by hand.

Do not "unify" them. The two formats disagree on purpose, and the disagreement
is the reason both exist.

## Nothing here writes an entity id

An instance carries a document-local `id` used only to resolve references
inside the same file. Rule 4 arriving at a save format: a name crosses, a
number does not.

An entity id in a game file would be stable until the first time somebody
edited the world in a text editor.

## The format is written against `ecs::Classes` and never against a schema

The properties a document holds are the ones a class declares. Adding a
property to a class adds it to the save file with nothing in this module
changing, and that is the whole design - a schema here would be a second
declaration of what a `Part` is, and rule 6's version of that is drift.

The corollary: **never special-case a class or a property by name.** The two
exceptions are `Name` and `Parent`, both structural, both listed in one
function, and both there because the document already answers them.

## Defaults are not written

A property equal to a fresh instance's is skipped. The consequence is
deliberate: a file loaded by a later build picks up that build's defaults for
anything it did not name.

If that ever becomes wrong for some property, the fix is a `PropertyDescriptor`
field saying so - not a list of exceptions in this module.

## An unknown class is refused and an unknown property is ignored

Not an inconsistency. A missing property loses one value; a missing class loses
a whole subtree, and silently dropping it opens a world that is missing things
nobody can name.

## `Xml.cpp` is a subset and must not grow into a library

No DTD, no external entities, no namespaces, no processing instructions. Every
one of those is a named XML attack, and the argument for hand-writing a parser
instead of vendoring one is that the grammar fits on a page and can be read.

If something needs a feature this parser does not have, it needs a different
format - not this file needing a feature.

**The scanner is `core::xml` since v0.15 and the writer is still here.** This
module's reader was one of three that refused the same things in three places -
`bake` could not call it, because `bake` is L9 and this is L10 - so `D00128`
moved the scanning down to a tier every caller can reach. What is left in
`Xml.cpp` is what is *this format's*: the tree, `XmlLimits`, `XmlStatus` and the
writer, which writes this dialect and has no possible second caller below L10.

Two consequences worth knowing before changing either half:

- **`XmlStatus` is a mapping and not a set of parse errors.** `core::xml::Fault`
  says malformed, truncated, refused or too many attributes, and `StatusOf` is
  where that becomes this format's vocabulary. `Refused` has to survive the
  trip - it is the only status that means somebody tried something.
- **An undeclared entity is refused where it is read, and CDATA is exempt.** A
  save file's scripts are CDATA and a `&` in Luau is an operator, so the
  document-wide sweep `Svg.cpp` uses would refuse a perfectly good world.
  `core/Xml.hpp` carries both policies and what breaks if they are collapsed.

## Another module's format is embedded, never restated

A block this file carries on behalf of another module goes in as that module's
own text, in one CDATA section, parsed by that module. `<Sources>` carries
programs; `<AssetPipelines>` carries `bake::PipelineSet`. Restating one as
elements would be a second grammar for one thing, and the interesting failure is
not that it breaks - it is that both parse and disagree about what they read.

**And such a block is refused on its own, not on the document's behalf.** A
malformed pipeline warns, drops what it carried and lets the world load; a
malformed instance refuses the file. The line is whether the loss is
recoverable: a world can be re-authored a recipe, and cannot be re-authored the
parts. Convention, not checked - a new block has to be written this way on
purpose.

## What is not saved yet, said out loud

`scene::SurfaceTable` is world state and is not written. Materials are
registered by scripts today, so a loaded world rebuilds its table when its
scripts run. The moment a studio lets somebody edit a material by hand, that
stops being true and this module gains a `<Surfaces>` section.

## One place bakes a hull, and every host calls it

`CollisionContent.hpp` is the only conversion from `assets::MeshData` to the
`collision::ConvexHull` and `collision::TriangleMesh` that a `scene::Collider`
names. The client had its own inline copy until v0.17 and the server had none,
which is how a headless server came to have mesh colliders it could not resolve:
every one of them fell back to the part's bound in silence, and a client and a
server disagreed about where a player was standing while both looked
self-consistent.

**A second copy of the conversion anywhere is the bug.** Two callers that build
a hull separately will eventually build different hulls - a different tolerance,
a different vertex order, one that welds and one that does not - and physics on
either side of a link has to reach the same answer.

**The built-ins are content that never arrives.** `assets::MakeBuiltin`
generates the six rather than shipping files, so they never travel the path an
arriving asset takes. Every host calls `RecordBuiltinCollisionShapes` on every
world it simulates; without it a `MeshPart` set to `Cube` with a hull collider
resolves to nothing.

**Bake once, merge many.** The `Add` functions fill a table and the
`Record`/`Merge` functions put one on a world, because quickhull over a model is
not free and `SetResource` copies a `scene::CollisionShapes` whole. A host with
several worlds fills one table and merges it; a host that called a per-world
function in a loop would run the same quickhull once per world. `Plane` hulls to
no faces, which is `BuildConvexHull`'s documented answer for a flat input and not
a failure - a quad collides as a quad.

## This module may see `assets`, and may not see `bake`

`Engine::assets` is here for `CollisionContent.cpp` alone: the mesh format, the
manifest and the chunk store, none of which open anything this module did not
already have bytes for. **`Engine::bake` remains forbidden** for the reason the
`bakegraph` edge exists - it carries the PNG, JPEG, GIF, BMP, OBJ, glTF and PMX
readers, `server` links this target, and a dedicated server has no business
holding a decoder. `D00102`.

Nothing here opens a file or a socket. A caller hands over bytes it already has,
and the trust boundary stays `delivery`'s.
