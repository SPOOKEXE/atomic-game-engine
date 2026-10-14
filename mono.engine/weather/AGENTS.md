# weather - module invariants

L8, shared. Weather definitions, deterministic evaluation, time anchors,
assembly decisions, stateful reference evolution and checkpoint codecs.

## Dependencies stay downward

This module may depend on core, parallel, ecs and scene. It may not depend on
render, graph, replication, assets, effects or physics. GPU resources and
device-specific layouts belong to render. Network adapters carry copied values
and wire this module through the program band.

WeatherCompute is authored semantic scene data. ECS owns its definition and
accepted timeline. A cache holds derived keys, resource generations or prepared
arithmetic, never another mutable weather definition or dirty-flag registry.

## Reference samples are independent of the viewer

Procedural samples are pure functions of the versioned definition, accepted
weather time and canonical lattice coordinate. Random purposes are independent.
Allocation order, GPU page slots, camera movement and render frame count may not
enter a seed. Integer overflow, signed conversion and rounding are explicit.
Scalar and batched paths must pass the same reference fixtures.

The eight-byte sample codec is a portable semantic encoding. It does not impose
a shader struct ABI on scene or require native FP8 arithmetic. Physical state,
residuals, topology and GPU rendering targets are additional memory.

Gameplay and authoritative events do not read back GPU clouds. They use shared
fixed-tick arithmetic under the engine's strict simulation policy. Cosmetic
lighting may differ across graphics backends without changing weather events.

## Time anchors preserve history that matters

The accepted timeline carries cumulative displacement and phase. A wind or rate
change first resolves the old anchor at the change tick, then establishes the
new anchor. Direct seeking never accumulates a client's render deltas or casts
the full absolute tick to a float. A modulo phase is seamless only where the
field explicitly has that period.

Stateful evolution requires complete state and compatible checkpoints. Do not
replace lost nonlinear history with a seed reset or advertise a cosmetic mean
as exact physical conservation. Vapour mixing ratio is weighted by dry-air
mass; condensate mass concentration is weighted by volume.

## Sparse maintenance is conservative

Five seconds is a maintenance cadence, not a simulation timestep or a frozen
visibility decision. Motion, growth and maintenance age contribute to bounds.
Teleports and definition changes request early maintenance. Exhausting a page
budget retains coarse visible coverage instead of publishing missing children.

Assembly merging has an explicit error bound and retains enough velocity or
reconstruction data to split again. Random zero-sum detail preserves encoded
totals; it cannot recover discarded shape. Camera-dependent LOD does not change
canonical stateful evolution.

## Checks and enforcement

Architecture checks enforce dependency height and tier. Owning header suites
verify sample parity, timeline continuity, encoding refusal, conservation,
assembly bounds and checkpoint completeness. Source checks enforce ECS
ownership and pointer-free crossing records where declarations are marked.
Benchmark suites write results to stdout and have corresponding just recipes.

Conservative visual error thresholds and shipping performance budgets are
measurement conventions until their acceptance fixtures exercise them.
