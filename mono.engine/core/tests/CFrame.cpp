#include <engine/core/types/CFrame.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <numbers>

TEST_SUITE_ID("engine.core.types.cframe")
// CFrame is a Vector3 plus a quaternion, so a change to Vector3 has to re-run
// this too.
TEST_DEPENDS("engine.core.types.vector3")
// `OrientedBoxBounds` is declared in this header and returns an AABB, so the
// oriented-bound cases at the bottom are here rather than beside the box.
TEST_DEPENDS("engine.core.types.aabb")

using Catch::Approx;
using engine::core::AABB;
using engine::core::CFrame;
using engine::core::OrientedBoxBounds;
using engine::core::Vector3;

TEST_CASE("an identity CFrame moves nothing", "[cframe]") {
	const CFrame identity;

	REQUIRE(identity.PointToWorldSpace({1.0f, 2.0f, 3.0f}) == Vector3{1.0f, 2.0f, 3.0f});
	REQUIRE(identity.QuaternionW == Approx(1.0f));
}

TEST_CASE("composed with its inverse it is the identity", "[cframe]") {
	const CFrame frame = CFrame(Vector3{4.0f, -2.0f, 7.0f}) * CFrame::Angles(0.4f, 1.2f, -0.3f);

	const CFrame identity = frame * frame.Inverse();
	const Vector3 point{3.0f, 1.0f, -5.0f};
	const Vector3 moved = identity.PointToWorldSpace(point);

	REQUIRE(moved.X == Approx(point.X).margin(1e-4));
	REQUIRE(moved.Y == Approx(point.Y).margin(1e-4));
	REQUIRE(moved.Z == Approx(point.Z).margin(1e-4));
}

TEST_CASE("a quarter turn about Y sends +Z to +X", "[cframe]") {
	constexpr float quarter = std::numbers::pi_v<float> / 2.0f;
	const Vector3 moved = CFrame::Angles(0.0f, quarter, 0.0f).VectorToWorldSpace(Vector3::ZAxis);

	REQUIRE(moved.X == Approx(1.0f).margin(1e-5));
	REQUIRE(moved.Y == Approx(0.0f).margin(1e-5));
	REQUIRE(moved.Z == Approx(0.0f).margin(1e-5));
}

TEST_CASE("a vector is rotated but not translated", "[cframe]") {
	const CFrame moved{Vector3{100.0f, 100.0f, 100.0f}};

	// PointToWorldSpace applies the translation; VectorToWorldSpace does not.
	// A direction that picks up a position is a bug that only shows once the
	// object moves away from the origin.
	REQUIRE(moved.PointToWorldSpace(Vector3::XAxis) == Vector3{101.0f, 100.0f, 100.0f});
	REQUIRE(moved.VectorToWorldSpace(Vector3::XAxis) == Vector3::XAxis);
}

TEST_CASE("LookAt faces the target along LookVector", "[cframe]") {
	const CFrame camera = CFrame::LookAt(Vector3{0.0f, 0.0f, 10.0f}, Vector3::Zero);
	const Vector3 look = camera.LookVector();

	// -Z, matching Roblox and matching what a camera faces.
	REQUIRE(look.X == Approx(0.0f).margin(1e-5));
	REQUIRE(look.Y == Approx(0.0f).margin(1e-5));
	REQUIRE(look.Z == Approx(-1.0f).margin(1e-5));
}

TEST_CASE("LookAt on a zero-length direction keeps the identity", "[cframe]") {
	const Vector3 point{1.0f, 2.0f, 3.0f};
	const CFrame degenerate = CFrame::LookAt(point, point);

	// There is no direction to face. An identity beats a NaN quaternion, which
	// would poison every transform composed with it.
	REQUIRE(degenerate.Position == point);
	REQUIRE(degenerate.QuaternionW == Approx(1.0f));
}

TEST_CASE("the basis vectors are orthogonal and unit", "[cframe]") {
	const CFrame frame = CFrame::Angles(0.3f, -1.1f, 0.7f);

	REQUIRE(frame.RightVector().Magnitude() == Approx(1.0f));
	REQUIRE(frame.UpVector().Magnitude() == Approx(1.0f));
	REQUIRE(frame.LookVector().Magnitude() == Approx(1.0f));
	REQUIRE(frame.RightVector().Dot(frame.UpVector()) == Approx(0.0f).margin(1e-5));
	REQUIRE(frame.UpVector().Dot(frame.LookVector()) == Approx(0.0f).margin(1e-5));
}

