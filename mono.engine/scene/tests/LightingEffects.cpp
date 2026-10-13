#include <engine/core/Bytes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/LightingEffects.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Sunlight.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

TEST_SUITE_ID("engine.scene.lighting-effects")

using engine::core::Name;
using engine::ecs::Classes;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using engine::scene::BloomEffect;
using engine::scene::DepthOfFieldEffect;
using engine::scene::FogEffect;
using engine::scene::GodRaysEffect;
using engine::scene::LightingOf;

namespace {
	Entity Lighting(Store &store) {
		engine::scene::InstallServices(store);
		const Entity lighting = store.FindFirstRoot("Lighting");
		REQUIRE(lighting != NULL_ENTITY);
		return lighting;
	}

	Entity Add(Store &store, const char *className, Entity parent) {
		const Entity effect = store.CreateInstance(Classes::Find(Name(className)), className);
		REQUIRE(effect != NULL_ENTITY);
		REQUIRE(store.SetParent(effect, parent));
		return effect;
	}

	void CheckValues(const BloomEffect *actual, const BloomEffect &expected) {
		REQUIRE(actual != nullptr);
		CHECK(actual->Intensity == expected.Intensity);
		CHECK(actual->Threshold == expected.Threshold);
		CHECK(actual->Radius == expected.Radius);
		CHECK(actual->Enabled == expected.Enabled);
	}

	void CheckValues(const DepthOfFieldEffect *actual, const DepthOfFieldEffect &expected) {
		REQUIRE(actual != nullptr);
		CHECK(actual->Intensity == expected.Intensity);
		CHECK(actual->FocusDistance == expected.FocusDistance);
		CHECK(actual->FocusRange == expected.FocusRange);
		CHECK(actual->Radius == expected.Radius);
		CHECK(actual->Enabled == expected.Enabled);
	}

	void CheckValues(const GodRaysEffect *actual, const GodRaysEffect &expected) {
		REQUIRE(actual != nullptr);
		CHECK(actual->Intensity == expected.Intensity);
		CHECK(actual->Threshold == expected.Threshold);
		CHECK(actual->Radius == expected.Radius);
		CHECK(actual->Enabled == expected.Enabled);
	}

	void CheckValues(const FogEffect *actual, const FogEffect &expected) {
		REQUIRE(actual != nullptr);
		CHECK(actual->Color == expected.Color);
		CHECK(actual->Start == expected.Start);
		CHECK(actual->End == expected.End);
		CHECK(actual->Enabled == expected.Enabled);
	}
}

TEST_CASE(
	"effect children are creatable and Lighting has no old effect properties", "[scene][lighting-effects]"
) {
	engine::scene::RegisterSceneClasses();
	Store store("lighting-effects.classes");
	const Entity lighting = Lighting(store);
	for (const char *className : {"BloomEffect", "DepthOfFieldEffect", "GodRaysEffect", "FogEffect"}) {
		const auto klass = Classes::Find(Name(className));
		REQUIRE(klass.IsValid());
		CHECK(Classes::Describe(klass).Creatable);
		CHECK(Add(store, className, lighting) != NULL_ENTITY);
	}
	float value = 0.0f;
	CHECK_FALSE(store.GetProperty(lighting, Name("BloomIntensity"), &value, sizeof(value)));
	CHECK_FALSE(store.GetProperty(lighting, Name("FogStart"), &value, sizeof(value)));
}

