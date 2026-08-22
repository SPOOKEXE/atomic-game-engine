// The sky, and the fog it deliberately does not replace.
//
// **`docs/ARCH_REVIEW.md` D4 reads the tree as having no fog at all.** It has,
// on the `Lighting` service, resolved by `LightingOf` and read by the renderer -
// so the first case here is that adding an atmosphere leaves it alone. A
// `scene::Fog` component would have been a second answer to what a world's
// distance fade is, which is rule 2.

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Atmosphere.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Sunlight.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.scene.atmosphere")

using Catch::Approx;
using engine::core::Color3;
using engine::core::Name;
using engine::ecs::Classes;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::scene::Atmosphere;
using engine::scene::AtmosphereClass;
using engine::scene::AtmosphereOf;
using engine::scene::Clouds;
using engine::scene::CloudsOf;
using engine::scene::InstallServices;
using engine::scene::LightingOf;
using engine::scene::LightingServiceComponent;
using engine::scene::RegisterSceneClasses;
using engine::scene::WorldLighting;

namespace {
	Entity Lighting(Store &store) {
		InstallServices(store);
		const Entity service = store.FindFirstRoot("Lighting");
		REQUIRE(service != engine::ecs::NULL_ENTITY);
		return service;
	}

	Entity AddAtmosphere(Store &store, const Atmosphere &air) {
		const Entity instance = store.CreateInstance(AtmosphereClass(), "Atmosphere");
		REQUIRE(store.SetParent(instance, Lighting(store)));
		store.Set(instance, air);
		return instance;
	}
}

TEST_CASE("a world with no atmosphere is clear air", "[scene][atmosphere]") {
	// Not the struct's own defaults, which describe a hazy day: a place that
	// never asked for haze must not acquire it on load.
	RegisterSceneClasses();
	Store store("atmosphere_test.none");
	Lighting(store);

	CHECK(AtmosphereOf(store).Density == Approx(0.0f));
	CHECK_FALSE(CloudsOf(store).Enabled);
}

TEST_CASE("an atmosphere does not disturb the fog beside it", "[scene][atmosphere]") {
	// The two are separate models and a world may author either. This is the
	// case that says D4's reading of the tree was wrong: the fog was always
	// there, on the service where it belongs.
	RegisterSceneClasses();
	Store store("atmosphere_test.fog");

	LightingServiceComponent *service = store.GetMutable<LightingServiceComponent>(Lighting(store));
	REQUIRE(service != nullptr);
	service->FogColor = Color3{0.2f, 0.3f, 0.4f};
	service->FogStart = 25.0f;
	service->FogEnd = 90.0f;

	Atmosphere air;
	air.Density = 0.8f;
	AddAtmosphere(store, air);

	const WorldLighting resolved = LightingOf(store);
	CHECK(resolved.FogStart == Approx(25.0f));
	CHECK(resolved.FogEnd == Approx(90.0f));
	CHECK(resolved.FogColor.B == Approx(0.4f));
	CHECK(resolved.Air.Density == Approx(0.8f));
}

TEST_CASE("the resolved lighting carries the sky", "[scene][atmosphere]") {
	// Resolved once, where the fog is resolved, so a portal's far half and the
	// eye's own view read one answer rather than searching the tree twice.
	RegisterSceneClasses();
	Store store("atmosphere_test.resolved");

	Atmosphere air;
	air.Colour = Color3{0.9f, 0.5f, 0.2f};
	air.Density = 0.25f;
	air.Haze = 3.0f;
	AddAtmosphere(store, air);

	const Entity clouds = store.CreateInstance(Classes::Find(Name("Clouds")), "Clouds");
	REQUIRE(store.SetParent(clouds, Lighting(store)));

	Clouds layer;
	layer.Cover = 0.3f;
	layer.WindSpeed = 12.0f;
	store.Set(clouds, layer);

	const WorldLighting resolved = LightingOf(store);
	CHECK(resolved.Air.Colour.R == Approx(0.9f));
	CHECK(resolved.Air.Haze == Approx(3.0f));
	CHECK(resolved.Sky.Enabled);
	CHECK(resolved.Sky.Cover == Approx(0.3f));
	CHECK(resolved.Sky.WindSpeed == Approx(12.0f));
}

TEST_CASE("authored values out of range are clamped on read", "[scene][atmosphere]") {
	// `LightingOf`'s rule for `FogStart` and `FogEnd`: these arrive from a file
	// and a wire, and a density above one is a shader reading past the end of a
	// lookup rather than a slightly thick day.
	RegisterSceneClasses();
	Store store("atmosphere_test.clamp");

	Atmosphere air;
	air.Density = 4.0f;
	air.Offset = -9.0f;
	air.Glare = 100.0f;
	air.Haze = -2.0f;
	AddAtmosphere(store, air);

	const Atmosphere resolved = AtmosphereOf(store);
	CHECK(resolved.Density == Approx(1.0f));
	CHECK(resolved.Offset == Approx(-1.0f));
	CHECK(resolved.Glare == Approx(10.0f));
	CHECK(resolved.Haze == Approx(0.0f));
}

TEST_CASE("a cloud layer under the atmosphere resolves too", "[scene][atmosphere]") {
	// A descendant walk rather than a child lookup, so an author following
	// either arrangement gets a sky.
	RegisterSceneClasses();
	Store store("atmosphere_test.nested");

	const Entity air = AddAtmosphere(store, Atmosphere{});
	const Entity clouds = store.CreateInstance(Classes::Find(Name("Clouds")), "Clouds");
	REQUIRE(store.SetParent(clouds, air));

	Clouds layer;
	layer.Cover = 0.75f;
	store.Set(clouds, layer);

	CHECK(CloudsOf(store).Cover == Approx(0.75f));
}

TEST_CASE("a renamed lighting service still finds the sky", "[scene][atmosphere]") {
	// This module's fixture rule: found by class and never by name, so a script
	// renaming `Lighting` cannot make the sky disappear.
	RegisterSceneClasses();
	Store store("atmosphere_test.renamed");

	Atmosphere air;
	air.Density = 0.6f;
	AddAtmosphere(store, air);

	REQUIRE(store.SetInstanceName(store.FindFirstRoot("Lighting"), "Weather"));
	CHECK(AtmosphereOf(store).Density == Approx(0.6f));
}
