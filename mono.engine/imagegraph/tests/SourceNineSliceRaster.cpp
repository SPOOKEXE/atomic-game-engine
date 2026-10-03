#include "../src/SourceNineSlice.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.source_nine_slice_raster")
using namespace engine::imagegraph;
TEST_CASE(
	"Nine Slice fixed corners surround scaled center with source labelled regions",
	"[imagegraph][source_2d][source_nine_slice]"
) {
	SourceNineSliceRecipe recipe;
	recipe.Source = {3, 3, std::vector<uint8_t>(3 * 3 * 4), 0};
	recipe.Splice = {1, 1, 1, 1};
	for (size_t i = 0; i < 9; ++i) {
		recipe.Source.Pixels[i * 4] = uint8_t((i + 1) * 20);
		recipe.Source.Pixels[i * 4 + 3] = 255;
	}
	const auto *entry = FindCatalogueEntry("pc.9_slice");
	REQUIRE(entry);
	Node node{"nine", "pc.9_slice", "", {}, {}};
	EvaluationRequest request;
	engine::imagegraph::detail::NodeContext context(node, *entry, request);
	for (int64_t filling : {int64_t{0}, int64_t{1}}) {
		recipe.FillingMode = filling;
		Image target{5, 4, std::vector<uint8_t>(5 * 4 * 4), 0};
		REQUIRE(
			engine::imagegraph::detail::StageSourceNineSlice(context, recipe, {5, 4}, target, {1, 1, 1, 1})
		);
		const std::vector<uint8_t> expected{20, 40,	 40,  40,  60,	80,	 100, 100, 100, 120,
											80, 100, 100, 100, 120, 140, 160, 160, 160, 180};
		for (size_t i = 0; i < expected.size(); ++i)
			CHECK(target.Pixels[i * 4] == expected[i]);
	}
}
TEST_CASE(
	"Nine Slice tint is quantized in internal staging before Normal outer draw",
	"[imagegraph][source_2d][source_nine_slice]"
) {
	SourceNineSliceRecipe recipe;
	recipe.Source = {1, 1, {255, 128, 64, 128}, 0};
	recipe.Splice = {};
	const auto *entry = FindCatalogueEntry("pc.9_slice");
	REQUIRE(entry);
	Node node{"nine", "pc.9_slice", "", {}, {}};
	EvaluationRequest request;
	engine::imagegraph::detail::NodeContext context(node, *entry, request);
	Image target{2, 2, std::vector<uint8_t>(2 * 2 * 4), 0};
	REQUIRE(
		engine::imagegraph::detail::StageSourceNineSlice(
			context, recipe, {2, 2}, target, {128 / 255., 1, 1, 128 / 255.}
		)
	);
	CHECK(target.Pixels[0] == 128);
	CHECK(target.Pixels[1] == 128);
	CHECK(target.Pixels[2] == 64);
	CHECK(target.Pixels[3] == 64);
}
TEST_CASE(
	"Nine Slice Repeat clips the final tile while Scale stretches the same source center",
	"[imagegraph][source_2d][source_nine_slice]"
) {
	SourceNineSliceRecipe recipe;
	recipe.Source = {5, 3, std::vector<uint8_t>(5 * 3 * 4), 0};
	recipe.Splice = {1, 1, 1, 1};
	for (size_t y = 0; y < 3; ++y)
		for (size_t x = 0; x < 5; ++x) {
			recipe.Source.Pixels[(y * 5 + x) * 4] = uint8_t((x + 1) * 20);
			recipe.Source.Pixels[(y * 5 + x) * 4 + 3] = 255;
		}
	const auto *entry = FindCatalogueEntry("pc.9_slice");
	REQUIRE(entry);
	Node node{"nine", "pc.9_slice", "", {}, {}};
	EvaluationRequest request;
	engine::imagegraph::detail::NodeContext context(node, *entry, request);
	for (int64_t mode : {int64_t{0}, int64_t{1}}) {
		recipe.FillingMode = mode;
		Image target{7, 3, std::vector<uint8_t>(7 * 3 * 4), 0};
		REQUIRE(
			engine::imagegraph::detail::StageSourceNineSlice(context, recipe, {7, 3}, target, {1, 1, 1, 1})
		);
		const std::array<uint8_t, 7> expected = mode == 0
													? std::array<uint8_t, 7>{20, 40, 40, 60, 80, 80, 100}
													: std::array<uint8_t, 7>{20, 40, 60, 80, 40, 60, 100};
		for (size_t x = 0; x < 7; ++x)
			CHECK(target.Pixels[(7 + x) * 4] == expected[x]);
	}
}
