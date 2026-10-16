# graphcore - module invariants

L2, `shared` tier. This module links only `Engine::core`.

## Declarations and compilation, never execution

Own stable node, schema and port names, typed connections, bounded dependency
compilation and derived dense topology. Domains own node vocabularies, payloads,
defaults, evaluators, clocks, caches and runtime state. No device, script VM,
audio mixer or world type belongs here.

Text identities survive serialization. Dense node and port indices refer only
to the declarations supplied to one compilation and must not become durable IDs.

## Bounds and replacement are public behavior

Callers supply explicit node, connection, schema, port and identity-byte limits.
Check counts, arithmetic and text budgets before allocating derived tables.
Refusal preserves the previously accepted output. Diagnostics use bounded text
and a closed cycle path containing at most one row per node plus its closing row.

## Ordering has two answers

AuthoredSweep serial order preserves an authored sweep: newly ready nodes later
in the current sweep run immediately; earlier nodes wait for the next sweep.
LowestReady serial order instead reconsiders the lowest ready declaration after
every emitted node, preserving deterministic engine scheduling. FifoReady starts
with ready nodes in declaration order, then appends newly ready nodes in authored
dependency order. Its internal indegrees and outgoing rows retain repeated edges:
the last prerequisite occurrence releases a node. Public adjacency remains sorted
and deduplicated under every policy. Execution waves
describe dependency depth separately. Flattening waves changes serial order and
can change audio summation. Serial ordering never depends on text sorting,
interned names, hash iteration or wall time.

## Port semantics stay explicit

Nonempty type names match exactly. Only the literal `any` is a wildcard; actual
runtime values still need domain validation. Inputs are required unless Optional
is true. Input MaximumConnections zero is unlimited. Outputs always fan out.
Defaults remain domain-owned and must make a disconnected input Optional here.
Connections are directed output to input. Duplicate exact connections are refused;
different pin connections may share one deduplicated topology edge.

Cycles are refused. Domains model delayed or history data outside the same-step
dependency list; no authored flag disables the cycle check.
