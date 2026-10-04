#include "fixtures/SourceFamilyEvaluation.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.source-family-evaluation")
TEST_CASE(
	"headless source-family workloads match analytical outputs and controls", "[imagegraph][source_family]"
) {
	using Fixture = engine::imagegraph::testing::SourceFamilyFixture;
	for (const auto family :
		 {Fixture::Family::Audio,
		  Fixture::Family::Gradient,
		  Fixture::Family::Solid,
		  Fixture::Family::Matrix,
		  Fixture::Family::WavControls,
		  Fixture::Family::ScalarMath,
		  Fixture::Family::Curve,
		  Fixture::Family::Vector,
		  Fixture::Family::HdrDirectional,
		  Fixture::Family::HdrZoom,
		  Fixture::Family::CubeSmall,
		  Fixture::Family::CubeLarge,
		  Fixture::Family::WavTimeline,
		  Fixture::Family::CylinderSmall,
		  Fixture::Family::CylinderProfile,
		  Fixture::Family::ConeDefault8,
		  Fixture::Family::ConeBounded104,
		  Fixture::Family::TorusDefault16x8,
		  Fixture::Family::TorusSmooth31x22,
		  Fixture::Family::UVSphereDefault8x16,
		  Fixture::Family::UVSphereSmooth31x22,
		  Fixture::Family::AudioWindow4096,
		  Fixture::Family::AudioWindow65536,
		  Fixture::Family::IcosphereDefault1,
		  Fixture::Family::IcosphereSmooth3,
		  Fixture::Family::Julia128,
		  Fixture::Family::Gabor128,
		  Fixture::Family::Herringbone128,
		  Fixture::Family::Honeycomb128,
		  Fixture::Family::Heightmap80,
		  Fixture::Family::Julia128EightIterations,
		  Fixture::Family::Gabor128Seeded,
		  Fixture::Family::Flow128DefaultDetail,
		  Fixture::Family::Bubble64SeededDefaultDensity,
		  Fixture::Family::BitmapTextEightRows}) {
		CAPTURE(static_cast<int>(family));
		Fixture fixture(family);
		const auto hash = fixture.Verify();
		fixture.Evaluate();
		CHECK(fixture.Verify() == hash);
		CHECK(fixture.InputHash != 0);
	}
}

TEST_CASE(
	"profiled Text workload refuses insufficient dimension allowance without replacing pixels",
	"[imagegraph][source_family]"
) {
	engine::imagegraph::testing::FontTextWorkload fixture;
	fixture.Evaluate();
	const auto before = fixture.Output;
	const auto hash = fixture.Verify();
	CHECK(fixture.TryEvaluate(1) == engine::imagegraph::Status::LimitExceeded);
	CHECK(fixture.Output.Images == before.Images);
	CHECK(fixture.Output.Items == before.Items);
	CHECK(fixture.Verify() == hash);
	fixture.Evaluate();
	CHECK(fixture.Verify() == hash);
}
