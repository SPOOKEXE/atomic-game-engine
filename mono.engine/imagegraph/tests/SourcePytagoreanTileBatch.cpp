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

TEST_SUITE_ID("engine.imagegraph.source_pytagorean_tile_batch")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Document PytagoreanGraph() {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {Node{
			"generator",
			"pc.pytagorean_tile",
			"",
			{},
			{{"dimension", Vector2{16, 16}},
			 {"dimension_unit", EnumValue{0}},
			 {"position", Vector2{0.5, 0.5}},
			 {"position_unit", EnumValue{0}},
			 {"scale", Vector2{0.25, 0.25}},
			 {"scale_unit", EnumValue{0}},
			 {"seed", 17.0},
			 {"tile_color", Gradient{0, {{0, Colour{220, 30, 40, 255}}, {1, Colour{20, 80, 230, 255}}}}},
			 {"render_type", EnumValue{0}}}
		}};
		document.Outputs = {{"out", "generator", "surface_out"}};
		return document;
	}
	Status Draw(const Document &document, uint64_t tick, Image &image, Diagnostic &diagnostic) {
		Plan plan;
		const auto compiled = Compile(document, plan, diagnostic);
		if (compiled != Status::Ok) return compiled;
		EvaluationRequest request;
		request.Tick = tick;
		return Evaluate(document, plan, "out", request, image, diagnostic);
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

TEST_CASE(
	"Pytagorean Tile animation and inherited instances survive Format9", "[source_2d][pytagorean_tile]"
) {
	auto document = PytagoreanGraph();
	document.Nodes[0].SourceAnimatedInputs = {"seed"};
	document.Keyframes = {
		{"generator", "seed", 0, 17.0}, {"generator", "seed", 1, 31.0}, {"generator", "seed", 2, 31.0}
	};
	Node instance{"copy", "pc.pytagorean_tile", "", {}, {}};
	instance.InstanceBase = "generator";
	document.Nodes.push_back(std::move(instance));
	document.Outputs.push_back({"copy", "copy", "surface_out"});
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	REQUIRE(restored == document);
	for (const uint64_t tick : {uint64_t{0}, uint64_t{1}, uint64_t{2}}) {
		Image source, inherited;
		REQUIRE(Draw(restored, tick, source, diagnostic) == Status::Ok);
		Plan plan;
		REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
		EvaluationRequest request;
		request.Tick = tick;
		REQUIRE(Evaluate(restored, plan, "copy", request, inherited, diagnostic) == Status::Ok);
		CHECK(inherited == source);
	}
	Image first, animated;
	REQUIRE(Draw(restored, 0, first, diagnostic) == Status::Ok);
	REQUIRE(Draw(restored, 1, animated, diagnostic) == Status::Ok);
	CHECK(first.Pixels != animated.Pixels);
}

TEST_CASE("Pytagorean Tile batch uses all source slots and array schedules", "[source_2d][pytagorean_tile]") {
	const auto *entry = FindCatalogueEntry("pc.pytagorean_tile");
	const auto executor = detail::FindExecutor("pc.pytagorean_tile");
	REQUIRE(entry);
	REQUIRE(executor);
	const std::array<std::string_view, 29> sourceSlots{
		{"dimension",		"position",		  "scale",		  "rotation",		"gap",
		 "tile_color",		"gap_color",	  "render_type",  "seed",			"texture",
		 "anti_aliasing",	"scale_map",	  "rotation_map", "gap_map",		"truchet",
		 "texture_seed",	"flip_threshold", "phase",		  "tile_color_map", "tile_color_map_range",
		 "random_angle",	"level_in",		  "mask",		  "uv_map",			"uv_mix",
		 "random_position", "random_scale",	  "shift",		  "level_out"}
	};
	for (size_t slot = 0; slot < sourceSlots.size(); ++slot) {
		const auto *input = FindCatalogueInput(*entry, sourceSlots[slot]);
		REQUIRE(input);
		CHECK(input->SourceIndex == int64_t(slot));
	}

	for (int64_t mode = 0; mode < 4; ++mode) {
		Node node{"generator", "pc.pytagorean_tile", "", {}, {}};
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		SetDefaults(context);
		SetValue(context, "dimension", ArrayValue{ValueType::Vector2, {Vector2{4, 4}, Vector2{5, 4}}});
		SetValue(context, "dimension_unit", EnumValue{0});
		SetValue(context, "position", Vector2{0.5, 0.5});
		SetValue(context, "position_unit", EnumValue{0});
		SetValue(context, "scale", Vector2{0.25, 0.25});
		SetValue(context, "scale_unit", EnumValue{0});
		SetValue(context, "gap", ArrayValue{ValueType::Scalar, {0.1, 0.4}});
		SetValue(
			context, "tile_color", Gradient{0, {{0, Colour{220, 30, 40, 255}}, {1, Colour{20, 80, 230, 255}}}}
		);
		SetValue(context, "render_type", EnumValue{0});
		SetValue(context, "seed", ArrayValue{ValueType::Scalar, {17.0, 31.0}});
		SetValue(context, "shift", ArrayValue{ValueType::Scalar, {0.0, 0.3}});
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
		if (mode == 2) {
			CHECK(batch.Images[0] != batch.Images[1]);
			CHECK(batch.Images[0] != batch.Images[2]);
		} else if (mode == 3) {
			CHECK(batch.Images[0] == batch.Images[4]);
			CHECK(batch.Images[0] != batch.Images[8]);
		}
		for (size_t row = 0; row < count; ++row) {
			CAPTURE(mode, row);
			size_t dimensionIndex, gapIndex, seedIndex, shiftIndex;
			if (mode == 0) {
				dimensionIndex = gapIndex = seedIndex = shiftIndex = row % 2;
			} else if (mode == 1) {
				dimensionIndex = gapIndex = seedIndex = shiftIndex = std::min(row, size_t{1});
			} else if (mode == 2) {
				dimensionIndex = row / 8;
				gapIndex = row / 4 % 2;
				seedIndex = row / 2 % 2;
				shiftIndex = row % 2;
			} else {
				// Inverse selection uses the full-slot suffix mirror, including singleton slots.
				dimensionIndex = row % 2;
				gapIndex = row / 2 % 2;
				seedIndex = row / 2 % 2;
				shiftIndex = row / 8;
			}
			const auto expected = RunNode(
				"pc.pytagorean_tile",
				{},
				{{"dimension", dimensionIndex == 0 ? Vector2{4, 4} : Vector2{5, 4}},
				 {"dimension_unit", EnumValue{0}},
				 {"position", Vector2{0.5, 0.5}},
				 {"position_unit", EnumValue{0}},
				 {"scale", Vector2{0.25, 0.25}},
				 {"scale_unit", EnumValue{0}},
				 {"gap", gapIndex == 0 ? 0.1 : 0.4},
				 {"tile_color", Gradient{0, {{0, Colour{220, 30, 40, 255}}, {1, Colour{20, 80, 230, 255}}}}},
				 {"render_type", EnumValue{0}},
				 {"seed", seedIndex == 0 ? 17.0 : 31.0},
				 {"shift", shiftIndex == 0 ? 0.0 : 0.3}}
			);
			INFO(expected.Message);
			REQUIRE(expected.Ok);
			CHECK(batch.Images[row] == expected.Output());
		}
	}
}

TEST_CASE(
	"Pytagorean Tile preflights cumulative shader work before publishing rows", "[source_2d][pytagorean_tile]"
) {
	const auto *entry = FindCatalogueEntry("pc.pytagorean_tile");
	const auto executor = detail::FindExecutor("pc.pytagorean_tile");
	REQUIRE(entry);
	REQUIRE(executor);
	Node node{"generator", "pc.pytagorean_tile", "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	SetDefaults(context);
	std::vector<ElementValue> dimensions(512, Vector2{4, 4});
	SetValue(context, "dimension", ArrayValue{ValueType::Vector2, std::move(dimensions)});
	SetValue(context, "dimension_unit", EnumValue{0});
	SetValue(context, "position", Vector2{0.5, 0.5});
	SetValue(context, "position_unit", EnumValue{0});
	SetValue(context, "scale", Vector2{0.25, 0.25});
	SetValue(context, "scale_unit", EnumValue{0});
	SetValue(context, "gap", 0.25);
	SetValue(context, "render_type", EnumValue{0});
	SetValue(context, "seed", 17.0);
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

TEST_CASE(
	"Pytagorean Tile refuses batch workspace over its byte budget before observation",
	"[source_2d][pytagorean_tile]"
) {
	const auto *entry = FindCatalogueEntry("pc.pytagorean_tile");
	const auto executor = detail::FindExecutor("pc.pytagorean_tile");
	REQUIRE(entry);
	REQUIRE(executor);
	Node node{"generator", "pc.pytagorean_tile", "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = 1;
	SetDefaults(context);
	SetValue(context, "attribute_array_process", EnumValue{0});
	context.InputProvenanceResolved = true;
	for (const auto &[port, value] : context.Values)
		context.ValueViews.emplace_back(port, &value);
	size_t observed = 0;
	const bool ran = detail::RunProcessorBatch(context, executor, Observe, &observed);
	CHECK_FALSE(ran);
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.FailurePort == "attribute_array_process");
	CHECK(observed == 0);
	CHECK(context.OutputImages.empty());
	CHECK(context.OutputImageArrays.empty());
}