TEST_CASE("composition applies the parent rotation to the child offset", "[cframe]") {
	constexpr float quarter = std::numbers::pi_v<float> / 2.0f;

	const CFrame parent = CFrame::Angles(0.0f, quarter, 0.0f);
	const Vector3 world = (parent * CFrame{Vector3::ZAxis}).Position;

	REQUIRE(world.X == Approx(1.0f).margin(1e-5));
	REQUIRE(world.Z == Approx(0.0f).margin(1e-5));
}

TEST_CASE("lerp slerps the rotation rather than the components", "[cframe]") {
	constexpr float half = std::numbers::pi_v<float>;
	const CFrame from;
	const CFrame to = CFrame::Angles(0.0f, half * 0.5f, 0.0f);

	const CFrame middle = from.Lerp(to, 0.5f);

	// Lerping the four components and normalising would land here too for a
	// small arc, but the rotation would not be constant-speed through it. The
	// check that catches a component lerp is that the result stays unit.
	const float length = std::sqrt(
		middle.QuaternionX * middle.QuaternionX + middle.QuaternionY * middle.QuaternionY +
		middle.QuaternionZ * middle.QuaternionZ + middle.QuaternionW * middle.QuaternionW
	);
	REQUIRE(length == Approx(1.0f));

	REQUIRE(from.Lerp(to, 0.0f).QuaternionW == Approx(from.QuaternionW));
	REQUIRE(from.Lerp(to, 1.0f).QuaternionY == Approx(to.QuaternionY));
}

TEST_CASE("NLerp selects the endpoints exactly", "[cframe]") {
	const CFrame from{Vector3{1.0f, 0.0f, 0.0f}};
	const CFrame to = CFrame::Angles(0.0f, 1.0f, 0.0f);

	REQUIRE(from.NLerp(to, 0.0f).QuaternionW == Approx(from.QuaternionW));
	REQUIRE(from.NLerp(to, 1.0f).QuaternionY == Approx(to.QuaternionY));
	REQUIRE(from.NLerp(to, 0.0f).Position.X == Approx(from.Position.X));
}

TEST_CASE("NLerp stays on the unit sphere", "[cframe]") {
	const CFrame from;
	const CFrame to = CFrame::Angles(0.4f, 0.9f, -0.3f);

	// A component lerp without the renormalise leaves the unit sphere, and a
	// non-unit quaternion is a rotation with a scale baked into it - every cube
	// drawn through one comes out the wrong size.
	for (float alpha = 0.0f; alpha <= 1.0f; alpha += 0.125f) {
		const CFrame middle = from.NLerp(to, alpha);
		const float length = std::sqrt(
			middle.QuaternionX * middle.QuaternionX + middle.QuaternionY * middle.QuaternionY +
			middle.QuaternionZ * middle.QuaternionZ + middle.QuaternionW * middle.QuaternionW
		);
		REQUIRE(length == Approx(1.0f));
	}
}

TEST_CASE("NLerp agrees with Lerp over a tick-sized arc", "[cframe]") {
	const CFrame from;
	// Six degrees: a full turn every second at a 60 Hz tick, which is faster
	// than anything in the demo scene actually spins.
	const CFrame to = CFrame::Angles(0.0f, 0.105f, 0.0f);

	// The claim the substitution rests on. Over an arc this short the two
	// agree far inside what a pixel can show, so the cheap one is not an
	// approximation anybody can see - it is the same picture.
	for (float alpha = 0.0f; alpha <= 1.0f; alpha += 0.125f) {
		const CFrame fast = from.NLerp(to, alpha);
		const CFrame exact = from.Lerp(to, alpha);

		REQUIRE(fast.QuaternionW == Approx(exact.QuaternionW).margin(0.001f));
		REQUIRE(fast.QuaternionX == Approx(exact.QuaternionX).margin(0.001f));
		REQUIRE(fast.QuaternionY == Approx(exact.QuaternionY).margin(0.001f));
		REQUIRE(fast.QuaternionZ == Approx(exact.QuaternionZ).margin(0.001f));
	}
}

