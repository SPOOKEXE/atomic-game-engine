#include <engine/graph/Cull.hpp>
#include <engine/graph/Frustum.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

TEST_SUITE_ID("engine.graph.frustum")

using Catch::Approx;
using engine::core::AABB;
using engine::core::CFrame;
using engine::core::Vector3;
using engine::graph::Cull;
using engine::graph::Frustum;
using engine::scene::Camera;
using engine::scene::DrawInstance;

namespace {
	// A camera at the origin looking down -Z, which is this engine's forward.
	//
	// Built through `scene::ResolveCamera` rather than by composing a
	// projection here — that function is the one place the engine decides what
	// a camera's matrices are, and a frustum derived from a second answer would
	// be testing agreement with something nothing else uses.
	Frustum Looking(const Vector3 &from = Vector3::Zero, float aspect = 16.0f / 9.0f) {
		Camera camera;
		camera.NearPlane = 0.1f;
		camera.FarPlane = 100.0f;

		return Frustum::FromViewProjection(
			engine::scene::ResolveCamera(CFrame{from}, camera, aspect).ViewProjection
		);
	}

	DrawInstance At(const Vector3 &position, float halfExtent = 0.5f) {
		DrawInstance instance;
		instance.Frame = CFrame{position};
		instance.HalfExtent = Vector3{halfExtent, halfExtent, halfExtent};
		return instance;
	}
}

TEST_CASE("a point in front is inside and one behind is not", "[graph][frustum]") {
	// The near plane is the one a wrong clip convention breaks: Vulkan's volume
	// is `0 <= z <= w` and OpenGL's is `-w <= z <= w`, and using the second here
	// would put the near plane behind the camera and pass everything.
	const Frustum frustum = Looking();

	CHECK(frustum.Contains(Vector3{0.0f, 0.0f, -10.0f}));
	CHECK_FALSE(frustum.Contains(Vector3{0.0f, 0.0f, 10.0f}));
}

TEST_CASE("the near and far planes bound what is visible", "[graph][frustum]") {
	const Frustum frustum = Looking();

	CHECK_FALSE(frustum.Contains(Vector3{0.0f, 0.0f, -0.05f}));
	CHECK(frustum.Contains(Vector3{0.0f, 0.0f, -0.2f}));
	CHECK(frustum.Contains(Vector3{0.0f, 0.0f, -99.0f}));
	CHECK_FALSE(frustum.Contains(Vector3{0.0f, 0.0f, -101.0f}));
}

TEST_CASE("the side planes widen with distance", "[graph][frustum]") {
	// The property that makes it a frustum rather than a box: a point far off
	// the axis is inside when it is far away and outside when it is close.
	const Frustum frustum = Looking();

	CHECK(frustum.Contains(Vector3{10.0f, 0.0f, -50.0f}));
	CHECK_FALSE(frustum.Contains(Vector3{10.0f, 0.0f, -1.0f}));
}

TEST_CASE("every plane's normal points inward", "[graph][frustum]") {
	// The sign convention the whole file rests on. A plane extracted with its
	// normal outward would reject everything it should accept, and the symptom
	// is a black screen rather than an obviously wrong test.
	const Frustum frustum = Looking();
	const Vector3 inside{0.0f, 0.0f, -10.0f};

	for (const auto &plane : frustum.Planes) {
		INFO("plane normal " << plane.Normal.X << ", " << plane.Normal.Y << ", " << plane.Normal.Z);
		CHECK(plane.SignedDistance(inside) >= 0.0f);
	}
}

TEST_CASE("plane normals are unit length, so distances are metres", "[graph][frustum]") {
	// A sign test would not need this. `SignedDistance` reports a real
	// distance, and the sphere test compares it against a radius — so an
	// unnormalised plane would cull spheres at an arbitrary scale.
	const Frustum frustum = Looking();

	for (const auto &plane : frustum.Planes) {
		CHECK(plane.Normal.Magnitude() == Approx(1.0f).margin(1e-4));
	}
}

