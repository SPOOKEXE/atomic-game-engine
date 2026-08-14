# bakegraph — module invariants

L9, `shared` tier. The words a bake pipeline is described in, and nothing that
runs one.

## `Engine::core` and nothing else

This module exists to keep a decoder out of a save format. `bake` carries every
importer the engine has — PNG, JPEG, GIF, BMP, OBJ, glTF, PMX — and nothing a
shipped game links may link it; `Engine::game` links *this*, so a dedicated
server parses a pipeline document with none of them.

That property is one link line wide. The day something here wants `assets`, ask
what for: a document is names and numbers, and a payload is `bake`'s. The tier
check will not stop it — `assets` is below this — so the guard is the row in
`mono.tools/architecture/expected_graph.json` and this paragraph.

## The namespace is still `engine::bake`

These are the same words they always were, and a module boundary is a link-line
fact. Renaming them would be churn at every call site to express something the
graph already states.

## A node kind is a closed list, and adding one is a format change

`NodeKind`'s ordinals are on the wire and its spellings are in the text, so a
kind is appended and never inserted, and both halves of `NodeText` /
`NodeFromText` move together. `IsBareNode` is public because two modules need
the same answer about the same list; a second copy of it beside the second
caller is exactly the drift it exists to prevent.

A kind this build does not have reads as a malformed line, deliberately — it is
indistinguishable from a typo, and guessing would bake something nobody wrote.
What that refusal *costs* is the caller's: `game::ReadAssetPipelines` drops the
world's pipelines and keeps the world.

## A set's names sort by text, not by `Name::operator<`

That operator orders by the interning counter, which is first-seen order and a
property of the process rather than of the document. A `PipelineSet` written
twice with the same contents has to produce the same bytes, or a save file stops
being diffable and a round-trip test stops meaning anything.

## `Read(Write(d))` is `d`, including for documents that would not build

Nothing here validates: recording is unchecked because an editor lets somebody
wire two nodes and then delete one, and `bake::Build` is where a document meets a
graph and can be wrong. So the format must round trip a broken document as
faithfully as a working one — a writer that quietly dropped an edit it thought
was nonsense would lose an author's work in progress.
