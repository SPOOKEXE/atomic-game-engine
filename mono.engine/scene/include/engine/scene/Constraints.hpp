#pragma once

// What holds two bodies to each other, as one component and many classes.
//
// **One generic six-degree-of-freedom joint, not a struct per kind.** Roblox's
// constraint family is about twenty classes, and writing twenty components would
// be twenty columns, twenty queries and twenty solver paths for something every
// constrained-body solver in existence expresses as one thing: two frames, six
// axes, and a motion mode per axis. Bullet calls it `btGeneric6DofSpring2Constraint`
// and PhysX calls it a D6 joint; the shape is the same in both and it is the
// shape here.
//
// The family falls out of the six modes rather than out of a `Kind` field:
//
// - **Weld / Rigid** is every axis `Locked`.
// - **BallSocket** is three angular axes `Free`.
// - **Hinge** is one angular axis `Free` and the rest `Locked`.
// - **Prismatic** is one linear axis `Free`.
// - **Cylindrical** is one linear and the matching angular axis `Free`.
// - **Rope / Rod** is the linear axes `Limited` to a distance.
// - **Spring** is a `Limited` axis with a non-zero `Stiffness`.
// - **AlignPosition / AlignOrientation** are the drive: an axis with a
//   `Stiffness` pulled towards `Target` rather than clamped by a limit.
//
// **A `Kind` enum would be the second answer.** With one it would be possible to
// write a `HingeConstraint` whose angular axes were all free, and the solver
// would have to decide which of the two statements it believed. The class is how
// a kind is said - `Instance.new("HingeConstraint")` copies a prototype row whose
// modes are already a hinge's - which is exactly the trade `Light` makes across
// its three classes and `Collider::Extent` makes across its shapes.
//
// **`NoCollisionConstraint` is deliberately not in the family.** It is not a
// joint at all: it is a broadphase filter, and `Collider::Layer` and
// `Collider::Mask` are already this engine's answer to what may touch what. A
// class here that quietly meant something in `spatial` would be the second
// answer rule 2 refuses.
//
// **Nothing in this module solves one, and nothing here may.** `scene` is L7 and
// holds data; the solver is `physics` at L8, which reads these rows and is not
// read by them. The accumulated impulses a warm-started solve needs are
// per-step, so they belong in `physics::PhysicsWorld` beside the contact
// manifolds rather than on an authored row - `RigidBody` having no sleeping flag
// is the same decision already made once.
//
// arch-waiver public-header: forward API. The solver that reads these rows is
// `physics`', and `docs/FUTURE_COMPONENTS.md` says what it has to grow.
// Decision 16.
//
// @tier L7 · shared

#include <engine/core/types/CFrame.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Entity.hpp>

