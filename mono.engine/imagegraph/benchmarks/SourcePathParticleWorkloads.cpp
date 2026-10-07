#include "../tests/fixtures/SourcePathParticleWorkloads.hpp"

#include "SourceWorkloadProfile.hpp"

TEST_SUITE_ID("engine.imagegraph.bench.path-particle")
using Fixture = engine::imagegraph::testing::SourcePathParticleFixture;
BENCH("CPU Path Bake length1 128unit line compiled-plan", 1) {
	static Profile<Fixture> p(Fixture::Kind::PathLength);
	p.Measure();
}
BENCH("CPU Path Bake amount64 128unit line compiled-plan", 1) {
	static Profile<Fixture> p(Fixture::Kind::PathAmount);
	p.Measure();
}
BENCH("CPU Particle3D state512 replay16frames seed12345", 1) {
	static Profile<Fixture> p(Fixture::Kind::ParticleState);
	p.Measure();
}
BENCH("CPU Particle3D vertices1530 prebuilt-state billboard", 1) {
	static Profile<Fixture> p(Fixture::Kind::ParticleVertices);
	p.Measure();
}

BENCH("CPU Spiral Path weighted line 64 downstream ratio samples compiled-plan", 1) {
	static Profile<Fixture> p(Fixture::Kind::SpiralSamples);
	p.Measure();
}
