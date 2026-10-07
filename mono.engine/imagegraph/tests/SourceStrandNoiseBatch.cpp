#include "../src/ProcessorBatch.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.source_strand_noise_batch")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Document StrandNoiseGraph() {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {Node{
			"generator",
			"pc.noise_strand",
			"",
			{},
			{{"dimension", Vector2{4, 3}},
			 {"dimension_unit", EnumValue{0}},
			 {"seed", 17.0},
			 {"thickness", .125}}
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

TEST_CASE("Strand noise animation and inherited instances survive Format9", "[source_2d][strand_noise]") {
	auto document = StrandNoiseGraph();
	document.Nodes[0].SourceAnimatedInputs = {"seed", "curve_shift"};
	document.Nodes[0].Values.push_back({"curve", Vector2{.2, .8}});
	document.Keyframes = {
		{"generator", "seed", 0, 17.0},
		{"generator", "seed", 1, 31.0},
		{"generator", "seed", 2, 31.0},
		{"generator", "curve_shift", 0, 0.0},
		{"generator", "curve_shift", 1, .25}
	};
	Node instance{"copy", "pc.noise_strand", "", {}, {}};
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

TEST_CASE("Strand noise array schedules select source slot rows", "[source_2d][strand_noise]") {
	const auto *entry = FindCatalogueEntry("pc.noise_strand");
	const auto executor = detail::FindExecutor("pc.noise_strand");
	REQUIRE(entry);
	REQUIRE(executor);
	const std::array<std::string_view, 17> sourceSlots{
		{"dimension",
		 "position",
		 "density",
		 "seed",
		 "slope",
		 "curve",
		 "curve_scale",
		 "thickness",
		 "curve_shift",
		 "axis",
		 "mode",
		 "opacity",
		 "mask",
		 "uv_map",
		 "uv_mix",
		 "level_in",
		 "level_out"}
	};
	for (size_t slot = 0; slot < sourceSlots.size(); ++slot) {
		const auto *input = FindCatalogueInput(*entry, sourceSlots[slot]);
		REQUIRE(input);
		CHECK(input->SourceIndex == int64_t(slot));
	}
	for (int64_t mode = 0; mode < 4; ++mode) {
		Node node{"generator", "pc.noise_strand", "", {}, {{"seed", 17.0}}};
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		SetDefaults(context);
		SetValue(context, "dimension", Vector2{4, 3});
		SetValue(context, "dimension_unit", EnumValue{0});
		SetValue(context, "position", Vector2{0, 0});
		SetValue(context, "position_unit", EnumValue{0});
		SetValue(context, "density", ArrayValue{ValueType::Scalar, {.25, .5}});
		SetValue(context, "seed", ArrayValue{ValueType::Scalar, {17.0, 31.0}});
		SetValue(context, "slope", .5);
		SetValue(context, "curve", Vector2{.2, .8});
		SetValue(context, "curve_scale", 2.0);
		SetValue(context, "curve_shift", ArrayValue{ValueType::Scalar, {0.0, .25}});
		SetValue(context, "axis", EnumValue{0});
		SetValue(context, "mode", EnumValue{0});
		SetValue(context, "thickness", .05);
		SetValue(context, "opacity", Vector2{.4, .9});
		SetValue(context, "uv_mix", 1.0);
		SetValue(context, "level_in", Vector2{0, 1});
		SetValue(context, "level_out", Vector2{0, 1});
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
		const size_t count = mode < 2 ? 2 : 8;
		REQUIRE(batch.Images.size() == count);
		CHECK(observed == count);
		for (size_t row = 0; row < count; ++row) {
			CAPTURE(mode, row);
			size_t densityIndex, seedIndex, curveShiftIndex;
			if (mode < 2) {
				densityIndex = seedIndex = curveShiftIndex = row % 2;
			} else if (mode == 2) {
				densityIndex = row / 4;
				seedIndex = row / 2 % 2;
				curveShiftIndex = row % 2;
			} else {
				densityIndex = seedIndex = curveShiftIndex = row % 2;
			}
			const auto expected = RunNode(
				"pc.noise_strand",
				{},
				{{"dimension", Vector2{4, 3}},
				 {"dimension_unit", EnumValue{0}},
				 {"position", Vector2{0, 0}},
				 {"position_unit", EnumValue{0}},
				 {"density", densityIndex == 0 ? .25 : .5},
				 {"seed", seedIndex == 0 ? 17.0 : 31.0},
				 {"slope", .5},
				 {"curve", Vector2{.2, .8}},
				 {"curve_scale", 2.0},
				 {"curve_shift", curveShiftIndex == 0 ? 0.0 : .25},
				 {"axis", EnumValue{0}},
				 {"mode", EnumValue{0}},
				 {"thickness", .05},
				 {"opacity", Vector2{.4, .9}},
				 {"uv_mix", 1.0},
				 {"level_in", Vector2{0, 1}},
				 {"level_out", Vector2{0, 1}}}
			);
			INFO(expected.Message);
			REQUIRE(expected.Ok);
			CHECK(batch.Images[row] == expected.Output());
		}
	}
}

TEST_CASE(
	"Strand noise rejects cumulative work before rows are observed or published", "[source_2d][strand_noise]"
) {
	const auto *entry = FindCatalogueEntry("pc.noise_strand");
	const auto executor = detail::FindExecutor("pc.noise_strand");
	REQUIRE(entry);
	REQUIRE(executor);
	Node node{"generator", "pc.noise_strand", "", {}, {{"seed", 17.0}}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	SetDefaults(context);
	SetValue(context, "dimension", Vector2{4, 4});
	SetValue(context, "dimension_unit", EnumValue{0});
	SetValue(context, "position", Vector2{0, 0});
	SetValue(context, "position_unit", EnumValue{0});
	SetValue(context, "density", 1.0);
	SetValue(context, "thickness", .125);
	std::vector<ElementValue> seeds(2000, 17.0);
	SetValue(context, "seed", ArrayValue{ValueType::Scalar, std::move(seeds)});
	SetValue(context, "axis", EnumValue{0});
	SetValue(context, "mode", EnumValue{0});
	SetValue(context, "curve", Vector2{0, 0});
	SetValue(context, "curve_scale", 1.0);
	SetValue(context, "curve_shift", 0.0);
	SetValue(context, "attribute_array_process", EnumValue{0});
	context.InputProvenanceResolved = true;
	context.ProcessorCount = 2000;
	for (const auto &[port, value] : context.Values)
		context.ValueViews.emplace_back(port, &value);
	size_t observed = 0;
	CHECK_FALSE(detail::RunProcessorBatch(context, executor, Observe, &observed));
	INFO(context.FailurePort << ":" << context.FailureMessage);
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.FailurePort == "surface_out");
	CHECK(context.FailureMessage.find("work") != std::string::npos);
	CHECK(observed == 0);
	CHECK(context.OutputImages.empty());
	CHECK(context.OutputImageArrays.empty());
}
