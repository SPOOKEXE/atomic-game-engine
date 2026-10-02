#include "../src/nodes/Processor.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.source_cos_gradient")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
TEST_CASE("Cos Gradient keeps each source geometric progress", "[imagegraph][source_2d]") {
	const std::array<double, 4> firstProgress{.25, .5, .625, 1 / std::sqrt(2.)};
	for (int64_t type = 0; type < 4; ++type) {
		const auto run = RunNode(
			"pc.gradient_cos",
			{},
			{{"dimension", Vector2{2, 2}},
			 {"dimension_unit", EnumValue{0}},
			 {"type", EnumValue{type}},
			 {"a", Vector3{.5, .5, .5}},
			 {"b", Vector3{.5, .5, .5}},
			 {"c", Vector3{1, 1, 1}},
			 {"d", Vector3{0, 0, 0}}}
		);
		INFO(type);
		INFO(run.Message);
		REQUIRE(run.Ok);
		const auto expected = detail::Quantize(.5 + .5 * std::cos(6.28318 * firstProgress[type]));
		CHECK(run.Output().Pixels[0] == expected);
		CHECK(run.Output().Pixels[3] == 255);
	}
}
TEST_CASE("Cos Gradient preserves its shared mapped endpoint and unused mask", "[imagegraph][source_2d]") {
	const Image white = imagegraph_test::MakeImage(1, 1, {255, 255, 255, 255});
	const Image black = imagegraph_test::MakeImage(1, 1, {0, 0, 0, 0});
	const auto run = RunNode(
		"pc.gradient_cos",
		{{"b_map", &white}, {"mask", &black}},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"a", Vector3{.2, .2, .2}},
		 {"a_max", Vector3{.3, .3, .3}},
		 {"b", Vector3{0, 0, 0}},
		 {"b_max", Vector3{.9, .9, .9}},
		 {"b_mapped", true},
		 {"c", Vector3{0, 0, 0}},
		 {"d", Vector3{0, 0, 0}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{128, 128, 128, 255});
}
TEST_CASE("Cos Gradient UV alpha survives a zero UV mix", "[imagegraph][source_2d]") {
	const Image uv = imagegraph_test::MakeImage(1, 1, {255, 0, 0, 64});
	const auto run = RunNode(
		"pc.gradient_cos",
		{{"uv_map", &uv}},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"uv_mix", 0.},
		 {"a", Vector3{.25, .5, .75}},
		 {"b", Vector3{0, 0, 0}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{64, 128, 191, 64});
}
TEST_CASE("Cos Gradient refuses undefined geometry and enabled maps", "[imagegraph][source_2d]") {
	const auto zero = RunNode(
		"pc.gradient_cos", {}, {{"dimension", Vector2{1, 1}}, {"dimension_unit", EnumValue{0}}, {"scale", 0.}}
	);
	CHECK(zero.Code == Status::UnsupportedExecution);
	const auto noMap = RunNode(
		"pc.gradient_cos",
		{},
		{{"dimension", Vector2{1, 1}}, {"dimension_unit", EnumValue{0}}, {"d_mapped", true}}
	);
	CHECK(noMap.Code == Status::InvalidValue);
}

TEST_CASE(
	"Cos Gradient submits the progress remap curve and validates uniform bounds", "[imagegraph][source_2d]"
) {
	Curve constant;
	constant.Header = {0, 1, 0, 0, 0, 1};
	constant.Anchors = {{0, 0, 0, .25, 0, 0}, {0, 0, 1, .25, 0, 0}};
	const auto run = RunNode(
		"pc.gradient_cos",
		{},
		{{"dimension", Vector2{2, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"progress_remap", constant},
		 {"a", Vector3{.5, .5, .5}},
		 {"b", Vector3{.5, .5, .5}},
		 {"c", Vector3{1, 1, 1}},
		 {"d", Vector3{0, 0, 0}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels[0] == 128);
	CHECK(run.Output().Pixels[4] == 128);
	constant.Anchors.resize(10, constant.Anchors.back());
	const auto oversized = RunNode(
		"pc.gradient_cos",
		{},
		{{"dimension", Vector2{1, 1}}, {"dimension_unit", EnumValue{0}}, {"progress_remap", constant}}
	);
	CHECK(oversized.Code == Status::UnsupportedExecution);
}
TEST_CASE(
	"Cos Gradient graph publishes owned pixels and preserves them under byte refusal",
	"[imagegraph][source_2d]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"cos",
		 "pc.gradient_cos",
		 "",
		 {},
		 {{"dimension", Vector2{2, 1}},
		  {"dimension_unit", EnumValue{0}},
		  {"a", Vector3{.25, .5, .75}},
		  {"b", Vector3{0, 0, 0}}}}
	};
	document.Outputs = {{"colour", "cos", "surface_out"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image result;
	const auto status = Evaluate(document, plan, "colour", {}, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	CHECK(result.Pixels == std::vector<uint8_t>{64, 128, 191, 255, 64, 128, 191, 255});
	const Image previous = result;
	CHECK(Evaluate(document, plan, "colour", {}, result, diagnostic, 1) == Status::LimitExceeded);
	CHECK(result.Pixels == previous.Pixels);
	CHECK(result.Width == previous.Width);
	CHECK(result.Height == previous.Height);
}

TEST_CASE("Cos Gradient retains complete pinned choices and widget clamping", "[imagegraph][source_2d]") {
	const auto *entry = FindCatalogueEntry("pc.gradient_cos");
	REQUIRE(entry);
	const auto *type = FindCatalogueInput(*entry, "type");
	REQUIRE(type);
	REQUIRE(type->SourceBehavior);
	CHECK(type->SourceBehavior->ChoiceClamp == SourceChoiceClamp::Default);
	REQUIRE(type->SourceChoices);
	CHECK(type->SourceChoices->Status == SourceChoicesStatus::Resolved);
	CHECK(type->SourceChoices->RawCount == 4);
	const std::array<std::string_view, 4> labels{"Linear", "Circular", "Radial", "Diamond"};
	REQUIRE(type->SourceChoices->Entries.size() == labels.size());
	for (size_t i = 0; i < labels.size(); ++i) {
		CHECK(type->SourceChoices->Entries[i].SourceIndex == int32_t(i));
		CHECK(type->SourceChoices->Entries[i].Label == labels[i]);
		CHECK_FALSE(type->SourceChoices->Entries[i].Separator);
	}
	for (const auto &[requested, bounded] : std::array<std::pair<int64_t, int64_t>, 2>{{{-4, 0}, {9, 3}}}) {
		const auto clamped = RunNode(
			"pc.gradient_cos",
			{},
			{{"dimension", Vector2{2, 2}}, {"dimension_unit", EnumValue{0}}, {"type", EnumValue{requested}}}
		);
		const auto reference = RunNode(
			"pc.gradient_cos",
			{},
			{{"dimension", Vector2{2, 2}}, {"dimension_unit", EnumValue{0}}, {"type", EnumValue{bounded}}}
		);
		INFO(clamped.Message);
		REQUIRE(clamped.Ok);
		REQUIRE(reference.Ok);
		CHECK(clamped.Output().Pixels == reference.Output().Pixels);
	}
}
