#pragma once

// The Basic Components: what a thing in a world is made of.
//
// One definition each, for every program. Before this module the client and the
// server each carried their own `Transform` and their own position component,
// which was deliberate only while there was nowhere shared to put them —
// `mono.client/AGENTS.md` and `mono.server/include/server/Simulation.hpp` both
// said so in as many words. Two definitions of one fact is the debt this
// module exists to pay off.
//
// **Every transform here is world space and nothing propagates it.** The
// instance tree is organisational, exactly as Roblox's is: parenting a part to
// a model moves nothing and re-resolves nothing. That is what buys the engine a
// world with no transform-hierarchy pass, no dirty cascade and a physics step
// that reads `Transform` directly — see `ecs/Instance.hpp`, which makes the
// same point from the storage side.
//
// **What is a component and what is a resource is not a naming question.**
// `ecs/AGENTS.md` gives the rule — componentise what you iterate, one-of-a-kind
// state is a resource — and this file is where it gets applied. `Surface` is a
// name rather than two floats because friction and restitution are the same
// two floats on thousands of rows; the floats live in a `SurfaceTable`
// resource, which the narrow phase reads once.
//
// **Padding is named where it exists.** A trivially copyable component is
// serialised as its object representation, padding included, and padding is
// never initialised — so two runs of one scene produce different bytes and
// `just determinism` fails somewhere far from here. `ecs::WorldTime` learned
// this the expensive way; the `Reserved` fields below are that lesson.
//
// @tier L7 · shared

#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/scene/Enums.hpp>
#include <engine/spatial/LayerMask.hpp>

#include <cstdint>

namespace engine::scene {

	// Where a thing is, in world space.
	//
	// One `CFrame`, so position and orientation move together and cannot be
	// updated half-way. No scale: scale describes what is being drawn or
	// collided with, and lives on `Bounds` and `Collider`.
	//
	// @since v0.4
	struct Transform {
		// Position and orientation in world space, never relative to a parent.
		core::CFrame Frame;
	};

	// Where it was when the current tick began.
	//
	// Rendering runs faster than the simulation ticks, so drawing at tick
	// positions makes everything judder at the beat frequency between the two.
	// A presentation pass interpolates from here to `Transform` by
	// `ecs::WorldTime::Alpha`.
	//
	// Deliberately its own component rather than a previous-frame matrix shared
	// with velocity buffers or temporal AA. `RENDER_PIPELINE.md` §14 flags that
	// reuse as coupling two features that are independent, and it is cheaper to
	// keep them apart now than to untangle them later.
	//
	// @since v0.4
	struct PreviousTransform {
		// Where `Transform::Frame` was when the current tick began.
		core::CFrame Frame;
	};

	// How far a thing reaches from its own origin, on each local axis.
	//
	// **The single source a world AABB is derived from**, by both the broad
	// phase and render culling. Kept as a local half-extent rather than a
	// world-space box because a world box is a function of this and
	// `Transform`, and storing the derived value is storing a second copy of a
	// fact that goes stale the first tick something moves.
	//
	// @since v0.4
	struct Bounds {
		// Half the extent on each local axis, in metres. Half, because that is
		// the form every containment and overlap test wants.
		core::Vector3 HalfExtent{0.5f, 0.5f, 0.5f};
	};

	// How fast a thing is going, and how fast it is turning.
	//
	// **Split from `RigidBody` on purpose.** Anything with a `Transform` and a
	// `Motion` moves — a platform, a projectile, a demo cube — and none of them
	// needs a mass. So `Integrate` runs over `<Transform, const Motion>` and
	// never loads a mass it does not use, which is the same reasoning that took
	// the half-extent out of the rows it was repeated on.
	//
	// @since v0.4
	struct Motion {
		// Metres per second in world space. Integrated against the fixed tick
		// delta, never against measured frame time — a tick is a function of
		// its state, so a recorded run replays.
		core::Vector3 Linear;

		// Radians per second about each world axis.
		core::Vector3 Angular;
	};

	// What makes a moving thing a body the solver may push.
	//
	// Present only on dynamic and kinematic parts, because `MakePart` decides
	// from `PartDesc::Anchored` whether to attach one at all. Static geometry
	// therefore lands in a different archetype rather than being visited and
	// skipped once per tick per entity.
	//
	// Widest-first with named padding, so the object representation a snapshot
	// writes holds no uninitialised bytes.
	//
	// @since v0.4
	struct RigidBody {
		// Kilograms. Ignored for a `Static` or `Kinematic` body, which is why
		// it is here rather than on `Motion`.
		float Mass = 1.0f;

