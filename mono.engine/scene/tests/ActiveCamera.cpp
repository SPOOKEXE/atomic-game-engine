#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/vec4.hpp>

TEST_SUITE_ID("engine.scene.activecamera")

using Catch::Approx;
using engine::core::CFrame;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::scene::ActiveCamera;
using engine::scene::Camera;
using engine::scene::CameraMatrices;
using engine::scene::RegisterSceneComponents;
using engine::scene::ResolveActiveCamera;
using engine::scene::ResolveCamera;
using engine::scene::Transform;

namespace activecamera_test {
	// Every case that touches a store calls this first. `Store::SetResource`
	// resolves a type to a component id, and a type first seen through the
	// automatic path takes the compiler's spelling of its name - after which
	// the explicit registration aborts rather than leaving two names for one
	// thing. Registering is idempotent, so calling it per case costs a hash
	// lookup and removes the ordering question entirely.
	void Ready() {
		RegisterSceneComponents();
	}
}

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

TEST_CASE("a zero aspect ratio yields identity rather than infinities", "[scene][activecamera]") {
	// A minimised window reports zero height every frame it is minimised. A
	// projection built from it is full of infinities, and those spread into
	// every culled bound before anybody notices where they came from.
	const CameraMatrices matrices = ResolveCamera(CFrame(), Camera{}, 0.0f);

	CHECK(matrices.Projection == glm::mat4(1.0f));
	CHECK(matrices.View == glm::mat4(1.0f));
	CHECK(matrices.ViewProjection == glm::mat4(1.0f));
}

TEST_CASE("resolving fills the resource from the row it names", "[scene][activecamera]") {
	activecamera_test::Ready();
	Store store("activecamera_test.resolve");

	const Entity eye = store.Create();
	store.Set(eye, Transform{CFrame(Vector3(0.0f, 2.0f, 8.0f))});
	store.Set(eye, Camera{});

	ActiveCamera active;
	active.Entity = eye;
	active.AspectRatio = 16.0f / 9.0f;
	store.SetResource(active);

	ResolveActiveCamera(store);

	const ActiveCamera *resolved = store.Resource<ActiveCamera>();
	REQUIRE(resolved != nullptr);

	const glm::vec4 origin = resolved->Matrices.View * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
	CHECK(origin.y == Approx(-2.0f));
	CHECK(origin.z == Approx(-8.0f));
}

TEST_CASE("a camera row that moves is picked up on the next resolve", "[scene][activecamera]") {
	// The resource holds a handle rather than a copy of the values precisely so
	// that there is one place a camera is written. If it cached the transform,
	// this case would keep the old position.
	activecamera_test::Ready();
	Store store("activecamera_test.moving");

	const Entity eye = store.Create();
	store.Set(eye, Transform{CFrame(Vector3(0.0f, 0.0f, 0.0f))});
	store.Set(eye, Camera{});

	ActiveCamera active;
	active.Entity = eye;
	active.AspectRatio = 1.0f;
	store.SetResource(active);
	ResolveActiveCamera(store);

	store.Set(eye, Transform{CFrame(Vector3(5.0f, 0.0f, 0.0f))});
	ResolveActiveCamera(store);

	const glm::vec4 origin =
		store.Resource<ActiveCamera>()->Matrices.View * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
	CHECK(origin.x == Approx(-5.0f));
}

TEST_CASE("a camera that goes away leaves the last matrices alone", "[scene][activecamera]") {
	// Stale rather than identity, deliberately. A stale view draws the frame
	// from where the camera was; identity draws it from inside the origin with
	// nothing in it, which looks like a renderer fault and sends the search to
	// the wrong module.
	activecamera_test::Ready();
	Store store("activecamera_test.missing");

	const Entity eye = store.Create();
	store.Set(eye, Transform{CFrame(Vector3(0.0f, 0.0f, 7.0f))});
	store.Set(eye, Camera{});

	ActiveCamera active;
	active.Entity = eye;
	active.AspectRatio = 1.0f;
	store.SetResource(active);
	ResolveActiveCamera(store);

	const glm::mat4 before = store.Resource<ActiveCamera>()->Matrices.View;
	REQUIRE(before != glm::mat4(1.0f));

	store.Destroy(eye);
	ResolveActiveCamera(store);

	CHECK(store.Resource<ActiveCamera>()->Matrices.View == before);
}

TEST_CASE("resolving a world with no active camera does nothing", "[scene][activecamera]") {
	// A headless world, or one being built. Not an error and not a reason to
	// abort a tick.
	activecamera_test::Ready();
	Store store("activecamera_test.none");
	ResolveActiveCamera(store);
	CHECK_FALSE(store.HasResource<ActiveCamera>());
}
