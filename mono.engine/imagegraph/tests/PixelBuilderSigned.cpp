#include "../src/PixelBuilderPayload.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.pixel_builder_signed")
using namespace engine::imagegraph;

TEST_CASE(
	"Pixel Builder signed replay keeps raw dimensions and clamps only surface allocation",
	"[imagegraph][source_2d]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"builder",
		 "pc.pixel_builder",
		 "",
		 {},
		 {{"dimension", Vector2{1, 1}}, {"dimension_unit", EnumValue{0}}}}
	};
	const auto *entry = FindCatalogueEntry("pc.pixel_builder");
	REQUIRE(entry);
	const auto executor = engine::imagegraph::detail::FindExecutor("pc.pixel_builder");
	REQUIRE(executor);
	EvaluationRequest request;
	engine::imagegraph::detail::NodeContext context(document.Nodes.front(), *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.EvaluationDocument = &document;
	context.PixelBuilderCanvas = Vector2{1, 1};
	REQUIRE(executor(context));
	const auto &recipe = std::get<DynamicSurfaceValue>(context.OutputValues.front().Data);
	Diagnostic diagnostic;
	Image output;
	const auto negative =
		engine::imagegraph::detail::RasterizePixelBuilder(recipe, {-2, 3}, output, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(negative == Status::Ok);
	CHECK(output.Width == 1);
	CHECK(output.Height == 3);
	const auto zero = engine::imagegraph::detail::RasterizePixelBuilder(recipe, {0, 0}, output, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(zero == Status::Ok);
	CHECK(output.Width == 1);
	CHECK(output.Height == 1);
	CHECK(recipe.Data->BaseDimension == Vector2{1, 1});
}
