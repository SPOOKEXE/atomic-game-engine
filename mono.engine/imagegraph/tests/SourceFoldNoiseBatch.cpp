#include "../src/ProcessorBatch.hpp"
#include "NodeHarness.hpp"

#include <engine/imagegraph/Surface.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.source_fold_noise_batch")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	size_t SourceInverseIndex(size_t row, size_t inputIndex) {
		constexpr std::array<size_t, 14> slotLengths{2, 1, 1, 2, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1};
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
	Image FloatPixel(SurfacePixel pixel) {
		Image image{1, 1, std::vector<uint8_t>(16), 0, SurfaceFormat::RGBA32Float};
		REQUIRE(StoreSurfacePixel(image, 0, 0, pixel));
		return image;
	}
}

TEST_CASE("Fold Noise retains source indices and follows all four row schedules", "[source_fold_noise]") {
	const auto *entry = FindCatalogueEntry("pc.fold_noise");
	const auto executor = detail::FindExecutor("pc.fold_noise");
	REQUIRE(entry);
	REQUIRE(executor);
	constexpr std::array<std::string_view, 14> slots{
		"dimension",
		"position",
		"scale",
		"iteration",
		"stretch",
		"amplitude",
		"mode",
		"rotation",
		"mask",
		"uv_map",
		"uv_mix",
		"level_in",
		"level_out",
		"detail"
	};
	for (size_t index = 0; index < slots.size(); ++index) {
		const auto *input = FindCatalogueInput(*entry, slots[index]);
		REQUIRE(input);
		CHECK(input->SourceIndex == static_cast<int64_t>(index));
	}
	const std::array dimensions{Vector2{4, 3}, Vector2{5, 3}};
	const std::array iterations{int64_t{2}, int64_t{5}};
	const std::array rotations{-15.0, 33.0};
	for (int64_t mode = 0; mode < 4; ++mode) {
		Node node{"generator", "pc.fold_noise", "", {}, {}};
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		SetDefaults(context);
		SetValue(context, "dimension", ArrayValue{ValueType::Vector2, {dimensions[0], dimensions[1]}});
		SetValue(context, "dimension_unit", EnumValue{0});
		SetValue(context, "position", Vector2{.13, -.19});
		SetValue(context, "position_unit", EnumValue{0});
		SetValue(context, "scale", Vector2{.7, .4});
		SetValue(context, "iteration", ArrayValue{ValueType::Integer, {iterations[0], iterations[1]}});
		SetValue(context, "stretch", 1.7);
		SetValue(context, "amplitude", 1.2);
		SetValue(context, "detail", Vector2{2.3, .8});
		SetValue(context, "mode", EnumValue{0});
		SetValue(context, "rotation", ArrayValue{ValueType::Scalar, {rotations[0], rotations[1]}});
		SetValue(context, "level_in", Vector2{.1, 1.2});
		SetValue(context, "level_out", Vector2{-.2, 1.3});
		SetValue(context, "uv_mix", 0.0);
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
					if (index == 3) return row / 2 % 2;
					return row % 2;
				}
				// The inverse schedule indexes the mirrored suffix of all 14 physical slots.
				return SourceInverseIndex(row, index);
			};
			const size_t dimensionIndex = sourceIndex(0);
			const size_t iterationIndex = sourceIndex(3);
			const size_t rotationIndex = sourceIndex(7);
			const auto expected = RunNode(
				"pc.fold_noise",
				{},
				{{"dimension", dimensions[dimensionIndex]},
				 {"dimension_unit", EnumValue{0}},
				 {"position", Vector2{.13, -.19}},
				 {"position_unit", EnumValue{0}},
				 {"scale", Vector2{.7, .4}},
				 {"iteration", iterations[iterationIndex]},
				 {"stretch", 1.7},
				 {"amplitude", 1.2},
				 {"detail", Vector2{2.3, .8}},
				 {"mode", EnumValue{0}},
				 {"rotation", rotations[rotationIndex]},
				 {"level_in", Vector2{.1, 1.2}},
				 {"level_out", Vector2{-.2, 1.3}},
				 {"uv_mix", 0.0},
				 {"attribute_color_depth", EnumValue{5}}}
			);
			INFO(expected.Port << ": " << expected.Message);
			REQUIRE(expected.Ok);
			SamePixels(images[row], expected.Output());
		}
	}
}

