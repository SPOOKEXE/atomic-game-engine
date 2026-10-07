#include "fixtures/SourcePixelKernels.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

TEST_SUITE_ID("engine.imagegraph.source_pixel_kernel_workloads")
TEST_CASE(
	"Pixel kernel benchmark graphs preserve analytical fields and bounded publication",
	"[imagegraph][pixel_kernel_workloads]"
) {
	using Fixture = engine::imagegraph::testing::SourcePixelKernelFixture;
	const auto kind = GENERATE(
		Fixture::Kind::Atlas16,
		Fixture::Kind::Atlas64,
		Fixture::Kind::PaletteRGB,
		Fixture::Kind::PaletteReverse,
		Fixture::Kind::EdgeSobel,
		Fixture::Kind::EdgeLaplacian,
		Fixture::Kind::Bokeh8,
		Fixture::Kind::Bokeh32,
		Fixture::Kind::FoldGreyscale,
		Fixture::Kind::FoldMap,
		Fixture::Kind::GaussianRandom,
		Fixture::Kind::GaussianConversion,
		Fixture::Kind::AnisoBlend,
		Fixture::Kind::AnisoMapped,
		Fixture::Kind::RasterRGB
	);
	INFO("workload=" << int(kind));
	Fixture fixture(kind);
	fixture.Run();
	CHECK(fixture.Verify() == fixture.ExpectedHash);
	fixture.CheckBudget();
}
