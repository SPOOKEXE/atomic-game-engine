#include <engine/imagegraph/Catalogue.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

TEST_SUITE_ID("engine.imagegraph.catalogue_color_depth")
using namespace engine::imagegraph;

TEST_CASE(
	"Source processor Color Depth is an attribute rather than a foreign input slot",
	"[imagegraph][color_depth]"
) {
	for (const auto &[source, defaultDepth] :
		 {std::pair{"Node_Solid", int64_t{1}}, std::pair{"Node_Invert", int64_t{0}}}) {
		const auto *entry = FindCatalogueSource(source);
		REQUIRE(entry);
		const auto *depth = FindCatalogueInput(*entry, "attribute_color_depth");
		REQUIRE(depth);
		CHECK(std::count_if(entry->Inputs.begin(), entry->Inputs.end(), [](const auto &input) {
			return input.Id == "attribute_color_depth";
		}) == 1);
		CHECK(depth->SourceIndex == -1);
		CHECK(depth->SourceKind == "Attribute");
		CHECK(depth->Name == "Color Depth");
		CHECK(depth->Type == ValueType::Enum);
		CHECK(CatalogueDefault(*depth) == std::optional<Value>{EnumValue{defaultDepth}});
		CHECK(CatalogueChoiceCount(*depth) == 9);
		CHECK(depth->Choices.starts_with("Input;Inherited;"));

		CHECK(depth->Choices.ends_with("32 bit Greyscale"));
		const auto property = std::find_if(
			entry->Schema.Properties.begin(), entry->Schema.Properties.end(), [](const auto &value) {
				return value.Id == "attribute_color_depth";
			}
		);
		REQUIRE(property != entry->Schema.Properties.end());
		CHECK(property->Type == ValueType::Enum);
	}
	const auto *number = FindCatalogueSource("Node_Number");
	REQUIRE(number);
	CHECK_FALSE(FindCatalogueInput(*number, "attribute_color_depth"));
}

TEST_CASE(
	"Unresolved source first-junction depth remains without a fabricated default", "[imagegraph][color_depth]"
) {
	const auto *entry = FindCatalogueSource("Node_Warp");
	REQUIRE(entry);
	const auto *depth = FindCatalogueInput(*entry, "attribute_color_depth");
	REQUIRE(depth);
	CHECK(depth->SourceIndex == -1);
	CHECK(depth->Default.empty());
	CHECK_FALSE(CatalogueDefault(*depth));
}
