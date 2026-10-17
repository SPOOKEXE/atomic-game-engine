# imagegraph - module invariants

L9, `shared` tier. The authored image graph and deterministic CPU evaluator.

## Core owns the document and bounded CPU decisions

This module links only `Engine::core`. It owns durable text identifiers, typed
authored values, graph validation, stable compile order and device-independent
CPU evaluation. It does not own files, decoders, a UI, jobs, renderer objects,
GPU memory or pixels beyond the lifetime of one bounded evaluation result.

The document is the authored truth. Compile plans and pixels are derived. Any
live parameters shared across worlds belong in ECS components at their owner,
not in private persistent vectors here. IDs that survive serialization are
text, never `core::Name::Id()`.

## Budgets are part of the public behavior

Document counts, dimensions and output bytes have fixed limits. Check them
before allocation or graph traversal, and report a diagnostic with durable
node and port text. Evaluation is pure for a document and selected output. It
uses no wall clock, ambient randomness or renderer state.

## Keep execution out of this module

Future GPU work belongs in `render`; editor interaction belongs in Studio;
asset decoding and file access belong in host adapters. Do not add a dependency
up the layer stack to make one of those paths convenient.