		// Fraction of linear speed shed per second. Zero is a vacuum.
		float LinearDamping = 0.0f;

		// Fraction of angular speed shed per second.
		float AngularDamping = 0.0f;

		// What the solver is allowed to do with this body.
		BodyKind Kind = BodyKind::Dynamic;

		// Explicit padding. Without it these three bytes are uninitialised and
		// go straight into a snapshot, which makes two runs of one scene
		// differ.
		//
		// **Three bytes and not two, because a `Sleeping` flag used to sit
		// here.** It does not any more: `v02v03v04.md`'s allocation table puts
		// sleeping in a different archetype so the query never visits a
		// sleeping row, and a flag on this row is the opposite of that — it is
		// only readable by making the visit the archetype move exists to avoid,
		// and it is the same state the solver already has to keep. `physics`
		// owns it now, and `physics/AGENTS.md` carries the whole decision.
		uint8_t Reserved[3] = {};
	};

	// What a thing collides as.
	//
	// Separate from `Bounds` because the two answer different questions: bounds
	// is the box everything derives a world AABB from, and this is the exact
	// shape the narrow phase intersects. A sphere's AABB is not a sphere.
	//
	// @since v0.4
	struct Collider {
		// The shape's dimensions, in metres, read according to `Shape`: box
		// half-extents on each axis, sphere radius in X, cylinder radius in X
		// and half-height in Y.
		//
		// One field rather than a union, because a union in a column is a byte
		// layout the storage cannot describe and the bindings cannot type.
		core::Vector3 Extent{0.5f, 0.5f, 0.5f};

		// Which layers this collider belongs to.
		//
		// A `spatial::LayerMask` rather than a `uint32_t`, and that matters
		// here more than anywhere: this field and the next are the same width
		// and mean opposite things, so passing them to a query the wrong way
		// round compiles and returns a plausible wrong answer. The named type
		// is what keeps an ordinary integer from becoming either of them by
		// accident.
		spatial::LayerMask Layer = spatial::LayerMask::Only(0);

		// Which layers this collider is tested against. A pair is considered
		// only when each side's layer is in the other's mask.
		spatial::LayerMask Mask = spatial::LayerMask::All();

		// Which shape `Extent` describes.
		ShapeKind Shape = ShapeKind::Box;

		// Whether contacts are reported without being solved. A trigger
		// produces events and no impulse.
		bool Trigger = false;

		// Explicit padding, for the reason `RigidBody::Reserved` gives.
		uint16_t Reserved = 0;
	};

	// What a thing is made of, as far as a contact is concerned.
	//
	// **A name, not coefficients.** Friction and restitution are the same two
	// floats on thousands of entities, which is the resource case out of
	// `ecs/AGENTS.md` almost word for word: it names a row in the
	// `SurfaceTable` resource, and the narrow phase reads that row once instead
	// of loading two floats per body per contact.
	//
	// @since v0.4
	struct Surface {
		// The material's stable name. Resolved against the world's
		// `SurfaceTable`; an unregistered name is a lookup that finds nothing,
		// deliberately, rather than a silent default.
		core::Name Material;
	};

	// What a thing looks like.
	//
	// Names rather than handles, because a mesh reference has to survive a save
	// file and a wire — `core::Name` is the type that rule has a name for. A
	// presentation module resolves a name to whatever device object it keeps;
	// nothing about that resolution belongs in a component.
	//
	// @since v0.4
	struct Visual {
		// Flat multiplier over whatever the material produces.
		core::Color3 Tint{1.0f, 1.0f, 1.0f};

		// The mesh to draw. An invalid name means the consumer's own default —
		// a unit cube, today.
		core::Name Mesh;

		// The material to draw it with. Distinct from `Surface::Material`,
		// which is what it *feels* like: a mirror-finish floor and a rubber
		// floor may share a surface and never a material.
		core::Name Material;

