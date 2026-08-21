#pragma once

// The Basic Components: what a thing in a world is made of.
//
// One definition each, for every program. Before this module the client and the
// server each carried their own `Transform` and their own position component,
// which was deliberate only while there was nowhere shared to put them -
// `mono.client/AGENTS.md` and `mono.server/include/server/Simulation.hpp` both
// said so in as many words. Two definitions of one fact is the debt this
// module exists to pay off.
//
// **Every transform here is world space and nothing propagates it.** The
// instance tree is organisational, exactly as Roblox's is: parenting a part to
// a model moves nothing and re-resolves nothing. That is what buys the engine a
// world with no transform-hierarchy pass, no dirty cascade and a physics step
// that reads `Transform` directly - see `ecs/Instance.hpp`, which makes the
// same point from the storage side.
//
// **What is a component and what is a resource is not a naming question.**
// `ecs/AGENTS.md` gives the rule - componentise what you iterate, one-of-a-kind
// state is a resource - and this file is where it gets applied. `Surface` is a
// name rather than two floats because friction and restitution are the same
// two floats on thousands of rows; the floats live in a `SurfaceTable`
// resource, which the narrow phase reads once.
//
// **Padding is named where it exists.** A trivially copyable component is
// serialised as its object representation, padding included, and padding is
// never initialised - so two runs of one scene produce different bytes and
// `just determinism` fails somewhere far from here. `ecs::WorldTime` learned
// this the expensive way; the `Reserved` fields below are that lesson.
//
// @tier L7 · shared

#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/scene/Enums.hpp>
#include <engine/spatial/LayerMask.hpp>

