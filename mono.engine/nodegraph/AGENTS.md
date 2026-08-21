# nodegraph

A typed node graph, an evaluator that does not block, and a Dear ImGui canvas
over both. `mono.studio` is the only consumer today: the Demo Nodes panel and
the render pipeline editor.

It was a separate repository until v0.18.0, vendored as a submodule. That made
this public repository unclonable while the other one was private, and the code
is ours and MIT, so it lives here now. `docs/DEFERRED.md` D00113 is the entry
that put it in one place; `just check-one-node-graph` is what keeps it there.

---

## It depends on nothing of ours, and that is load-bearing

`links` is `[]` in `mono.tools/architecture/expected_graph.json`, and a change
that adds an entry needs a better reason than convenience.

Nothing here needs `Engine::core`. There is no `Name` to intern, no `Log` to
write, no `Clock` to read - a graph is a document, and what it needs is a
string, a hash and a worker. Reaching for `core` would put the platform layer
underneath a module that currently compiles against the standard library and a
widget toolkit, in exchange for nothing.

Specifically **not**:

- **`Engine::render`.** The canvas paints into an `ImDrawList` and never sees a
  device. `Preview.hpp` hands out CPU pixels and lets the *host* decide what an
  upload is - which is what lets the same graph draw in the editor, in a test
  with no GPU, and in a headless run.
- **`Engine::ui`.** A sibling at a higher layer. The edge is allowed to run
  `ui -> nodegraph` and never the other way; reversing it would make the
  editor's toolkit and the node canvas impossible to build separately.
- **`Engine::parallel`.** See the evaluator rule below. This is not an
  oversight.
- **`ecs`, `world`, `scene`.** A node graph is a document. It is not in a world,
  it does not tick, and nothing in it is an entity.

## A port's type is a string, and never an ordinal

`DataType::Id` and `NodeType::Id` are `std::string`. This is the root
`AGENTS.md` rule 4 in its most literal form: both cross a save file, and an id
derived from registration order would connect different things the moment two
registrations swapped. Refuse any change that interns these to an integer for
speed. The compare is not on a hot path - it happens when a link is made, not
when one is drawn.

An unregistered id is **not** a wildcard. `DataTypes::CanConnect` refuses it.
A typo in a port's type would otherwise connect to everything, which is the
worst available reading of a mistake. `ANY_TYPE` is the wildcard and it is
spelled.

## This module ships no node types

The library knows no vocabulary. Every node type arrives through
`NodeTypes::Register`, and the set the studio's Demo Nodes panel uses lives in
`mono.studio`, not here. That seam is the reason this is reusable at all: a host
with its own vocabulary links this and gets no terrain nodes it did not ask for.

`tests/` therefore registers its **own** fixture rather than importing anybody's
node set. A test that reaches for the studio's demo nodes has coupled the
library's suite to a consumer's content, and the content will move for reasons
that have nothing to do with the model.

## A picture belongs to the wire, not to the node

`DataType::Preview` turns a payload into a `PreviewImage`. It is on the *data
type* because an input's payload was produced upstream by a node the reader has
never heard of, and the only thing both ends agree on is what the wire carries.
Moving it to `NodeType` would make a node's inputs undrawable, which is the
feature it exists for.

Payloads are `std::any`. The library never learns what one holds. When a payload
has no sensible picture - a number - the answer is no picture, not a grey
square.

## The evaluator never blocks, and must not start

`Evaluator::Run` is called once a frame. It evaluates every sync node it can,
hands each ready async node to a worker, collects whatever finished since last
time, and returns. A node whose input is still being computed is left for the
next call. Two independent branches therefore run at once with nothing in here
scheduling them, because readiness is the schedule.

**This is why `Engine::parallel` is the wrong tool and its absence is
deliberate.** Root `AGENTS.md` rule 5 says work inside a tick may be parallel
and work across ticks may not, and `Jobs::For` blocks until done to enforce
exactly that. These nodes span frames on purpose - that is the whole feature, a
graph carrying something genuinely slow staying editable while it works. Putting
`Jobs::For` behind `Run` would freeze the editor for the length of the slowest
node.

`RunReport` counts `Skipped` and `Waiting` separately and they must stay
separate: a node with no evaluation will never produce anything, and a waiting
one is about to.

## The hash is what stops it recomputing for ever

A result is kept against `Graph::Hash`, so editing one widget recomputes exactly
the sub-tree below it. A hash that takes in anything that changes every frame -
a time, a pointer, an unordered container's iteration order - turns the whole
graph into a per-frame recompute, and it does so silently. There is a suite case
for the hash settling; keep it passing.

## No public header includes imgui.h

Every ImGui mention in `include/` is prose. `Types.hpp` carries its own `Colour`
instead of an `ImVec4` for this reason. That is what lets `imgui` be `VENDOR`
here rather than `VENDOR_PUBLIC`, and it is what lets the model half be tested
with no ImGui context at all - which is the half where a mistake is silent.

If a canvas type genuinely has to be public, put it behind an opaque handle
before widening the vendor.

## Warnings

This is first-party code now and compiles with `-Wall -Wextra -Wpedantic`, and
with `-Werror` under the `ci` preset. As a vendored library it was included
`SYSTEM` and could not fail a build. Do not reach for a `SYSTEM` include or a
pragma to quieten something here - fix it.

## Not here yet

- **No undo.** The editor's selection and drag state live in `Canvas`, and
  nothing records a document delta. `mono.studio` owns undo for its own
  documents; do not add half of one here.
- **No multi-user editing.** `Serialize.hpp` is a whole-document read and write.
  There is no patch format, and the team-create edit stream in `mono.studio`
  does not carry graphs.
- **No node type versioning.** A saved graph naming a type this build does not
  have loads as a node with no evaluation, counted as `Skipped`. That is the
  honest failure and it is deliberate, but there is no migration hook, so
  renaming a shipped node type breaks saved graphs.