TEST_CASE("effect property bounds and raw component writes resolve correctly", "[scene][lighting-effects]") {
	engine::scene::RegisterSceneClasses();
	Store store("lighting-effects.bounds");
	const Entity lighting = Lighting(store);
	const Entity bloom = Add(store, "BloomEffect", lighting);
	const Entity depth = Add(store, "DepthOfFieldEffect", lighting);
	const Entity rays = Add(store, "GodRaysEffect", lighting);
	const Entity fog = Add(store, "FogEffect", lighting);
	const float high = 1000000.0f;
	const float low = -2.0f;
	REQUIRE(store.SetProperty(bloom, Name("Intensity"), &high, sizeof(high)));
	REQUIRE(store.SetProperty(bloom, Name("Threshold"), &high, sizeof(high)));
	REQUIRE(store.SetProperty(bloom, Name("Radius"), &high, sizeof(high)));
	REQUIRE(store.SetProperty(depth, Name("Intensity"), &high, sizeof(high)));
	REQUIRE(store.SetProperty(depth, Name("FocusDistance"), &high, sizeof(high)));
	REQUIRE(store.SetProperty(depth, Name("FocusRange"), &high, sizeof(high)));
	REQUIRE(store.SetProperty(depth, Name("Radius"), &high, sizeof(high)));
	REQUIRE(store.SetProperty(rays, Name("Intensity"), &high, sizeof(high)));
	REQUIRE(store.SetProperty(rays, Name("Threshold"), &high, sizeof(high)));
	REQUIRE(store.SetProperty(rays, Name("Radius"), &high, sizeof(high)));
	REQUIRE(store.SetProperty(fog, Name("Start"), &low, sizeof(low)));
	REQUIRE(store.SetProperty(fog, Name("End"), &low, sizeof(low)));
	CHECK(store.Get<BloomEffect>(bloom)->Intensity == 100.0f);
	CHECK(store.Get<BloomEffect>(bloom)->Threshold == 10000.0f);
	CHECK(store.Get<BloomEffect>(bloom)->Radius == 128.0f);
	CHECK(store.Get<DepthOfFieldEffect>(depth)->Intensity == 1.0f);
	CHECK(store.Get<DepthOfFieldEffect>(depth)->FocusDistance == 100000.0f);
	CHECK(store.Get<DepthOfFieldEffect>(depth)->FocusRange == 100000.0f);
	CHECK(store.Get<DepthOfFieldEffect>(depth)->Radius == 128.0f);
	CHECK(store.Get<GodRaysEffect>(rays)->Intensity == 100.0f);
	CHECK(store.Get<GodRaysEffect>(rays)->Threshold == 10000.0f);
	CHECK(store.Get<GodRaysEffect>(rays)->Radius == 512.0f);
	const auto bounded = LightingOf(store);
	CHECK(bounded.FogStart == 0.0f);
	CHECK(bounded.FogEnd > bounded.FogStart);

	store.GetMutable<BloomEffect>(bloom)->Intensity = -1.0f;
	store.GetMutable<DepthOfFieldEffect>(depth)->Intensity = 9.0f;
	store.GetMutable<GodRaysEffect>(rays)->Radius = -1.0f;
	store.GetMutable<FogEffect>(fog)->Start = 12.0f;
	const auto raw = LightingOf(store);
	CHECK(raw.BloomIntensity == 0.0f);
	CHECK(raw.DepthOfFieldIntensity == 1.0f);
	CHECK(raw.GodRayRadius == 0.0f);
	CHECK(raw.FogStart == 12.0f);
	CHECK(raw.FogEnd == std::nextafter(12.0f, std::numeric_limits<float>::max()));
	store.GetMutable<FogEffect>(fog)->Start = std::numeric_limits<float>::max();
	store.GetMutable<FogEffect>(fog)->End = std::numeric_limits<float>::quiet_NaN();
	const auto extreme = LightingOf(store);
	CHECK(extreme.FogStart == std::nextafter(std::numeric_limits<float>::max(), 0.0f));
	CHECK(extreme.FogEnd == std::numeric_limits<float>::max());
	CHECK(std::isfinite(extreme.FogStart));
	CHECK(std::isfinite(extreme.FogEnd));
	store.GetMutable<FogEffect>(fog)->Start = std::numeric_limits<float>::infinity();
	store.GetMutable<FogEffect>(fog)->End = -std::numeric_limits<float>::infinity();
	const auto infinite = LightingOf(store);
	CHECK(infinite.FogStart == extreme.FogStart);
	CHECK(infinite.FogEnd == extreme.FogEnd);
}

TEST_CASE("first descendant of each effect kind wins until moved or destroyed", "[scene][lighting-effects]") {
	engine::scene::RegisterSceneClasses();
	Store store("lighting-effects.selection");
	const Entity lighting = Lighting(store);
	const Entity outside = Add(store, "BloomEffect", store.FindFirstRoot("Workspace"));
	store.GetMutable<BloomEffect>(outside)->Intensity = 99.0f;
	const Entity first = Add(store, "BloomEffect", lighting);
	const Entity second = Add(store, "BloomEffect", lighting);
	store.GetMutable<BloomEffect>(first)->Intensity = 2.0f;
	store.GetMutable<BloomEffect>(second)->Intensity = 3.0f;
	CHECK(LightingOf(store).BloomIntensity == 2.0f);
	store.GetMutable<BloomEffect>(first)->Enabled = false;
	CHECK(LightingOf(store).BloomIntensity == 0.0f);
	REQUIRE(store.SetInstanceName(first, "Renamed"));
	CHECK(LightingOf(store).BloomIntensity == 0.0f);
	REQUIRE(store.SetParent(first, outside));
	CHECK(LightingOf(store).BloomIntensity == 3.0f);
	const Entity nested = Add(store, "GodRaysEffect", second);
	store.GetMutable<GodRaysEffect>(nested)->Intensity = 4.0f;
	CHECK(LightingOf(store).GodRayIntensity == 4.0f);
	store.Destroy(nested);
	CHECK(LightingOf(store).GodRayIntensity == 0.0f);
	store.Destroy(second);
	CHECK(LightingOf(store).BloomIntensity == 0.0f);
	REQUIRE(store.SetParent(first, lighting));
	CHECK(LightingOf(store).BloomIntensity == 0.0f);
	store.GetMutable<BloomEffect>(first)->Enabled = true;
	CHECK(LightingOf(store).BloomIntensity == 2.0f);
}