TEST_CASE("NLerp takes the shortest arc", "[cframe]") {
	const CFrame from;

	// A quaternion and its negation are the same orientation. Interpolating
	// toward the negated one without the sign flip goes the long way round -
	// which on a spinning object is a visible backwards snap once a revolution,
	// and the reason Lerp's behaviour cannot simply be assumed here.
	CFrame negated;
	negated.QuaternionW = -from.QuaternionW;
	negated.QuaternionX = -from.QuaternionX;
	negated.QuaternionY = -from.QuaternionY;
	negated.QuaternionZ = -from.QuaternionZ;

	// Same orientation at both ends, so every point on the shortest path
	// between them is that orientation too.
	const CFrame middle = from.NLerp(negated, 0.5f);
	REQUIRE(std::abs(middle.QuaternionW) == Approx(1.0f));
	REQUIRE(middle.QuaternionX == Approx(0.0f).margin(1e-5f));
	REQUIRE(middle.QuaternionY == Approx(0.0f).margin(1e-5f));
	REQUIRE(middle.QuaternionZ == Approx(0.0f).margin(1e-5f));
}

TEST_CASE("NLerp moves the position linearly", "[cframe]") {
	const CFrame from{Vector3{0.0f, 0.0f, 0.0f}};
	const CFrame to{Vector3{10.0f, -4.0f, 2.0f}};

	// Only the rotation is approximated. Position is the same straight line
	// Lerp draws, and a renderer interpolating one but not the other would
	// separate an object from its own orientation.
	const CFrame middle = from.NLerp(to, 0.25f);
	REQUIRE(middle.Position.X == Approx(2.5f));
	REQUIRE(middle.Position.Y == Approx(-1.0f));
	REQUIRE(middle.Position.Z == Approx(0.5f));
}

TEST_CASE("ToMatrix puts the position in the fourth column", "[cframe]") {
	const glm::mat4 matrix = CFrame{Vector3{1.0f, 2.0f, 3.0f}}.ToMatrix();

	// Column-major, ready for a uniform buffer. Transposed here would put the
	// translation in the bottom row and every object at the origin.
	REQUIRE(matrix[3][0] == Approx(1.0f));
	REQUIRE(matrix[3][1] == Approx(2.0f));
	REQUIRE(matrix[3][2] == Approx(3.0f));
	REQUIRE(matrix[3][3] == Approx(1.0f));
}

// --- ToAngles ---------------------------------------------------------------
//
// The property surface exposes an `Orientation` a script can read, modify and
// assign back, so the pair has to be an exact inverse rather than merely a
// plausible Euler extraction. `part.Orientation = part.Orientation` drifting a
// little on every assignment is a bug that only appears in a loop.

namespace {
	// Compares rotations rather than angle triples. Two different triples can
	// name one rotation - that is what gimbal lock *is* - so asserting on the
	// angles would fail for a correct answer.
	void RequireSameRotation(const CFrame &produced, const CFrame &expected, float margin = 1.0e-4f) {
		for (const Vector3 &probe :
			 {Vector3::XAxis, Vector3::YAxis, Vector3::ZAxis, Vector3{0.37f, -0.62f, 0.19f}}) {
			const Vector3 first = produced.VectorToWorldSpace(probe);
			const Vector3 second = expected.VectorToWorldSpace(probe);
			REQUIRE(first.X == Approx(second.X).margin(margin));
			REQUIRE(first.Y == Approx(second.Y).margin(margin));
			REQUIRE(first.Z == Approx(second.Z).margin(margin));
		}
	}
}

TEST_CASE("ToAngles inverts Angles across all three axes", "[cframe]") {
	constexpr float DEGREE = std::numbers::pi_v<float> / 180.0f;

	// A spread rather than one triple, and deliberately not axis-aligned: a
	// wrong Euler *order* reproduces any single-axis rotation correctly and
	// fails the moment two turns compose.
	const float samples[] = {-170.0f, -95.0f, -44.0f, -1.0f, 0.0f, 17.0f, 89.0f, 133.0f};

	for (const float pitch : samples) {
		for (const float yaw : samples) {
			for (const float roll : samples) {
				const CFrame original = CFrame::Angles(pitch * DEGREE, yaw * DEGREE, roll * DEGREE);
				const Vector3 recovered = original.ToAngles();
				RequireSameRotation(CFrame::Angles(recovered.X, recovered.Y, recovered.Z), original);
			}
		}
	}
}

