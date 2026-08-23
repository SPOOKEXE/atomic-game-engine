// The sky, and the fog it deliberately does not replace.
//
// **`docs/ARCH_REVIEW.md` D4 reads the tree as having no fog at all.** It has,
// on the `Lighting` service, resolved by `LightingOf` and read by the renderer -
// so the first case here is that adding an atmosphere leaves it alone. A
// `scene::Fog` component would have been a second answer to what a world's
// distance fade is, which is rule 2.

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/EnumTable.hpp>
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
using engine::scene::Environment;
using engine::scene::EnvironmentOf;
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
	CHECK(resolved.EnvironmentState.Air.Density == Approx(0.8f));
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
	CHECK(resolved.EnvironmentState.Air.Colour.R == Approx(0.9f));
	CHECK(resolved.EnvironmentState.Air.Haze == Approx(3.0f));
	CHECK(resolved.EnvironmentState.CloudLayer.Enabled);
	CHECK(resolved.EnvironmentState.CloudLayer.Cover == Approx(0.3f));
	CHECK(resolved.EnvironmentState.CloudLayer.WindSpeed == Approx(12.0f));
}

TEST_CASE("only the first provider of each kind below Lighting resolves", "[scene][atmosphere][skybox]") {
	RegisterSceneClasses();
	Store store("atmosphere_test.providers");
	const Entity lighting = Lighting(store);

	const Entity outside = store.CreateInstance(Classes::Find(Name("SkyboxCompute")), "Outside");
	store.GetMutable<engine::scene::SkyboxCompute>(outside)->Seed = 90;

	const Entity firstSky = store.CreateInstance(Classes::Find(Name("SkyboxTextures")), "FirstSky");
	REQUIRE(store.SetParent(firstSky, lighting));
	store.GetMutable<engine::scene::SkyboxTextures>(firstSky)->Front = Name("sky/front.atex");

	const Entity secondSky = store.CreateInstance(Classes::Find(Name("SkyboxCompute")), "SecondSky");
	REQUIRE(store.SetParent(secondSky, lighting));
	store.GetMutable<engine::scene::SkyboxCompute>(secondSky)->Seed = 22;

	const Entity firstCloud = store.CreateInstance(Classes::Find(Name("CloudProcedural")), "FirstCloud");
	REQUIRE(store.SetParent(firstCloud, lighting));
	store.GetMutable<Clouds>(firstCloud)->Cover = 0.2f;

	const Entity secondCloud = store.CreateInstance(Classes::Find(Name("CloudCompute")), "SecondCloud");
	REQUIRE(store.SetParent(secondCloud, lighting));
	store.GetMutable<Clouds>(secondCloud)->Cover = 0.9f;

	const Environment environment = EnvironmentOf(store);
	CHECK(environment.Skybox == engine::scene::SkyboxSource::Textures);
	CHECK(environment.Textures.Front == Name("sky/front.atex"));
	CHECK(environment.CloudLayer.Cover == Approx(0.2f));
	CHECK_FALSE(environment.CloudVolume.Enabled);
}

TEST_CASE("compute variants carry the common authored component", "[scene][atmosphere][compute]") {
	RegisterSceneClasses();
	Store store("atmosphere_test.compute");
	const Entity lighting = Lighting(store);

	const Entity air =
		store.CreateInstance(Classes::Find(Name("AtmosphereProcedural")), "ComputedAtmosphere");
	REQUIRE(store.SetParent(air, lighting));
	store.GetMutable<Atmosphere>(air)->Density = 0.7f;
	store.GetMutable<engine::scene::AtmosphereProcedural>(air)->Samples = 31;

	const Entity clouds = store.CreateInstance(Classes::Find(Name("CloudCompute")), "ComputedClouds");
	REQUIRE(store.SetParent(clouds, lighting));
	store.GetMutable<Clouds>(clouds)->Cover = 0.4f;
	store.GetMutable<engine::scene::CloudCompute>(clouds)->Steps = 23;

	const Environment environment = EnvironmentOf(store);
	CHECK(environment.HasAtmosphere);
	CHECK(environment.Air.Density == Approx(0.7f));
	CHECK(environment.AirCompute.Enabled);
	CHECK(environment.AirCompute.Samples == 31);
	CHECK(environment.HasClouds);
	CHECK(environment.CloudLayer.Cover == Approx(0.4f));
	CHECK(environment.CloudVolume.Enabled);
	CHECK(environment.CloudVolume.Steps == 23);
}

