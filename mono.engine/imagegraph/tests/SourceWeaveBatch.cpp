#include "../src/ProcessorBatch.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.source_weave_batch")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Document WeaveGraph() {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {Node{
			"generator",
			"pc.weave",
			"",
			{},
			{{"dimension", Vector2{8, 8}},
			 {"dimension_unit", EnumValue{0}},
			 {"position", Vector2{0.5, 0.5}},
			 {"position_unit", EnumValue{0}},
			 {"scale", Vector2{1.75, 1.5}},
			 {"scale_unit", EnumValue{0}},
			 {"seed", 17.0},
			 {"weave_pattern", EnumValue{0}},
			 {"width", Vector2{0.8, 0.8}},
			 {"color_type", EnumValue{1}},
			 {"color", Colour{255, 32, 16, 255}},
			 {"color_2", Colour{16, 32, 255, 255}},
			 {"shading_curved", true}}
		}};
		document.Outputs = {{"out", "generator", "surface_out"}};
		return document;
	}
	Status Draw(
		const Document &document, std::string_view output, uint64_t tick, Image &image, Diagnostic &diagnostic
	) {
		Plan plan;
		const auto compiled = Compile(document, plan, diagnostic);
		if (compiled != Status::Ok) return compiled;
		EvaluationRequest request;
		request.Tick = tick;
		return Evaluate(document, plan, std::string(output), request, image, diagnostic);
	}
	void SetDefaults(detail::NodeContext &context) {
		for (const auto &input : context.Entry.Inputs)
			if (const auto value = CatalogueDefault(input)) context.Values.emplace_back(input.Id, *value);
	}
	void SetValue(detail::NodeContext &context, std::string_view port, Value value) {
		for (auto &[id, current] : context.Values)
			if (id == port) {
				current = std::move(value);
				return;
			}
		context.Values.emplace_back(port, std::move(value));
	}
	bool Observe(detail::NodeContext &, void *state) {
		++*static_cast<size_t *>(state);
		return true;
	}
}

TEST_CASE("Weave animation and inherited instances survive Format9", "[source_2d][weave]") {
	auto document = WeaveGraph();
	document.Nodes[0].SourceAnimatedInputs = {"seed"};
	document.Keyframes = {
		{"generator", "seed", 0, 17.0}, {"generator", "seed", 1, 31.0}, {"generator", "seed", 2, 31.0}
	};
	Node instance{"copy", "pc.weave", "", {}, {}};
	instance.InstanceBase = "generator";
	document.Nodes.push_back(std::move(instance));
	document.Outputs.push_back({"copy", "copy", "surface_out"});
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	REQUIRE(restored == document);
	for (const uint64_t tick : {uint64_t{0}, uint64_t{1}, uint64_t{2}}) {
		Image source, inherited;
		REQUIRE(Draw(restored, "out", tick, source, diagnostic) == Status::Ok);
		REQUIRE(Draw(restored, "copy", tick, inherited, diagnostic) == Status::Ok);
		CHECK(inherited == source);
	}
	Image first, animated;
	REQUIRE(Draw(restored, "out", 0, first, diagnostic) == Status::Ok);
	REQUIRE(Draw(restored, "out", 1, animated, diagnostic) == Status::Ok);
	CHECK(first.Pixels != animated.Pixels);
}

