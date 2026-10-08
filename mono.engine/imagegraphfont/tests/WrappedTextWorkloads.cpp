#include "fixtures/WrappedTextBoundary.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraphfont.wrapped_text_workloads")
TEST_CASE(
	"Profiled native wrapped Text uses decoded advances and preserves results on refusal",
	"[wrapped_text_workloads]"
) {
	using engine::imagegraphfont::testing::WrappedTextBoundary;
	for (const auto kind :
		 {WrappedTextBoundary::Operation::Paragraph,
		  WrappedTextBoundary::Operation::Spaces,
		  WrappedTextBoundary::Operation::Batch,
		  WrappedTextBoundary::Operation::TrimmedText}) {
		DYNAMIC_SECTION("operation " << static_cast<int>(kind)) {
			WrappedTextBoundary fixture(kind);
			REQUIRE(fixture.Run());
			const auto hash = fixture.Verify();
			const auto priorMeasurements = fixture.Measured;
			const auto priorPixels = fixture.Host.Output;
			CHECK_FALSE(fixture.Run(1));
			CHECK(fixture.Measured == priorMeasurements);
			CHECK(fixture.Host.Output == priorPixels);
			CHECK(fixture.Verify() == hash);
			REQUIRE(fixture.Run());
			CHECK(fixture.Verify() == hash);
		}
	}
}
