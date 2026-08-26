#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/vec4.hpp>

#include <cmath>

TEST_SUITE_ID("engine.scene.activecamera")

using Catch::Approx;
using engine::core::CFrame;
using engine::core::Vector3;
using engine::scene::Camera;
using engine::scene::CameraMatrices;
using engine::scene::ResolveCamera;

TEST_CASE("a camera at the origin looks down negative Z", "[scene][activecamera]") {
	// The engine is right-handed, Y-up, -Z forward. A view matrix that had the
	// sign backwards would put everything behind the camera, which reads as a
	// culling bug.
	const CameraMatrices matrices = ResolveCamera(CFrame(), Camera{}, 16.0f / 9.0f);

	const glm::vec4 ahead = matrices.View * glm::vec4(0.0f, 0.0f, -5.0f, 1.0f);
	CHECK(ahead.z == Approx(-5.0f));
}

TEST_CASE("the view is the inverse of the camera's frame", "[scene][activecamera]") {
	// A camera ten metres up sees the origin ten metres below it, which is the
	// inverse and not the frame. Getting this backwards moves the world with
	// the camera instead of past it.
	const CFrame frame(Vector3(0.0f, 10.0f, 0.0f));
	const CameraMatrices matrices = ResolveCamera(frame, Camera{}, 1.0f);

	const glm::vec4 origin = matrices.View * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
	CHECK(origin.y == Approx(-10.0f));
	CHECK(origin.x == Approx(0.0f));
}

TEST_CASE("the product is projection times view, in that order", "[scene][activecamera]") {
	// Cached so that three passes cannot disagree about it. Cached wrongly, it
	// is a transform nobody can reproduce from the two matrices beside it.
	const CFrame frame(Vector3(3.0f, 1.0f, -4.0f));
	const CameraMatrices matrices = ResolveCamera(frame, Camera{}, 4.0f / 3.0f);

	const glm::mat4 expected = matrices.Projection * matrices.View;
	for (int column = 0; column < 4; column++) {
		for (int row = 0; row < 4; row++) {
			CHECK(matrices.ViewProjection[column][row] == Approx(expected[column][row]));
		}
	}
}

TEST_CASE("widening the window widens the field and stretches nothing", "[scene][activecamera]") {
	// **The invariant behind "the viewport image is stretched", pinned so that
	// the next report of it has something to fail.** Both of the v0.19 reports
	// - the studio's viewport and the `MipProbe` mesh while the camera flies -
	// reduce to one question: does a wider window change how tall a thing is?
	// It was answered in pixels twice, from captures at two window shapes and
	// from a moving camera against a stopped one, and both times the answer was
	// no. A measurement taken once is a story; this is the arithmetic those
	// pixels came out of, checked every run.
	//
	// `glm::perspective` keeps the *vertical* field and widens horizontally,
	// which is the convention every engine with a `FieldOfView` property uses,
	// and each case below is one consequence of that sentence.
	const Camera lens;
	const float half = lens.FieldOfViewRadians * 0.5f;

	// Five, spanning square to twice as wide as a cinema screen, because a
	// stretch that only shows at one shape is exactly what a single ratio would
	// miss.
	const float ratios[] = {1.0f, 4.0f / 3.0f, 16.0f / 9.0f, 21.0f / 9.0f, 4.0f};

	for (const float aspect : ratios) {
		INFO("aspect " << aspect);
		const CameraMatrices matrices = ResolveCamera(CFrame(), lens, aspect);

		// A point on the axis is the centre of the picture whatever the shape.
		const glm::vec4 ahead = matrices.ViewProjection * glm::vec4(0.0f, 0.0f, -10.0f, 1.0f);
		CHECK((ahead.x / ahead.w) == Approx(0.0f).margin(1e-5));
		CHECK((ahead.y / ahead.w) == Approx(0.0f).margin(1e-5));

		// **The vertical field does not move.** A point at the top edge of it,
		// ten metres out, lands on the top edge of the picture - at every
		// aspect. This is the case that fails if a projection ever divides by
		// the aspect on the wrong axis, and it is the one a person reads as
		// "the picture is squashed".
		const float top = std::tan(half) * 10.0f;
		const glm::vec4 high = matrices.ViewProjection * glm::vec4(0.0f, top, -10.0f, 1.0f);
		CHECK((high.y / high.w) == Approx(1.0f).epsilon(1e-4));

		// **And the horizontal field widens by exactly the aspect.** A point at
		// `tan(fovY/2) * aspect * distance` is on the side edge, which is what
		// "wider window, more world" means arithmetically.
		const float side = std::tan(half) * aspect * 10.0f;
		const glm::vec4 wide = matrices.ViewProjection * glm::vec4(side, 0.0f, -10.0f, 1.0f);
		CHECK((wide.x / wide.w) == Approx(1.0f).epsilon(1e-4));

		// A square in the world stays square on screen: the same offset up and
		// across projects to NDC coordinates whose ratio is the aspect, which is
		// the pixel-space statement that nothing is stretched. In pixels the two
		// are equal, because the NDC axes are divided by different pixel counts
		// in exactly that ratio.
		const glm::vec4 corner = matrices.ViewProjection * glm::vec4(1.0f, 1.0f, -10.0f, 1.0f);
		const float acrossNdc = corner.x / corner.w;
		const float upNdc = corner.y / corner.w;
		CHECK((upNdc / acrossNdc) == Approx(aspect).epsilon(1e-4));
	}
}

TEST_CASE("the picture is a function of the pose and the shape and nothing else", "[scene][activecamera]") {
	// **The other half of the same report: "when you fly the camera around".**
	// A projection is a function of where the camera is and what shape the
	// window is, so a camera that arrived by moving must produce the same
	// matrices as one that was placed there - there is no third input for a
	// history to hide in. Measured once, from a scene that swept a full turn
	// over eight ticks against one placed at the final yaw, which came out
	// byte-identical; this is that with the renderer taken out of it.
	const Camera lens;
	const CFrame pose(Vector3(3.0f, 8.0f, -12.0f));

	const CameraMatrices placed = ResolveCamera(pose, lens, 16.0f / 9.0f);

	// The same pose reached after resolving somewhere else first. If anything
	// were cached across calls - a frustum, a last aspect, a previous frame -
	// this is where it would show.
	(void)ResolveCamera(CFrame(Vector3(-40.0f, 2.0f, 60.0f)), lens, 1.0f);
	(void)ResolveCamera(CFrame(Vector3(0.0f, 0.0f, 0.0f)), lens, 4.0f);
	const CameraMatrices arrived = ResolveCamera(pose, lens, 16.0f / 9.0f);

	for (int column = 0; column < 4; column++) {
		for (int row = 0; row < 4; row++) {
			CHECK(arrived.ViewProjection[column][row] == Approx(placed.ViewProjection[column][row]));
		}
	}
}

TEST_CASE("a zero aspect ratio yields identity rather than infinities", "[scene][activecamera]") {
	// A minimised window reports zero height every frame it is minimised. A
	// projection built from it is full of infinities, and those spread into
	// every culled bound before anybody notices where they came from.
	const CameraMatrices matrices = ResolveCamera(CFrame(), Camera{}, 0.0f);

	CHECK(matrices.Projection == glm::mat4(1.0f));
	CHECK(matrices.View == glm::mat4(1.0f));
	CHECK(matrices.ViewProjection == glm::mat4(1.0f));
}