TEST_CASE("ToAngles round-trips at the gimbal poles", "[cframe]") {
	constexpr float HALF_PI = std::numbers::pi_v<float> / 2.0f;

	// Pitch at ±90° is where cos(pitch) reaches zero and the yaw and roll axes
	// coincide. The triple that comes back is not the one that went in - it
	// cannot be, the information is genuinely gone - but the rotation it
	// rebuilds has to be identical.
	for (const float pitch : {HALF_PI, -HALF_PI}) {
		for (const float yaw : {0.0f, 0.9f, -2.1f}) {
			for (const float roll : {0.0f, 0.6f, -1.4f}) {
				const CFrame original = CFrame::Angles(pitch, yaw, roll);
				const Vector3 recovered = original.ToAngles();

				REQUIRE(std::isfinite(recovered.X));
				REQUIRE(std::isfinite(recovered.Y));
				REQUIRE(std::isfinite(recovered.Z));

				// A looser margin than the sweep above, and it is float
				// precision rather than slack. At the pole `sin(pitch)` comes
				// back as 0.99999994, so the axis the extraction leans on has
				// about 3.5e-4 of magnitude left to carry a ratio - that error
				// is the arithmetic's floor here, not the algorithm's.
				RequireSameRotation(CFrame::Angles(recovered.X, recovered.Y, recovered.Z), original, 1.0e-3f);
			}
		}
	}
}

// --- Roblox parity ----------------------------------------------------------
//
// v0.18 brought the script-facing CFrame up to Roblox's surface, and these hold
// the parts of it that are easy to get plausibly wrong. `docs/RELEASING.md` has
// nothing to say about it; the thing that matters is that a script pasted from
// Roblox behaves the same here, and every case below is one a wrong answer would
// still look reasonable in.

TEST_CASE("the two Euler orders are genuinely different", "[cframe]") {
	// **The bug this pair exists to stop.** `CFrame::Angles` composes Y-X-Z and
	// the comment above it claimed that was Roblox's `CFrame.Angles` order. It
	// is not: Roblox's `Angles` is an alias for `fromEulerAnglesXYZ` and
	// composes X-Y-Z. `Angles` here is Roblox's `fromOrientation`.
	//
	// A single-axis turn is identical under either order, which is exactly why
	// nothing caught it for eighteen versions - so this asserts on a triple with
	// two non-zero turns, where the two orders must disagree.
	const CFrame yxz = CFrame::Angles(0.5f, 0.9f, 0.0f);
	const CFrame xyz = CFrame::FromEulerAnglesXYZ(0.5f, 0.9f, 0.0f);

	CHECK(yxz.AngleBetween(xyz) > 0.1f);

	// And one axis at a time agrees, which is the other half of the claim: a
	// port that only ever turned about Y was never affected.
	for (int axis = 0; axis < 3; axis++) {
		const float turn = 0.7f;
		const CFrame first =
			CFrame::Angles(axis == 0 ? turn : 0.0f, axis == 1 ? turn : 0.0f, axis == 2 ? turn : 0.0f);
		const CFrame second = CFrame::FromEulerAnglesXYZ(
			axis == 0 ? turn : 0.0f, axis == 1 ? turn : 0.0f, axis == 2 ? turn : 0.0f
		);
		CHECK(first.AngleBetween(second) == Approx(0.0f).margin(1.0e-5f));
	}
}

TEST_CASE("ToEulerAnglesXYZ inverts FromEulerAnglesXYZ", "[cframe]") {
	constexpr float DEGREE = std::numbers::pi_v<float> / 180.0f;

	// The same spread `ToAngles` is held to, and for the same reason: a wrong
	// order reproduces any single-axis rotation and fails when two compose.
	const float samples[] = {-170.0f, -95.0f, -44.0f, -1.0f, 0.0f, 17.0f, 89.0f, 133.0f};

	for (const float rx : samples) {
		for (const float ry : samples) {
			for (const float rz : samples) {
				const CFrame original = CFrame::FromEulerAnglesXYZ(rx * DEGREE, ry * DEGREE, rz * DEGREE);
				const Vector3 recovered = original.ToEulerAnglesXYZ();
				RequireSameRotation(
					CFrame::FromEulerAnglesXYZ(recovered.X, recovered.Y, recovered.Z), original
				);
			}
		}
	}
}

