#include <engine/core/types/CFrame.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

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

	REQUIRE(identity.PointToWorldSpace({ 1.0f, 2.0f, 3.0f }) == Vector3 { 1.0f, 2.0f, 3.0f });
	REQUIRE(identity.QuaternionW == Approx(1.0f));
}

TEST_CASE("composed with its inverse it is the identity", "[cframe]") {
	const CFrame frame = CFrame(Vector3 { 4.0f, -2.0f, 7.0f })
		* CFrame::Angles(0.4f, 1.2f, -0.3f);

	const CFrame identity = frame * frame.Inverse();
	const Vector3 point { 3.0f, 1.0f, -5.0f };
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
	const CFrame moved { Vector3 { 100.0f, 100.0f, 100.0f } };

	// PointToWorldSpace applies the translation; VectorToWorldSpace does not.
	// A direction that picks up a position is a bug that only shows once the
	// object moves away from the origin.
	REQUIRE(moved.PointToWorldSpace(Vector3::XAxis) == Vector3 { 101.0f, 100.0f, 100.0f });
	REQUIRE(moved.VectorToWorldSpace(Vector3::XAxis) == Vector3::XAxis);
}

TEST_CASE("LookAt faces the target along LookVector", "[cframe]") {
	const CFrame camera = CFrame::LookAt(Vector3 { 0.0f, 0.0f, 10.0f }, Vector3::Zero);
	const Vector3 look = camera.LookVector();

	// -Z, matching Roblox and matching what a camera faces.
	REQUIRE(look.X == Approx(0.0f).margin(1e-5));
	REQUIRE(look.Y == Approx(0.0f).margin(1e-5));
	REQUIRE(look.Z == Approx(-1.0f).margin(1e-5));
}

TEST_CASE("LookAt on a zero-length direction keeps the identity", "[cframe]") {
	const Vector3 point { 1.0f, 2.0f, 3.0f };
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
	const Vector3 world = (parent * CFrame { Vector3::ZAxis }).Position;

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
		middle.QuaternionX * middle.QuaternionX + middle.QuaternionY * middle.QuaternionY
		+ middle.QuaternionZ * middle.QuaternionZ + middle.QuaternionW * middle.QuaternionW);
	REQUIRE(length == Approx(1.0f));

	REQUIRE(from.Lerp(to, 0.0f).QuaternionW == Approx(from.QuaternionW));
	REQUIRE(from.Lerp(to, 1.0f).QuaternionY == Approx(to.QuaternionY));
}

TEST_CASE("ToMatrix puts the position in the fourth column", "[cframe]") {
	const glm::mat4 matrix = CFrame { Vector3 { 1.0f, 2.0f, 3.0f } }.ToMatrix();

	// Column-major, ready for a uniform buffer. Transposed here would put the
	// translation in the bottom row and every object at the origin.
	REQUIRE(matrix[3][0] == Approx(1.0f));
	REQUIRE(matrix[3][1] == Approx(2.0f));
	REQUIRE(matrix[3][2] == Approx(3.0f));
	REQUIRE(matrix[3][3] == Approx(1.0f));
}
