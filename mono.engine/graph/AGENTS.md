# graph - module invariants

L9. What is drawn, decided before anything is bound.

## This module owns decisions; `render` owns the device

The split is the reason the module exists, and it is the one thing to keep.

| | `graph` | `render` |
|---|---|---|
| Answers | which instances, in what order, through what passes | how to bind, upload and draw them |
| Tier | `shared` | `client` |
| Testable | fully, on any machine | only by running the client |

**A frustum test is six dot products and can be asserted against; a draw call
needs a GPU and can only be looked at.** Every decision that moved here moved
because it could then be tested - `tests/Frustum.cpp` pins the clip convention,
the plane orientation and the conservative direction of the box test, and none
of that was checkable while it lived inside a render pass.

So: **nothing here may name a device type.** No `SDL_GPU*`, no pipeline, no
buffer, no texture handle. If a change wants one, it belongs on the other side
of the seam.

## `shared`, and that is load-bearing

A server-tier host publishes a view for a world it hosts - `world::ViewChannel`
is the seam - and culling that view is the same arithmetic wherever it runs. A
`client` tier here would mean a headless host shipping its whole draw list
across a process boundary for the far side to throw most of it away.

## Culling is the one optimisation that changes what is correct

Every other one makes the same picture faster. This one makes a **different**
picture, and is only allowed because the difference is invisible.

So the direction of the error is not a preference: **a wrong "visible" costs a
draw call and a wrong "hidden" is a hole in the world.** Every test here is
biased that way - the box test uses the positive vertex, which never rejects
something visible, and a degenerate matrix produces a frustum that accepts
everything rather than nothing.

A change that makes culling tighter is a change that has to argue it cannot
reject anything real.

## The frustum comes from the matrix, never from the camera's fields

`scene::ResolveCamera` is the one place the engine decides what a camera's
matrices are. A frustum built from a field of view and an aspect ratio kept
separately is a second answer to the same question, and the two disagree the
first time one of them is changed - which reads as geometry popping at the
screen edge on one machine and not another.

## The clip convention is Vulkan's and it is not negotiable

`GLM_FORCE_DEPTH_ZERO_TO_ONE` is pinned in `core`'s build for the whole engine,
so the near plane is `z ≥ 0` rather than `z ≥ -w`. The OpenGL extraction puts
the near plane behind the camera and clips the entire scene - which presents as
a renderer that draws nothing rather than as a convention mismatch, and is the
first thing to check if it ever does.

## The pipeline is a list, not a resolver

`Pipeline` is stages in declaration order with a validity check, and that is
deliberately less than a general frame graph. The kind that topologically sorts
resource reads and writes earns its complexity at twenty passes and costs more
than it returns at four.

**What `Validate` checks is the mistake that actually happens at this size**: a
stage reading a target nothing earlier wrote. On a GPU that is not a crash - it
is a frame lit by whatever was in that memory, which reads as a lighting bug
rather than an ordering one.

If the stage count ever justifies a resolver, replacing this is the change to
make. Growing it into one a field at a time is not.
