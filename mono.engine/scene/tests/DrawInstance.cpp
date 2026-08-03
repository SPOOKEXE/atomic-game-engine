#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <type_traits>

TEST_SUITE_ID("engine.scene.drawinstance")

using engine::core::CFrame;
using engine::core::Name;
using engine::core::Vector3;
using engine::scene::Bounds;
using engine::scene::DrawInstance;
using engine::scene::Transform;
using engine::scene::Visual;

TEST_CASE("a draw instance survives being copied as bytes", "[scene][drawinstance]") {
	// The property the type exists for: the day a world is a process, this
	// crosses as a block of memory. A field that owned an allocation would
	// compile, pass every test that stayed in one process, and become a
	// use-after-free the day the world moved.
	REQUIRE(std::is_trivially_copyable_v<DrawInstance>);

	DrawInstance instance;
	instance.Frame = CFrame(Vector3(1.0f, 2.0f, 3.0f));
	instance.HalfExtent = Vector3(2.0f, 0.5f, 2.0f);
	instance.Mesh = Name("drawinstance_test.Mesh");

	DrawInstance copy;
	std::memcpy(&copy, &instance, sizeof(DrawInstance));

	CHECK(copy.Frame.Position == instance.Frame.Position);
	CHECK(copy.HalfExtent == instance.HalfExtent);
	CHECK(copy.Mesh == instance.Mesh);
}

TEST_CASE("a default draw instance is a white unit cube at the origin", "[scene][drawinstance]") {
	const DrawInstance instance;
	CHECK(instance.Frame.Position == Vector3::Zero);
	CHECK(instance.HalfExtent.X == 0.5f);
	CHECK(instance.Tint.R == 1.0f);

	// Names rather than handles, and invalid means "the consumer's default".
	// A handle would be a number that only means something inside one process,
	// which is the rule this whole payload is shaped by.
	CHECK_FALSE(instance.Mesh.IsValid());
	CHECK_FALSE(instance.Material.IsValid());
}

TEST_CASE("a draw instance is built from scene components without conversion", "[scene][drawinstance]") {
	// The producer copies rather than converts, which is the reason
	// `HalfExtent` is a half-extent and `Tint` is a `Color3`: a producer that
	// had to convert would be a producer with a device's conventions in it.
	Transform transform;
	transform.Frame = CFrame(Vector3(4.0f, 0.0f, -2.0f));

	Bounds bounds;
	bounds.HalfExtent = Vector3(1.0f, 3.0f, 1.0f);

	Visual visual;
	visual.Mesh = Name("drawinstance_test.Wedge");
	visual.Material = Name("drawinstance_test.Oak");
	visual.Tint = engine::core::Color3(0.25f, 0.5f, 0.75f);

	const DrawInstance instance{
		transform.Frame, bounds.HalfExtent, visual.Tint, visual.Mesh, visual.Material
	};

	CHECK(instance.Frame.Position == transform.Frame.Position);
	CHECK(instance.HalfExtent == bounds.HalfExtent);
	CHECK(instance.Tint.G == 0.5f);
	CHECK(instance.Mesh == visual.Mesh);
	CHECK(instance.Material == visual.Material);
}
