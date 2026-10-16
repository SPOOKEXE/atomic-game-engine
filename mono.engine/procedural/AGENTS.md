# procedural - module invariants

L3, shared tier. This module links only `Engine::core` and `Engine::graphcore`.

## Pure, bounded generation

Recipes, copied spatial facts, named random streams, regional keys and generated
placements live here. The ECS owns authored facts. Studio, scripts, assets,
renderer, files and world publication stay in their owning modules. A request
does not retain a pointer into any of them.

Every count and byte collection has a hard ceiling. Refusal leaves the caller's
previous compiled recipe, key or region output unchanged. The caller verifies
opaque digests with `assets::Hasher` before enqueue and publication; this module
never treats its own digest bytes as proof of source authenticity.

## Regional identity

Canonical recipe bytes sort stable text names by unsigned byte and normalize
negative zero. Candidate names derive from global integer cells before
filtering. Named counter streams never depend on traversal, ticket, revision,
allocation or worker order. A region owns the half-open cell rectangle
`[minX,maxX) x [minY,maxY)`.

Regional keys include only the bounded local query coverage and relevant
source leaves. A complete empty local query has an explicit coverage witness.
The host verifies coverage provenance and invalidates both old and new expanded
bounds when a source moves. A whole-document revision never enters a region key.

## Topology and output

`graphcore` validates and orders the authored typed placement nodes; it does
not evaluate them. The domain keeps forest, village lot and road rules here.
Output is sorted by stable text identity. Roads connect named entrances to named villages within
the declared reach. A failed generation never publishes partial placements.
