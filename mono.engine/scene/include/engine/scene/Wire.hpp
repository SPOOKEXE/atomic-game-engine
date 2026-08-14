#pragma once

// What a replicated component looks like on a datagram, and what that costs.
//
// **A `Transform` is a `core::CFrame` and a `core::CFrame` is twenty-eight
// bytes**, which is most of every delta a moving world produces. Twelve of them
// are a position nobody can see to the last micrometre and sixteen are a unit
// quaternion, which carries one redundant number: its own length. So a position
// crosses as three fixed-point values on a stated grid and a rotation crosses as
// *smallest-three* - the largest component dropped, the other three sent, the
// dropped one recovered from unit length - and twenty-eight bytes become ten.
// `Motion` is two `Vector3`s and becomes twelve.
//
// **This is what goes on the wire and never what goes in the store.** The
// server is authoritative and a quantised value must not feed back into its own
// simulation, or `just determinism` and `just replay-check` stop being
// byte-identical. That is why these live in `ecs::WireFormat` rather than over
// `TypeDescriptor::Write`: a codec fitted over the file serialisation would make
// every recording lossy, and the replay check would go on passing because it
// would be comparing one lossy file against another. `ecs/TypeDescriptor.hpp`
// carries the rest of that argument.
//
// **The grid is a world-size decision, not a constant.** A step of two
// millimetres over a 128-metre world is a different number over a four-kilometre
// one, so the grid below is stated as *the world's extent divided into steps*
// and the error it introduces is stated in metres rather than hoped for.
// `WorldBounds::HalfExtent` is the world's half of that pair and defaults to the
// same 64 m; `WireCoversWorld` is how a world says it fits.
//
// **Outside the stated extent, a coordinate is clamped and never wrapped.** A
// clamped entity is visibly stuck against the boundary of the world it was
// declared to be inside, which is a wrong answer somebody can see and locate. A
// wrapped one is at the far side of the world, indistinguishable from a
// teleport the server meant - the silent aliasing this file refuses. Every
// decode clamps as well as every encode, because the sixteen bits arriving from
// a peer are not the sixteen bits an encoder wrote.
//
// @tier L7 · shared

#include <engine/ecs/TypeDescriptor.hpp>

#include <cstdint>

namespace engine::scene {

	// Steps from the origin to the far edge, on each axis, for every
	// sixteen-bit quantity here.
	//
	// **32767 of the 32768 a signed sixteen-bit code offers, and the one that
	// is dropped buys symmetry.** With all 32768 the negative edge would reach
	// one step further than the positive one, so `+HalfExtent` would not be
	// representable, an entity pinned against the far wall - which is exactly
	// where `Bounce` puts one - would decode a whole step short, and the error
	// bound below would need two halves. One code is a cheaper price than a
	// bound with an exception in it.
	inline constexpr int32_t WIRE_STEPS = 32767;

	// How far the position grid reaches from the origin, in metres.
	//
	// The same 64 m `WorldBounds::HalfExtent` defaults to, and that is not a
	// coincidence: this is the world's extent expressed on the wire. A world
	// authored larger does not silently lose entities - it has them clamped to
	// this, which `WireCoversWorld` is for saying at the place the size is
	// chosen rather than discovering per entity.
	inline constexpr float WIRE_POSITION_HALF_EXTENT_METRES = 64.0f;

	// Metres between two adjacent position codes: 1.953 mm at 64 m.
	inline constexpr float WIRE_POSITION_STEP_METRES =
		WIRE_POSITION_HALF_EXTENT_METRES / static_cast<float>(WIRE_STEPS);

	// The most a decoded coordinate differs from the one encoded, in metres.
	//
	// **Half a step, everywhere in the world including both edges** - 0.977 mm
	// at the extent above. Per axis, so the worst case on a 3D distance is
	// sqrt(3) of it, 1.69 mm. Stated rather than hoped for, and
	// `engine.scene.wire` measures it across the whole extent rather than
	// trusting this line.
	inline constexpr float WIRE_POSITION_ERROR_METRES = WIRE_POSITION_STEP_METRES * 0.5f;

	// The largest a quaternion's second-largest component can be.
	//
	// One over root two. Whichever component of a unit quaternion is largest is
	// at least a half, and the other three are therefore each no larger than
	// this - which is the whole reason smallest-three costs less than sending
	// four components of the same precision.
	inline constexpr float WIRE_ROTATION_LIMIT = 0.70710678118654752440f;

	// Steps from zero to `WIRE_ROTATION_LIMIT`, for each of the three
	// components that are sent.
	//
	// 511 of the 512 a ten-bit signed field offers, dropped for the symmetry
	// `WIRE_STEPS` explains. Ten bits each plus a two-bit index for the
	// component that was left out is exactly thirty-two, so a rotation is one
	// four-byte word rather than four floats.
	inline constexpr int32_t WIRE_ROTATION_STEPS = 511;

