# Physics and collision optimisation candidates

## Status

This is a shortlist of work that remains after the current broadphase,
persistent contact data, speculative contacts, continuous sweeps, and
island-coloured solver paths landed. It is not a feature plan. See
[`physics-expansion.md`](physics-expansion.md) for product work.

Each candidate needs a release-preset scene, a baseline, a bounded resource
cost, and a deterministic result before implementation. A plausible hot loop
is not evidence.

## Candidates

### Reuse narrowphase manifolds for settled pairs

The persistent pair and impulse data can avoid rebuilding a manifold only when
both transforms remain within an explicit position and rotation tolerance. The
cache must invalidate on shape and collision-layer changes. Measure a
sleep-heavy scene against the retained-byte cost and verify that contact events
remain unchanged.

### Warm-start convex distance queries

Cache a previous separating direction or simplex for hull-heavy pairs. Reject
it when its metric becomes degenerate or transform change invalidates it. This
is useful only if profiling attributes material time to convex support calls.

### Cook a private mesh midphase

Immutable mesh colliders can carry a private deterministic triangle index
constructed at bake or load time. Compare it with the present path on a
triangle-dense scene, record retained bytes, and preserve stable triangle
feature identities for warm starts.

### Split only measured oversized constraint islands

Independent islands and colour waves already provide the ordinary parallel
path. If a release scene exposes one large island as the limiting span, split
it into conflict-free batches and retain a serial straddler set. Keep the
batch order deterministic and prove that every conflicting row reaches the
serial set.

### Add solver substeps only for a demonstrated stability failure

Substeps require contact anchors in local space and rescaled cached impulses
when the step length changes. They must improve a named stack, vehicle, or
fast-contact case at a bounded total solver cost. Do not make a global rate
change solely to add them.

### Improve character grounding with a measured controller case

Evaluate a capsule controller and multi-ray support probe against the current
character path only when stairs, slopes, or moving supports fail a playable
demo. The controller keeps collision ownership in physics and publishes intent
through the existing character boundary.

### Consider wide traversal only after scalar traversal dominates

Packed SIMD box tests and ray-order traversal are representation changes to a
tree that already has a deterministic scalar path. Adopt them only with a
query workload whose measured traversal cost repays the added layout and test
surface.

### Keep query/update concurrency out of scope until it exists

The current tick boundary owns updates and queries. Alternating roots and
atomic widening add complexity without a measured concurrent reader. Revisit
only when a real query path must overlap broadphase mutation.

## Guardrails

- Preserve deterministic pair and constraint order after every acceleration.
- Account for live and retained bytes beside timing improvements.
- Keep broadphase structures private to physics or spatial ownership.
- Do not replace a working path without a release-preset comparison and a
  scenario that benefits.
