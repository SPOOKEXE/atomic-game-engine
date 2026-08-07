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

	// The material a part is drawn with when nobody says otherwise.
	//
	// **A function rather than a constant, because a `core::Name` is interned
	// and interning needs the registry to exist.** A namespace-scope `const
	// core::Name` would be constructed during static initialisation, in an order
	// nothing specifies, and the registry it needs is itself a function-local
	// static somewhere else. A function-local static is initialised on first
	// call and therefore after whatever it depends on, which is the same
	// arrangement `NormalIdEnum` in `Part.cpp` uses for the same reason.
	//
	// Interned once per process and returned by reference. `Name.hpp` states the
	// rule — constructing from a literal takes the registry mutex and hashes a
	// string — and this is read once per default-constructed `Visual`, which is
	// once per row a column grows by.
	//
	// @return `Plastic`, a member of the `Material` enum `scene::Part.cpp`
	//         registers.
	// @since v0.8
	const core::Name &DefaultMaterial();

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
		//
		// **`Plastic` rather than invalid**, so a part that nobody has authored
		// a material for reads back a member of the enum instead of a blank. An
		// invalid name here meant the properties panel showed an empty combo on
		// every fresh part — a control whose current value is not one of the
		// values it offers — and a script reading `part.Material` got something
		// it could not compare against `Enum.Material.Plastic` even though that
		// is exactly what the part was drawn as.
		//
		// `Mesh` above is deliberately *not* given the same treatment: an
		// invalid mesh means "the consumer's own default", which is a real and
		// useful state, and there is no name for the unit cube to give it.
		core::Name Material = DefaultMaterial();

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

	// What a surface is made of, beyond the flat colour `Visual` carries.
	//
	// Roblox's `SurfaceAppearance`, and `ROADMAP.md` v0.9 asks for it "as
	// components" rather than as a child instance — which is the right shape
	// here for `Surface`'s reason: it is read once per drawable per frame, and
	// a child object would make that a tree walk.
	//
	// **On `BasePart` rather than on `MeshPart`, so every drawable has one.**
	// That is a real cost — a name and two more fields on a column that holds
	// four thousand cubes — and it is paid deliberately. The alternative is an
	// optional component, which means the draw-list pass either joins it per
	// row or walks the world twice; `client::CollectInstances` is a batched
	// parallel loop over a fixed signature, and an optional column is precisely
	// what that shape cannot express. A dense column of mostly-invalid names is
	// sixteen bytes an entity and no branches.
	//
	// **Only `ColourMap` is sampled today**, and the other maps a physically
	// based pipeline wants are deliberately absent rather than declared and
	// ignored. `RENDER_PIPELINE.md` puts the G-buffer at v0.10; a
	// `MetalnessMap` field that nothing reads would be half a feature somebody
	// would reasonably assume worked.
	//
	// @since v0.9
	struct SurfaceAppearance {
		// The texture sampled for base colour, multiplied by `Visual::Tint` and
		// by the submesh's own base colour.
		//
		// A name, for `Visual::Mesh`'s reason: a texture reference has to
		// survive a save file and a wire. An invalid name means the submesh's
		// own texture is used, and a submesh with none draws its base colour
		// flat — which is how an untextured import looks right rather than
		// black.
		core::Name ColourMap;

		// Below this alpha a fragment is discarded rather than blended, when
		// `Mode` is `Clip`.
		float AlphaCutoff = 0.5f;

		// How the alpha channel of `ColourMap` is treated.
		//
		// **Three modes and not a bool**, because the third is the one a
		// character model needs: hair and eyelashes are authored as cut-out
		// planes, and blending them costs a per-pane sort that a discard does
		// not.
		AlphaMode Mode = AlphaMode::Opaque;

		// Explicit padding, for the reason every other `Reserved` gives.
		uint8_t Reserved[3] = {};
	};

	// Which tags an entity carries, as a bitmask.
	//
	// **A mask and not a list of names, and the names live in a `TagTable`
	// resource.** `AGENTS.md` rule 4 in both directions: a tag crosses a save
	// file as its string, and inside one process it is a bit — so a render pass
	// asking "is this instance in the group this surface draws" is an `and`
	// rather than a string compare per instance per view.
	//
	// The alternative was a `std::vector<core::Name>` per entity, which is a
	// heap allocation on a component that has to survive being memcpy'd across
	// a process boundary — rule 3 forbids it outright.
	//
	// Thirty-two tags per world is the ceiling, and it is a real one:
	// `Tagging.hpp` says what happens at thirty-three.
	//
	// @since v0.9
	struct Tags {
		// One bit per registered tag. Zero is untagged, which is every entity
		// nobody has said anything about.
		uint32_t Mask = 0;
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

	// Something that makes a noise.
	//
	// **Where it is heard from is its parent's business, not this
	// component's**, and that is the whole shape of the design. A `Sound` under
	// `Workspace` is heard everywhere at one level; a `Sound` inside a part is
	// heard from that part and falls off with distance. Roblox's rule, kept for
	// `scene/AGENTS.md`'s standing reason — a tree that differs from the one
	// scripts expect is a migration nobody asked for — and it is also the right
	// rule: it means "attach a sound to a thing" is `Parent = thing` rather than
	// a second field naming what a hierarchy already says.
	//
	// So there is no position here and there must not be one. A `Sound` with an
	// `Emitter` position of its own would be a second opinion about where a
	// thing is, which is rule 2 with a speaker attached.
	//
	// **This module holds what a sound *is*; it plays nothing.** `scene` is
	// `shared` and a server has no mixer — it decides what is audible and
	// replicates that, and the sound is produced where somebody is listening.
	// The client walks these rows and drives `engine::audio`.
	//
	// Widest-first with the flags last, so the object representation a snapshot
	// writes holds no uninitialised bytes between fields.
	//
	// @since v0.9
	struct Sound {
		// The published asset that plays — a manifest name, extension
		// included, exactly as `Visual::Mesh` names a mesh.
		//
		// **A name and never a path.** The manifest is the one place a name
		// becomes content, and a component holding a path would be a second.
		// An invalid name means this sound has nothing to play, which is what
		// a freshly created one is until a script says otherwise.
		core::Name SoundId;

		// How loud, with 1 being the sample as it was authored.
		//
		// Values above 1 are legal here and clamped once at the device, which
		// is `Sample.hpp`'s rule about headroom reaching the property surface:
		// a mixer sums, and defensive attenuation at every stage loses range it
		// cannot get back.
		float Volume = 0.5f;

		// How close the listener must be for a positional sound to be at full
		// volume, in metres.
		//
		// Ignored entirely when the parent has no place in the world, because
		// then there is nothing to be far from.
		float RollOffMinDistance = 10.0f;

		// Where a positional sound has fallen to silence, in metres.
		float RollOffMaxDistance = 200.0f;

		// Whether it starts again when it ends.
		bool Looped = false;

		// Whether it should be sounding now.
		//
		// **A property rather than a `Play()` method, and that is a statement
		// about the binding rather than about audio.** Script methods live on
		// one metatable shared by every instance — `script/src/Instances.cpp` —
		// so a `Play` there would be a method on every `Part` in the world.
		// Roblox has this property too and `sound.Playing = true` is what it
		// means; the day classes can carry their own methods, `Play()` sets
		// this and nothing else changes.
		bool Playing = false;
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

		// Which tags an instance must carry to appear in this camera's texture.
		//
		// **The half of tagging `ROADMAP.md` v0.9 asks for by name** — "render
		// pipeline capabilities for filtering tagged objects for redirected
		// pipeline work". A surface camera with a filter draws its group and
		// nothing else, which is what makes a second pipeline *redirected*
		// rather than merely a second copy of the same scene.
		//
		// **Zero means everything**, so filtering costs nothing for the scenes
		// that do not use it and a mirror stays a mirror without being told to
		// reflect the world.
		//
		// A mask rather than a name, for `Tags::Mask`'s reason: this is compared
		// against every instance in the view, and a name would be a lookup per
		// instance per pass. `TagTable::Register` is what turns one into the
		// other, once, wherever the camera is authored.
		uint32_t TagFilter = 0;

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
		// **When it is parented, the lens is the engine's too** — `NearPlaneZ`
		// is put at the glass and `FieldOfView` is fitted to the pane, because a
		// frustum that does not cover the pane draws a hard-edged rectangle of
		// reflection on a bare wall. A script setting either on a parented camera
		// is overwritten on the next frame, exactly as one setting its `CFrame`
		// is. Parent it to the world to own all three.
		//
		// Stored as the enum's underlying type so the component stays trivially
		// copyable and the row stays the size it was.
		NormalId Face = NormalId::Front;

		// Explicit padding, for the reason every other `Reserved` gives.
		uint8_t Reserved[2] = {};
	};

	// A point on a part, carried with it.
	//
	// **The one exception to "every transform here is world space and nothing
	// propagates it", and it is narrower than it looks.** An `Attachment` is not
	// a `PVInstance` and carries no `Transform`: it holds *its own* pair of
	// frames, one authored relative to a parent part and one derived from it by
	// a single flat pass. So there is still no transform hierarchy, no dirty
	// cascade and no per-entity parent walk in the simulation — there is one
	// loop over one component type, and everything else in this file is
	// untouched.
	//
	// **The derived frame is a field rather than a getter, and the reason is a
	// beam.** A beam reads both of its attachments' world frames every frame,
	// and a getter that resolved by walking to the parent would be two hierarchy
	// lookups and two `CFrame` products per beam per frame — for a value that is
	// the same for every reader within one frame. `ecs/AGENTS.md`'s rule against
	// two copies of a fact bends here for the reason `CameraMatrices` bends it:
	// the second copy is a *cache with one writer*, and `ResolveAttachments` is
	// that writer.
	//
	// **An attachment on nothing keeps its local frame as its world frame.**
	// Roblox's rule — an `Attachment` parented to a `Model` or to the tree root
	// has no part to be relative to — and it is what makes an attachment usable
	// as a bare point in space, which is what a beam between two world positions
	// needs.
	//
	// @since v0.10
	struct Attachment {
		// Where this point sits, relative to the parent part's own frame.
		//
		// A `CFrame` and not a `Vector3`, because an attachment carries a
		// direction as well as a position: a beam leaves along the attachment's
		// axis and a particle emitter's cone opens around it. A point with no
		// orientation would make both of those a second field.
		core::CFrame Frame;

		// The same point in world space, as of the last `ResolveAttachments`.
		//
		// **Derived, never authored.** A script writing this is writing a value
		// that is overwritten before anything reads it, which is why the property
		// surface exposes `WorldCFrame` as read-only and `CFrame` as writable —
		// the same split `GuiObject`'s absolutes have.
		core::CFrame WorldFrame;
	};

	// What kind of light an entity emits.
	//
	// **Three classes and one component**, which is the trade `Collider::Extent`
	// already makes across three shapes: the three differ by two fields, and
	// three components would be three columns, three queries and three upload
	// paths for something the renderer packs into one array either way.
	//
	// @since v0.10
	enum class LightKind : uint8_t {
		// Radiates in every direction from a point. Ignores `Angle` and `Face`.
		Point = 0,

		// A cone about the parent's `Face`, `Angle` degrees wide.
		Spot = 1,

		// A cone about the parent's `Face`, emitted from the whole face rather
		// than from a point.
		Surface = 2,
	};

	// Something that gives off light.
	//
	// **Where it shines from is its parent's business**, exactly as `Sound`'s is
	// — a `PointLight` inside a part lights the world from that part, and one
	// parented to an `Attachment` lights from the attachment. So there is no
	// position here and there must not be one, for `Sound`'s reason: a second
	// opinion about where a thing is, is rule 2 with a bulb attached.
	//
	// **Nothing in this module lights anything.** `scene` is `shared` and a
	// server has no renderer; the client walks these rows and fills its lighting
	// uniforms, which is the same split `Visual::Mesh` has against the renderer
	// and `Sound` has against the mixer.
	//
	// Widest-first with the flags last, so the object representation a snapshot
	// writes holds no uninitialised bytes between fields.
	//
	// @since v0.10
	struct Light {
		// What colour the light is.
		core::Color3 Colour{1.0f, 1.0f, 1.0f};

		// How strong it is, with 1 being Roblox's default.
		float Brightness = 1.0f;

		// How far it reaches, in metres.
		//
		// **A hard cutoff rather than a physical falloff**, which is Roblox's
		// `Range` and is also what a forward renderer needs: a light with no end
		// is a light every fragment in the world has to be tested against, and
		// the range is what lets a tile or cluster pass reject it.
		float Range = 8.0f;

		// How wide a spot or surface light's cone is, in degrees.
		//
		// Ignored entirely for a `Point`, which is why it is one field rather
		// than a separate component: a column of unused floats on the point
		// lights is four bytes, and a second archetype is a second query.
		float Angle = 90.0f;

		// Which face of the parent part a spot or surface light points out of.
		//
		// Stored as the enum so the component stays trivially copyable, and its
		// ordinals are the format — `scene/Enums.hpp` says why `NormalId` is the
		// one enum here whose numbers may never be reordered.
		NormalId Face = NormalId::Front;

		// Which of the three this is.
		LightKind Kind = LightKind::Point;

		// Whether it casts a shadow.
		//
		// **Authored and not yet read**, which is stated rather than hidden: the
		// renderer has one shadow-casting directional light and no shadow map per
		// point light. Declared anyway because it is the field that decides
		// whether a light is cheap, and a game authored without it would have to
		// be re-authored the day the pass exists. `SurfaceAppearance`'s rule
		// about `MetalnessMap` cuts the other way here and deliberately: that was
		// a field a physically based pipeline would *interpret*, and this is one
		// an author *decides*.
		bool Shadows = false;

		// Whether it is on.
		bool Enabled = true;
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
