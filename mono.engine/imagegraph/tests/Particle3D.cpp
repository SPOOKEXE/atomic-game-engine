#include <engine/imagegraph/Particle3D.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>

TEST_SUITE_ID("engine.imagegraph.particle_3d")
using namespace engine::imagegraph;

TEST_CASE("Particle render flags round halfway to even", "[imagegraph][particle3d]") {
	ParticleRecord3D particle;
	for (float flags : {.5f, 1.5f, 2.5f, 3.5f, -.5f, -1.5f}) {
		particle.RenderFlags = flags;
		bool billboard = true;
		REQUIRE(ParticleBillboard3D(particle, billboard));
		CHECK_FALSE(billboard);
	}
	for (float flags : {1.f, 3.f, -1.f, .5001f}) {
		particle.RenderFlags = flags;
		bool billboard = false;
		REQUIRE(ParticleBillboard3D(particle, billboard));
		CHECK(billboard);
	}
	particle.RenderFlags = 0x1p31f;
	bool unchanged = true;
	CHECK_FALSE(ParticleBillboard3D(particle, unchanged));
	CHECK(unchanged);
}

TEST_CASE(
	"Particle records retain life velocity reserved words and validate before traversal",
	"[imagegraph][particle3d]"
) {
	ParticleRecord3D particle{1, 2, 30, 17, 4, {5, 6, 7}, {.2f, .3f, .4f, .5f}, {8, 9, 10, 11}};
	CHECK(ValidParticleRecord3D(particle));
	const auto copy = particle;
	CHECK(copy == particle);
	CHECK(copy.Velocity[3] == 11);
	CHECK(copy.Reserved[2] == 7);
	const std::array records{particle, particle};
	CHECK_FALSE(ValidParticleRecords3D(records, 1));
	CHECK(ValidParticleRecords3D(records, 2));
	particle.LifeTime = std::numeric_limits<float>::infinity();
	CHECK_FALSE(ValidParticleRecord3D(particle));
}
