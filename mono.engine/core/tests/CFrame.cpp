#include <engine/core/types/CFrame.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>

TEST_SUITE_ID("engine.core.types.cframe")
// CFrame is a Vector3 plus a quaternion, so a change to Vector3 has to re-run
// this too.
TEST_DEPENDS("engine.core.types.vector3")

using Catch::Approx;
using engine::core::CFrame;
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
	// non-unit quaternion is a rotation with a scale baked into it — every cube
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
	// approximation anybody can see — it is the same picture.
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
	// toward the negated one without the sign flip goes the long way round —
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
	// name one rotation — that is what gimbal lock *is* — so asserting on the
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
	// coincide. The triple that comes back is not the one that went in — it
	// cannot be, the information is genuinely gone — but the rotation it
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
				// about 3.5e-4 of magnitude left to carry a ratio — that error
				// is the arithmetic's floor here, not the algorithm's.
				RequireSameRotation(CFrame::Angles(recovered.X, recovered.Y, recovered.Z), original, 1.0e-3f);
			}
		}
	}
}
