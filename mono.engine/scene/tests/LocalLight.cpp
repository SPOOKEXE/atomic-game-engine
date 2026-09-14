#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/LocalLight.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <numbers>

using engine::core::CFrame;
using engine::core::Color3;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::scene::Attachment;
using engine::scene::Light;
using engine::scene::LightKind;
using engine::scene::LocalLightRejection;
using engine::scene::ResolvedLocalLight;
using engine::scene::ResolveLocalLight;
using engine::scene::Transform;

TEST_SUITE_ID("engine.scene.locallight")

TEST_CASE("local lights resolve transform and attachment parents with renderer RGB", "[scene]") {
	Store store("local_lights");
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();
	const Entity part = store.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Part");
	store.Set(part, Transform{CFrame(Vector3{2.0f, 3.0f, 4.0f})});
	const Entity point =
		store.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("PointLight")), "Point");
	REQUIRE(store.SetParent(point, part));
	Light authored;
	authored.Colour = Color3{0.5f, 0.25f, 0.125f};
	authored.Brightness = 4.0f;
	authored.Range = 12.0f;
	store.Set(point, authored);

	ResolvedLocalLight resolved;
	REQUIRE(ResolveLocalLight(store, point, *store.Get<Light>(point), resolved) == LocalLightRejection::None);
	CHECK(resolved.Position == Vector3{2.0f, 3.0f, 4.0f});
	CHECK(resolved.Colour.R == Catch::Approx(2.0f));
	CHECK(resolved.Colour.G == Catch::Approx(1.0f));
	CHECK(resolved.ConeCosine == -1.0f);

	const Entity attachment =
		store.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Attachment")), "Attachment");
	store.Set(attachment, Attachment{CFrame{}, CFrame(Vector3{6.0f, 7.0f, 8.0f})});
	const Entity spot =
		store.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("SpotLight")), "Spot");
	REQUIRE(store.SetParent(spot, attachment));
	authored.Kind = LightKind::Spot;
	authored.Angle = 360.0f;
	store.Set(spot, authored);
	REQUIRE(ResolveLocalLight(store, spot, *store.Get<Light>(spot), resolved) == LocalLightRejection::None);
	CHECK(resolved.Position == Vector3{6.0f, 7.0f, 8.0f});
	CHECK(resolved.ConeCosine == Catch::Approx(0.0f).margin(0.0001f));
	authored.Face = engine::scene::NormalId::Top;
	store.GetMutable<Attachment>(attachment)->WorldFrame =
		CFrame(Vector3{6.0f, 7.0f, 8.0f}) * CFrame::Angles(0.0f, 0.0f, std::numbers::pi_v<float> * 0.5f);
	store.Set(spot, authored);
	REQUIRE(ResolveLocalLight(store, spot, *store.Get<Light>(spot), resolved) == LocalLightRejection::None);
	const Vector3 expected = store.Get<Attachment>(attachment)
								 ->WorldFrame.VectorToWorldSpace(engine::scene::NormalOf(authored.Face));
	CHECK(resolved.Direction == expected);
	CHECK(resolved.Direction != engine::scene::NormalOf(authored.Face));
	authored.Kind = LightKind::Surface;
	authored.Angle = 0.0f;
	store.Set(spot, authored);
	REQUIRE(ResolveLocalLight(store, spot, *store.Get<Light>(spot), resolved) == LocalLightRejection::None);
	CHECK(resolved.ConeCosine == Catch::Approx(1.0f));
}

TEST_CASE("local light source eligibility names rejected authored rows", "[scene]") {
	Store store("local_lights_rejected");
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();
	const Entity light =
		store.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("PointLight")), "Light");
	store.Set(light, Light{});
	ResolvedLocalLight resolved;
	CHECK(
		ResolveLocalLight(store, light, *store.Get<Light>(light), resolved) ==
		LocalLightRejection::MissingParent
	);

	const Entity parent =
		store.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Folder")), "Parent");
	REQUIRE(store.SetParent(light, parent));
	CHECK(
		ResolveLocalLight(store, light, *store.Get<Light>(light), resolved) ==
		LocalLightRejection::ParentHasNoPlacement
	);
	Light disabled;
	disabled.Enabled = false;
	store.Set(light, disabled);
	CHECK(
		ResolveLocalLight(store, light, *store.Get<Light>(light), resolved) == LocalLightRejection::Disabled
	);
	disabled.Enabled = true;
	disabled.Brightness = 0.0f;
	store.Set(light, disabled);
	CHECK(
		ResolveLocalLight(store, light, *store.Get<Light>(light), resolved) ==
		LocalLightRejection::NonPositiveBrightness
	);
	disabled.Brightness = 1.0f;
	disabled.Range = 0.0f;
	store.Set(light, disabled);
	CHECK(
		ResolveLocalLight(store, light, *store.Get<Light>(light), resolved) ==
		LocalLightRejection::NonPositiveRange
	);
}
