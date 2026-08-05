# game — module invariants

L10, `shared` tier. The game file format, and nothing about what reads one.

## A game file is not a snapshot, and neither replaces the other

`world::Universe::Save` writes a running universe — entity ids, tick counters,
bus state — and is same-build only. This writes *authored content*: it has to
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
changing, and that is the whole design — a schema here would be a second
declaration of what a `Part` is, and rule 6's version of that is drift.

The corollary: **never special-case a class or a property by name.** The two
exceptions are `Name` and `Parent`, both structural, both listed in one
function, and both there because the document already answers them.

## Defaults are not written

A property equal to a fresh instance's is skipped. The consequence is
deliberate: a file loaded by a later build picks up that build's defaults for
anything it did not name.

If that ever becomes wrong for some property, the fix is a `PropertyDescriptor`
field saying so — not a list of exceptions in this module.

## An unknown class is refused and an unknown property is ignored

Not an inconsistency. A missing property loses one value; a missing class loses
a whole subtree, and silently dropping it opens a world that is missing things
nobody can name.

## `Xml.cpp` is a subset and must not grow into a library

No DTD, no external entities, no namespaces, no processing instructions. Every
one of those is a named XML attack, and the argument for hand-writing a parser
instead of vendoring one is that the grammar fits on a page and can be read.

If something needs a feature this parser does not have, it needs a different
format — not this file needing a feature.

## What is not saved yet, said out loud

`scene::SurfaceTable` is world state and is not written. Materials are
registered by scripts today, so a loaded world rebuilds its table when its
scripts run. The moment a studio lets somebody edit a material by hand, that
stops being true and this module gains a `<Surfaces>` section.
