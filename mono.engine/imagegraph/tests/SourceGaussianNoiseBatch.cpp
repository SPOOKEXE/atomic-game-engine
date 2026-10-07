#include "../src/ProcessorBatch.hpp"
#include "NodeHarness.hpp"

#include <engine/imagegraph/Surface.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.source_gaussian_noise_batch")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	size_t SourceInverseIndex(size_t row, size_t inputIndex) {
		constexpr std::array<size_t, 12> slotLengths{2, 2, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1};
		std::array<size_t, slotLengths.size()> suffix{};
		size_t product = 1;
		for (size_t index = slotLengths.size(); index > 0; --index) {
			suffix[index - 1] = product;
			product *= slotLengths[index - 1];
		}
		return row / suffix[slotLengths.size() - 1 - inputIndex] % slotLengths[inputIndex];
	}
	void SetDefaults(detail::NodeContext &context) {
		for (const auto &input : context.Entry.Inputs)
			if (const auto value = CatalogueDefault(input)) context.Values.emplace_back(input.Id, *value);
	}
	void SetValue(detail::NodeContext &context, std::string_view port, Value value) {
		std::erase_if(context.CatalogueDefaultInputs, [&](std::string_view id) { return id == port; });
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
	void SamePixels(const Image &actual, const Image &expected) {
		CHECK(actual.Width == expected.Width);
		CHECK(actual.Height == expected.Height);
		CHECK(actual.Format == expected.Format);
		CHECK(actual.Pixels == expected.Pixels);
	}
}