TEST_CASE("ToEulerAnglesXYZ round-trips at the gimbal poles", "[cframe]") {
	constexpr float HALF_PI = std::numbers::pi_v<float> / 2.0f;

	// ry at ±90° is where this order locks, where `ToAngles` locks on pitch. The
	// triple that comes back is not the one that went in - the information is
	// genuinely gone - but the rotation has to be identical.
	for (const float ry : {HALF_PI, -HALF_PI}) {
		for (const float rx : {0.0f, 0.9f, -2.1f}) {
			for (const float rz : {0.0f, 0.6f, -1.4f}) {
				const CFrame original = CFrame::FromEulerAnglesXYZ(rx, ry, rz);
				const Vector3 recovered = original.ToEulerAnglesXYZ();
				RequireSameRotation(
					CFrame::FromEulerAnglesXYZ(recovered.X, recovered.Y, recovered.Z), original
				);
			}
		}
	}
}

TEST_CASE("axis and angle survive a round trip", "[cframe]") {
	const Vector3 axis = Vector3{1.0f, 2.0f, -0.5f}.Unit();
	const CFrame frame = CFrame::FromAxisAngle(axis, 1.1f);

	Vector3 recoveredAxis;
	float recoveredAngle = 0.0f;
	frame.ToAxisAngle(recoveredAxis, recoveredAngle);

	CHECK(recoveredAngle == Approx(1.1f).margin(1.0e-4f));
	RequireSameRotation(CFrame::FromAxisAngle(recoveredAxis, recoveredAngle), frame);

	// **The identity has no axis, and reports one anyway.** A zero vector would
	// be an axis nothing can normalise, so a caller feeding the pair straight
	// back would get a NaN out of a perfectly ordinary frame.
	Vector3 stillAxis;
	float noAngle = -1.0f;
	CFrame().ToAxisAngle(stillAxis, noAngle);
	CHECK(noAngle == Approx(0.0f));
	CHECK(stillAxis.Magnitude() == Approx(1.0f));

	// A degenerate axis is the identity rather than a NaN quaternion.
	CHECK(CFrame::FromAxisAngle(Vector3::Zero, 2.0f).QuaternionW == Approx(1.0f));
}

TEST_CASE("FromMatrix orthonormalises a basis it is given", "[cframe]") {
	// Deliberately skewed and unnormalised, which is what a script that computed
	// two directions actually hands over. A quaternion built from this without
	// correction is a rotation that also scales and shears.
	const CFrame frame =
		CFrame::FromMatrix(Vector3{1.0f, 2.0f, 3.0f}, Vector3{2.0f, 0.0f, 0.0f}, Vector3{0.3f, 4.0f, 0.0f});

	CHECK(frame.Position == Vector3{1.0f, 2.0f, 3.0f});

	// Every column unit length and mutually perpendicular, or it is not a
	// rotation at all.
	const Vector3 x = frame.RightVector();
	const Vector3 y = frame.UpVector();
	const Vector3 z = frame.ZVector();

	CHECK(x.Magnitude() == Approx(1.0f).margin(1.0e-5f));
	CHECK(y.Magnitude() == Approx(1.0f).margin(1.0e-5f));
	CHECK(z.Magnitude() == Approx(1.0f).margin(1.0e-5f));
	CHECK(x.Dot(y) == Approx(0.0f).margin(1.0e-5f));
	CHECK(x.Dot(z) == Approx(0.0f).margin(1.0e-5f));
	CHECK(y.Dot(z) == Approx(0.0f).margin(1.0e-5f));

	// Parallel inputs name a plane rather than a basis, and are the identity
	// rather than a NaN.
	const CFrame degenerate = CFrame::FromMatrix(Vector3::Zero, Vector3::XAxis, Vector3{2.0f, 0.0f, 0.0f});
	CHECK(degenerate.QuaternionW == Approx(1.0f));
}

TEST_CASE("a rotation between two directions carries one onto the other", "[cframe]") {
	const Vector3 from = Vector3{1.0f, 1.0f, 0.0f}.Unit();
	const Vector3 to = Vector3{0.0f, 1.0f, 1.0f}.Unit();

	const Vector3 carried = CFrame::FromRotationBetweenVectors(from, to).VectorToWorldSpace(from);
	CHECK(carried.X == Approx(to.X).margin(1.0e-5f));
	CHECK(carried.Y == Approx(to.Y).margin(1.0e-5f));
	CHECK(carried.Z == Approx(to.Z).margin(1.0e-5f));

	// **Antiparallel is the case with no shortest answer**, and the one where a
	// naive cross product is near zero and its direction is noise. Any half-turn
	// about any perpendicular is correct; what is not acceptable is a NaN or a
	// rotation that fails to arrive.
	const Vector3 reversed = CFrame::FromRotationBetweenVectors(Vector3::XAxis, -Vector3::XAxis)
								 .VectorToWorldSpace(Vector3::XAxis);
	CHECK(reversed.X == Approx(-1.0f).margin(1.0e-4f));
	CHECK(reversed.Y == Approx(0.0f).margin(1.0e-4f));
	CHECK(reversed.Z == Approx(0.0f).margin(1.0e-4f));
}

