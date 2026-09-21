# Revisit ideas

## Status

This used to be a 6,905-line brainstorm. Its system proposals duplicated the
canonical plans in this directory, while many of its claims predated work that
has since landed. Do not use it as a design or implementation plan.

## Canonical plans

Use the system plan that owns the work:

- [`audio-system.md`](audio-system.md)
- [`camera-and-cinematics.md`](camera-and-cinematics.md)
- [`character-system.md`](character-system.md)
- [`input-system.md`](input-system.md)
- [`navigation-ai-system.md`](navigation-ai-system.md)
- [`physics-expansion.md`](physics-expansion.md)
- [`prefab-package-system.md`](prefab-package-system.md)
- [`procedural-generation.md`](procedural-generation.md)
- [`session-and-social.md`](session-and-social.md)
- [`terrain-system.md`](terrain-system.md)
- [`ui-system.md`](ui-system.md)
- [`vfx-system.md`](vfx-system.md)
- [`world-streaming.md`](world-streaming.md)

The active release work remains in [`ROADMAP.md`](../../ROADMAP.md). Physics
performance candidates live in [`OPTIMISATIONS_PHYSICS.md`](OPTIMISATIONS_PHYSICS.md).

## Deferred orphaned ideas

These ideas do not yet have an owning system plan. Add one to a system plan
only after a concrete product need and a measurable acceptance gate exist.

- Virtual geometry, virtual texturing, virtual shadow maps, and GPU-driven
  scene submission as one measured renderer residency program.
- HLOD, impostors, foliage, decals, water, volumetrics, and distance fields as
  content-scale rendering features.
- Temporal rendering, dynamic resolution, variable-rate shading, and
  automatic quality allocation as a coherent frame-budget policy.
- A first-class material and shader permutation pipeline.
- A frame capture and replay format for renderer diagnosis.
- A shared signed-distance-field service, only if more than one subsystem has
  a measured need for it.
- Runtime simulation and animation LOD, after the owning gameplay systems
  define acceptable loss of fidelity.
- An engine-wide performance-budget and regression suite, based on real
  release scenes rather than generic thresholds.
- Time-travel inspection and subsystem freezing in Studio, after recording
  exposes a stable snapshot contract.
- Pipeline versioning, graph overrides, and live algorithm replacement, only
  where a safe immutable handoff already exists.

Each item is deliberately a short reminder, not a specification. The old
document contained competing designs for all of them and made it impossible to
tell which, if any, was current.
