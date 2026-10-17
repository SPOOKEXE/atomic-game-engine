// The Lighting service resolved into the value consumed by every scene pass.
//
// These cases deliberately test the resolver rather than property storage. A
// property can round-trip through the ECS while changing no rendered state,
// which is the regression this suite exists to prevent.

#include "fixtures/SunlightParity.hpp"

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/LightingEffects.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Sunlight.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.scene.sunlight")

using Catch::Approx;
using engine::core::Color3;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::scene::InstallServices;
using engine::scene::LightingServiceComponent;
using engine::scene::Sun;
using engine::scene::WorldLighting;

namespace {
	LightingServiceComponent &Lighting(Store &store) {
		InstallServices(store);
		const Entity service = store.FindFirstRoot("Lighting");
		LightingServiceComponent *lighting = store.GetMutable<LightingServiceComponent>(service);
		REQUIRE(lighting != nullptr);
		return *lighting;
	}

	void CheckColour(const Color3 &actual, const Color3 &wanted) {
		CHECK(actual.R == Approx(wanted.R));
		CHECK(actual.G == Approx(wanted.G));
		CHECK(actual.B == Approx(wanted.B));
	}
}

TEST_CASE("Lighting reaches every resolved render term", "[scene][sunlight]") {
	engine::scene::RegisterSceneClasses();
	Store store("sunlight.authored");

	LightingServiceComponent &authored = Lighting(store);
	authored.ClockTime = 12.0f;
	authored.GeographicLatitude = 0.0f;
	authored.Brightness = 3.0f;
	authored.Ambient = Color3{0.1f, 0.2f, 0.3f};
	authored.OutdoorAmbient = Color3{0.4f, 0.5f, 0.6f};
	const Entity service = store.FindFirstRoot("Lighting");
	const auto addEffect = [&](const char *className) {
		const Entity effect =
			store.CreateInstance(engine::ecs::Classes::Find(engine::core::Name(className)), className);
		REQUIRE(store.SetParent(effect, service));
		return effect;
	};
	const Entity fogEntity = addEffect("FogEffect");
	const Entity bloomEntity = addEffect("BloomEffect");
	const Entity depthEntity = addEffect("DepthOfFieldEffect");
	const Entity raysEntity = addEffect("GodRaysEffect");
	auto *fog = store.GetMutable<engine::scene::FogEffect>(fogEntity);
	auto *bloom = store.GetMutable<engine::scene::BloomEffect>(bloomEntity);
	auto *depth = store.GetMutable<engine::scene::DepthOfFieldEffect>(depthEntity);
	auto *rays = store.GetMutable<engine::scene::GodRaysEffect>(raysEntity);
	REQUIRE(fog != nullptr);
	REQUIRE(bloom != nullptr);
	REQUIRE(depth != nullptr);
	REQUIRE(rays != nullptr);
	fog->Color = Color3{0.7f, 0.8f, 0.9f};
	fog->Start = 10.0f;
	fog->End = 40.0f;
	bloom->Intensity = 1.25f;
	bloom->Threshold = 0.8f;
	bloom->Radius = 9.0f;
	depth->Intensity = 0.8f;
	depth->FocusDistance = 21.0f;
	depth->FocusRange = 5.0f;
	depth->Radius = 7.0f;
	rays->Intensity = 1.4f;
	rays->Threshold = 0.6f;
	rays->Radius = 72.0f;

	const WorldLighting resolved = engine::scene::LightingOf(store);
	CHECK(resolved.Direction.X == Approx(0.0f).margin(0.0001f));
	CHECK(resolved.Direction.Y == Approx(-1.0f));
	CHECK(resolved.Direction.Z == Approx(0.0f).margin(0.0001f));
	CheckColour(resolved.Ambient, authored.Ambient);
	CheckColour(resolved.OutdoorAmbient, authored.OutdoorAmbient);
	CheckColour(resolved.Direct, Color3{3.0f, 3.0f, 3.0f});
	CheckColour(resolved.FogColor, Color3{0.7f, 0.8f, 0.9f});
	CHECK(resolved.FogStart == Approx(10.0f));
	CHECK(resolved.FogEnd == Approx(40.0f));
	CHECK(resolved.BloomThreshold == Approx(0.8f));
	CHECK(resolved.BloomIntensity == Approx(1.25f));
	CHECK(resolved.BloomRadius == Approx(9.0f));
	CHECK(resolved.DepthOfFieldIntensity == Approx(0.8f));
	CHECK(resolved.DepthOfFieldFocusDistance == Approx(21.0f));
	CHECK(resolved.DepthOfFieldFocusRange == Approx(5.0f));
	CHECK(resolved.DepthOfFieldRadius == Approx(7.0f));
	CHECK(resolved.GodRayIntensity == Approx(1.4f));
	CHECK(resolved.GodRayThreshold == Approx(0.6f));
	CHECK(resolved.GodRayRadius == Approx(72.0f));
}

TEST_CASE("clock and latitude move the sun and remove night light", "[scene][sunlight]") {
	engine::scene::RegisterSceneClasses();
	Store store("sunlight.solar");
	LightingServiceComponent &authored = Lighting(store);
	authored.Brightness = 2.0f;

	authored.ClockTime = 6.0f;
	authored.GeographicLatitude = 0.0f;
	const WorldLighting sunrise = engine::scene::LightingOf(store);
	CHECK(sunrise.Direction.X == Approx(1.0f));
	CHECK(sunrise.Direction.Y == Approx(0.0f).margin(0.0001f));
	CHECK(sunrise.Direction.Magnitude() == Approx(1.0f));
	CheckColour(sunrise.Direct, Color3{});

	authored.ClockTime = 0.0f;
	const WorldLighting midnight = engine::scene::LightingOf(store);
	CHECK(midnight.Direction.Y == Approx(1.0f));
	CheckColour(midnight.Direct, Color3{});

	authored.ClockTime = 12.0f;
	authored.GeographicLatitude = 60.0f;
	const WorldLighting northernNoon = engine::scene::LightingOf(store);
	CHECK(northernNoon.Direction.Y == Approx(-0.5f));
	CHECK(northernNoon.Direction.Z == Approx(-0.8660254f));
	CheckColour(northernNoon.Direct, Color3{1.0f, 1.0f, 1.0f});
}

TEST_CASE("the legacy Sun resource remains an explicit override", "[scene][sunlight]") {
	engine::scene::RegisterSceneClasses();
	Store store("sunlight.override");
	LightingServiceComponent &authored = Lighting(store);
	authored.ClockTime = 12.0f;
	authored.Ambient = Color3{0.1f, 0.1f, 0.1f};
	const Entity fog =
		store.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("FogEffect")), "Fog");
	REQUIRE(store.SetParent(fog, store.FindFirstRoot("Lighting")));
	store.GetMutable<engine::scene::FogEffect>(fog)->Start = 12.0f;

	Sun override;
	override.Direction = Vector3{0.0f, -4.0f, 0.0f};
	override.Ambient = Color3{0.9f, 0.8f, 0.7f};
	store.SetResource(override);

	const WorldLighting resolved = engine::scene::LightingOf(store);
	CHECK(resolved.Direction == Vector3{0.0f, -1.0f, 0.0f});
	CheckColour(resolved.Ambient, override.Ambient);
	CHECK(resolved.FogStart == Approx(12.0f));
}

TEST_CASE(
	"lighting resolves fresh authored state in root creation order", "[scene][sunlight][sunlight-parity]"
) {
	REQUIRE_NOTHROW(sunlight_fixture::Verify());
}
