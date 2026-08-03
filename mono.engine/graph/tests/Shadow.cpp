#include <engine/graph/Shadow.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

TEST_SUITE_ID("engine.graph.shadow")

using Catch::Approx;
using engine::core::AABB;
using engine::core::CFrame;
using engine::core::Vector3;
using engine::graph::BoundsOfAll;
using engine::graph::FitDirectionalLight;
using engine::scene::DrawInstance;

namespace {
	// Where a world point lands in the light's clip space.
	glm::vec3 Project(const glm::mat4 &lightViewProjection, const Vector3 &point) {
		const glm::vec4 clip = lightViewProjection * glm::vec4{point.X, point.Y, point.Z, 1.0f};
		return glm::vec3{clip} / clip.w;
	}

	DrawInstance At(const Vector3 &position, float halfExtent = 0.5f) {
		DrawInstance instance;
		instance.Frame = CFrame{position};
		instance.HalfExtent = Vector3{halfExtent, halfExtent, halfExtent};
		return instance;
	}
}

TEST_CASE("everything in the bounds lands inside the light's clip volume", "[graph][shadow]") {
	// **The property the whole fit exists for.** A caster outside the map is a
	// caster that does not shadow, and the symptom is an object floating with
	// nothing under it.
	const AABB bounds = AABB::FromCentre(Vector3{0.0f, 0.0f, 0.0f}, Vector3{20.0f, 5.0f, 20.0f});
	const glm::mat4 light = FitDirectionalLight(bounds, Vector3{-0.45f, -0.8f, -0.4f});

	// Every corner of the box, which is the worst case for a fit.
	for (int corner = 0; corner < 8; corner++) {
		const Vector3 point{
			(corner & 1) != 0 ? bounds.Maximum.X : bounds.Minimum.X,
			(corner & 2) != 0 ? bounds.Maximum.Y : bounds.Minimum.Y,
			(corner & 4) != 0 ? bounds.Maximum.Z : bounds.Minimum.Z,
		};

		const glm::vec3 clip = Project(light, point);
		INFO("corner " << corner << " at " << clip.x << ", " << clip.y << ", " << clip.z);

		CHECK(clip.x >= -1.0f);
		CHECK(clip.x <= 1.0f);
		CHECK(clip.y >= -1.0f);
		CHECK(clip.y <= 1.0f);

		// **Vulkan's depth range**, zero to one. An OpenGL-convention
		// projection would put the near half at negative depth, and every
		// comparison against the map would be half a unit out.
		CHECK(clip.z >= 0.0f);
		CHECK(clip.z <= 1.0f);
	}
}

TEST_CASE("the fit does not change size as the light turns", "[graph][shadow]") {
	// **Rotation invariance is why the fit is to a sphere and not to the box
	// axis by axis.** A map that resizes as the sun moves makes its own shadow
	// edges crawl, and that reads as a filtering bug rather than as a fit one.
	const AABB bounds = AABB::FromCentre(Vector3::Zero, Vector3{10.0f, 3.0f, 10.0f});

	// **The matrix's own scale, not the separation of two world points.** Two
	// points along world X foreshorten as the light turns away from them, so
	// their clip-space distance moves even when the map does not — the first
	// version of this test measured that and failed for the right reason.
	//
	// What is actually invariant is the extent the projection maps to the clip
	// boundary, and for `Projection * View` with an orthonormal view that is the
	// length of the first row's `xyz`: one over the fitted radius.
	const auto scale = [&bounds](const Vector3 &direction) {
		const glm::mat4 light = FitDirectionalLight(bounds, direction);
		return glm::length(glm::vec3{light[0][0], light[1][0], light[2][0]});
	};

	const float straightDown = scale(Vector3{0.0f, -1.0f, 0.0f});

	for (const Vector3 &direction :
		 {Vector3{-0.5f, -0.7f, -0.5f},
		  Vector3{1.0f, -0.2f, 0.0f},
		  Vector3{0.0f, -0.3f, 1.0f},
		  Vector3{-1.0f, -1.0f, -1.0f}}) {
		INFO("direction " << direction.X << ", " << direction.Y << ", " << direction.Z);
		CHECK(scale(direction) == Approx(straightDown).margin(1e-5));
	}
}

TEST_CASE("a light straight down does not produce NaN", "[graph][shadow]") {
	// The ordinary case for a sun, and the one where a naive `lookAt` has its
	// up vector parallel to its forward — which produces a matrix full of NaN
	// rather than an error, and a scene that renders black with nothing in the
	// log.
	const glm::mat4 light =
		FitDirectionalLight(AABB::FromCentre(Vector3::Zero, Vector3::One * 5.0f), Vector3{0.0f, -1.0f, 0.0f});

	const glm::vec3 clip = Project(light, Vector3::Zero);
	CHECK(std::isfinite(clip.x));
	CHECK(std::isfinite(clip.y));
	CHECK(std::isfinite(clip.z));
}

TEST_CASE("a light with no direction shadows nothing", "[graph][shadow]") {
	// The conservative answer. A caller with no direction has a bug, and a
	// scene rendered entirely in shadow would hide it behind a symptom that
	// looks like a renderer fault.
	const glm::mat4 light = FitDirectionalLight(AABB::FromCentre(Vector3::Zero, Vector3::One), Vector3::Zero);

	CHECK(light == glm::mat4{1.0f});
}

TEST_CASE("something nearer the light is nearer in the map", "[graph][shadow]") {
	// The comparison the fragment shader makes. If depth ran the other way,
	// every surface would shadow itself and nothing else.
	const AABB bounds = AABB::FromCentre(Vector3::Zero, Vector3{5.0f, 10.0f, 5.0f});
	const glm::mat4 light = FitDirectionalLight(bounds, Vector3{0.0f, -1.0f, 0.0f});

	const glm::vec3 high = Project(light, Vector3{0.0f, 8.0f, 0.0f});
	const glm::vec3 low = Project(light, Vector3{0.0f, -8.0f, 0.0f});

	CHECK(high.z < low.z);
}

TEST_CASE("the bounds of a draw list cover every instance", "[graph][shadow]") {
	const std::vector<DrawInstance> instances{
		At(Vector3{-10.0f, 0.0f, 0.0f}),
		At(Vector3{10.0f, 0.0f, 0.0f}),
		At(Vector3{0.0f, 5.0f, 0.0f}, 2.0f),
	};

	const AABB bounds = BoundsOfAll(instances);

	CHECK(bounds.Minimum.X == Approx(-10.5f));
	CHECK(bounds.Maximum.X == Approx(10.5f));
	CHECK(bounds.Maximum.Y == Approx(7.0f));
}

TEST_CASE("an empty draw list gives a unit box rather than an inverted one", "[graph][shadow]") {
	// Nothing here accumulates from an empty sentinel — `AABB.hpp` says so and
	// says why — and a light fitted to an inverted box frames nothing.
	const AABB bounds = BoundsOfAll({});

	CHECK(bounds.Minimum.X <= bounds.Maximum.X);
	CHECK(bounds.Size().X == Approx(2.0f));
}