TEST_CASE("object space inverts world space", "[cframe]") {
	const CFrame frame = CFrame(Vector3{3.0f, -1.0f, 8.0f}) * CFrame::Angles(0.3f, -0.8f, 1.2f);
	const Vector3 point{2.0f, 5.0f, -4.0f};

	const Vector3 there = frame.PointToObjectSpace(frame.PointToWorldSpace(point));
	CHECK(there.X == Approx(point.X).margin(1.0e-4f));
	CHECK(there.Y == Approx(point.Y).margin(1.0e-4f));
	CHECK(there.Z == Approx(point.Z).margin(1.0e-4f));

	// A direction ignores the translation, which is the whole difference from
	// the point form and the thing a caller reaches for the other name for.
	const Vector3 direction = frame.VectorToObjectSpace(frame.VectorToWorldSpace(point));
	CHECK(direction.X == Approx(point.X).margin(1.0e-4f));

	// The frame forms compose the same way.
	const CFrame other = CFrame(Vector3{-2.0f, 0.5f, 1.0f}) * CFrame::Angles(1.0f, 0.2f, -0.4f);
	RequireSameRotation(frame.ToObjectSpace(frame.ToWorldSpace(other)), other);
}

TEST_CASE("GetComponents reports Roblox's twelve in Roblox's order", "[cframe]") {
	const CFrame frame = CFrame(Vector3{7.0f, -3.0f, 2.0f}) * CFrame::Angles(0.0f, 0.0f, 0.0f);
	const std::array<float, 12> parts = frame.GetComponents();

	CHECK(parts[0] == Approx(7.0f));
	CHECK(parts[1] == Approx(-3.0f));
	CHECK(parts[2] == Approx(2.0f));

	// An unrotated frame's rotation is the identity matrix, so the diagonal is
	// ones and everything else is zero - which also pins that the nine are laid
	// out three to a row rather than three to a column.
	CHECK(parts[3] == Approx(1.0f));
	CHECK(parts[7] == Approx(1.0f));
	CHECK(parts[11] == Approx(1.0f));

	// **Row-major, and this is the case that tells the two apart.** A quarter
	// turn about Y puts -1 at R02 and +1 at R20; transposed it would be the
	// other way round, and every 12-argument round trip would mirror.
	constexpr float QUARTER = std::numbers::pi_v<float> / 2.0f;
	const std::array<float, 12> turned = CFrame::Angles(0.0f, QUARTER, 0.0f).GetComponents();
	CHECK(turned[5] == Approx(1.0f).margin(1.0e-5f));
	CHECK(turned[9] == Approx(-1.0f).margin(1.0e-5f));
}

TEST_CASE("FuzzyEq and AngleBetween see past a negated quaternion", "[cframe]") {
	// **A quaternion and its negation are the same rotation.** A component-wise
	// comparison calls them a mismatch of 2.0, which is the largest wrong answer
	// available - so both of these compare the angle instead.
	CFrame frame = CFrame::Angles(0.4f, 1.1f, -0.2f);
	CFrame negated = frame;
	negated.QuaternionX = -frame.QuaternionX;
	negated.QuaternionY = -frame.QuaternionY;
	negated.QuaternionZ = -frame.QuaternionZ;
	negated.QuaternionW = -frame.QuaternionW;

	CHECK(frame.AngleBetween(negated) == Approx(0.0f).margin(1.0e-4f));
	CHECK(frame.FuzzyEq(negated, 1.0e-3f));

	// And a real difference is still a difference, in both position and rotation.
	CHECK_FALSE(frame.FuzzyEq(CFrame::Angles(0.4f, 1.6f, -0.2f), 1.0e-3f));
	CHECK_FALSE(CFrame(Vector3::Zero).FuzzyEq(CFrame(Vector3{1.0f, 0.0f, 0.0f}), 1.0e-3f));
}