TEST_CASE("a box straddling a plane is visible", "[graph][frustum]") {
	// The direction the error has to go: a `true` costs a draw call, and a
	// `false` is a hole in the world.
	const Frustum frustum = Looking();

	// Centred behind the near plane, but big enough to reach past it.
	CHECK(frustum.Intersects(AABB::FromCentre(Vector3{0.0f, 0.0f, 1.0f}, Vector3{0.0f, 0.0f, 5.0f})));

	// And one wholly behind is not.
	CHECK_FALSE(frustum.Intersects(AABB::FromCentre(Vector3{0.0f, 0.0f, 20.0f}, Vector3{1.0f, 1.0f, 1.0f})));
}

TEST_CASE("a sphere is culled by its radius", "[graph][frustum]") {
	const Frustum frustum = Looking();

	CHECK(frustum.Intersects(Vector3{0.0f, 0.0f, 5.0f}, 10.0f));
	CHECK_FALSE(frustum.Intersects(Vector3{0.0f, 0.0f, 5.0f}, 1.0f));
}

TEST_CASE("a degenerate matrix accepts everything rather than nothing", "[graph][frustum]") {
	// The conservative direction. A projection with a zero field of view is a
	// caller's bug, and culling the world away would hide it behind a symptom
	// that looks like a renderer fault.
	const Frustum frustum = Frustum::FromViewProjection(glm::mat4{0.0f});

	CHECK(frustum.Contains(Vector3{0.0f, 0.0f, -10.0f}));
	CHECK(frustum.Contains(Vector3{1000.0f, 1000.0f, 1000.0f}));
}

TEST_CASE("a rotated cube is bounded by what it actually reaches", "[graph][frustum]") {
	// `FromOrientedBox`, not the centre and the half-extent. A unit cube turned
	// forty-five degrees reaches root two, and a bound smaller than the shape
	// is a part that vanishes as it turns near the screen edge.
	DrawInstance turned;
	turned.Frame = CFrame::Angles(0.0f, 0.7853982f, 0.0f);
	turned.HalfExtent = Vector3{0.5f, 0.5f, 0.5f};

	const AABB bound = engine::graph::BoundsOf(turned);
	CHECK(bound.Size().X == Approx(1.41421356f).margin(1e-3));
	CHECK(bound.Size().Y == Approx(1.0f).margin(1e-3));
}

TEST_CASE("culling keeps what is in front and drops what is behind", "[graph][frustum]") {
	const Frustum frustum = Looking();

	const std::vector<DrawInstance> instances{
		At(Vector3{0.0f, 0.0f, -10.0f}),
		At(Vector3{0.0f, 0.0f, 10.0f}),
		At(Vector3{0.0f, 0.0f, -20.0f}),
		At(Vector3{500.0f, 0.0f, -10.0f}),
	};

	std::vector<uint32_t> visible;
	CHECK(Cull(instances, frustum, visible) == 2);
	CHECK(visible == std::vector<uint32_t>{0, 2});
}

TEST_CASE("culling preserves the order it was given", "[graph][frustum]") {
	// Ordering is `scene::OrderForDrawing`'s job and this must not disturb it:
	// the two compose, cull first and order second over the survivors.
	const Frustum frustum = Looking();

	std::vector<DrawInstance> instances;
	for (int index = 0; index < 32; index++) {
		instances.push_back(At(Vector3{0.0f, 0.0f, -1.0f - static_cast<float>(index)}));
	}

	std::vector<uint32_t> visible;
	CHECK(Cull(instances, frustum, visible) == instances.size());

	for (uint32_t index = 0; index < visible.size(); index++) {
		CHECK(visible[index] == index);
	}
}

TEST_CASE("culling an empty list is empty", "[graph][frustum]") {
	std::vector<uint32_t> visible;
	CHECK(Cull({}, Looking(), visible) == 0);
	CHECK(visible.empty());
}
