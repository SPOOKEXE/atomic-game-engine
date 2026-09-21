# Render cleanup after hooks

## Status

This is the remaining internal cleanup after the render-hook contract in
[`RENDER-HOOKS.md`](RENDER-HOOKS.md) is accepted. The active release sequence
and hook requirements live in [`ROADMAP.md`](../../ROADMAP.md). This plan does
not change hook behaviour, the compiled render graph, or the current portal and
mirror optics.

## Boundaries to preserve

- `Renderer::Render` remains the only batch coordinator and the compiled graph
  remains the only render scheduler.
- Hooks keep their owned values, bounded asynchronous readbacks, exact node and
  view identities, and renderer-owner-thread polling contract.
- A submitted frame alone commits history, uploads, and hook copies. Failed
  submission leaves retryable state pending and publishes no completed bundle.
- Public headers do not gain device types. New render records stay private to
  the module.

## View recording ownership split

Split `ViewRecording` by lifetime, then delete each old field when it moves:

| Record | Owns |
|---|---|
| `ViewInput` | Synchronous caller data that may not survive recording |
| `PreparedView` | Resolved camera, scene plan, targets, graph selection, and immutable capture decisions |
| `RecordingState` | Command identity, pass state, timestamps, pending history, and recording outcome |

Use a private borrowed render context for the existing persistent owners. It is
wiring, never a second `Renderer::Impl`: device, pipelines, residency, targets,
readback, submission, hook binder, and telemetry keep one authoritative owner
each. Extract preparation, graph execution, presentation, and submission as
separate calls only after their current failure behaviour is characterized.

## One explicit surface frontier

`SurfaceCapturePlan` is the sole expansion plan for mirrors and portals.
Replace recursive planning and recording only after plan parity is proven. The
frontier is a bounded depth-first stack with explicit enter, child, record, and
leave phases. It must retain source-slot order and child-before-parent
recording:

```text
enter parent
  complete child A and record it
  complete child B and record it
record parent from completed children
leave parent
```

This order is a target-lifetime rule. Siblings may reuse a lower-level pooled
target only after the preceding subtree has recorded every consumer. Preserve
stable target keys, successful-write readiness, root mirror history, mirror
self-skip, portal-arrival skip, external-image exclusion, derived child
cameras, clipping, visibility, and terminal fallbacks. The frontier validates
depth, entry count, dimensions, finite matrices, and total pixels before
publishing any plan.

After CPU planner parity, route `MirrorNodes` and `PortalNodes` through the
shared plan and narrow recording adapter. Delete their recursive GPU recorders
only when image fixtures and target-reuse traces pass.

## Delivery gates

1. Freeze current graph, hook, surface-plan, history, and failure fixtures.
2. Extract one persistent owner at a time with no copied mutable state.
3. Separate the three view records and then the preparation, execution,
   presentation, and submission calls.
4. Replace planner recursion with the bounded frontier, retaining exact CPU
   plan fixtures and reporting frontier high-water mark.
5. Converge mirror and portal recording on the shared plan, then delete legacy
   recursive paths.
6. Run focused CPU traces for ordering, budgets, failure atomicity, target
   reuse, and hook lifecycle. Finish with approved GPU fixtures and a release
   profile reporting capture entries, pixels, resource bytes, and readback
   latency.

No phase adds a second pass list, graph runtime, callback graph, or permanent
frame runner.
