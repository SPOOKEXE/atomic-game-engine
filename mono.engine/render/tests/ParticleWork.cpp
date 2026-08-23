#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <ParticleWork.hpp>

TEST_SUITE_ID("engine.render.particlework")

using engine::render::ParticleStepDelta;
using engine::render::ParticleWorkgroups;
using engine::render::ParticleWorkItem;

TEST_CASE("particle work items match the shader's two-word resident layout", "[render][particles]") {
	const ParticleWorkItem item{71, 4098};
	const auto *words = reinterpret_cast<const uint32_t *>(&item);

	STATIC_REQUIRE(sizeof(ParticleWorkItem) == sizeof(uint32_t) * 2);
	CHECK(words[0] == 71);
	CHECK(words[1] == 4098);
}

TEST_CASE("small emitters share full compute groups instead of padding per emitter", "[render][particles]") {
	constexpr uint32_t emitters = 102'400;
	constexpr uint32_t slotsPerEmitter = 6;
	constexpr uint32_t workItems = emitters * slotsPerEmitter;

	CHECK(ParticleWorkgroups(workItems) == 9'600);
	CHECK(ParticleWorkgroups(workItems) < emitters / 10);
	CHECK(ParticleWorkgroups(0) == 0);
	CHECK(ParticleWorkgroups(65) == 2);
}

TEST_CASE("an uncapped redraw does not advance a resident particle revision twice", "[render][particles]") {
	CHECK(ParticleStepDelta(40, 40, 1.0f / 60.0f, 0.0f) == 0.0f);
	CHECK(ParticleStepDelta(40, 41, 1.0f / 60.0f, 0.0f) == 1.0f / 60.0f);
	CHECK(ParticleStepDelta(41, 41, 1.0f / 60.0f, 0.025f) == 0.025f);
	CHECK(ParticleStepDelta(40, 41, 1.0f / 60.0f, 0.025f) == 0.025f + 1.0f / 60.0f);
}
