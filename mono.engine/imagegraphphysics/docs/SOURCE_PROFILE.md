# Source rigid-body native profile

The backend pins [official Box2D 3.1.0](https://github.com/erincatto/box2d/tree/d5935a7a1853eb0f4aca92b369f37929d02c7e11)
under its MIT license. The source Pixel Composer revision is
`b69eca232217360cf1502ef0223523d818606652`.

That source bundles extension `gmlBox2D` version `0.0.1` with binary libraries.
Its extension metadata leaves author and license blank. The Linux library
depends on `libbox2d.so.3` and imports `b2DefaultSurfaceMaterial`. Those facts
identify the 3.1 API family, whose official release introduced surface materials,
but do not establish the bundled Box2D minor or patch version. The backend uses
official licensed source rather than executing or redistributing that wrapper.

Copied spawn controls follow `node_rigid_object.gml`: texture dimensions become
box half extents or the smaller circle radius, positions divide by simulation
scale, authored rotations become positive radians, initial velocities stay in
world units. The source reads mass and collision group but does not apply them
in its spawn path. The native profile retains Box2D's default density and filter.

The four optional boundary segments follow `node_rigid_group_inline.gml`,
including their extended endpoints. Gravity, sleeping, and continuous collision
are recorded per frame. Stepping follows `node_rigid_render.gml`: milliseconds
divide by 1000 and both simulate and captured playing must be true. Every seek
rebuilds the ordered spawn/force/step event history in one owner world, preserving
native contact history. Consumer names, processor rows, and local command ordinals identify each event.
Multiple render events can step the same world within a tick; a requested event
capture freezes the intermediate snapshot without changing the complete recording.

Limits are 4096 frames, 256 named bodies, 65536 total ordered commands,
256 bytes per body name, quality 1 through 64, timestep 0 through 1000 ms, and
finite bounded numeric controls. Invalid records leave the prior output intact.

The native backend covers boxes, circles, convex polygon fixtures with up to eight
points, authored static segments, native contacts, kinematic sensors and step-owned
overlaps, force/impulse/torque/angular impulse, activation, physical overrides,
weld and motor joints, and force-threshold breaking after each render step.
Fracture commands may explicitly set density and recompute native mass. Ordinary
object spawn preserves default density. Explosions retain the source nodes'
different impulse scaling and unscaled world-space radius comparison.

The pinned Linux wrapper's local force and impulse functions call
`b2Body_GetLocalPoint`, then pass that result directly to the world-point force
API without rotating the force vector. The native profile retains that quirk.
Its mass setter initializes only the mass field of `b2MassData`, leaving centre
and inertia uninitialized. The native profile preserves valid native centre and
inertia when changing mass. That deterministic behavior cannot establish parity
with uninitialized source wrapper state.

Three graph actors are integrated: the inline owner, object, and render nodes.
Owned textures render through the native graph. Headless tests cover contacts,
seek/reset replay, paused versus played frames, and asymmetric texture rotation.
The remaining fifteen actors, fracture mesh-generation actions, source spawner
and collision history, source executable scheduler parity, and the exact bundled
Box2D version remain acceptance work. The braced Verlet example exercises native
constraints and does not demonstrate this rigid backend.

The CPU rigid Render and Render ID profiles admit at most 64 million
body-pixel visits across one processor batch. Texture lists admit at most
256 entries before cloning. Refusal preserves the caller's previously published
output and replay journal. Stream spawns require typed captured seed observations
and ordered random draws; these request captures currently have no durable file
codec. Active Burst remains unsupported because the pinned source compares the
current frame against a junction reference after rebinding its frame variable.
