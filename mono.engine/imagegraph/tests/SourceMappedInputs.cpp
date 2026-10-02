#include "../src/SourceMappedInputs.hpp"

#include "../src/SourceGetterProjection.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.source_mapped_inputs")
using namespace engine::imagegraph;
TEST_CASE(
	"Mapped Bevel height projects its physical Int getter over both endpoints", "[source_mapped_inputs]"
) {
	const auto *entry = FindCatalogueEntry("pc.bevel");
	REQUIRE(entry);
	const auto *range = FindCatalogueInput(*entry, "height_map_range");
	REQUIRE(range);
	CHECK(CatalogueDefault(*range) == Value{Vector2{0, 4}});
	CHECK(detail::SourceMappedSynthetic(*entry, *range));
	Node node{"bevel", "pc.bevel", "", {}, {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	const Value height = int64_t{4}, mapped = true, endpoints = Vector2{.5, 2.5};
	context.ValueViews = {{"height", &height}, {"height_mapped", &mapped}, {"height_map_range", &endpoints}};
	{
		detail::SourceGetterProjection getter(context);
		REQUIRE(getter.Prepare());
		REQUIRE(context.Find("height"));
		CHECK(*context.Find("height") == Value{ArrayValue{ValueType::Integer, {int64_t{0}, int64_t{2}}}});
		Vector2 actual;
		REQUIRE(detail::ReadSourceMappedRange(context, "height", actual));
		CHECK(actual == Vector2{0, 2});
	}
	CHECK(context.Find("height") == &height);
	CHECK(context.Find("height_map_range") == &endpoints);
	const Value disabled = false;
	context.ValueViews.emplace_back("height_mapped", &disabled);
	detail::SourceGetterProjection getter(context);
	REQUIRE(getter.Prepare());
	CHECK(context.Find("height") == &height);
}
