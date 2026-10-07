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

TEST_SUITE_ID("engine.imagegraph.source_perlin_batch")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Document PerlinGraph() {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {Node{
			"generator",
			"pc.perlin",
			"",
			{},
			{{"dimension", Vector2{2, 2}},
			 {"dimension_unit", EnumValue{0}},
			 {"scale", Vector2{3, 2}},
			 {"seed", 17.0}}
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

TEST_CASE("Perlin animation and inherited instances survive Format9", "[source_2d][perlin]") {
	auto document = PerlinGraph();
	document.Nodes[0].SourceAnimatedInputs = {"seed"};
	document.Keyframes = {
		{"generator", "seed", 0, 17.0}, {"generator", "seed", 1, 31.0}, {"generator", "seed", 2, 31.0}
	};
	Node instance{"copy", "pc.perlin", "", {}, {}};
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
	Image first, seedAnimated;
	REQUIRE(Draw(restored, "out", 0, first, diagnostic) == Status::Ok);
	REQUIRE(Draw(restored, "out", 1, seedAnimated, diagnostic) == Status::Ok);
	CHECK(first.Pixels != seedAnimated.Pixels);
}

TEST_CASE("Perlin array schedules select native scalar and image rows", "[source_2d][perlin]") {
	const auto *entry = FindCatalogueEntry("pc.perlin");
	const auto executor = detail::FindExecutor("pc.perlin");
	REQUIRE(entry);
	REQUIRE(executor);
	const std::array<std::string_view, 23> nativeSlots{
		{"dimension",	"position",		 "scale",		  "iteration",	   "tile",		"seed",
		 "color_mode",	"color_r_range", "color_g_range", "color_b_range", "scale_map", "rotation",
		 "mask",		"phase",		 "scaling",		  "amplitude",	   "uv_map",	"uv_mix",
		 "scaling_map", "amplitude_map", "blend_method",  "level_in",	   "level_out"}
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
		Node node{"generator", "pc.perlin", "", {}, {{"seed", 17.0}}};
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		SetDefaults(context);
		SetValue(context, "dimension", ArrayValue{ValueType::Vector2, {Vector2{1, 1}, Vector2{2, 1}}});
		SetValue(context, "dimension_unit", EnumValue{0});
		SetValue(context, "position_unit", EnumValue{0});
		SetValue(context, "scaling", ArrayValue{ValueType::Scalar, {1.5, 2.5}});
		SetValue(context, "amplitude", ArrayValue{ValueType::Scalar, {.25, .75}});
		SetValue(context, "seed", 17.0);
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
		const size_t count = mode < 2 ? 2 : 16;
		REQUIRE(batch.Images.size() == count);
		CHECK(observed == count);
		for (size_t row = 0; row < count; ++row) {
			size_t dimensionIndex, scalingIndex, amplitudeIndex, uvIndex;
			if (mode == 0 || mode == 1) {
				dimensionIndex = scalingIndex = amplitudeIndex = uvIndex = row;
			} else if (mode == 2) {
				dimensionIndex = row / 8;
				scalingIndex = row / 4 % 2;
				amplitudeIndex = row / 2 % 2;
				uvIndex = row % 2;
			} else {
				dimensionIndex = row % 2;
				scalingIndex = amplitudeIndex = uvIndex = row / 8;
			}
			const Vector2 dimension = dimensionIndex == 0 ? Vector2{1, 1} : Vector2{2, 1};
			const Image &uv = uvIndex == 0 ? uv0 : uv1;
			const auto expected = RunNode(
				"pc.perlin",
				{{"uv_map", &uv}},
				{{"dimension", dimension},
				 {"dimension_unit", EnumValue{0}},
				 {"position_unit", EnumValue{0}},
				 {"seed", 17.0},
				 {"scaling", scalingIndex == 0 ? 1.5 : 2.5},
				 {"amplitude", amplitudeIndex == 0 ? .25 : .75},
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

TEST_CASE("Perlin cumulative work is rejected before rows are observed or published", "[source_2d][perlin]") {
	const auto *entry = FindCatalogueEntry("pc.perlin");
	const auto executor = detail::FindExecutor("pc.perlin");
	REQUIRE(entry);
	REQUIRE(executor);
	Node node{"generator", "pc.perlin", "", {}, {{"seed", 17.0}}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	SetDefaults(context);
	std::vector<ElementValue> dimensions(2000, Vector2{4, 4});
	SetValue(context, "dimension", ArrayValue{ValueType::Vector2, std::move(dimensions)});
	SetValue(context, "dimension_unit", EnumValue{0});
	SetValue(context, "position_unit", EnumValue{0});
	SetValue(context, "seed", 17.0);
	SetValue(context, "iteration", int64_t{1});
	SetValue(context, "color_mode", EnumValue{0});
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