TEST_CASE("disabled fog leaves a black background and a far no-fog range", "[scene][lighting-effects]") {
	engine::scene::RegisterSceneClasses();
	Store store("lighting-effects.fog");
	const Entity lighting = Lighting(store);
	const auto blank = LightingOf(store);
	CHECK(blank.FogColor == engine::core::Color3{});
	CHECK(blank.FogStart >= 1000000000.0f);
	const Entity first = Add(store, "FogEffect", lighting);
	const Entity second = Add(store, "FogEffect", lighting);
	store.GetMutable<FogEffect>(first)->Enabled = false;
	store.GetMutable<FogEffect>(second)->Start = 10.0f;
	CHECK(LightingOf(store).FogStart == blank.FogStart);
	store.Destroy(first);
	CHECK(LightingOf(store).FogStart == 10.0f);
}

TEST_CASE("effect children survive a store snapshot and a subtree clone", "[scene][lighting-effects]") {
	engine::scene::RegisterSceneClasses();
	Store source("lighting-effects.snapshot");
	const Entity lighting = Lighting(source);
	const Entity bloom = Add(source, "BloomEffect", lighting);
	const Entity depth = Add(source, "DepthOfFieldEffect", lighting);
	const Entity rays = Add(source, "GodRaysEffect", lighting);
	const Entity fog = Add(source, "FogEffect", lighting);
	BloomEffect bloomValues;
	bloomValues.Intensity = 2.5f;
	bloomValues.Threshold = 0.75f;
	bloomValues.Radius = 11.0f;
	DepthOfFieldEffect depthValues;
	depthValues.Intensity = 0.6f;
	depthValues.FocusDistance = 31.0f;
	depthValues.FocusRange = 3.0f;
	depthValues.Radius = 9.0f;
	GodRaysEffect raysValues;
	raysValues.Intensity = 4.0f;
	raysValues.Threshold = 0.4f;
	raysValues.Radius = 65.0f;
	raysValues.Enabled = false;
	FogEffect fogValues;
	fogValues.Color = {0.2f, 0.3f, 0.4f};
	fogValues.Start = 18.0f;
	fogValues.End = 82.0f;
	source.Set(bloom, bloomValues);
	source.Set(depth, depthValues);
	source.Set(rays, raysValues);
	source.Set(fog, fogValues);
	const Entity bloomCopy = source.CloneInstance(bloom);
	const Entity depthCopy = source.CloneInstance(depth);
	const Entity raysCopy = source.CloneInstance(rays);
	const Entity fogCopy = source.CloneInstance(fog);
	REQUIRE(bloomCopy != NULL_ENTITY);
	REQUIRE(depthCopy != NULL_ENTITY);
	REQUIRE(raysCopy != NULL_ENTITY);
	REQUIRE(fogCopy != NULL_ENTITY);
	CheckValues(source.Get<BloomEffect>(bloomCopy), bloomValues);
	CheckValues(source.Get<DepthOfFieldEffect>(depthCopy), depthValues);
	CheckValues(source.Get<GodRaysEffect>(raysCopy), raysValues);
	CheckValues(source.Get<FogEffect>(fogCopy), fogValues);
	CHECK(LightingOf(source).BloomIntensity == 2.5f);
	CHECK(LightingOf(source).GodRayIntensity == 0.0f);
	engine::core::ByteWriter writer;
	REQUIRE(source.Save(writer));
	Store restored("lighting-effects.restored");
	engine::core::ByteReader reader(writer.Bytes());
	REQUIRE(restored.Load(reader));
	CheckValues(restored.Get<BloomEffect>(bloom), bloomValues);
	CheckValues(restored.Get<DepthOfFieldEffect>(depth), depthValues);
	CheckValues(restored.Get<GodRaysEffect>(rays), raysValues);
	CheckValues(restored.Get<FogEffect>(fog), fogValues);
	CHECK(LightingOf(restored).BloomIntensity == 2.5f);
	CHECK(LightingOf(restored).DepthOfFieldFocusDistance == 31.0f);
	CHECK(LightingOf(restored).GodRayIntensity == 0.0f);
	CHECK(LightingOf(restored).FogStart == 18.0f);
}