	// The most a decoded rotation differs from the one encoded, in radians.
	//
	// **0.24 degrees, and it is derived rather than measured.** Each sent
	// component lands within half a step, so the three of them are within
	// sqrt(3)/2 of a step - 0.0012. The dropped component is recovered as
	// sqrt(1 - s), whose error is the error in `s` over twice the component
	// itself; that component is at least a half, so the division at most
	// doubles nothing and the term is at most 0.0017. Together the quaternion
	// moves by at most 0.0021, and an angle is twice the quaternion distance.
	//
	// A sweep over four hundred thousand random orientations plus every
	// equal-component case reaches 0.00408, so this is a bound with three per
	// cent of room in it rather than a comfortable one. `engine.scene.wire`
	// asserts both halves: that nothing exceeds it, and that something comes
	// close enough for it to mean anything.
	inline constexpr float WIRE_ROTATION_ERROR_RADIANS = 0.0042f;

	// How fast the linear velocity grid reaches, in metres per second.
	//
	// Faster than anything this engine simulates, because the cost of reaching
	// too far is precision nobody notices and the cost of reaching too short is
	// a projectile that arrives slower than it left.
	inline constexpr float WIRE_LINEAR_HALF_EXTENT_METRES_PER_SECOND = 256.0f;

	// The most a decoded linear velocity differs from the one encoded.
	//
	// **3.9 mm/s, and the number to compare it against is the position grid
	// rather than zero.** Over one tick of a 60 Hz world that is 65 micrometres
	// of travel - fifteen times below `WIRE_POSITION_ERROR_METRES` - so a
	// velocity quantised this coarsely cannot be seen in any position derived
	// from it within a tick. That is the whole justification for velocity
	// having a grid of its own rather than the position one.
	inline constexpr float WIRE_LINEAR_ERROR_METRES_PER_SECOND =
		WIRE_LINEAR_HALF_EXTENT_METRES_PER_SECOND / static_cast<float>(WIRE_STEPS) * 0.5f;

	// How fast the angular velocity grid reaches, in radians per second.
	// Ten turns a second, which is past what anything readable spins at.
	inline constexpr float WIRE_ANGULAR_HALF_EXTENT_RADIANS_PER_SECOND = 64.0f;

	// The most a decoded angular velocity differs from the one encoded, in
	// radians per second. Just under a milliradian.
	inline constexpr float WIRE_ANGULAR_ERROR_RADIANS_PER_SECOND =
		WIRE_ANGULAR_HALF_EXTENT_RADIANS_PER_SECOND / static_cast<float>(WIRE_STEPS) * 0.5f;

	// How long a position integrated from a decoded velocity stays better than
	// the decoded position it started from, in seconds.
	//
	// **A quarter of a second, and it is the ratio of the two extents above
	// rather than a number somebody picked.** Interpolating between two decoded
	// poses keeps the error inside `WIRE_POSITION_ERROR_METRES` whatever the
	// elapsed time. *Integrating* does not: the position error is the one it
	// started with plus `WIRE_LINEAR_ERROR_METRES_PER_SECOND` times the seconds
	// since, so it grows linearly and the bound is a function of time rather
	// than of the grid. The two are equal when
	//
	//     t = WIRE_POSITION_ERROR_METRES / WIRE_LINEAR_ERROR_METRES_PER_SECOND
	//
	// and both errors are half a step of their own grid, so the step counts
	// cancel and what is left is 64 m over 256 m/s. Per axis and on a 3D
	// distance alike, for the same reason.
	//
	// Past it the guess is worse-conditioned than the last thing the authority
	// actually said, which is where `replication::InterpolationSettings::
	// ExtrapolateSeconds` stops guessing and lets the world hold.
	// `engine.scene.wire` measures the growth rather than trusting this
	// paragraph, and `client.replicated` pins the two constants against each
	// other because `replication` may not see this header.
	inline constexpr float WIRE_DEAD_RECKON_SECONDS =
		WIRE_POSITION_HALF_EXTENT_METRES / WIRE_LINEAR_HALF_EXTENT_METRES_PER_SECOND;

	// Bytes one `Transform` occupies on the wire, against twenty-eight in the
	// store: three sixteen-bit axes and one packed rotation.
	inline constexpr uint32_t WIRE_TRANSFORM_BYTES = 10;

	// Bytes one `Motion` occupies on the wire, against twenty-four in the
	// store: six sixteen-bit axes.
	inline constexpr uint32_t WIRE_MOTION_BYTES = 12;

	// Whether the position grid covers a world of this size.
	//
	// **The check belongs where a world's size is authored**, because that is
	// the one place that knows the number and the only place that can do
	// anything about it. A world larger than this still replicates; its
	// entities are clamped to `WIRE_POSITION_HALF_EXTENT_METRES` and pile up
	// against a wall that is not the world's.
	//
	// @param halfExtent The world's `WorldBounds::HalfExtent`, in metres.
	// @return `true` when every position in that world is representable.
	constexpr bool WireCoversWorld(float halfExtent) {
		return halfExtent <= WIRE_POSITION_HALF_EXTENT_METRES;
	}

	// The compact form `Transform` crosses a replication wire in.
	//
	// @return Ten bytes: three quantised axes and one packed rotation.
	ecs::WireFormat TransformWire();

	// The compact form `Motion` crosses a replication wire in.
	//
	// @return Twelve bytes: three quantised linear axes and three angular ones.
	ecs::WireFormat MotionWire();
}