#include <cstddef>
#include <cstdint>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {

	// What one axis of a constraint is allowed to do.
	//
	// **Three modes and not two**, because `Limited` is what separates a rope
	// from a weld and a hinge with stops from a hinge without. A solver that had
	// only `Locked` and `Free` would need a second component to say "free between
	// these two numbers", which is the per-kind variant this design exists to
	// avoid.
	//
	// The numbers are the format: `Constraint` is trivially copied and its object
	// representation reaches a file, so reordering this is a format change. That
	// is `NormalId`'s decision restated.
	//
	// @since v0.19
	enum class ConstraintMotion : uint8_t {
		// Held at zero. The axis contributes a hard row to the solve.
		Locked = 0,

		// Held between `Constraint::Lower` and `Constraint::Upper` for this axis,
		// and free between them.
		Limited = 1,

		// Unconstrained. The axis contributes nothing unless it is driven.
		Free = 2,
	};

	// How many axes a constraint has: three linear and three angular.
	inline constexpr size_t CONSTRAINT_AXES = 3;

	// Two bodies held to each other, on a constraint instance.
	//
	// **Two attachments and not two parts**, which is Roblox's arrangement and is
	// right for the reason `Attachment` exists at all: a joint is anchored at a
	// point with an orientation, and naming the parts would leave the frame to be
	// stated somewhere else. The constraint's own frame is `Attachment0`'s, so
	// "the hinge axis" is that attachment's X axis and an author aims a joint by
	// turning an attachment they can see.
	//
	// **`Attachment1` may be null and that is a real case.** A constraint with one
	// end unattached is pinned to the world at `Attachment0`'s frame, which is how
	// a swinging door hangs off a wall that is not a body.
	//
	// @since v0.19
	struct Constraint {
		// Where the joint is anchored on the first body.
		//
		// **Widest first**, so the object representation a snapshot writes holds
		// no padding between the handles and the frames below them.
		ecs::Entity Attachment0;

		// Where it is anchored on the second, or a null entity to pin it to the
		// world.
		ecs::Entity Attachment1;

		// Where a driven joint is trying to get to, in `Attachment0`'s frame.
		//
		// **The drive and the limit are the same six axes read two ways**, which
		// is what folds `AlignPosition` and `AlignOrientation` into this type
		// instead of giving them one each: an axis with a `Stiffness` is pulled
		// towards this frame, and an axis without one is clamped by its limit.
		// PhysX's D6 drive is the same arrangement, and having tried the
		// alternative once - a separate mover component - the engine would then
		// have two things that both move a body towards a frame.
		core::CFrame Target;

		// What each linear axis of `Attachment0`'s frame may do.
		ConstraintMotion Linear[CONSTRAINT_AXES] = {
			ConstraintMotion::Locked,
			ConstraintMotion::Locked,
			ConstraintMotion::Locked,
		};

		// What each angular axis may do.
		ConstraintMotion Angular[CONSTRAINT_AXES] = {
			ConstraintMotion::Locked,
			ConstraintMotion::Locked,
			ConstraintMotion::Locked,
		};

		// Whether the solver acts on this joint at all.
		//
		// Roblox's `Constraint.Enabled`, and a flag rather than removing the
		// component because a game toggling a joint should not pay an archetype
		// move and a structural message per toggle.
		bool Enabled = true;

		// Explicit padding, for the reason `Components.hpp` opens with: this
		// component is trivially copyable and its object representation reaches a
		// file.
		//
		// **Five, and the number is what the members add up to rather than a
		// target.** An `ecs::Entity` is eight-byte aligned and so is the whole
		// struct, so without these the four limit arrays and the four scalars
		// would leave a four-byte hole at the end that nothing initialises and
		// every save and every delta would carry.
		uint8_t Reserved[5] = {};

		// How far a `Limited` linear axis may travel each way, in metres.
		//@{
		float LinearLower[CONSTRAINT_AXES] = {};
		float LinearUpper[CONSTRAINT_AXES] = {};
		//@}

		// How far a `Limited` angular axis may turn each way, in radians.
		//
		// **Radians and not degrees, unlike the property surface.** Everything the
		// solver does is trigonometric and Roblox's constraint angles are degrees,
		// so one of the two has to convert; doing it in the property setter is
		// `Orientation`'s arrangement and keeps the conversion at the one place a
		// human types a number.
		//@{
		float AngularLower[CONSTRAINT_AXES] = {};
		float AngularUpper[CONSTRAINT_AXES] = {};
		//@}

		// How hard a driven axis pulls towards `Target`, in newtons per metre.
		// Zero is an undriven joint, which is every joint an author has not asked
		// to move.
		float Stiffness = 0.0f;

		// How much a driven axis resists the speed it is closing at, in newton
		// seconds per metre. Roblox's `AlignPosition.Responsiveness` is this and a
		// stiffness rolled into one number; two are kept here because a
		// critically damped drive is `2 * sqrt(k * m)` and a game that wants one
		// cannot express it through a single dial.
		float Damping = 0.0f;

		// The most force a driven linear axis may apply, in newtons.
		//
		// **Infinite is not the default**, because an unbounded drive against a
		// wall is how a solver produces a body moving at a kilometre a second.
		// Roblox defaults `MaxForce` to a large finite number for the same reason.
		float MaxForce = 100000.0f;

		// The most torque a driven angular axis may apply, in newton metres.
		float MaxTorque = 100000.0f;
	};

	// Whether every axis of a constraint is held at zero.
	//
	// **The one predicate this module offers about a constraint**, because it is
	// the question two separate consumers ask: a solver deciding whether to build
	// six rows or one rigid block, and an editor deciding whether to draw a joint
	// or a weld. Two statements of it would disagree about a joint whose axes were
	// locked one at a time.
	//
	// @param joint The constraint.
	// @return `true` when all six axes are `Locked`.
	bool IsRigidJoint(const Constraint &joint);

	// Whether any axis of a constraint is driven towards `Target`.
	//
	// A joint with a stiffness and nothing free to move is still a weld, so this
	// asks about the stiffness and about the modes together.
	//
	// @param joint The constraint.
	// @return `true` when a non-locked axis has a non-zero stiffness.
	bool IsDrivenJoint(const Constraint &joint);

	// The bodies a constraint joins, resolved through its attachments.
	//
	// **A function rather than two more fields**, for the reason `EquippedTool` is
	// a walk rather than a flag: an attachment's parent already says which part it
	// is on, and a cached pair would go stale the first time a script reparents
	// one.
	//
	// @param store   The world.
	// @param joint   The constraint.
	// @param first   Filled with the part `Attachment0` sits on.
	// @param second  Filled with the part `Attachment1` sits on, or a null entity.
	// @return `true` when at least the first end resolves to a part.
	bool
	JointBodies(const ecs::Store &store, const Constraint &joint, ecs::Entity &first, ecs::Entity &second);

	// The base `Constraint` class id, registering the scene tree on first call.
	//
	// The concrete classes - `WeldConstraint`, `BallSocketConstraint`,
	// `HingeConstraint`, `PrismaticConstraint`, `CylindricalConstraint`,
	// `RopeConstraint` and `SpringConstraint` - all derive from it and differ only
	// in the prototype row `Instance.new` copies.
	//
	// @return The class id.
	ecs::ClassId ConstraintClass();
}
