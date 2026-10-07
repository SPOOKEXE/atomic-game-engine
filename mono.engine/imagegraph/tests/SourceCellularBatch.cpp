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

TEST_SUITE_ID("engine.imagegraph.source_cellular_batch")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Document CellularGraph() {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {Node{
			"generator",
			"pc.cellular",
			"",
			{},
			{{"dimension", Vector2{2, 2}}, {"dimension_unit", EnumValue{0}}, {"seed", 17.0}}
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

TEST_CASE("Cellular animation and inherited instances survive Format9", "[source_2d][cellular]") {
	auto document = CellularGraph();
	document.Nodes[0].SourceAnimatedInputs = {"seed", "phase"};
	document.Keyframes = {
		{"generator", "seed", 0, 17.0},
		{"generator", "seed", 1, 31.0},
		{"generator", "seed", 2, 31.0},
		{"generator", "phase", 0, 0.0},
		{"generator", "phase", 1, 0.0},
		{"generator", "phase", 2, 90.0}
	};
	Node instance{"copy", "pc.cellular", "", {}, {}};
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
		CHECK(inherited.Width == source.Width);
		CHECK(inherited.Height == source.Height);
		CHECK(inherited.Format == source.Format);
		CHECK(inherited.Pixels == source.Pixels);
	}
	Image first, seedAnimated, phaseAnimated;
	REQUIRE(Draw(restored, "out", 0, first, diagnostic) == Status::Ok);
	REQUIRE(Draw(restored, "out", 1, seedAnimated, diagnostic) == Status::Ok);
	REQUIRE(Draw(restored, "out", 2, phaseAnimated, diagnostic) == Status::Ok);
	CHECK(first.Pixels != seedAnimated.Pixels);
	CHECK(seedAnimated.Pixels != phaseAnimated.Pixels);
}

TEST_CASE("Cellular array schedules select native scalar and image rows", "[source_2d][cellular]") {
	const auto *entry = FindCatalogueEntry("pc.cellular");
	const auto executor = detail::FindExecutor("pc.cellular");
	REQUIRE(entry);
	REQUIRE(executor);
	const std::array<std::string_view, 28> nativeSlots{
		{"dimension", "position",  "scale",		 "seed",		 "type",
		 "contrast",  "pattern",   "middle",	 "radial_scale", "radial_shatter",
		 "colored",	  "scale_map", "rotation",	 "mask",		 "phase",
		 "inverted",  "iteration", "blend_mode", "iter_scale",	 "iter_amplitude",
		 "uv_map",	  "uv_mix",	   "level_in",	 "randomness",	 "size",
		 "gap",		  "gap_color", "level_out"}
	};
	for (size_t slot = 0; slot < nativeSlots.size(); ++slot) {
		const auto *input = FindCatalogueInput(*entry, nativeSlots[slot]);
		REQUIRE(input);
		CHECK(input->SourceIndex == int64_t(slot));
	}
	const Image uv0{1, 1, {24, 213, 0, 255}, 0};
	const Image uv1{1, 1, {221, 48, 0, 255}, 0};
	ImageArray uvImages;
	uvImages.Images = {uv0, uv1};
	uvImages.Items = {{size_t{0}}, {size_t{1}}};
	for (int64_t mode = 0; mode < 4; ++mode) {
		Node node{"generator", "pc.cellular", "", {}, {{"seed", 17.0}}};
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		SetDefaults(context);
		SetValue(context, "dimension", ArrayValue{ValueType::Vector2, {Vector2{1, 1}, Vector2{2, 1}}});
		SetValue(context, "dimension_unit", EnumValue{0});
		SetValue(context, "position", Vector2{0, 0});
		SetValue(context, "position_unit", EnumValue{0});
		SetValue(context, "seed", 17.0);
		SetValue(context, "type", EnumValue{0});
		SetValue(context, "pattern", EnumValue{0});
		SetValue(context, "scale", 4.0);
		SetValue(context, "uv_mix", 1.0);
		SetValue(context, "attribute_array_process", EnumValue{mode});
		context.ImageArrays.emplace_back("uv_map", &uvImages);
		context.InputProvenanceResolved = true;
		for (const auto &[port, value] : context.Values)
			context.ValueViews.emplace_back(port, &value);
		size_t observed = 0;
		const bool ran = detail::RunProcessorBatch(context, executor, Observe, &observed);
		INFO(context.FailurePort << ":" << context.FailureMessage);
		REQUIRE(ran);
		REQUIRE(context.OutputImageArrays.size() == 1);
		const auto &batch = context.OutputImageArrays.front().second;
		const std::array<std::pair<size_t, size_t>, 2> paired{{{0, 0}, {1, 1}}};
		const std::array<std::pair<size_t, size_t>, 4> expanded =
			mode == 2 ? std::array<std::pair<size_t, size_t>, 4>{{{0, 0}, {0, 1}, {1, 0}, {1, 1}}}
					  : std::array<std::pair<size_t, size_t>, 4>{{{0, 0}, {1, 0}, {0, 1}, {1, 1}}};
		const size_t count = mode < 2 ? paired.size() : expanded.size();
		REQUIRE(batch.Images.size() == count);
		CHECK(observed == count);
		for (size_t row = 0; row < count; ++row) {
			const auto [dimensionIndex, uvIndex] = mode < 2 ? paired[row] : expanded[row];
			const Vector2 dimension = dimensionIndex == 0 ? Vector2{1, 1} : Vector2{2, 1};
			const Image &uv = uvIndex == 0 ? uv0 : uv1;
			const auto expected = RunNode(
				"pc.cellular",
				{{"uv_map", &uv}},
				{{"dimension", dimension},
				 {"dimension_unit", EnumValue{0}},
				 {"position", Vector2{0, 0}},
				 {"position_unit", EnumValue{0}},
				 {"seed", 17.0},
				 {"type", EnumValue{0}},
				 {"pattern", EnumValue{0}},
				 {"scale", 4.0},
				 {"uv_mix", 1.0}}
			);
			INFO(expected.Message);
			REQUIRE(expected.Ok);
			CHECK(batch.Images[row].Width == expected.Output().Width);
			CHECK(batch.Images[row].Height == expected.Output().Height);
			CHECK(batch.Images[row].Format == expected.Output().Format);
			CHECK(batch.Images[row].Pixels == expected.Output().Pixels);
		}
	}
}

TEST_CASE("Cellular rejects cumulative batch work before observing or publishing", "[source_2d][cellular]") {
	const auto *entry = FindCatalogueEntry("pc.cellular");
	const auto executor = detail::FindExecutor("pc.cellular");
	REQUIRE(entry);
	REQUIRE(executor);
	Node node{"generator", "pc.cellular", "", {}, {{"seed", 17.0}}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	SetDefaults(context);
	std::vector<ElementValue> dimensions(2000, Vector2{3, 3});
	SetValue(context, "dimension", ArrayValue{ValueType::Vector2, std::move(dimensions)});
	SetValue(context, "dimension_unit", EnumValue{0});
	SetValue(context, "seed", 17.0);
	SetValue(context, "type", EnumValue{0});
	SetValue(context, "pattern", EnumValue{0});
	SetValue(context, "scale", 4.0);
	SetValue(context, "iteration", int64_t{1});
	SetValue(context, "attribute_array_process", EnumValue{0});
	context.InputProvenanceResolved = true;
	for (const auto &[port, value] : context.Values)
		context.ValueViews.emplace_back(port, &value);
	context.ProcessorCount = 2000;
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
