#include <engine/scene/GpuParticleField.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

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
	CHECK(HasGpuParticleLayer(field, GpuParticleLayer::Condensation));
	CHECK(HasGpuParticleLayer(field, GpuParticleLayer::Rain));
	CHECK(HasGpuParticleLayer(field, GpuParticleLayer::Debris));
	field.Layers = static_cast<uint8_t>(GpuParticleLayer::Rain);
	CHECK_FALSE(HasGpuParticleLayer(field, GpuParticleLayer::Condensation));
	CHECK(HasGpuParticleLayer(field, GpuParticleLayer::Rain));
	field.Enabled = false;
	CHECK_FALSE(HasGpuParticleLayer(field, GpuParticleLayer::Rain));
}