TEST_CASE("Fold Noise animation survives native Format9 round trip", "[source_fold_noise]") {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {Node{
		"generator",
		"pc.fold_noise",
		"",
		{},
		{{"dimension", Vector2{8, 6}},
		 {"dimension_unit", EnumValue{0}},
		 {"position", Vector2{.13, -.19}},
		 {"position_unit", EnumValue{0}},
		 {"scale", Vector2{.7, .4}},
		 {"iteration", int64_t{3}},
		 {"stretch", 1.7},
		 {"amplitude", 1.2},
		 {"detail", Vector2{2.3, .8}},
		 {"mode", EnumValue{0}},
		 {"rotation", 0.0},
		 {"level_in", Vector2{.1, 1.2}},
		 {"level_out", Vector2{-.2, 1.3}}}
	}};
	document.Nodes[0].SourceAnimatedInputs = {"rotation", "iteration"};
	document.Keyframes = {
		{"generator", "rotation", 0, 0.0},
		{"generator", "rotation", 1, 71.0},
		{"generator", "iteration", 0, int64_t{1}},
		{"generator", "iteration", 1, int64_t{6}}
	};
	Node inherited{"copy", "pc.fold_noise", "", {}, {}};
	inherited.InstanceBase = "generator";
	document.Nodes.push_back(std::move(inherited));
	document.Outputs = {{"out", "generator", "surface_out"}, {"copy", "copy", "surface_out"}};
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	REQUIRE(restored == document);
	Plan plan;
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	EvaluationRequest firstRequest, laterRequest;
	laterRequest.Tick = 1;
	Image first, later, firstCopy, laterCopy;
	REQUIRE(Evaluate(restored, plan, "out", firstRequest, first, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(restored, plan, "out", laterRequest, later, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(restored, plan, "copy", firstRequest, firstCopy, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(restored, plan, "copy", laterRequest, laterCopy, diagnostic) == Status::Ok);
	SamePixels(firstCopy, first);
	SamePixels(laterCopy, later);
	CHECK(first.Pixels != later.Pixels);
}

TEST_CASE(
	"Fold Noise projects Surface dimensions, ISlider rows, and range endpoints", "[source_fold_noise]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		Node{"source", "pc.solid", "", {}, {{"dimension", Vector2{2, 1}}, {"dimension_unit", EnumValue{0}}}},
		Node{
			"generator",
			"pc.fold_noise",
			"",
			{},
			{{"dimension_unit", EnumValue{0}},
			 {"position_unit", EnumValue{0}},
			 {"scale", Vector2{.7, .4}},
			 {"stretch", 1.7},
			 {"amplitude", 1.2},
			 {"mode", EnumValue{0}},
			 {"rotation", 23.0},
			 {"level_out", Vector2{-.2, 1.3}},
			 {"uv_mix", 0.0},
			 {"attribute_color_depth", EnumValue{5}}}
		}
	};
	document.Links = {
		{"source", "surface_out", "generator", "dimension"},
		{"source", "surface_out", "generator", "position"},
		{"source", "surface_out", "generator", "scale"},
		{"source", "surface_out", "generator", "iteration"},
		{"source", "surface_out", "generator", "level_in"},
		{"source", "surface_out", "generator", "level_out"},
		{"source", "surface_out", "generator", "detail"}
	};
	document.Outputs = {{"out", "generator", "surface_out"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	ImageArray linked;
	REQUIRE(EvaluateArray(document, plan, "out", {}, linked, diagnostic) == Status::Ok);
	REQUIRE(linked.Images.size() == 2);
	for (size_t row = 0; row < linked.Images.size(); ++row) {
		const auto direct = RunNode(
			"pc.fold_noise",
			{},
			{{"dimension", Vector2{2, 1}},
			 {"dimension_unit", EnumValue{0}},
			 {"position", Vector2{2, 1}},
			 {"position_unit", EnumValue{0}},
			 {"scale", Vector2{2, 1}},
			 {"iteration", row == 0 ? 2.0 : 1.0},
			 {"stretch", 1.7},
			 {"amplitude", 1.2},
			 {"mode", EnumValue{0}},
			 {"rotation", 23.0},
			 {"level_in", Vector2{2, 1}},
			 {"level_out", Vector2{2, 1}},
			 {"detail", Vector2{2, 1}},
			 {"uv_mix", 0.0},
			 {"attribute_color_depth", EnumValue{5}}}
		);
		INFO(row << ": " << direct.Port << ": " << direct.Message);
		REQUIRE(direct.Ok);
		SamePixels(linked.Images[row], direct.Output());
	}
}

TEST_CASE(
	"Fold Noise preflights aggregate work before observers or output allocation", "[source_fold_noise]"
) {
	const auto *entry = FindCatalogueEntry("pc.fold_noise");
	const auto executor = detail::FindExecutor("pc.fold_noise");
	REQUIRE(entry);
	REQUIRE(executor);
	Node node{"generator", "pc.fold_noise", "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	SetDefaults(context);
	std::vector<ElementValue> dimensions(2, Vector2{1, 1});
	SetValue(context, "dimension", ArrayValue{ValueType::Vector2, std::move(dimensions)});
	SetValue(context, "dimension_unit", EnumValue{0});
	SetValue(context, "iteration", int64_t{8000});
	SetValue(context, "detail", Vector2{1, 1});
	SetValue(context, "amplitude", 1.0);
	SetValue(context, "attribute_array_process", EnumValue{0});
	context.InputProvenanceResolved = true;
	for (const auto &[port, value] : context.Values)
		context.ValueViews.emplace_back(port, &value);
	context.ProcessorCount = 2;
	size_t observed = 0;
	CHECK_FALSE(detail::RunProcessorBatch(context, executor, Observe, &observed));
	INFO(context.FailurePort << ": " << context.FailureMessage);
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.FailurePort == "surface_out");
	CHECK(context.FailureMessage.find("work") != std::string::npos);
	CHECK(observed == 0);
	CHECK(context.OutputImages.empty());
	CHECK(context.OutputImageArrays.empty());
}

TEST_CASE("Fold Noise refuses output bytes before observers or allocation", "[source_fold_noise]") {
	const auto *entry = FindCatalogueEntry("pc.fold_noise");
	const auto executor = detail::FindExecutor("pc.fold_noise");
	REQUIRE(entry);
	REQUIRE(executor);
	Node node{"generator", "pc.fold_noise", "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = 1;
	SetDefaults(context);
	SetValue(context, "dimension", Vector2{1, 1});
	SetValue(context, "dimension_unit", EnumValue{0});
	SetValue(context, "iteration", int64_t{0});
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

TEST_CASE("Fold Noise rejects mask overflow before publishing a batch", "[source_fold_noise]") {
	const auto *entry = FindCatalogueEntry("pc.fold_noise");
	const auto executor = detail::FindExecutor("pc.fold_noise");
	REQUIRE(entry);
	REQUIRE(executor);
	const Image uv = FloatPixel({.5, .5, 0, 3.0e38});
	const Image mask = FloatPixel({10, 10, 10, 1});
	Node node{"generator", "pc.fold_noise", "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.Images.emplace_back("uv_map", &uv);
	context.Images.emplace_back("mask", &mask);
	SetDefaults(context);
	SetValue(context, "dimension", Vector2{8, 6});
	SetValue(context, "dimension_unit", EnumValue{0});
	SetValue(context, "mode", EnumValue{1});
	SetValue(context, "uv_mix", 0.0);
	SetValue(context, "attribute_color_depth", EnumValue{5});
	context.InputProvenanceResolved = true;
	for (const auto &[port, value] : context.Values)
		context.ValueViews.emplace_back(port, &value);
	size_t observed = 0;
	CHECK_FALSE(detail::RunProcessorBatch(context, executor, Observe, &observed));
	INFO(context.FailurePort << ": " << context.FailureMessage);
	CHECK(context.FailureCode == Status::InvalidValue);
	CHECK(context.FailurePort == "mask");
	CHECK(observed == 0);
	CHECK(context.OutputImages.empty());
	CHECK(context.OutputImageArrays.empty());
}
