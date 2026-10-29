#include <engine/core/Bytes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/ViewEye.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.scene.vieweye")

TEST_CASE("a view eye leaves the camera transform intact", "[scene][vieweye]") {
	using namespace engine;
	scene::RegisterSceneComponents();
	ecs::Store store("view eye");
	const auto camera = store.Create();
	const auto other = store.Create();
	store.Set(camera, scene::Transform{core::CFrame(core::Vector3{1, 2, 3})});
	store.Set(other, scene::Transform{core::CFrame(core::Vector3{4, 5, 6})});
	store.SetResource(scene::ViewEye{camera, core::CFrame(core::Vector3{7, 8, 9})});

	core::CFrame frame;
	REQUIRE(scene::ResolveViewEye(store, camera, frame));
	CHECK(frame.Position == core::Vector3{7, 8, 9});
	CHECK(store.Get<scene::Transform>(camera)->Frame.Position == core::Vector3{1, 2, 3});
	core::ByteWriter snapshot;
	REQUIRE(store.Save(snapshot));
	ecs::Store restored("restored view");
	core::ByteReader reader(snapshot.Bytes());
	REQUIRE(restored.Load(reader));
	REQUIRE(restored.Resource<scene::ViewEye>() != nullptr);
	CHECK(restored.Resource<scene::ViewEye>()->Camera == ecs::NULL_ENTITY);
	REQUIRE(scene::ResolveViewEye(store, other, frame));
	CHECK(frame.Position == core::Vector3{4, 5, 6});
	store.ResourceMutable<scene::ViewEye>()->Camera = ecs::NULL_ENTITY;
	REQUIRE(scene::ResolveViewEye(store, camera, frame));
	CHECK(frame.Position == core::Vector3{1, 2, 3});
	store.Destroy(camera);
	CHECK_FALSE(scene::ResolveViewEye(store, camera, frame));
}