TEST_CASE("Orthonormalize restores a drifted quaternion", "[cframe]") {
	// This type stores a quaternion, so the only drift it can accumulate is
	// length - it cannot shear the way a stored 3x3 matrix can, which is what
	// Roblox's `Orthonormalize` is really for.
	CFrame drifted = CFrame::Angles(0.3f, 0.7f, 0.1f);
	drifted.QuaternionX *= 1.5f;
	drifted.QuaternionY *= 1.5f;
	drifted.QuaternionZ *= 1.5f;
	drifted.QuaternionW *= 1.5f;

	const CFrame fixed = drifted.Orthonormalize();
	const float length = std::sqrt(
		fixed.QuaternionX * fixed.QuaternionX + fixed.QuaternionY * fixed.QuaternionY +
		fixed.QuaternionZ * fixed.QuaternionZ + fixed.QuaternionW * fixed.QuaternionW
	);
	CHECK(length == Approx(1.0f).margin(1.0e-5f));

	// A zero quaternion has no direction to keep, and normalising one is a
	// division by zero rather than an error.
	CFrame empty;
	empty.QuaternionW = 0.0f;
	CHECK(empty.Orthonormalize().QuaternionW == Approx(1.0f));
}

TEST_CASE("a box turned 45 degrees about Y grows by root two", "[cframe]") {
	// The case that separates a real oriented bound from one that rotates only
	// the centre. A centre-only version passes identity and quarter turns -
	// both leave the extent alone - and fails only here, where the diagonal
	// becomes the width.
	constexpr float eighth = std::numbers::pi_v<float> / 4.0f;
	const AABB bound = OrientedBoxBounds(CFrame::Angles(0.0f, eighth, 0.0f), Vector3{0.5f, 0.5f, 0.5f});

	const float root2 = std::numbers::sqrt2_v<float>;
	REQUIRE(bound.Size().X == Approx(root2).margin(1e-5));
	REQUIRE(bound.Size().Z == Approx(root2).margin(1e-5));

	// Y is the axis turned about, so it does not grow at all.
	REQUIRE(bound.Size().Y == Approx(1.0f).margin(1e-5));
	REQUIRE(bound.Centre().X == Approx(0.0f).margin(1e-5));
}

TEST_CASE("an unrotated oriented box is the box itself", "[cframe]") {
	const AABB bound = OrientedBoxBounds(CFrame{Vector3{3.0f, -2.0f, 1.0f}}, Vector3{1.0f, 2.0f, 3.0f});

	REQUIRE(bound.Minimum == Vector3{2.0f, -4.0f, -2.0f});
	REQUIRE(bound.Maximum == Vector3{4.0f, 0.0f, 4.0f});
}

TEST_CASE("a quarter turn about Y swaps the X and Z extents", "[cframe]") {
	constexpr float quarter = std::numbers::pi_v<float> / 2.0f;
	const AABB bound = OrientedBoxBounds(CFrame::Angles(0.0f, quarter, 0.0f), Vector3{1.0f, 2.0f, 3.0f});

	REQUIRE(bound.Size().X == Approx(6.0f).margin(1e-5));
	REQUIRE(bound.Size().Y == Approx(4.0f).margin(1e-5));
	REQUIRE(bound.Size().Z == Approx(2.0f).margin(1e-5));
}

TEST_CASE("an oriented bound is never smaller than the shape inside it", "[cframe]") {
	// The property the whole function exists for, stated directly: every corner
	// of the rotated box has to land inside the bound. A bound that is too
	// small drops contacts and reports nothing.
	const CFrame frame = CFrame(Vector3{2.0f, -1.0f, 4.0f}) * CFrame::Angles(0.4f, 1.2f, -0.3f);
	const Vector3 halfExtent{0.5f, 1.5f, 0.25f};
	const AABB bound = OrientedBoxBounds(frame, halfExtent);

	for (int corner = 0; corner < 8; corner++) {
		const Vector3 local{
			(corner & 1) ? halfExtent.X : -halfExtent.X,
			(corner & 2) ? halfExtent.Y : -halfExtent.Y,
			(corner & 4) ? halfExtent.Z : -halfExtent.Z,
		};
		// A hair of slack, because the corner and the bound are two different
		// float expressions for the same quantity.
		const Vector3 world = frame.PointToWorldSpace(local);
		REQUIRE(
			AABB::FromCentre(bound.Centre(), bound.Size() * 0.5f + Vector3{1e-4f, 1e-4f, 1e-4f})
				.Contains(world)
		);
	}
}
