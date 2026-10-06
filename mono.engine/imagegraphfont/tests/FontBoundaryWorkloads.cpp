#include "fixtures/FontHostBoundary.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraphfont.font_boundary_workloads")
using engine::imagegraphfont::testing::FontHostBoundary;
TEST_CASE(
	"font profiling inputs have literal BDF coverage, real SDF signs and Unicode Text pixels",
	"[font_boundary_workloads]"
) {
	for (const auto kind :
		 {FontHostBoundary::Operation::Coverage,
		  FontHostBoundary::Operation::Distance,
		  FontHostBoundary::Operation::UnicodeText,
		  FontHostBoundary::Operation::OutlineCoverage,
		  FontHostBoundary::Operation::OutlineDistance}) {
		DYNAMIC_SECTION("operation " << static_cast<int>(kind)) {
			CAPTURE(static_cast<int>(kind));
			FontHostBoundary fixture(kind);
			engine::core::Metrics::Drain();
			const auto run = fixture.Run();
			INFO(fixture.Failure << fixture.DiagnosticValue.Message);
			REQUIRE(run);
			fixture.VerifyCounters(engine::core::Metrics::Drain());
			const auto hash = fixture.Verify();
			const auto priorObservation = fixture.Observation;
			const auto priorImage = fixture.Output;
			CHECK_FALSE(fixture.Run(1));
			CHECK(fixture.Observation == priorObservation);
			CHECK(fixture.Output == priorImage);
			CHECK(fixture.Verify() == hash);
			engine::core::Metrics::Drain();
			REQUIRE(fixture.Run());
			fixture.VerifyCounters(engine::core::Metrics::Drain());
			CHECK(fixture.Verify() == hash);
			CHECK(fixture.InputHash != 0);
		}
	}
}
TEST_CASE(
	"profiled font file still requires exact read grants and content policy", "[font_boundary_workloads]"
) {
	using namespace engine::imagegraph;
	FontHostBoundary fixture(FontHostBoundary::Operation::Coverage);
	REQUIRE(fixture.Run());
	const auto prior = fixture.Observation;
	const auto hash = fixture.Verify();
	fixture.Request.ResolvedPath += ".ungranted";
	CHECK_FALSE(fixture.Run());
	CHECK(fixture.Failure.find("exact content grant") != std::string::npos);
	CHECK(fixture.Observation == prior);
	fixture.Request.ResolvedPath = fixture.File.Path.string();
	fixture.Policy.Allow(engine::assets::FormOfName(fixture.File.Path.string()), false);
	REQUIRE(fixture.Owner.Replace(
		fixture.Configuration, fixture.Policy, Limits::MaximumEvaluationBytes, fixture.DiagnosticValue
	));
	CHECK_FALSE(fixture.Run());
	CHECK(fixture.Observation == prior);
	fixture.Policy.Allow(engine::assets::FormOfName(fixture.File.Path.string()), true);
	REQUIRE(fixture.Owner.Replace(
		fixture.Configuration, fixture.Policy, Limits::MaximumEvaluationBytes, fixture.DiagnosticValue
	));
	REQUIRE(fixture.Run());
	CHECK(fixture.Verify() == hash);
}