#include <cstdint>
#include <string>

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

	// Where a thing is grasped, relative to its own placement.
	//
	// **Roblox's `PivotOffset`, and the reason a pivot needs storage at all is
	// that a placement has no natural handle.** A `Transform` says where the
	// *centre* of something is, which is what a solver and a broad phase want
	// and almost never what an author wants: a door turns on its hinge, a lid on
	// its rim, a character stands on the ground under its feet. `PivotTo` is
	// "put this thing's handle here", and without an offset it can only ever
	// mean "put its centre here".
	//
	// **A column on every `PVInstance` rather than an optional component**, and
	// the trade is `SurfaceAppearance`'s exactly: an optional one means a
	// draw-time or query-time join, and `GetPivot` is asked per selection per
	// frame by the editor's gizmo. Twenty-eight bytes an entity and no branches.
	//
	// **Identity is the default and means "the centre".** So a part nobody has
	// given a pivot behaves precisely as it did before this existed, which is
	// what makes the field safe to add to every placed thing.
	//
	// @since v0.10
	struct Pivot {
		// The handle, in the instance's own frame. Composed as
		// `Transform::Frame * Offset` to get a world pivot.
		core::CFrame Offset;
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

	// That this body went through a portal, and how far the portal turned it.
	//
	// **A record rather than a movement, and the difference is which machine
	// needs it.** `scene::CrossPortals` maps a crossing body's placement and its
	// velocity, and that is the whole of the simulation - but a player's view
	// direction is not in either. It lives in `CameraController::Angles`, which
	// is a *resource on whichever host is looking*: a client's own, never the
	// authority's. So the host that moves the body cannot turn the camera, and
	// the host that owns the camera never sees the crossing - it receives a
	// transform that has already arrived somewhere else.
	//
	// This is the fact that crosses between them. It hangs off the body, so
	// replication carries it with everything else that body owns, and the eye
	// following it is a client reading its own subject's row.
	//
	// **The serial is what makes it an event.** A `Turn` on its own is a value
	// that happens to be the same after two identical crossings, so a delta
	// carrying only the angle would deliver the first and swallow the second -
	// a portal that works once. A counter changes on every crossing whatever the
	// angle was, and a consumer that has seen a number knows it has acted.
	//
	// @since v0.15
	struct PortalTransit {
		// How many times this body has been through a hole. Starts at one:
		// zero is "never", which is what a consumer that has seen nothing holds.
		uint32_t Serial = 0;

		// The yaw the last crossing turned it by, in radians, and only the yaw.
		//
		// **Only the yaw, because only the yaw is the player's to keep.** Pitch
		// is theirs and a portal that rolled a camera would be one nobody could
		// walk through twice. Measured off the map itself rather than off the
		// body, so it is the same number for anything that goes through and does
		// not depend on which way the crosser happened to be facing.
		float Turn = 0.0f;
	};

	// The last `PortalTransit::Serial` a presenting host has drawn.
	//
	// **Render-side history with no wire form, exactly like
	// `PreviousTransform`** - and it is here for the same problem that one has.
	// `CrossPortals` maps a crossing body's `PreviousTransform` through the seam
	// so the frames between the tick and the next one blend *inside* the
	// destination room rather than across the hundred units between the panes.
	// That fix is local to whoever simulated the crossing, and a client did not:
	// it receives a `Transform` that has jumped and holds a `PreviousTransform`
	// from the room the body left, so it interpolates straight through the gap
	// and the character is drawn once or twice somewhere in between. What that
	// looks like is a body streaking across the world on every crossing.
	//
	// **A serial rather than a flag, and it is the same serial the camera
	// already follows.** A flag has to be cleared by somebody and is lost with
	// the packet that carried it; a counter is idempotent, survives a dropped
	// delta, and answers "how many crossings have I not drawn yet" rather than
	// "was there one". `PortalTransit` crosses the wire already, so this needed
	// no new packet at all - which is what the "portal move packet" question was
	// really asking.
	//
	// The authority writes this at the moment it crosses a body, so the snap
	// below is a no-op there and the mapped `PreviousTransform` it computed
	// survives. A replica never writes it except by drawing, so the snap fires
	// exactly once per crossing per viewer.
	//
	// @since v0.17
	struct PortalTransitSeen {
		// Which crossing this viewer has already accounted for. Compared with
		// the transit's own serial: equal means the snap has happened, and any
		// other value means it is due.
		uint32_t Serial = 0;
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
	// `Motion` moves - a platform, a projectile, a demo cube - and none of them
	// needs a mass. So `Integrate` runs over `<Transform, const Motion>` and
	// never loads a mass it does not use, which is the same reasoning that took
	// the half-extent out of the rows it was repeated on.
	//
	// @since v0.4
	struct Motion {
		// Metres per second in world space. Integrated against the fixed tick
		// delta, never against measured frame time - a tick is a function of
		// its state, so a recorded run replays.
		core::Vector3 Linear;

		// Radians per second about each world axis.
		core::Vector3 Angular;
	};

	// Whether the world may move this part.
	//
	// **A tag rather than a flag.** Until v0.15 this was said by the *absence*
	// of `RigidBody`, which put the decision and the parameters in one
	// component: anchoring a part therefore threw away its mass and its drag,
	// and unanchoring it brought the defaults back rather than what the author
	// had typed. Those two jobs are separate now - `RigidBody` describes the
	// part and this says what the world may do with it.
	//
	// **Presence rather than a boolean, so the dynamic queries match the
	// archetype instead of visiting every part and rejecting most of them.**
	// Whether a component is on a row is a property of the archetype, so
	// `Query<...>()` naming this term costs one test per table per plan and
	// nothing per row - which is the whole reason this is a component and not a
	// `bool` on `RigidBody`.
	//
	// **Absence is static, and that is the point of the polarity.** Until v0.18
	// the tag was `Anchored` and marked the immovable ones, so `BasePart`'s
	// class set had to *add* a component to reach the safe default and every
	// placed thing in a scene carried one. Static is the overwhelming majority
	// of a world - walls, floors, props - and the majority should be the case
	// that stores nothing. A bare `Transform` is now static because it is bare,
	// which is also what a reader expects of a thing with no physics on it.
	//
	// It also turns every dynamic query from an exclusion into a positive term,
	// and a positive term is what the ECS matches archetypes on.
	//
	// **Paired with `Motion`, and the pair is what makes sleeping expressible.**
	// This is whether the world *may* move it; `Motion` is whether it is moving
	// now. A sleeping body keeps this and loses `Motion` - `physics::Publish`
	// takes it away, which is the archetype move sleeping is built on. So:
	//
	// - this and `Motion`: awake and simulated.
	// - this, no `Motion`: asleep. Immovable for the tick, and the solver's wake
	//   pass can give it back.
	// - neither: static. Immovable, and there is no way back short of a script
	//   assigning `Anchored = false`.
	//
	// The middle row is why the tag cannot be dropped in favour of testing
	// `Motion` alone. Both a wall and a sleeping crate lack `Motion` and both
	// take infinite mass in the solve, but only one of them is coming back, and
	// this is what says which. `physics::FactsFor` reads it for exactly that.
	//
	// Roblox's `Anchored` is the negation of this, and `scene::AnchoredProperty`
	// is the only place that inversion is spelled.
	//
	// @since v0.18, and `Anchored` with the opposite polarity since v0.15
	struct Simulated {};

	// What a part weighs, how it sheds speed, and what the solver may do with
	// it.
	//
	// **On every `BasePart`, simulated or not**, because all four fields are
	// authored rather than simulated: an author types a mass and a drag, and a
	// part that is anchored for a while should still have them afterwards.
	// `Simulated` is what decides whether the solver visits the row.
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
		// sleeping row, and a flag on this row is the opposite of that - it is
		// only readable by making the visit the archetype move exists to avoid,
		// and it is the same state the solver already has to keep. `physics`
		// owns it now, and `physics/AGENTS.md` carries the whole decision.
		uint8_t Reserved[3] = {};
	};

	// Which player simulates a body, when it is not the server.
	//
	// **Absent means the server owns it, and that is the whole default.** Every
	// world this engine has ever run is server-owned throughout, so ownership had
	// to cost an unowned world nothing - a component nobody attaches is an
	// archetype nobody visits, where a `bool ServerOwned = true` on `RigidBody`
	// would have been a byte on every body in every scene to say what all of them
	// already said.
	//
	// **A `Player` instance rather than a `replication::ClientId`.** A client
	// handle is `replication`'s at L11 and this is `scene` at L7, so naming one
	// here would invert the stack; but the deeper reason is that a script is the
	// thing that assigns ownership and a script has a `Player`, not a socket. The
	// host maps the one to the other, which is the same direction every other
	// authority decision travels. It is also what Roblox does.
	//
	// **What this does *not* yet do**: nothing reads it. Physics still integrates
	// every body on the server and `Authority` still sends every replicated
	// component to every interested client, exactly as before. It is here first
	// so ownership is expressible and observable before it is load-bearing -
	// making it load-bearing means a client→server state path, and that is a wire
	// change with a trust decision inside it rather than a component.
	//
	// An `ecs::Entity` needs no hand-written serialiser: it is a directory index
	// and a snapshot restores the directory exactly, which is the same reason
	// `ecs.Hierarchy` uses the generated form.
	//
	// @since v0.13
	struct NetworkOwner {
		// The `Player` instance that simulates this body. A null entity is the
		// same statement as having no component at all - a script that sets the
		// owner to `nil` removes it rather than storing a hole.
		ecs::Entity Player;
	};

	// A reason this world must keep ticking even with nobody in it.
	//
	// **Occupancy is a host's question and this is the game's answer to it.**
	// `world::DecideLifecycle` suspends a world nobody is in, and a host can see
	// players and viewports and nothing else - so a world whose NPCs are walking
	// a route, whose economy is settling, or which is counting down between
	// rounds looks exactly like an abandoned one from outside. A script attaching
	// this to any entity says otherwise, and it is the only way to say it that
	// does not require the engine to guess what a game considers activity.
	//
	// **Attached to an entity rather than set on the world**, because the thing
	// that needs the world awake is a thing in it: the NPC, the conveyor, the
	// round timer. That makes the lifetime automatic - destroy the entity and the
	// claim goes with it, which is the failure mode a world-level flag has, where
	// somebody sets it and the code that would have cleared it never runs.
	//
	// Any number of entities may hold one and the world stays awake while at
	// least one does. Cheap on the shape every game actually has: the host walks
	// this component, and a game that never attaches one has no rows to walk.
	//
	// **Deliberately not replicated** - see `replication::LocalToTheClient`. It
	// is a statement about hosting rather than about what the world looks like,
	// and a client has no use for it and no business setting it.
	//
	// @since v0.13
	struct AwakeWorld {
		// Why, for whoever reads the log or the panel.
		//
		// **Required rather than optional, and that is the point of it.** A world
		// that will not sleep is a world that costs a machine indefinitely, and
		// the question somebody eventually asks is not whether one is held awake
		// but *what* is holding it. A tag component answers that with a scan of
		// entity ids; a reason answers it with a sentence.
		core::Name Reason;
	};

	// What a thing collides as.
	//
	// Separate from `Bounds` because the two answer different questions: bounds
	// is the box everything derives a world AABB from, and this is the exact
	// shape the narrow phase intersects. A sphere's AABB is not a sphere.
	//
	// @since v0.4
	// Marks a collider that exists only because its owner is standing in a hole.
	//
	// **A body in a seam is in two rooms and the solver knows about one.** A
	// character walking into a doorway is held up by the near room's floor and by
	// nothing on the far side, so a far room whose floor is a stud higher lets it
	// clip and one a stud lower lets it hang. `scene::CutAndCloneSeams` answers
	// the same question for the picture; this is the contact half, and the two
	// are deliberately different mechanisms - a picture and a contact have
	// nothing to share but the seam.
	//
	// **What is mapped is the far room's geometry, not the body.** The obvious
	// arrangement is a kinematic twin of the body placed on the far side, and it
	// needs every contact the twin resolves mapped back through the seam as an
	// impulse on the original - a second solver path, in a module that has one.
	// Mapping the other way needs none of that: the far room's colliders are
	// copied *into the near room* through the inverse seam, where they are
	// ordinary static geometry and the body is pushed by them in its own space.
	// The same trick a shadow through a hole wants, for the same reason.
	//
	// **Not saved, not replicated, not drawn.** A proxy is created in
	// `PreSimulation` and destroyed in `PostSimulation`, so it never survives the
	// tick that made it; it is parented to nothing, so `SyncRendered` never marks
	// it and the tree that serialises never reaches it; and
	// `replication::LocalToTheClient` names it, so nothing puts it on the wire.
	//
	// @since v0.15
	struct PortalProxy {
		// The body this was made for, so a proxy never collides with the very
		// thing it is holding up.
		ecs::Entity Owner = ecs::NULL_ENTITY;
	};

	// What a part collides as, which is not what it draws as.
	//
	// **Separate from `Visual` deliberately.** A part's picture and its shape
	// are different questions - an invisible wall is a collider with no visual
	// and a decoration is a visual with no collider - and folding them into one
	// row would make every part pay for both.
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

		// Which baked shape this collides as, for `ShapeKind::Hull` and
		// `ShapeKind::Mesh`. Invalid, and unread, for the other three.
		//
		// **A name and not a handle**, which is the rule `Visual::Mesh` states
		// and this follows for the same reason: a `server`-tier host writes this
		// and has no device, a save file has to survive being reopened, and rule
		// 4 of the root `AGENTS.md` says anything crossing a boundary is
		// identified by its string. Whoever loads content resolves it once,
		// into `scene::CollisionShapes`.
		//
		// **A name nothing has baked collides as the part's bound**, which is
		// the fallback `physics` applies and is stated here because it is the
		// behaviour an author sees. The alternative - a part that silently stops
		// colliding while a mesh streams in - is a floor that is not there for
		// two seconds after a level loads.
		//
		// **Placed here rather than after `Shape` on purpose.** A `core::Name`
		// needs four-byte alignment and the three bytes after `Shape` are a
		// tail; put there it would have cost eight rather than four.
		//
		// @since v0.17
		core::Name Geometry;

		// Which shape `Extent` describes, or which kind of baked geometry
		// `Geometry` names.
		ShapeKind Shape = ShapeKind::Box;

		// Whether contacts are reported without being solved. A trigger
		// produces events and no impulse.
		bool Trigger = false;

		// Explicit padding, for the reason `RigidBody::Reserved` gives.
		uint16_t Reserved = 0;
	};

	// What one part overrides about the physics of its own material.
	//
	// **Roblox's `CustomPhysicalProperties`, as a component.** `Surface` names a
	// material and `SurfaceTable` says what that material feels like, which is
	// the right shape for the ninety-nine parts in a scene made of the same
	// wood. This is the hundredth: the crate that is deliberately heavier, the
	// ramp that is deliberately slippery. It is an override of a shared fact and
	// not a replacement for it.
	//
	// **`Custom` decides, and it is a field rather than the component's
	// presence.** The component is on every `BasePart` - `SurfaceAppearance`
	// carries the argument for a dense column over an optional one, and a
	// properties panel that could only show these fields on *some* parts would
	// be a panel with a hole in it. So the flag is what says "use these numbers
	// rather than the material's", and a part nobody has touched is four floats
	// of defaults that nothing reads.
	//
	// **Density and not mass.** `RigidBody::Mass` is what the solver wants and
	// stays the one place a mass is written; this is what a mass is *made* of,
	// so a part resized after its density was set weighs what its new size says
	// rather than what it weighed before. `scene::MassOf` is the one rule, and
	// both the solver and the properties panel ask it.
	//
	// **Drag is deliberately not here.** `RigidBody::LinearDamping` and
	// `AngularDamping` are drag and have been since v0.4; a second pair on this
	// component would be two places to write one number, and the panel shows the
	// pair that the integrator actually reads.
	//
	// @since v0.14
	struct PhysicsProperties {
		// Kilograms per cubic metre, used with the collider's volume when
		// `Custom` is set. Roblox's default part density.
		float Density = 0.7f;

		// Coulomb friction, replacing the material's when `Custom` is set.
		float Friction = 0.5f;

		// Restitution - 0 for a dead stop, 1 for a lossless bounce - replacing
		// the material's when `Custom` is set.
		float Elasticity = 0.0f;

		// Whether any of the three above are used at all.
		bool Custom = false;

		// Explicit padding, for the reason `RigidBody::Reserved` gives: three
		// uninitialised bytes go straight into a snapshot and make two runs of
		// one scene differ.
		uint8_t Reserved[3] = {};
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
	// file and a wire - `core::Name` is the type that rule has a name for. A
	// presentation module resolves a name to whatever device object it keeps;
	// nothing about that resolution belongs in a component.
	//
	// @since v0.4
	struct Visual {
		// Flat multiplier over whatever the material produces.
		core::Color3 Tint{1.0f, 1.0f, 1.0f};

		// The mesh to draw. An invalid name means the consumer's own default -
		// a unit cube, today.
		core::Name Mesh;

		// Which mesh `Bounds::HalfExtent` was last shaped to fit.
		//
		// **`Size` is a box the mesh is stretched into**, so a part whose box is
		// the wrong shape distorts whatever is put in it - and only the geometry
		// knows the right shape. An editor therefore reshapes the box when a mesh
		// is chosen. The question this field answers is *when it may stop*.
		//
		// **A name rather than a "fitted" flag, because the flag has no good
		// value to be reset by.** What must happen is: fit when the mesh changes,
		// and never again. A bool needs somebody to clear it on every write to
		// `Mesh`, in every path that writes one - a property panel, a script, a
		// game file, a replication delta - and the one that forgets produces a
		// part that silently stops fitting. Recording *which* mesh the box was
		// shaped for makes the comparison the condition: `Fitted != Mesh` is
		// exactly "the mesh changed since the box was shaped", with nothing to
		// keep in step.
		//
		// **Saved, which is what makes reopening a place leave sizes alone.** A
		// part somebody deliberately squashed would otherwise be reshaped the
		// first time its mesh arrived in a new session - a scene that quietly
		// rearranges itself on load, which is the worst kind of surprise because
		// nothing did it.
		//
		// Invalid on a part nothing has fitted, which is every part that has
		// never named a mesh.
		//
		// @since v0.10
		core::Name Fitted;

		// How much of what is behind shows through, 0 to 1.
		//
		// **The field is cheap and the ordering is not**, which is why this
		// arrived with a renderer feature rather than with a binding. Opaque
		// geometry draws in any order and transparent geometry does not, so a
		// non-zero value here puts the entity in a second pass sorted
		// back-to-front per view - the first thing the renderer does that
		// depends on *which camera is looking*.
		//
		// **Distinct from `Visible`, and they must not be conflated.** A part at
		// `Transparency = 1` is invisible and still collides; a part with
		// `Visible = false` is not submitted at all. A draw path that treated
		// one as the other would either give invisible parts no physics or make
		// hidden parts cost a sort.
		float Transparency = 0.0f;

		// Which surface texture this entity shows, or -1 for none.
		//
		// **Sixteen bits since v0.17, and it is here rather than below `Visible`
		// so the row did not grow.** It was an `int8_t` sitting in the named
		// padding at the tail, which put a ceiling of a hundred and twenty-seven
		// mirrors in the smallest field in the engine. Widened in place it would
		// have needed two-byte alignment after a `bool` and cost four bytes;
		// moved up against `Transparency` it is already aligned, and the two
		// bytes come out of `Reserved` instead. `sizeof(Visual)` is what it was.
		//
		// See `DrawInstance::Surface` for what the field means and why it is a
		// mirror feature rather than a general one.
		int16_t Surface = -1;

		// Whether this entity is submitted for drawing at all.
		bool Visible = true;

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
		// **Only opaque geometry reaches the shadow pass anyway** - a blended
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
		// - they are named anyway, because the day somebody re-registers this
		// type without one is the day three uninitialised bytes start ending up
		// in a recording.
		//
		// **`Surface`, `CastShadow` and `Locked` each took one of the three
		// bytes this originally held**, which is what named padding is for: a
		// field that fits goes in the hole rather than widening the row.
		// `Transparency` did not fit - a float needs four-byte alignment and
		// these are the tail after a `bool` - so the struct grew by four for
		// that one and by nothing for the other three.
		//
		// **Widened back to four at v0.12, deliberately and once.** The hole was
		// empty and the next `bool` would have grown the row anyway; taking the
		// four bytes now means the *next four* one-byte fields are free rather
		// than the next one costing four and the three after it being free. Same
		// total, paid at a moment somebody chose.
		//
		// **What the four bytes cost, stated rather than waved at.** `Visual` is
		// on every drawable, so a scene of 4096 parts pays 16 KB - one L2 way on
		// most machines, against a component the draw-list walk reads once per
		// entity per frame. `client::CollectInstances` is the loop that would
		// feel it and it is bandwidth-bound on `Transform` long before this.
		//
		// **The invariant is that this stays the last member.** A field appended
		// after it reopens a hole in the middle silently, which
		// `engine.scene.components` checks by asserting that the padding ends
		// where the struct does.

		// Whether an editor's click may select this entity.
		//
		// **Roblox's `BasePart.Locked`, and it is authoring data rather than an
		// editor mode.** A locked part is one somebody deliberately took out of
		// reach - a baseplate, a wall they keep catching while boxing over it -
		// and the whole value of saying so is that it survives a save and comes
		// back tomorrow. An editor-side set of "parts I am ignoring" would be a
		// second copy of a fact the world could have held, kept nowhere the
		// game file can see.
		//
		// **It changes nothing about the simulation.** Physics, the draw list
		// and every script read straight past it; the only consumer is a
		// pointer pick, which is why the field is one byte in existing padding
		// rather than a component of its own.
		//
		// **Not the same as `Simulated`**, which is where it would most easily
		// be confused: that decides whether the physics moves it, and this
		// decides whether a person can grab it. A locked part still falls.
		//
		// @since v0.12
		bool Locked = false;

		// Room for the next three one-byte fields.
		//
		// **Four until v0.17, and `Surface` took one of them when it widened to
		// sixteen bits.** The other went to alignment: the field moved up beside
		// `Transparency` to get its two-byte alignment for free, which shifted
		// the three `bool`s down one and left three bytes here instead of four.
		// Same total, and the row is the size it was.
		//
		// **Explicit, because padding is never initialised and this component
		// reaches a snapshot.** `Visual` is registered with a written serialiser
		// so these bytes do not cross today - they are named anyway, because the
		// day somebody re-registers this type without one is the day three
		// uninitialised bytes start ending up in a recording and every
		// comparison of two worlds becomes unreliable. `ecs::WorldTime` learned
		// that the expensive way and `just determinism` is what catches it.
		uint8_t Reserved[3] = {};
	};

	// How much of `Visual` a *viewer* has decided to see through, never the
	// world.
	//
	// **Roblox's `BasePart.LocalTransparencyModifier`, and the same reason for
	// it: a camera that clips into its own subject has to fade the geometry in
	// front of the eye, and doing that by writing `Visual::Transparency` would
	// be one machine editing a fact every other machine draws by.** A crate a
	// poppercam thinned out for one viewer must stay solid for everyone else
	// standing in the room, and `Transparency` is `scene.Visual`'s field -
	// replicated, signed, and the authority's to mean something by.
	//
	// **On the class the same way `SurfaceAppearance` is, for the identical
	// reason.** `client::CollectInstances` is a batched parallel walk over a
	// fixed signature, and an optional column is exactly what that shape cannot
	// express - see `SurfaceAppearance`'s own header. Four bytes on every part
	// is the price already paid for the four components ahead of it in this
	// file.
	//
	// **Overrides rather than adds, and only away from zero.** Roblox's field
	// is additive and this one is not: an override is what "take priority over
	// standard transparency if not set to 0" asks for, and it is also the
	// simpler rule for a script to reason about - a fade driven by distance
	// does not have to know what `Transparency` already held to cancel it back
	// out. `MakeDrawInstance` is where the override happens.
	//
	// **Never signed, never sent - see `replication::LocalToTheClient`.** A
	// `scene.`-prefixed component replicates by default, and the whole point of
	// this one is a value the authority does not get an opinion about. It is
	// registered so it can be a dense column at all, and excluded by name so
	// that registration never becomes a leak.
	//
	// **Writable only through `scene::SetLocalTransparency`, and not through
	// `Store::SetProperty`.** The ordinary property door refuses every write on
	// an adopt-only store, because a script setting a value the next
	// authoritative delta overwrites is a bug that hides - `Store::
	// SetPropertyValue` carries the whole argument. That refusal is exactly
	// right for a fact the authority owns and exactly wrong for one it was
	// never going to send in the first place: a player standing in a replica
	// has to be able to fade their own character, and a property that could
	// not be written on a replica would make the feature work only in
	// single-player. So this is read as an ordinary computed property and
	// written through a dedicated door, the same shape `ecs::SetAttribute`
	// already uses for the same reason.
	//
	// @since v0.18
	struct LocalTransparency {
		// 0 leaves `Visual::Transparency` alone. Anything else replaces it, for
		// this viewer, for as long as this row exists.
		float Value = 0.0f;
	};

	// The text a `StringValue` or a `LocalizationTable` carries.
	//
	// **One component for both, because both are an instance whose whole content
	// is a string.** Roblox has a `ValueBase` family - `StringValue`, `IntValue`,
	// `BoolValue` and the rest - and this engine has the one member of it that
	// something already needs: Rojo maps `*.txt` onto a `StringValue` and
	// `*.csv` onto a `LocalizationTable`, and a folder sync that could not build
	// either is a folder sync that silently drops files.
	//
	// **The other value classes are deliberately absent.** A class per primitive
	// is Roblox's answer to not having attributes; this engine has
	// `ecs::Attributes`, which holds a typed value under a name on *any*
	// instance and is strictly better for the job. Adding `IntValue` would be
	// adding a second way to hang a number off the tree.
	//
	// **A `LocalizationTable` here holds its CSV and does not resolve
	// anything.** Translation lookup is a service with a locale, a fallback
	// chain and a runtime API, and none of that is a file mapping - what this
	// buys is that the file survives a sync and a save, in the instance Roblox
	// would have put it in, ready for whoever writes the service.
	//
	// @since v0.12
	struct TextContent {
		// The file's contents, verbatim.
		//
		// **Not interned.** A `core::Name` never releases, and this is a value a
		// game *computes* as often as it reads one - `ecs::PropertyType::String`
		// exists for exactly this distinction and `D00020` is the leak that
		// established it.
		std::string Value;
	};

	// The GLSL a `ShaderScript` holds, as the world holds it.
	//
	// **Fragment stage only, and the omission is the design.** A vertex shader
	// has to agree with the renderer's instance layout, and `render/AGENTS.md`
	// says that layout is private and stays private - so an author who could
	// supply one would be authoring against a struct nobody promised to keep.
	// The fragment stage needs only the varyings and the sampler and uniform
	// slots `opaque.frag` already declares, which are a stated interface.
	//
	// **Not device data, which is what makes it legal here at all.** Apply this
	// module's own test: a headless server can hold this string, save it and
	// replicate it, exactly as it holds a Luau script's source. What it cannot
	// do is compile it - that is `render::ShaderLibrary`, at L12.
	//
	// @since v0.15
	struct ShaderSource {
		// The GLSL, verbatim.
		//
		// **Not interned**, for `TextContent::Value`'s reason: a `core::Name`
		// never releases and shader text is edited, so interning every revision
		// of it is `D00020`'s leak with a compiler attached.
		std::string Code;

		// Bumped every time `Code` is written through the `Source` property.
		//
		// **This is what makes a recompile a comparison rather than a string
		// diff.** `render::ShaderCompiler`'s header names "a `ShaderScript`
		// whose revision changed" as the case a runtime compiler exists for; a
		// library holding the last revision it compiled can answer "is this
		// still current" with an integer compare, per script, per frame.
		//
		// Zero is a script nobody has written to. It is not a version anybody
		// saves against - it counts writes in this process and starts again at
		// whatever a file restores.
		uint32_t Revision = 0;
	};

	// What a surface is made of, beyond the flat colour `Visual` carries.
	//
	// Roblox's `SurfaceAppearance`, and `ROADMAP.md` v0.9 asks for it "as
	// components" rather than as a child instance - which is the right shape
	// here for `Surface`'s reason: it is read once per drawable per frame, and
	// a child object would make that a tree walk.
	//
	// **On `BasePart` rather than on `MeshPart`, so every drawable has one.**
	// That is a real cost - a name and two more fields on a column that holds
	// four thousand cubes - and it is paid deliberately. The alternative is an
	// optional component, which means the draw-list pass either joins it per
	// row or walks the world twice; `client::CollectInstances` is a batched
	// parallel loop over a fixed signature, and an optional column is precisely
	// what that shape cannot express. A dense column of mostly-invalid names is
	// sixteen bytes an entity and no branches.
	//
	// **The other four maps are here now, and the rule they were held back by is
	// the reason they could arrive.** They were deliberately absent rather than
	// declared and ignored, because a field nothing reads is half a feature
	// somebody would reasonably assume worked. v0.11's G-buffer is the pass that
	// samples them, so they are declared and read on the same change.
	//
	// @since v0.9
	struct SurfaceAppearance {
		// The texture sampled for base colour, multiplied by `Visual::Tint` and
		// by the submesh's own base colour.
		//
		// A name, for `Visual::Mesh`'s reason: a texture reference has to
		// survive a save file and a wire. An invalid name means the submesh's
		// own texture is used, and a submesh with none draws its base colour
		// flat - which is how an untextured import looks right rather than
		// black.
		core::Name ColourMap = {};

		// The surface's other maps, sampled by the G-buffer pass.
		//
		// **Invalid is the ordinary state and means "use the default".** A
		// material authored flat has no normal map, and the shader falls back to
		// the geometric normal rather than to a texture of straight-up vectors
		// somebody has to remember to author. Roughness and occlusion fall back
		// to constants for the same reason.
		//
		// Height is sampled as a bounded parallax offset by the default PBR
		// G-buffer and forward surface paths. Invalid keeps the original UV.
		//@{
		core::Name NormalMap = {};
		core::Name RoughnessMap = {};
		core::Name OcclusionMap = {};
		core::Name HeightMap = {};

		// What this surface emits with no light on it. Invalid means nothing,
		// which is what almost every surface emits.
		core::Name EmissiveMap = {};
		//@}

		// Which shader this surface is drawn with, or invalid for the engine's.
		//
		// **Derived, exactly as the maps above are.** `ResolveMaterials` writes
		// it from the `Material` child's `MaterialRef::Shader`, so a part is
		// authored in one place and the draw-list pass reads one column - the
		// same arrangement, and the same reason, as `ColourMap`.
		//
		// **A name and not a handle**, rule 4: it survives a save file and a
		// wire, and it may name a `ShaderScript` in this world or a built-in
		// this engine ships. Which of those it is, is `render::ShaderLibrary`'s
		// question and not one a headless host can answer.
		//
		// @since v0.15
		core::Name Shader = {};

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

	// Which material an instance names.
	//
	// **The second field arrived with something that resolves it**, which is the
	// rule the first one's comment used to state alone: `Asset` was one field on
	// purpose because "a reference nothing resolves" is half a feature. `Shader`
	// is here because `render::ShaderLibrary` compiles what it names and
	// `ResolveMaterials` carries it onto the part in the same pass as the maps.
	//
	// **On the `Material` instance and not on the part**, which is what makes
	// this an object an author adds rather than a property every drawable
	// carries. `Materials.hpp` carries the argument in full, including why
	// `SurfaceAppearance` is still the field the draw path reads.
	//
	// @since v0.10
	struct MaterialRef {
		// The material asset's name - what a publisher called the `.amat`.
		//
		// A name, for `Visual::Mesh`'s reason: the reference has to survive a
		// save file and a wire. **An invalid one is `Material = None`**: an
		// instance somebody added and has not chosen a material for, which
		// resolves to no texture and draws `render::DefaultTexture`. That is the
		// default a fresh `Material` starts at, deliberately - the enum this
		// replaces defaulted to `Plastic`, a value the renderer could not act on,
		// and a default that means "nothing yet" is the honest one.
		core::Name Asset;

		// Which shader draws the parts wearing this material, or invalid for
		// the engine's own.
		//
		// **A name and not an entity handle, even though it usually names a
		// `ShaderScript` in this very world.** Rule 4: a handle is meaningless
		// in the world a save file is loaded into, and the same name also has to
		// be able to mean a shader this engine ships and no world contains. One
		// spelling covers both, and `render::ShaderLibrary` is the one place
		// that decides which it found.
		//
		// **On the material rather than on the part**, which is what makes it a
		// decision an author makes once for everything wearing it - the reason
		// `Material` is an instance at all. A part with no `Material` child is
		// never visited by the resolve pass and draws with the engine's shader.
		//
		// **Defaulted rather than left to the aggregate**, matching
		// `Visual::Shader` one struct up and for the plainer reason too: every
		// construction site names the members before it and stops, and
		// `-Wmissing-field-initializers` is fatal under the `ci` preset.
		//
		// @since v0.15
		core::Name Shader = {};
	};

	// Which tags an entity carries, as a bitmask.
	//
	// **A mask and not a list of names, and the names live in a `TagTable`
	// resource.** `AGENTS.md` rule 4 in both directions: a tag crosses a save
	// file as its string, and inside one process it is a bit - so a render pass
	// asking "is this instance in the group this surface draws" is an `and`
	// rather than a string compare per instance per view.
	//
	// The alternative was a `std::vector<core::Name>` per entity, which is a
	// heap allocation on a component that has to survive being memcpy'd across
	// a process boundary - rule 3 forbids it outright.
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
	// **A component, and there may be several** - a spectator, a cutscene, a
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
	// `scene/AGENTS.md`'s standing reason - a tree that differs from the one
	// scripts expect is a migration nobody asked for - and it is also the right
	// rule: it means "attach a sound to a thing" is `Parent = thing` rather than
	// a second field naming what a hierarchy already says.
	//
	// So there is no position here and there must not be one. A `Sound` with an
	// `Emitter` position of its own would be a second opinion about where a
	// thing is, which is rule 2 with a speaker attached.
	//
	// **This module holds what a sound *is*; it plays nothing.** `scene` is
	// `shared` and a server has no mixer - it decides what is audible and
	// replicates that, and the sound is produced where somebody is listening.
	// The client walks these rows and drives `engine::audio`.
	//
	// Widest-first with the flags last, so the object representation a snapshot
	// writes holds no uninitialised bytes between fields.
	//
	// @since v0.9
	struct Sound {
		// The published asset that plays - a manifest name, extension
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
		// one metatable shared by every instance - `script/src/LuauInstances.cpp` -
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
	// of them - but more than that, the presence of this component is what a
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
		// opaque image on it, and one number cannot say both - which is exactly
		// what went wrong: fading a mirror faded its reflection with it, so
		// there was no way to author glass that reflects.
		//
		// At 0 the image is solid and covers whatever the part would have drawn,
		// **whatever the part's own transparency is** - a fully transparent pane
		// still shows its reflection, which is what a mirror is. At 1 the image
		// is gone and the part draws as itself.
		float ImageTransparency = 0.0f;

		// Which tags an instance must carry to appear in this camera's texture.
		//
		// **The half of tagging `ROADMAP.md` v0.9 asks for by name** - "render
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

		// How many times a second this surface may redraw.
		//
		// **A surface is a whole scene render and there is no reason it should
		// keep the screen's rate.** A room of mirrors at 165 hertz is a room of
		// full scene passes at 165 hertz, and nothing a viewer can see in a
		// pane at arm's length needs them: a reflection is already a frame
		// behind by construction, and the eye cannot tell a reflection updated
		// at 120 from one updated at 165 while it can very much tell the
		// difference in frame time.
		//
		// **120 by default rather than uncapped**, because that is the rate
		// above which nobody has reported seeing the difference and below which
		// several people have reported the cost. A display slower than this
		// never reaches the cap and pays nothing for it; a fast one draws the
		// world at its own rate and the mirrors at this one.
		//
		// **Zero is uncapped**, which is what a surface wants when it is the
		// subject rather than the scenery - a camera feed somebody is looking
		// straight at, or a test that needs one render per frame.
		//
		// **Frames are dropped, never queued.** A surface past its interval
		// draws on the next frame that asks; one inside it keeps the texture it
		// has, and its matrices keep describing the camera that drew it, which
		// is the same contract the content signature already gives. So a capped
		// surface is not a delayed surface, it is one whose staleness has a
		// stated bound.
		//
		// @since v0.15
		float FPS = 120.0f;

		// Which surface index this camera writes.
		//
		// One today, and the field exists because the pipeline that replaces
		// this one will have several - a stage list that had to be rewritten to
		// add a second mirror would be a stage list that encoded the count.
		//
		// Negative is the scene pass's explicit "do not render": a disabled
		// portal, an edge-on mirror, or a pane the per-world limit did not have
		// room for this frame all clear their slot this way.
		int16_t Surface = 0;

		// What the image is put through before a pane shows it.
		//
		// **A grade on the way out, not a second render.** The surface pass is
		// unchanged whatever this says - the texture holds an ordinary picture
		// of the world - and the effect is applied where the pane samples it, in
		// `opaque.frag`. So it costs nothing to render and nothing to bind, and
		// two panes sampling one surface could in principle show it two ways.
		//
		// @since v0.13
		SurfaceEffect Effect = SurfaceEffect::None;

		// Which face of the parent part this camera projects off.
		//
		// **Only meaningful when this camera is parented to a `BasePart`**, which
		// is the arrangement `AimSurfaceCameras` exists for: the camera is placed
		// by the engine, mirrored through that face's plane, rather than by a
		// script computing the reflection itself. A camera parented to the world
		// keeps whatever `CFrame` it was given, because there is no face to
		// project off.
		//
		// **When it is parented, the lens is the engine's too** - `NearPlaneZ`
		// is put at the glass and `FieldOfView` is fitted to the pane, because a
		// frustum that does not cover the pane draws a hard-edged rectangle of
		// reflection on a bare wall. A script setting either on a parented camera
		// is overwritten on the next frame, exactly as one setting its `CFrame`
		// is. Parent it to the world to own all three.
		//
		// Stored as the enum's underlying type so the component stays trivially
		// copyable and the row stays the size it was.
		NormalId Face = NormalId::Front;

		// **There is no `Reserved` here any more, and that is the reserve having
		// done its job rather than the rule being dropped.** It held one byte;
		// `Surface` widening to sixteen bits at v0.17 took it, and the field
		// moved up above `Effect` so that its two-byte alignment came out of the
		// order rather than out of a fourth word. The row is twenty bytes with
		// no hole in it, which is what the reserve existed to guarantee - and
		// `ecs::AuditComponents` is what will say so the moment that stops being
		// true.
		//
		// The next byte-wide field costs four. That is the honest price and it
		// is better paid by whoever wants the field than pre-paid here by
		// widening a row every mirror in a world carries.
	};

	// Where a surface camera sends the other end of its hole.
	//
	// **A portal is a `SurfaceCamera` with a different rule for where it
	// stands, and this component is that rule.** Everything downstream is
	// unchanged: the camera is still placed by `AimSurfaceCameras`, still
	// rendered by the surface pass, still projected onto the pane by
	// `opaque.frag`, still filtered by `TagFilter`. What changes is one matrix -
	// a mirror reflects the eye through its own plane, and a portal maps it
	// through `destination · source⁻¹`.
	//
	// **The non-Euclidean part is that nothing constrains the pair of frames to
	// describe one space.** A destination rotated, moved or scaled anywhere at
	// all gives a room bigger on the inside, or a corridor that turns through
	// more than four right angles - with no separate feature, no exotic maths
	// and no second renderer. `NON-EUCLIDEAN.md` is the investigation that
	// settled this, and it is the whole insight of the demo it was filed
	// against.
	//
	// @since v0.14
	struct Portal {
		// The part this one leads to.
		//
		// **An entity rather than a name, because both ends are in one world.**
		// Rule 3 forbids a handle that crosses a world boundary and this never
		// does: a portal pairs two parts of one store, which is exactly the case
		// `PropertyType::Reference` exists for.
		//
		// `NULL_ENTITY`, or a destination that has been deleted, **falls back to
		// a mirror** rather than to a blank pane. A surface that stopped
		// reflecting reads as something to go and fix; a pane that vanished
		// reads as a rendering bug.
		ecs::Entity Destination;

		// Which world's contents the pane shows, or an invalid `Name` for this
		// one.
		//
		// **A name and not a handle, which is the only shape rule 3 allows.** An
		// `ecs::Entity` names a row in one store and means something else in
		// every other; a world's name is what already crosses - `Postbox::
		// Teleport` addresses by it for the same reason - so this is the same
		// arrangement a teleport uses, applied to what a camera draws instead of
		// to where a player goes.
		//
		// **`Destination` is still read, and still has to be a part in *this*
		// world.** It is what the camera is placed against: `AimSurfaceCameras`
		// maps the eye through `destination · source⁻¹` and that arithmetic
		// needs a `Transform` and `Bounds` it can reach. So a cross-world portal
		// is authored as a local stand-in placed where the far world's pane is,
		// and this field then says whose *instances* are drawn through it.
		//
		// That is exact when the two worlds share a coordinate frame - which is
		// the arrangement anybody builds a portal pair in, and the one the
		// `immersive-portals-demo` scenes use. A far world laid out somewhere
		// else entirely wants its offset baked into the stand-in's placement,
		// which is the same lie a same-world portal already tells and is
		// `NON-EUCLIDEAN.md`'s whole subject.
		//
		// **The host resolves it, not `AimSurfaceCameras`.** A store cannot
		// reach another store; `client::AttachForeignSurfaces` is what looks the
		// world up, copies its draw list and points the surface at it. A name
		// that matches no world leaves the pane showing this world, which is the
		// same fallback an unlinked portal gets and fails the same visible way.
		//
		// **Defaulted rather than left to the aggregate.** Every construction
		// site names the members before this one and stops, and
		// `-Wmissing-field-initializers` is fatal under the `ci` preset - so
		// this said "not set" in thirty-five warnings rather than in one `= {}`.
		// A default-constructed `Name` is the invalid one, which is already what
		// "no destination" means here.
		//
		// @since v0.14
		core::Name DestinationWorld = {};

		// Whether this mouth participates in the portal system.
		//
		// False removes the mouth from the per-view void capture, seam cuts,
		// crossings and portal lighting. The destination stays authored, so
		// switching this back on restores the same link without rebuilding it.
		//
		// @since v0.16
		bool Enabled = true;

		// Whether this mouth may be entered from behind as well as from in
		// front.
		//
		// **True is what a portal has always been and is what a pair wants.**
		// The map carries this pane's front hemisphere to the far pane's back
		// one and its back to the far pane's front, so a two-way mouth is one
		// rigid map that is its own inverse - walk through and walk back and you
		// are where you started.
		//
		// False is the one-way door: an entrance you can walk into but not out
		// of, or an exit that drops you into a room whose own pane you must not
		// be pulled back through. The pane still draws from both sides - what is
		// refused is the crossing, and only the crossing, because a mouth that
		// vanished when you walked round it would read as a rendering fault
		// rather than as a rule.
		//
		// In front means on the side the face's normal points at, which is the
		// side `SeamCarries` calls "not yet through".
		//
		// @since v0.19
		bool Bidirectional = true;

		// Explicit padding, for the reason every other `Reserved` gives.
		//
		// An `Entity` is eight bytes, a `Name` is four and the two flags are one
		// each, so the type's own alignment leaves two the compiler inserted and
		// nobody declared. `Column::Write` sends `sizeof(T)` bytes and does not
		// know which of them a member claimed.
		uint8_t Reserved[2] = {};
	};

	// The frustum a surface camera renders through, fitted to its pane.
	//
	// **Derived every frame and never authored**, which is why it is a component
	// rather than a property: `AimSurfaceCameras` writes it beside the
	// `Transform` it writes, from the same measurement of the same pane, and a
	// number here that disagreed with that placement would be a frustum aimed at
	// somewhere the camera is not.
	//
	// **It does not cross the wire.** A reflection is *of the viewer*, so a lens
	// computed on the authority is correct for the authority's camera and wrong
	// for every client watching - `client/Replicated.hpp` states that rule for
	// the placement and this is the same fact. `replication::LocalToTheClient`
	// names it, and both ends recompute it from the mirror that *does* cross.
	//
	// **Off-axis, which is what makes it a window rather than a cone.** The four
	// extents are independent, so a viewer standing to one side gets a frustum
	// that leans - covering exactly the pane and nothing else. The symmetric fit
	// this replaces spent half its texels on the far side of the face normal,
	// and `SurfaceCameras.hpp` named an off-axis frustum as what it was waiting
	// for.
	//
	// @since v0.14
	struct SurfaceLens {
		// The frustum's edges at `NearPlane`, in view space.
		//
		// Signed and independent: `Left` is negative and `Right` positive for a
		// viewer square on, and both slide the same way as the viewer moves
		// aside. That asymmetry is the point - see the type's comment.
		//@{
		float Left = -0.1f;
		float Right = 0.1f;
		float Bottom = -0.1f;
		float Top = 0.1f;
		//@}

		// Near clipping distance, in metres. The extents above are measured at
		// this plane, so the two cannot be read apart.
		float NearPlane = 0.1f;

		// Far clipping distance, in metres.
		float FarPlane = 500.0f;

		// The plane everything behind is clipped against, in world space.
		//
		// **A real oblique clip, and on a portal it is not optional.** The
		// destination is set into a wall, so the wall itself, its back face and
		// whatever stands behind it are all inside the frustum and would draw
		// over the view - the hole would show the back of the wall it leads
		// through. Skewing the projection's near plane onto this one is
		// Lengyel's method and is what removes them.
		//
		// A mirror wants the same thing for a smaller reason: the pane's own
		// plane, so the frame and the back of the glass do not occlude the
		// reflection. That used to be approximated by pushing `NearPlane` out
		// parallel to the face, which over-clips at grazing angles.
		//
		// **A zero normal means no oblique clip**, and the ordinary near plane
		// is used unmodified. That is what an unparented surface camera gets.
		core::Vector3 ClipNormal;

		// The plane's distance along `ClipNormal` from the origin, so that a
		// point `p` is kept when `ClipNormal · p - ClipDistance >= 0`.
		float ClipDistance = 0.0f;

		// What moved the pane into the space this camera was fitted to.
		//
		// **The other half of the portal, and without it a hole shows nothing.**
		// A pane reads its image by projecting *its own world position* through
		// the camera's matrix - `opaque.vert` does exactly that. For a mirror
		// the camera was fitted to the pane where it stands, so the raw position
		// lands on the image. For a portal the camera was fitted to the pane
		// **mapped to the destination**, three hundred units away in the demo,
		// and the raw position projects to somewhere outside the frustum
		// entirely: every fragment fails the `0..1` test and the pane falls back
		// to its own colour. That is a portal that places a camera, fits a
		// frustum, renders a texture and shows none of it.
		//
		// So the sampling matrix is `ViewProjection · Mapping` while the surface
		// is *rendered* with `ViewProjection` alone. The two are one code path
		// again, because this is the same transform the placement used.
		//
		// **Identity for a mirror rather than the reflection**, and the two
		// agree wherever it matters: a reflection fixes every point of the plane
		// it reflects through, so on the pane's own face they are the same map.
		// They differ off it - the sides and back of the pane's box - and
		// identity is what a mirror has always sampled with there.
		//
		// **The rigid half only.** A hole between two panes of different sizes
		// maps by a similarity, and the scale it carries lives in the two fields
		// below because a `CFrame` is a position and a rotation and nothing else.
		// `scene::SeamTransform` is the whole map and is what both halves are
		// taken from.
		core::CFrame Mapping;

		// The point `MappingScale` is taken about, which is the source pane's
		// centre in world space.
		//
		// **Carried rather than derived from the pane**, because the pane a
		// consumer has is a box and the centre this needs is the centre of one
		// *face* of it - which is `ReachOf` and a face id away, and is exactly
		// the sort of second derivation that ends up disagreeing by a
		// half-extent.
		//
		// @since v0.15
		core::Vector3 MappingOrigin;

		// How much bigger the far pane is, from `scene::PortalSeam::Scale`.
		//
		// One for a mirror, one for a matched pair, and one for anything a host
		// fills in by hand - so a consumer that has never heard of a scaled
		// portal composes the same matrix it always did.
		//
		// @since v0.15
		float MappingScale = 1.0f;
	};

	// A point on a part, carried with it.
	//
	// **The one exception to "every transform here is world space and nothing
	// propagates it", and it is narrower than it looks.** An `Attachment` is not
	// a `PVInstance` and carries no `Transform`: it holds *its own* pair of
	// frames, one authored relative to a parent part and one derived from it by
	// a single flat pass. So there is still no transform hierarchy, no dirty
	// cascade and no per-entity parent walk in the simulation - there is one
	// loop over one component type, and everything else in this file is
	// untouched.
	//
	// **The derived frame is a field rather than a getter, and the reason is a
	// beam.** A beam reads both of its attachments' world frames every frame,
	// and a getter that resolved by walking to the parent would be two hierarchy
	// lookups and two `CFrame` products per beam per frame - for a value that is
	// the same for every reader within one frame. `ecs/AGENTS.md`'s rule against
	// two copies of a fact bends here for the reason `CameraMatrices` bends it:
	// the second copy is a *cache with one writer*, and `ResolveAttachments` is
	// that writer.
	//
	// **An attachment on nothing keeps its local frame as its world frame.**
	// Roblox's rule - an `Attachment` parented to a `Model` or to the tree root
	// has no part to be relative to - and it is what makes an attachment usable
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
		// surface exposes `WorldCFrame` as read-only and `CFrame` as writable -
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
	// - a `PointLight` inside a part lights the world from that part, and one
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
		// ordinals are the format - `scene/Enums.hpp` says why `NormalId` is the
		// one enum here whose numbers may never be reordered.
		NormalId Face = NormalId::Front;

		// Which of the three this is.
		LightKind Kind = LightKind::Point;

		// Reserved for a local-shadow pass. It is deliberately not exposed as a
		// property until a renderer consumes it.
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
	// everything. The gap is the batch path - a system writing through a raw
	// column pointer advances no per-row stamp, because there is no per-row
	// write to hang one on, and `Store::MarkAllChanged` over-reports by design.
	//
	// So a consumer that must know exactly which rows differ recomputes this at
	// `PostSimulation` and compares. It costs a pass over the data it is
	// hashing, every tick, whether anything moved or not - which is why the
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
	// one number describing the whole world, so both are this - and the server's
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
	// how coarse that grid is depends entirely on how far the world reaches -
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