TEST_CASE("Weave array schedules select source slot rows", "[source_2d][weave]") {
	const auto *entry = FindCatalogueEntry("pc.weave");
	const auto executor = detail::FindExecutor("pc.weave");
	REQUIRE(entry);
	REQUIRE(executor);
	const std::array<std::string_view, 21> sourceSlots{
		{"dimension",	  "uv_map",		"uv_mix",	  "mask",	 "position",	 "scale",
		 "angle",		  "bg_color",	"color",	  "seed",	 "width",		 "shading",
		 "shading_curve", "shade_span", "color_type", "color_2", "random_color", "shade_color",
		 "weave_pattern", "weave_map",	"shift"}
	};
	for (size_t slot = 0; slot < sourceSlots.size(); ++slot) {
		const auto *input = FindCatalogueInput(*entry, sourceSlots[slot]);
		REQUIRE(input);
		CHECK(input->SourceIndex == int64_t(slot));
	}
	for (int64_t mode = 0; mode < 4; ++mode) {
		Node node{"generator", "pc.weave", "", {}, {}};
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		SetDefaults(context);
		SetValue(context, "dimension", ArrayValue{ValueType::Vector2, {Vector2{4, 4}, Vector2{5, 4}}});
		SetValue(context, "dimension_unit", EnumValue{0});
		SetValue(context, "uv_mix", 1.0);
		SetValue(context, "position", Vector2{0.5, 0.5});
		SetValue(context, "position_unit", EnumValue{0});
		SetValue(context, "scale", Vector2{1.75, 1.5});
		SetValue(context, "scale_unit", EnumValue{0});
		SetValue(context, "angle", 0.0);
		SetValue(context, "width", ArrayValue{ValueType::Vector2, {Vector2{0.4, 0.4}, Vector2{0.6, 0.6}}});
		SetValue(context, "seed", ArrayValue{ValueType::Scalar, {17.0, 31.0}});
		SetValue(context, "weave_pattern", EnumValue{0});
		SetValue(context, "color_type", EnumValue{2});
		SetValue(context, "shift", ArrayValue{ValueType::Scalar, {0.0, 0.25}});
		SetValue(context, "shading_curved", true);
		SetValue(context, "attribute_array_process", EnumValue{mode});
		context.InputProvenanceResolved = true;
		for (const auto &[port, value] : context.Values)
			context.ValueViews.emplace_back(port, &value);
		size_t observed = 0;
		const bool ran = detail::RunProcessorBatch(context, executor, Observe, &observed);
		INFO(mode << ":" << context.FailurePort << ":" << context.FailureMessage);
		REQUIRE(ran);
		REQUIRE(context.OutputImageArrays.size() == 1);
		const auto &batch = context.OutputImageArrays.front().second;
		const size_t count = mode < 2 ? 2 : 16;
		REQUIRE(batch.Images.size() == count);
		CHECK(observed == count);
		for (size_t row = 0; row < count; ++row) {
			CAPTURE(mode, row);
			size_t dimensionIndex, seedIndex, widthIndex, shiftIndex;
			if (mode == 0) {
				dimensionIndex = seedIndex = widthIndex = shiftIndex = row % 2;
			} else if (mode == 1) {
				dimensionIndex = seedIndex = widthIndex = shiftIndex = std::min(row, size_t{1});
			} else if (mode == 2) {
				dimensionIndex = row / 8;
				seedIndex = row / 4 % 2;
				widthIndex = row / 2 % 2;
				shiftIndex = row % 2;
			} else {
				// Inverse selection uses the 21 physical source slots, including singleton slots.
				dimensionIndex = row % 2;
				seedIndex = widthIndex = row / 2 % 2;
				shiftIndex = row / 8;
			}
			const auto expected = RunNode(
				"pc.weave",
				{},
				{{"dimension", dimensionIndex == 0 ? Vector2{4, 4} : Vector2{5, 4}},
				 {"dimension_unit", EnumValue{0}},
				 {"uv_mix", 1.0},
				 {"position", Vector2{0.5, 0.5}},
				 {"position_unit", EnumValue{0}},
				 {"scale", Vector2{1.75, 1.5}},
				 {"scale_unit", EnumValue{0}},
				 {"angle", 0.0},
				 {"width", widthIndex == 0 ? Vector2{0.4, 0.4} : Vector2{0.6, 0.6}},
				 {"seed", seedIndex == 0 ? 17.0 : 31.0},
				 {"weave_pattern", EnumValue{0}},
				 {"color_type", EnumValue{2}},
				 {"shift", shiftIndex == 0 ? 0.0 : 0.25},
				 {"shading_curved", true}}
			);
			INFO(expected.Message);
			REQUIRE(expected.Ok);
			CHECK(batch.Images[row] == expected.Output());
		}
	}
}

TEST_CASE("Weave admits cumulative shader work before publishing rows", "[source_2d][weave]") {
	const auto *entry = FindCatalogueEntry("pc.weave");
	const auto executor = detail::FindExecutor("pc.weave");
	REQUIRE(entry);
	REQUIRE(executor);
	Node node{"generator", "pc.weave", "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	SetDefaults(context);
	std::vector<ElementValue> dimensions(512, Vector2{4, 4});
	SetValue(context, "dimension", ArrayValue{ValueType::Vector2, std::move(dimensions)});
	SetValue(context, "dimension_unit", EnumValue{0});
	SetValue(context, "position", Vector2{0.5, 0.5});
	SetValue(context, "position_unit", EnumValue{0});
	SetValue(context, "scale", Vector2{1.75, 1.5});
	SetValue(context, "scale_unit", EnumValue{0});
	SetValue(context, "width", Vector2{2, 2});
	SetValue(context, "weave_pattern", EnumValue{0});
	SetValue(context, "color_type", EnumValue{1});
	SetValue(context, "seed", 17.0);
	SetValue(context, "shading_curved", true);
	SetValue(context, "attribute_array_process", EnumValue{0});
	context.InputProvenanceResolved = true;
	for (const auto &[port, value] : context.Values)
		context.ValueViews.emplace_back(port, &value);
	size_t observed = 0;
	const bool ran = detail::RunProcessorBatch(context, executor, Observe, &observed);
	INFO(context.FailurePort << ": " << context.FailureMessage);
	CHECK_FALSE(ran);
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.FailurePort == "surface_out");
	CHECK(observed == 0);
	CHECK(context.OutputImages.empty());
	CHECK(context.OutputImageArrays.empty());
}
