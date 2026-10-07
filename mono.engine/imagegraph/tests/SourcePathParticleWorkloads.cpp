#include "fixtures/SourcePathParticleWorkloads.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

TEST_SUITE_ID("engine.imagegraph.source_path_particle_workloads")
TEST_CASE(
	"Path Bake and Particle CPU workloads retain source oracles and bounded outputs",
	"[imagegraph][path_particle_workloads]"
) {
	using Fixture = engine::imagegraph::testing::SourcePathParticleFixture;
	const auto kind = GENERATE(
		Fixture::Kind::PathLength,
		Fixture::Kind::PathAmount,
		Fixture::Kind::ParticleState,
		Fixture::Kind::ParticleVertices
	);
	INFO("workload=" << int(kind));
	Fixture fixture(kind);
	fixture.Run();
	CHECK(fixture.Verify() == fixture.ExpectedHash);
	fixture.CheckBudget();
}