		// How much of what is behind shows through, 0 to 1.
		//
		// **The field is cheap and the ordering is not**, which is why this
		// arrived with a renderer feature rather than with a binding. Opaque
		// geometry draws in any order and transparent geometry does not, so a
		// non-zero value here puts the entity in a second pass sorted
		// back-to-front per view — the first thing the renderer does that
		// depends on *which camera is looking*.
		//
		// **Distinct from `Visible`, and they must not be conflated.** A part at
		// `Transparency = 1` is invisible and still collides; a part with
		// `Visible = false` is not submitted at all. A draw path that treated
		// one as the other would either give invisible parts no physics or make
		// hidden parts cost a sort.
		float Transparency = 0.0f;

		// Whether this entity is submitted for drawing at all.
		bool Visible = true;

		// Which surface texture this entity shows, or -1 for none.
		//
		// **Fitted into the padding rather than growing the struct.** There were
		// three named bytes after `Visible` and there are two now; the type is
		// the same size it was. See `DrawInstance::Surface` for what the field
		// means and why it is a mirror feature rather than a general one.
		int8_t Surface = -1;

		// Whether this entity is drawn into the shadow map.
		//
		// **A third distinct question, and the three must not be collapsed into
		// one.** `Visible` decides whether the entity is submitted at all,
		// `Transparency` decides which camera pass it lands in, and this decides
		// whether it occludes the sun. They come apart constantly: a glass pane
		// is submitted, sorted back-to-front, and casts nothing; a collision
		// volume is invisible and still needs its physics; a decorative shell
		// inside a building is drawn and would only double-shadow the wall
		// around it.
		//
		// **Only opaque geometry reaches the shadow pass anyway** — a blended
		// fragment writing full depth would cast a solid shadow, which is the
		// most obviously wrong thing glass can do, and the renderer already
		// draws the opaque head alone. So this is the switch for the case the
		// transparency rule does not already cover: an *opaque* thing that
		// should not occlude.
		//
		// Defaults to true, because the surprising default is the one where
		// authored geometry silently stops casting.
		bool CastShadow = true;

		// Explicit padding. `Visual` is registered with an explicit writer
		// because it holds names, so these bytes do not reach a snapshot today
		// — they are named anyway, because the day somebody re-registers this
		// type without one is the day three uninitialised bytes start ending up
		// in a recording.
		//
		// **One now, not two.** `CastShadow` took another, which is what named
		// padding is for: a field that fits goes in the hole rather than
		// widening the row. `Surface` took the first the same way, and
		// `Transparency` did not fit — a float needs four-byte alignment and
		// these are the tail after a `bool` — so the struct grew by four for
		// that one and by nothing for the other two.
		uint8_t Reserved[1] = {};
	};

	// A point of view on a world.
	//
	// **A component, and there may be several** — a spectator, a cutscene, a
	// security monitor. Which one is live, and its resolved matrices, is the
	// `ActiveCamera` resource, so "where is the camera" stays a lookup rather
	// than a search over every row.
	//
	// Holds no aspect ratio: that belongs to whatever is being drawn into, not
	// to the camera, and a world that stored it would be a world with a window
	// size in it.
	//
	// @since v0.4
	struct Camera {
		// Vertical field of view, in radians.
		float FieldOfViewRadians = 1.22f; // 70 degrees

		// Near clipping distance, in metres.
		float NearPlane = 0.1f;

		// Far clipping distance, in metres.
		float FarPlane = 500.0f;
	};

	// A camera that renders into a texture rather than onto the screen.
	//
	// **A second component beside `Camera`, not a field on it.** Most cameras
	// do not render to a texture, and a field would put two numbers on every one
	// of them — but more than that, the presence of this component is what a
	// consumer queries for. A flag would mean walking every camera to find the
	// one that has it set.
	//
	// The result is what `render::SurfaceView` renders and what an instance
	// carrying `Visual::Surface` samples a frame later. That staleness is the
	// design rather than a limitation: it is what breaks the dependency cycle
	// between a mirror and what it reflects, and `world::ViewChannel` assumed it
	// from the start.
	//
	// @since v0.6
	struct SurfaceCamera {
		// How big the texture is, in pixels.
		//
		// Not square by requirement: a wide mirror wants a wide target, and
		// giving it a square one wastes half the texels on nothing.
		uint16_t Width = 1024;

		// The other axis.
		uint16_t Height = 1024;