TEST_CASE(
	"Gaussian Noise resolves Surface getters into their source value shapes", "[source_gaussian_noise]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		Node{"source", "pc.solid", "", {}, {{"dimension", Vector2{2, 1}}, {"dimension_unit", EnumValue{0}}}},
		Node{
			"generator",
			"pc.noise_gaussian",
			"",
			{},
			{{"dimension_unit", EnumValue{0}}, {"seed", 17.25}, {"attribute_color_depth", EnumValue{5}}}
		}
	};
	document.Links = {
		{"source", "surface_out", "generator", "dimension"},
		{"source", "surface_out", "generator", "position"},
		{"source", "surface_out", "generator", "scale"},
		{"source", "surface_out", "generator", "rotation"},
		{"source", "surface_out", "generator", "mean"},
		{"source", "surface_out", "generator", "varience"},
		{"source", "surface_out", "generator", "level_in"},
		{"source", "surface_out", "generator", "level_out"}
	};
	document.Outputs = {{"out", "generator", "surface_out"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	ImageArray linked;
	REQUIRE(EvaluateArray(document, plan, "out", {}, linked, diagnostic) == Status::Ok);
	REQUIRE(linked.Images.size() == 2);
	const std::array<double, 2> scalarRows{2, 1};
	for (size_t row = 0; row < linked.Images.size(); ++row) {
		CAPTURE(row);
		const auto direct = RunNode(
			"pc.noise_gaussian",
			{},
			{{"dimension", Vector2{2, 1}},
			 {"dimension_unit", EnumValue{0}},
			 {"seed", 17.25},
			 {"position", Vector2{2, 1}},
			 {"rotation", scalarRows[row]},
			 {"scale", Vector2{2, 1}},
			 {"mean", scalarRows[row]},
			 {"varience", scalarRows[row]},
			 {"level_in", Vector2{2, 1}},
			 {"level_out", Vector2{2, 1}},
			 {"attribute_color_depth", EnumValue{5}}}
		);
		INFO(diagnostic.Message << "; " << direct.Port << ": " << direct.Message);
		REQUIRE(direct.Ok);
		SamePixels(linked.Images[row], direct.Output());
	}
}

TEST_CASE("Gaussian Noise retains 12 source slots and follows all row schedules", "[source_gaussian_noise]") {
	const auto *entry = FindCatalogueEntry("pc.noise_gaussian");
	const auto executor = detail::FindExecutor("pc.noise_gaussian");
	REQUIRE(entry);
	REQUIRE(executor);
	constexpr std::array<std::string_view, 12> slots{
		"dimension",
		"seed",
		"position",
		"rotation",
		"scale",
		"level_in",
		"mean",
		"varience",
		"use_conversion",
		"conv_surf_1",
		"conv_surf_2",
		"level_out"
	};
	for (size_t index = 0; index < slots.size(); ++index) {
		const auto *input = FindCatalogueInput(*entry, slots[index]);
		REQUIRE(input);
		CHECK(input->SourceIndex == static_cast<int64_t>(index));
	}
	const std::array dimensions{Vector2{4, 3}, Vector2{5, 3}};
	const std::array seeds{17.25, -17.25};
	const std::array rotations{-15.0, 33.0};
	for (int64_t mode = 0; mode < 4; ++mode) {
		Node node{"generator", "pc.noise_gaussian", "", {}, {}};
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		SetDefaults(context);
		SetValue(context, "dimension", ArrayValue{ValueType::Vector2, {dimensions[0], dimensions[1]}});
		SetValue(context, "dimension_unit", EnumValue{0});
		SetValue(context, "seed", ArrayValue{ValueType::Scalar, {seeds[0], seeds[1]}});
		SetValue(context, "position", Vector2{.13, -.19});
		SetValue(context, "rotation", ArrayValue{ValueType::Scalar, {rotations[0], rotations[1]}});
		SetValue(context, "scale", Vector2{1, 1});
		SetValue(context, "mean", .5);
		SetValue(context, "varience", .5);
		SetValue(context, "level_in", Vector2{0, 1});
		SetValue(context, "level_out", Vector2{0, 1});
		SetValue(context, "use_conversion", false);
		SetValue(context, "attribute_color_depth", EnumValue{5});
		SetValue(context, "attribute_array_process", EnumValue{mode});
		context.InputProvenanceResolved = true;
		for (const auto &[port, value] : context.Values)
			context.ValueViews.emplace_back(port, &value);
		size_t observed = 0;
		const bool ran = detail::RunProcessorBatch(context, executor, Observe, &observed);
		INFO(mode << ": " << context.FailurePort << ": " << context.FailureMessage);
		REQUIRE(ran);
		REQUIRE(context.OutputImageArrays.size() == 1);
		const auto &images = context.OutputImageArrays.front().second.Images;
		const size_t expectedRows = mode < 2 ? 2 : 8;
		REQUIRE(images.size() == expectedRows);
		CHECK(observed == expectedRows);
		for (size_t row = 0; row < images.size(); ++row) {
			CAPTURE(mode, row);
			const auto sourceIndex = [&](size_t index) {
				if (mode == 0) return row % 2;
				if (mode == 1) return std::min(row, size_t{1});
				if (mode == 2) {
					if (index == 0) return row / 4;
					if (index == 1) return row / 2 % 2;
					return row % 2;
				}
				// The inverse schedule uses the mirrored suffix of all physical slots.
				return SourceInverseIndex(row, index);
			};
			const size_t dimensionIndex = sourceIndex(0);
			const size_t seedIndex = sourceIndex(1);
			const size_t rotationIndex = sourceIndex(3);
			const auto expected = RunNode(
				"pc.noise_gaussian",
				{},
				{{"dimension", dimensions[dimensionIndex]},
				 {"dimension_unit", EnumValue{0}},
				 {"seed", seeds[seedIndex]},
				 {"position", Vector2{.13, -.19}},
				 {"rotation", rotations[rotationIndex]},
				 {"scale", Vector2{1, 1}},
				 {"mean", .5},
				 {"varience", .5},
				 {"level_in", Vector2{0, 1}},
				 {"level_out", Vector2{0, 1}},
				 {"use_conversion", false},
				 {"attribute_color_depth", EnumValue{5}}}
			);
			INFO(expected.Port << ": " << expected.Message);
			REQUIRE(expected.Ok);
			SamePixels(images[row], expected.Output());
		}
	}
}

TEST_CASE("Gaussian Noise animation and inherited nodes survive Format9", "[source_gaussian_noise]") {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {Node{
		"generator",
		"pc.noise_gaussian",
		"",
		{},
		{{"dimension", Vector2{8, 6}},
		 {"dimension_unit", EnumValue{0}},
		 {"seed", 17.25},
		 {"position", Vector2{.13, -.19}},
		 {"rotation", 0.0},
		 {"scale", Vector2{1, 1}},
		 {"mean", .5},
		 {"varience", .5},
		 {"level_in", Vector2{0, 1}},
		 {"level_out", Vector2{0, 1}}}
	}};
	document.Nodes[0].SourceAnimatedInputs = {"seed", "rotation"};
	document.Keyframes = {
		{"generator", "seed", 0, 17.25},
		{"generator", "seed", 1, -17.25},
		{"generator", "rotation", 0, 0.0},
		{"generator", "rotation", 1, 33.0}
	};
	Node inherited{"copy", "pc.noise_gaussian", "", {}, {}};
	inherited.InstanceBase = "generator";
	document.Nodes.push_back(std::move(inherited));
	document.Outputs = {{"out", "generator", "surface_out"}, {"copy", "copy", "surface_out"}};
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	REQUIRE(restored == document);
	Plan plan;
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	for (const uint64_t tick : {uint64_t{0}, uint64_t{1}}) {
		EvaluationRequest request;
		request.Tick = tick;
		Image source, copy;
		REQUIRE(Evaluate(restored, plan, "out", request, source, diagnostic) == Status::Ok);
		REQUIRE(Evaluate(restored, plan, "copy", request, copy, diagnostic) == Status::Ok);
		SamePixels(copy, source);
		if (tick == 0) continue;
		EvaluationRequest firstRequest;
		Image first;
		REQUIRE(Evaluate(restored, plan, "out", firstRequest, first, diagnostic) == Status::Ok);
		CHECK(first.Pixels != source.Pixels);
	}
}

TEST_CASE("Gaussian Noise admits work and bytes before observers or outputs", "[source_gaussian_noise]") {
	const auto *entry = FindCatalogueEntry("pc.noise_gaussian");
	const auto executor = detail::FindExecutor("pc.noise_gaussian");
	REQUIRE(entry);
	REQUIRE(executor);
	{
		Node node{"generator", "pc.noise_gaussian", "", {}, {}};
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		SetDefaults(context);
		std::vector<ElementValue> dimensions(1000, Vector2{4, 4});
		SetValue(context, "dimension", ArrayValue{ValueType::Vector2, std::move(dimensions)});
		SetValue(context, "dimension_unit", EnumValue{0});
		SetValue(context, "seed", 17.25);
		SetValue(context, "attribute_array_process", EnumValue{0});
		context.InputProvenanceResolved = true;
		for (const auto &[port, value] : context.Values)
			context.ValueViews.emplace_back(port, &value);
		context.ProcessorCount = 1000;
		size_t observed = 0;
		CHECK_FALSE(detail::RunProcessorBatch(context, executor, Observe, &observed));
		INFO(context.FailurePort << ": " << context.FailureMessage);
		CHECK(context.FailureCode == Status::LimitExceeded);
		CHECK(context.FailurePort == "surface_out");
		CHECK(observed == 0);
		CHECK(context.OutputImages.empty());
		CHECK(context.OutputImageArrays.empty());
	}
	{
		Node node{"generator", "pc.noise_gaussian", "", {}, {}};
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = 1;
		SetDefaults(context);
		SetValue(context, "dimension", Vector2{1, 1});
		SetValue(context, "dimension_unit", EnumValue{0});
		SetValue(context, "seed", 17.25);
		context.InputProvenanceResolved = true;
		for (const auto &[port, value] : context.Values)
			context.ValueViews.emplace_back(port, &value);
		size_t observed = 0;
		CHECK_FALSE(detail::RunProcessorBatch(context, executor, Observe, &observed));
		INFO(context.FailurePort << ": " << context.FailureMessage);
		CHECK(context.FailureCode == Status::LimitExceeded);
		CHECK(observed == 0);
		CHECK(context.OutputImages.empty());
		CHECK(context.OutputImageArrays.empty());
	}
}