TEST_CASE("enabled properties preserve authored provider switches", "[scene][atmosphere][enabled]") {
	RegisterSceneClasses();
	Store store("atmosphere_test.enabled");
	const Entity lighting = Lighting(store);
	const Entity sky = store.CreateInstance(Classes::Find(Name("SkyboxTextures")), "Sky");
	const Entity clouds = store.CreateInstance(Classes::Find(Name("CloudCompute")), "Clouds");
	REQUIRE(store.SetParent(sky, lighting));
	REQUIRE(store.SetParent(clouds, lighting));

	const bool disabled = false;
	REQUIRE(store.SetProperty(sky, Name("Enabled"), &disabled, sizeof(disabled)));
	REQUIRE(store.SetProperty(clouds, Name("Enabled"), &disabled, sizeof(disabled)));
	CHECK_FALSE(store.Get<engine::scene::SkyboxTextures>(sky)->Enabled);
	CHECK_FALSE(store.Get<Clouds>(clouds)->Enabled);

	const Environment environment = EnvironmentOf(store);
	CHECK_FALSE(environment.Textures.Enabled);
	CHECK_FALSE(environment.CloudLayer.Enabled);
}

TEST_CASE("compute shader choices are closed dropdowns", "[scene][atmosphere][compute][enum]") {
	RegisterSceneClasses();
	Store store("atmosphere_test.shaders");
	const Entity lighting = Lighting(store);

	const Entity sky = store.CreateInstance(Classes::Find(Name("SkyboxCompute")), "Sky");
	const Entity clouds = store.CreateInstance(Classes::Find(Name("CloudCompute")), "Clouds");
	const Entity air = store.CreateInstance(Classes::Find(Name("AtmosphereProcedural")), "Atmosphere");
	REQUIRE(store.SetParent(sky, lighting));
	REQUIRE(store.SetParent(clouds, lighting));
	REQUIRE(store.SetParent(air, lighting));

	const Name nebula("Nebula");
	const Name voxel("Voxel");
	const Name mars("Mars");
	REQUIRE(store.SetProperty(sky, Name("Shader"), &nebula, sizeof(nebula)));
	REQUIRE(store.SetProperty(clouds, Name("Shader"), &voxel, sizeof(voxel)));
	REQUIRE(store.SetProperty(air, Name("Shader"), &mars, sizeof(mars)));
	CHECK(store.Get<engine::scene::SkyboxCompute>(sky)->Shader == engine::scene::SkyboxComputeShader::Nebula);
	CHECK(store.Get<engine::scene::CloudCompute>(clouds)->Shader == engine::scene::CloudComputeShader::Voxel);
	CHECK(
		store.Get<engine::scene::AtmosphereProcedural>(air)->Shader ==
		engine::scene::AtmosphereProceduralShader::Mars
	);

	const Name unknown("NotAComputeShader");
	CHECK_FALSE(store.SetProperty(sky, Name("Shader"), &unknown, sizeof(unknown)));
	CHECK(engine::ecs::EnumTable::MembersOf(Name("SkyboxComputeShader")).size() == 5);
	CHECK(engine::ecs::EnumTable::MembersOf(Name("CloudComputeShader")).size() == 4);
	CHECK(engine::ecs::EnumTable::MembersOf(Name("AtmosphereProceduralShader")).size() == 4);
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
