#include "../src/nodes/SourceFlipMask.hpp"
#include "FluidPayload.hpp"

#include <engine/imagegraph/FlipReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.flip_mask")
using namespace engine::imagegraph;
TEST_CASE(
	"FLIP solid shader reference preserves point-filter dilation and "
	"column-major solid indexing",
	"[imagegraph]"
) {
	Node node{"solid", "pc.flip_solid", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	Image image;
	image.Width = 11;
	image.Height = 11;
	image.Pixels.resize(11 * 11 * 4);
	REQUIRE(StoreSurfacePixel(image, 3, 6, {1, 1, 1, 1}));
	Image mask;
	REQUIRE(detail::BuildSourceFlipMask(context, image, 11, 11, .1, 1, mask));
	for (uint32_t y = 0; y < 11; ++y)
		for (uint32_t x = 0; x < 11; ++x)
			CHECK(mask.Pixels[size_t(y) * 11 + x] == uint8_t(x >= 2 && x <= 4 && y >= 5 && y <= 7 ? 255 : 0));
	FluidDomainSettings settings;
	settings.Width = 20;
	settings.Height = 20;
	settings.Spacing = 2;
	FluidDomainValue domain;
	Diagnostic diagnostic;
	REQUIRE(
		ResetFlipReplay(settings, 0, 0, Limits::MaximumEvaluationBytes, domain, diagnostic) == Status::Ok
	);
	const auto layout = detail::FluidDomainLayout(settings);
	REQUIRE(layout);
	REQUIRE(layout->Columns == 11);
	REQUIRE(layout->Rows == 11);
	detail::ApplySourceFlipSolidMask(*domain.Data, mask, 11, 11);
	const auto &solid = domain.Data->Buffers[size_t(FluidBuffer::Solid)];
	CHECK(solid[3 * 11 + 6] == 1);
	CHECK(solid[6 * 11 + 3] == 0);
	REQUIRE(detail::BuildSourceFlipMask(context, image, 11, 11, .1, -2, mask));
	for (uint32_t y = 0; y < 11; ++y)
		for (uint32_t x = 0; x < 11; ++x)
			CHECK(mask.Pixels[size_t(y) * 11 + x] == uint8_t(x == 3 && y == 6 ? 255 : 0));
	// step(threshold,w) compares source float uniforms, including equal values.
	image.Pixels.assign(image.Pixels.size(), 0);
	REQUIRE(detail::BuildSourceFlipMask(context, image, 11, 11, 0, 0, mask));
	for (const auto pixel : mask.Pixels)
		CHECK(pixel == 255);
}
