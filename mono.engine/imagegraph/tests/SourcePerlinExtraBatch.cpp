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

TEST_SUITE_ID("engine.imagegraph.source_perlin_extra_batch")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Document PerlinExtraGraph() {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {Node{
			"generator",
			"pc.perlin_extra",
			"",
			{},
			{{"dimension", Vector2{3, 2}},
			 {"dimension_unit", EnumValue{0}},
			 {"position", Vector2{.17, .31}},
			 {"position_unit", EnumValue{0}},
			 {"scale", Vector2{3.25, 2.75}},
			 {"scale_mapped", true},
			 {"noise_type", EnumValue{2}},
			 {"parameter_a", .2},
			 {"parameter_a_mapped", true},
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

TEST_CASE("Extra Perlin animation and inherited instances survive Format9", "[source_2d][perlin_extra]") {
	auto document = PerlinExtraGraph();
	document.Nodes[0].SourceAnimatedInputs = {"seed", "parameter_a"};
	document.Keyframes = {
		{"generator", "seed", 0, 17.0},
		{"generator", "seed", 1, 31.0},
		{"generator", "seed", 2, 31.0},
		{"generator", "parameter_a", 0, .2},
		{"generator", "parameter_a", 1, .2},
		{"generator", "parameter_a", 2, .8}
	};
	Node instance{"copy", "pc.perlin_extra", "", {}, {}};
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
	Image first, seedAnimated, parameterAnimated;
	REQUIRE(Draw(restored, "out", 0, first, diagnostic) == Status::Ok);
	REQUIRE(Draw(restored, "out", 1, seedAnimated, diagnostic) == Status::Ok);
	REQUIRE(Draw(restored, "out", 2, parameterAnimated, diagnostic) == Status::Ok);
	CHECK(first.Pixels != seedAnimated.Pixels);
	CHECK(seedAnimated.Pixels != parameterAnimated.Pixels);
}

TEST_CASE("Extra Perlin array schedules select native scalar and image rows", "[source_2d][perlin_extra]") {
	const auto *entry = FindCatalogueEntry("pc.perlin_extra");
	const auto executor = detail::FindExecutor("pc.perlin_extra");
	REQUIRE(entry);
	REQUIRE(executor);
	const std::array<std::string_view, 22> nativeSlots{
		{"dimension",	"position",		 "scale",			"iteration",	   "tile",		 "seed",
		 "color_mode",	"color_r_range", "color_g_range",	"color_b_range",   "noise_type", "parameter_a",
		 "parameter_b", "scale_map",	 "parameter_a_map", "parameter_b_map", "rotation",	 "mask",
		 "uv_map",		"uv_mix",		 "level_in",		"level_out"}
	};
	for (size_t slot = 0; slot < nativeSlots.size(); ++slot) {
		const auto *input = FindCatalogueInput(*entry, nativeSlots[slot]);
		REQUIRE(input);
		CHECK(input->SourceIndex == int64_t(slot));
	}
	const Image uv0{1, 1, {24, 213, 0, 255}, 0};
	const Image uv1{1, 1, {221, 48, 0, 255}, 0};
	const Image map{1, 1, {128, 128, 128, 255}, 0};
	ImageArray uvImages;
	uvImages.Images = {uv0, uv1};
	uvImages.Items = {{size_t{0}}, {size_t{1}}};
	for (int64_t mode = 0; mode < 4; ++mode) {
		Node node{"generator", "pc.perlin_extra", "", {}, {{"seed", 17.0}}};
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		SetDefaults(context);
		SetValue(context, "dimension", ArrayValue{ValueType::Vector2, {Vector2{1, 1}, Vector2{2, 1}}});
		SetValue(context, "dimension_unit", EnumValue{0});
		SetValue(context, "position", Vector2{.17, .31});
		SetValue(context, "position_unit", EnumValue{1});
		SetValue(context, "scale", Vector2{7.7, 6.3});
		SetValue(context, "scale_mapped", true);
		SetValue(context, "iteration", ArrayValue{ValueType::Integer, {int64_t{1}, int64_t{3}}});
		SetValue(context, "seed", 17.0);
		SetValue(context, "noise_type", EnumValue{2});
		SetValue(context, "parameter_a", .37);
		SetValue(context, "parameter_a_mapped", true);
		SetValue(context, "rotation", ArrayValue{ValueType::Scalar, {12.0, 37.0}});
		SetValue(context, "uv_mix", 1.0);
		SetValue(context, "attribute_array_process", EnumValue{mode});
		context.ImageArrays.emplace_back("uv_map", &uvImages);
		context.Images.emplace_back("scale_map", &map);
		context.Images.emplace_back("parameter_a_map", &map);
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
			// Inverse uses the mirrored suffix across all 22 source slots.
			const size_t dimensionIndex = mode == 3 ? row % 2 : mode < 2 ? row : row / 8;
			const size_t iterationIndex = mode == 3 ? row % 2 : mode < 2 ? row : row / 4 % 2;
			const size_t rotationIndex = mode == 3 ? row / 4 % 2 : mode < 2 ? row : row / 2 % 2;
			const size_t uvIndex = mode == 3 ? row / 4 % 2 : mode < 2 ? row : row % 2;
			// Reference position resolves against first Dimension {1,1}, yielding these pixel values.
			const auto expected = RunNode(
				"pc.perlin_extra",
				{{"uv_map", uvIndex == 0 ? &uv0 : &uv1}, {"scale_map", &map}, {"parameter_a_map", &map}},
				{{"dimension", dimensionIndex == 0 ? Vector2{1, 1} : Vector2{2, 1}},
				 {"dimension_unit", EnumValue{0}},
				 {"position", Vector2{.17, .31}},
				 {"position_unit", EnumValue{0}},
				 {"scale", Vector2{7.7, 6.3}},
				 {"scale_mapped", true},
				 {"iteration", int64_t(iterationIndex == 0 ? 1 : 3)},
				 {"seed", 17.0},
				 {"noise_type", EnumValue{2}},
				 {"parameter_a", .37},
				 {"parameter_a_mapped", true},
				 {"rotation", rotationIndex == 0 ? 12.0 : 37.0},
				 {"uv_mix", 1.0},
				 {"level_out", Vector2{0, 1}}}
			);
			INFO("array mode=" << mode << " row=" << row);
			INFO(expected.Message);
			REQUIRE(expected.Ok);
			CHECK(batch.Images[row].Width == expected.Output().Width);
			CHECK(batch.Images[row].Height == expected.Output().Height);
			CHECK(batch.Images[row].Format == expected.Output().Format);
			CHECK(batch.Images[row].Pixels == expected.Output().Pixels);
		}
	}
}

TEST_CASE(
	"Extra Perlin refuses cumulative work before observing or publishing", "[source_2d][perlin_extra]"
) {
	const auto *entry = FindCatalogueEntry("pc.perlin_extra");
	const auto executor = detail::FindExecutor("pc.perlin_extra");
	REQUIRE(entry);
	REQUIRE(executor);
	Node node{"generator", "pc.perlin_extra", "", {}, {{"seed", 17.0}}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	SetDefaults(context);
	std::vector<ElementValue> dimensions(2000, Vector2{4, 4});
	SetValue(context, "dimension", ArrayValue{ValueType::Vector2, std::move(dimensions)});
	SetValue(context, "dimension_unit", EnumValue{0});
	SetValue(context, "iteration", int64_t{2});
	SetValue(context, "noise_type", EnumValue{0});
	SetValue(context, "scale", Vector2{3, 4});
	SetValue(context, "scale_mapped", true);
	SetValue(context, "seed", 17.0);
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

TEST_CASE(
	"Extra Perlin refuses byte exhaustion before observing or publishing", "[source_2d][perlin_extra]"
) {
	const auto *entry = FindCatalogueEntry("pc.perlin_extra");
	const auto executor = detail::FindExecutor("pc.perlin_extra");
	REQUIRE(entry);
	REQUIRE(executor);
	Node node{"generator", "pc.perlin_extra", "", {}, {{"seed", 17.0}}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = 1;
	SetDefaults(context);
	SetValue(context, "dimension", Vector2{1, 1});
	SetValue(context, "dimension_unit", EnumValue{0});
	SetValue(context, "scale", Vector2{3, 4});
	SetValue(context, "scale_mapped", true);
	SetValue(context, "seed", 17.0);
	SetValue(context, "attribute_array_process", EnumValue{0});
	context.InputProvenanceResolved = true;
	for (const auto &[port, value] : context.Values)
		context.ValueViews.emplace_back(port, &value);
	size_t observed = 0;
	CHECK_FALSE(detail::RunProcessorBatch(context, executor, Observe, &observed));
	INFO(context.FailurePort << ":" << context.FailureMessage);
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(observed == 0);
	CHECK(context.OutputImages.empty());
	CHECK(context.OutputImageArrays.empty());
}
