#pragma once

// The named kinds a body and a collider come in.
//
// Two closed sets, and closed is the point. A shape is not an open extension
// point: every one of them costs an exact narrow-phase pair against every other
// shape, so adding a case here is a decision about how much collision code
// exists rather than a value somebody drops in.
//
// **Names are the format, numbers are not.** These sit in components that a
// snapshot and a replication delta both carry, and both write the component
// through the writer its registration declares — so the underlying numbers stay
// free to move and must never be written anywhere by hand.
//
// @tier L7 · shared

#include <cstdint>

namespace engine::scene {

	// How the solver is allowed to move a body.
	//
	// Separate from whether the entity has a `RigidBody` at all. A part with no
	// `RigidBody` is not a static body — it is not a body, and no query the
	// physics pipeline runs will ever visit it. This says what to do with the
	// ones it does visit.
	//
	// @since v0.4
	enum class BodyKind : uint8_t {
		// Never moved by the solver and never integrated. Its transform is
		// whatever put it there.
		Static,

		// Moved by whoever owns it — a platform, an animation, a script — and
		// never by a contact. Pushes dynamic bodies and is pushed by nothing.
		Kinematic,

		// Moved by forces and contacts. The ordinary case.
		Dynamic,
	};

	// What shape a collider actually is.
	//
	// Three, because the exact narrow phase needs a pair function per unordered
	// pair and three shapes is already six of them. A fourth is ten.
	//
	// @since v0.4
	enum class ShapeKind : uint8_t {
		// A box, half-extents on each local axis.
		Box,

		// A sphere, radius in X.
		Sphere,

		// A cylinder about the local Y axis, radius in X and half-height in Y.
		Cylinder,
	};

	// Returns a stable, human-readable name for a body kind.
	//
	// For logs and diagnostics. Not a format: nothing parses these back.
	//
	// @param kind The kind to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(BodyKind kind);

	// Returns a stable, human-readable name for a shape kind.
	//
	// @param kind The kind to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(ShapeKind kind);
}
