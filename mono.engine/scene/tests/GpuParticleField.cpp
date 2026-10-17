#include <engine/core/Bytes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/GpuParticleField.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.scene.gpuparticlefield")

TEST_CASE("GPU particle fields normalize authored requests to fixed presets", "[scene][particles]") {
	using engine::scene::NormalizeGpuParticleCount;
	CHECK(NormalizeGpuParticleCount(0) == 1'048'576);
	CHECK(NormalizeGpuParticleCount(1) == 262'144);
	CHECK(NormalizeGpuParticleCount(262'144) == 262'144);
	CHECK(NormalizeGpuParticleCount(262'145) == 524'288);
	CHECK(NormalizeGpuParticleCount(524'288) == 524'288);
	CHECK(NormalizeGpuParticleCount(524'289) == 1'048'576);
	CHECK(NormalizeGpuParticleCount(1'048'577) == 2'000'000);
	CHECK(NormalizeGpuParticleCount(4'194'304) == 5'000'000);
	CHECK(NormalizeGpuParticleCount(5'000'000) == 5'000'000);
	CHECK(NormalizeGpuParticleCount(10'000'000) == 10'000'000);
	CHECK(NormalizeGpuParticleCount(15'000'000) == 15'000'000);
	CHECK(NormalizeGpuParticleCount(20'000'000) == 20'000'000);
	CHECK(NormalizeGpuParticleCount(16'777'217) == 20'000'000);
	CHECK(NormalizeGpuParticleCount(20'000'001) == 50'000'000);
	CHECK(NormalizeGpuParticleCount(UINT32_MAX) == 50'000'000);
}

TEST_CASE("GPU particle field layers are explicit and disable the whole field", "[scene][particles]") {
	using engine::scene::GpuParticleField;
	using engine::scene::GpuParticleLayer;
	using engine::scene::HasGpuParticleLayer;
	GpuParticleField field;
	CHECK(HasGpuParticleLayer(field, GpuParticleLayer::First));
	CHECK(HasGpuParticleLayer(field, GpuParticleLayer::Second));
	CHECK(HasGpuParticleLayer(field, GpuParticleLayer::Third));
	field.Layers = static_cast<uint8_t>(GpuParticleLayer::Second);
	CHECK_FALSE(HasGpuParticleLayer(field, GpuParticleLayer::First));
	CHECK(HasGpuParticleLayer(field, GpuParticleLayer::Second));
	field.Enabled = false;
	CHECK_FALSE(HasGpuParticleLayer(field, GpuParticleLayer::Second));
}

TEST_CASE("GPU field authoring is bounded transactional and durable", "[scene][particles]") {
	using namespace engine;
	scene::RegisterSceneClasses();
	ecs::Store store("generic-particle-field");
	const auto instance = store.CreateInstance(ecs::Classes::Find(core::Name("GpuParticleField")), "Field");
	store.Observe<scene::GpuParticleField>();
	core::ByteWriter sample;
	for (float value : {1.0f, 2.0f, 3.0f, 10.0f, 4.0f, 5.0f, 6.0f})
		sample.WriteFloat(value);
	sample.WriteUInt32(2);
	REQUIRE(scene::SetGpuParticleSpawnSamples(store, instance, sample.Bytes()));
	const auto authored = *store.Get<scene::GpuParticleField>(instance);
	REQUIRE(authored.SpawnSamples.size() == 1);
	CHECK(authored.SpawnSamples[0].Position == core::Vector3{1, 2, 3});
	CHECK(authored.SpawnSamples[0].Layer == 2);
	store.ClearChanges();
	REQUIRE(scene::SetGpuParticleSpawnSamples(store, instance, sample.Bytes()));
	CHECK_FALSE(store.Changed<scene::GpuParticleField>(instance));
	CHECK_FALSE(scene::SetGpuParticleSpawnSamples(store, instance, sample.Bytes().first(31)));
	std::vector<std::byte> oversized(
		(scene::MAX_GPU_PARTICLE_SPAWN_SAMPLES + 1) * scene::GPU_PARTICLE_SPAWN_SAMPLE_BYTES
	);
	CHECK_FALSE(scene::SetGpuParticleSpawnSamples(store, instance, oversized));
	core::ByteWriter invalid;
	invalid.WriteFloat(std::numeric_limits<float>::quiet_NaN());
	for (float value : {2.0f, 3.0f, 10.0f, 4.0f, 5.0f, 6.0f})
		invalid.WriteFloat(value);
	invalid.WriteUInt32(0);
	CHECK_FALSE(scene::SetGpuParticleSpawnSamples(store, instance, invalid.Bytes()));
	CHECK(store.Get<scene::GpuParticleField>(instance)->SpawnSamples == authored.SpawnSamples);
	scene::GpuParticleStyle style;
	style.Colour = {1, 0, 0};
	style.Size = 4;
	style.Acceleration = {0, -9.8f, 0};
	REQUIRE(scene::SetGpuParticleStyle(store, instance, 2, style));
	const auto definition = scene::GpuParticleDefinition(*store.Get<scene::GpuParticleField>(instance));
	scene::GpuParticleField decoded;
	REQUIRE(scene::ReadGpuParticleDefinition(definition, decoded));
	CHECK(decoded.Styles[2] == style);
	CHECK(decoded.SpawnSamples == authored.SpawnSamples);
	CHECK_FALSE(scene::ReadGpuParticleDefinition("v1:xx", decoded));
	CHECK(decoded.Styles[2] == style);
	core::ByteWriter snapshot;
	REQUIRE(store.Save(snapshot));
	ecs::Store restored("restored-field");
	core::ByteReader reader(snapshot.Bytes());
	REQUIRE(restored.Load(reader));
	CHECK(restored.Get<scene::GpuParticleField>(instance)->SpawnSamples == authored.SpawnSamples);
	CHECK(restored.Get<scene::GpuParticleField>(instance)->Styles[2] == style);
	store.SetAdoptOnly(true);
	CHECK_FALSE(scene::SetGpuParticleSpawnSamples(store, instance, {}));
	CHECK_FALSE(scene::SetGpuParticleStyle(store, instance, 0, style));
	store.SetAdoptOnly(false);
	REQUIRE(scene::SetGpuParticleSpawnSamples(store, instance, {}));
	CHECK(store.Get<scene::GpuParticleField>(instance)->SpawnSamples.empty());
}

TEST_CASE("GPU field maximum samples and styles keep admission transactional", "[scene][particles]") {
	using namespace engine;
	scene::RegisterSceneClasses();
	ecs::Store store("field-admission");
	const auto instance = store.CreateInstance(ecs::Classes::Find(core::Name("GpuParticleField")), "Field");
	core::ByteWriter maximum;
	for (size_t index = 0; index < scene::MAX_GPU_PARTICLE_SPAWN_SAMPLES; ++index) {
		for (float value : {1.0f, 2.0f, 3.0f, 3600.0f, 4.0f, 5.0f, 6.0f})
			maximum.WriteFloat(value);
		maximum.WriteUInt32(2);
	}
	REQUIRE(scene::SetGpuParticleSpawnSamples(store, instance, maximum.Bytes()));
	const auto before = scene::GpuParticleDefinition(*store.Get<scene::GpuParticleField>(instance));
	scene::GpuParticleField decoded;
	REQUIRE(scene::ReadGpuParticleDefinition(before, decoded));
	CHECK(decoded.SpawnSamples.size() == scene::MAX_GPU_PARTICLE_SPAWN_SAMPLES);
	const auto decodedBefore = decoded.SpawnSamples;
	CHECK_FALSE(scene::ReadGpuParticleDefinition(before + "00", decoded));
	CHECK_FALSE(
		scene::ReadGpuParticleDefinition(std::string_view(before).substr(0, before.size() - 2), decoded)
	);
	CHECK(decoded.SpawnSamples == decodedBefore);
	core::ByteWriter zeroLifetime;
	for (float value : {1.0f, 2.0f, 3.0f, 0.0f, 4.0f, 5.0f, 6.0f})
		zeroLifetime.WriteFloat(value);
	zeroLifetime.WriteUInt32(0);
	CHECK_FALSE(scene::SetGpuParticleSpawnSamples(store, instance, zeroLifetime.Bytes()));
	scene::GpuParticleStyle oversizedStyle;
	oversizedStyle.Size = 65;
	CHECK_FALSE(scene::SetGpuParticleStyle(store, instance, 0, oversizedStyle));
	oversizedStyle.Size = 1;
	oversizedStyle.Acceleration.X = std::numeric_limits<float>::infinity();
	CHECK_FALSE(scene::SetGpuParticleStyle(store, instance, 0, oversizedStyle));
	CHECK(scene::GpuParticleDefinition(*store.Get<scene::GpuParticleField>(instance)) == before);
}

TEST_CASE("replica particle authoring requires predicted local ownership", "[scene][particles]") {
	using namespace engine;
	scene::RegisterSceneClasses();
	ecs::Store store("local-fields");
	const auto klass = ecs::Classes::Find(core::Name("GpuParticleField"));
	const auto authority = store.CreateInstance(klass, "Authority");
	const auto local = store.CreatePredictedInstance(klass, "Local");
	store.Set(authority, ecs::ClientLocal{});
	store.SetAdoptOnly(true);
	core::ByteWriter bytes;
	for (float value : {0.0f, 0.0f, 0.0f, 10.0f, 1.0f, 2.0f, 3.0f})
		bytes.WriteFloat(value);
	bytes.WriteUInt32(0);
	CHECK_FALSE(scene::SetGpuParticleSpawnSamples(store, authority, bytes.Bytes()));
	CHECK_FALSE(scene::SetGpuParticleSpawnSamples(store, local, bytes.Bytes()));
	store.Set(local, ecs::ClientLocal{});
	REQUIRE(scene::SetGpuParticleSpawnSamples(store, local, bytes.Bytes()));
	scene::GpuParticleStyle style;
	style.Alpha = .5;
	REQUIRE(scene::SetGpuParticleStyle(store, local, 0, style));
	CHECK_FALSE(scene::SetGpuParticleStyle(store, authority, 0, style));
	const auto definition = scene::GpuParticleDefinition(*store.Get<scene::GpuParticleField>(local));
	// Owned script writes use the explicit authoring door after checking ownership.
	REQUIRE(store.SetPropertyAuthored(local, core::Name("Definition"), &definition, sizeof(definition)));
	CHECK_FALSE(
		store.SetPropertyAuthored(authority, core::Name("Definition"), &definition, sizeof(definition))
	);
	CHECK(store.Get<scene::GpuParticleField>(authority)->SpawnSamples.empty());
}