		// How much of the part behind shows through the projected image, 0 to 1.
		//
		// **A second opacity, and the reason there are two is that they are two
		// different things.** `Visual::Transparency` is how much of the *world*
		// shows through the pane; this is how much of the *pane* shows through
		// the reflection. A mirror is a transparent sheet of glass with an
		// opaque image on it, and one number cannot say both — which is exactly
		// what went wrong: fading a mirror faded its reflection with it, so
		// there was no way to author glass that reflects.
		//
		// At 0 the image is solid and covers whatever the part would have drawn,
		// **whatever the part's own transparency is** — a fully transparent pane
		// still shows its reflection, which is what a mirror is. At 1 the image
		// is gone and the part draws as itself.
		float ImageTransparency = 0.0f;

		// Which surface index this camera writes.
		//
		// One today, and the field exists because the pipeline that replaces
		// this one will have several — a stage list that had to be rewritten to
		// add a second mirror would be a stage list that encoded the count.
		int8_t Surface = 0;

		// Which face of the parent part this camera projects off.
		//
		// **Only meaningful when this camera is parented to a `BasePart`**, which
		// is the arrangement `AimSurfaceCameras` exists for: the camera is placed
		// by the engine, mirrored through that face's plane, rather than by a
		// script computing the reflection itself. A camera parented to the world
		// keeps whatever `CFrame` it was given, because there is no face to
		// project off.
		//
		// Stored as the enum's underlying type so the component stays trivially
		// copyable and the row stays the size it was.
		NormalId Face = NormalId::Front;

		// Explicit padding, for the reason every other `Reserved` gives.
		uint8_t Reserved[2] = {};
	};

	// A content hash of what a consumer last saw, for consumers that cannot
	// observe a column version.
	//
	// **This is the fallback and it must stay labelled as one.** Change
	// detection is `ecs::ChangeChannel`: a column carries a version, a write
	// through `Set` or `GetMutable` advances it, and that covers almost
	// everything. The gap is the batch path — a system writing through a raw
	// column pointer advances no per-row stamp, because there is no per-row
	// write to hang one on, and `Store::MarkAllChanged` over-reports by design.
	//
	// So a consumer that must know exactly which rows differ recomputes this at
	// `PostSimulation` and compares. It costs a pass over the data it is
	// hashing, every tick, whether anything moved or not — which is why the
	// answer is almost always the column version instead.
	//
	// Add one only where the batch path is genuinely the writer. If this starts
	// appearing next to components nothing writes in bulk, it has spread and
	// the fix is to delete it there rather than to make it cheaper.
	//
	// @since v0.4
	struct QuickHash {
		// The hash as of the last `PostSimulation`. Zero is a real value and
		// not "unset": what makes a comparison meaningful is that both sides
		// were computed by the same function, not that either is non-zero.
		uint64_t Value = 0;
	};

	// How far a world reaches from its own origin.
	//
	// **A resource, and that is the whole point of the type.** It arrived here
	// as two: `mono.server`'s `WorldBounds`, the cube entities bounce inside,
	// and `mono.client`'s `SceneBounds`, the radius the camera frames. Both are
	// one number describing the whole world, so both are this — and the server's
	// was a *component* first, which is what makes the reasoning worth keeping:
	// it was the same four bytes on every one of four thousand entities, a
	// column in the archetype and a load in the bounce loop's inner body for a
	// number the loop already knew. `ecs/AGENTS.md` names that exact shape.
	// Hoisting it also makes "the world is 128 wide" a property of the world
	// rather than something 4096 entities happen to agree on.
	//
	// Not a world AABB. A world AABB is a function of `Transform` and `Bounds`
	// and is derived per entity; this is authored, it is one per world, and
	// nothing recomputes it.
	//
	// **The replication wire's position grid is the other half of this
	// number.** `Wire.hpp` quantises a `Transform` over a stated extent, and
	// how coarse that grid is depends entirely on how far the world reaches —
	// two millimetres over 128 metres is a different figure over four
	// kilometres. `WireCoversWorld` is the check, and it belongs beside
	// whatever authors this value rather than inside the encoder, which sees
	// one component and not a world.
	//
	// @since v0.4
	struct WorldBounds {
		// Metres from the origin to the edge, on each axis, so the world spans
		// twice this. Stored as the half because that is the form a containment
		// test wants, and deriving it per entity per tick is arithmetic nobody
		// needs to repeat.
		float HalfExtent = 64.0f;
	};
}
