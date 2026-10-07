#include "../src/ProcessorBatch.hpp"
#include "NodeHarness.hpp"

#include <engine/imagegraph/Surface.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.source_wavelet_noise_batch")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	size_t SourceInverseIndex(size_t row, size_t inputIndex) {
		constexpr std::array<size_t, 15> slotLengths{2, 1, 1, 2, 2, 3, 1, 1, 1, 2, 1, 2, 1, 1, 1};
		std::array<size_t, slotLengths.size()> suffix{};
		size_t product = 1;
		for (size_t i = slotLengths.size(); i > 0; --i) {
			suffix[i - 1] = product;
			product *= slotLengths[i - 1];
		}
		return row / suffix[slotLengths.size() - 1 - inputIndex] % slotLengths[inputIndex];
	}

	Document WaveletSourceGraph(bool progressLink = false) {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			Node{
				"source",
				"pc.solid",
				"",
				{},
				{{"dimension", Vector2{2, 1}},
				 {"dimension_unit", EnumValue{0}},
				 {"attribute_color_depth", EnumValue{5}}}
			},
			Node{
				"generator",
				"pc.wavelet_noise",
				"",
				{},
				{{"seed", 17.25},
				 {"detail", 1.24},
				 {"dimension_unit", EnumValue{0}},
				 {"position_unit", EnumValue{0}},
				 {"scale_mapped", true},
				 {"progress_mapped", true},
				 {"detail_mapped", true},
				 {"attribute_color_depth", EnumValue{5}}}
			}
		};
		document.Links = {
			{"source", "surface_out", "generator", "dimension"},
			{"source", "surface_out", "generator", "position"},
			{"source", "surface_out", "generator", "scale"},
			{"source", "surface_out", "generator", "level_in"}
		};
		if (progressLink) document.Links.push_back({"source", "surface_out", "generator", "progress"});
		document.Outputs = {{"out", "generator", "surface_out"}};
		return document;
	}
	Image FloatPixel(SurfacePixel pixel) {
		Image image{1, 1, std::vector<uint8_t>(16), 0, SurfaceFormat::RGBA32Float};
		REQUIRE(StoreSurfacePixel(image, 0, 0, pixel));
		return image;
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

TEST_CASE("Wavelet Noise animation and inherited nodes round trip in Format9", "[source_wavelet_noise]") {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {Node{
		"generator",
		"pc.wavelet_noise",
		"",
		{},
		{{"dimension", Vector2{8, 6}},
		 {"dimension_unit", EnumValue{0}},
		 {"position", Vector2{.13, -.19}},
		 {"position_unit", EnumValue{1}},
		 {"scale", Vector2{4, 7}},
		 {"scale_mapped", true},
		 {"progress", .37},
		 {"progress_mapped", true},
		 {"detail", 1.24},
		 {"detail_mapped", true},
		 {"seed", 17.25}}
	}};
	document.Nodes[0].SourceAnimatedInputs = {"seed", "progress"};
	document.Keyframes = {
		{"generator", "seed", 0, 17.25},
		{"generator", "seed", 1, -17.25},
		{"generator", "progress", 0, .37},
		{"generator", "progress", 1, -.5}
	};
	Node inherited{"copy", "pc.wavelet_noise", "", {}, {}};
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
	}
	EvaluationRequest firstRequest, animatedRequest;
	animatedRequest.Tick = 1;
	Image first, animated;
	REQUIRE(Evaluate(restored, plan, "out", firstRequest, first, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(restored, plan, "out", animatedRequest, animated, diagnostic) == Status::Ok);
	CHECK(first.Pixels != animated.Pixels);
}

TEST_CASE("Wavelet Noise input source indices retain all 15 physical slots", "[source_wavelet_noise]") {
	const auto *entry = FindCatalogueEntry("pc.wavelet_noise");
	REQUIRE(entry);
	constexpr std::array<std::string_view, 15> slots{
		"dimension",
		"position",
		"scale",
		"seed",
		"progress",
		"detail",
		"scale_map",
		"progress_map",
		"detail_map",
		"rotation",
		"mask",
		"uv_map",
		"uv_mix",
		"level_in",
		"level_out"
	};
	for (size_t index = 0; index < slots.size(); ++index) {
		const auto *input = FindCatalogueInput(*entry, slots[index]);
		REQUIRE(input);
		CHECK(input->SourceIndex == int64_t(index));
	}
}

TEST_CASE(
	"Wavelet Noise document getters project Surface values and mapped endpoint pairs",
	"[source_wavelet_noise]"
) {
	Document scalar = WaveletSourceGraph();
	Plan scalarPlan;
	Diagnostic diagnostic;
	REQUIRE(Compile(scalar, scalarPlan, diagnostic) == Status::Ok);
	Image linked;
	REQUIRE(Evaluate(scalar, scalarPlan, "out", {}, linked, diagnostic) == Status::Ok);
	const auto direct = RunNode(
		"pc.wavelet_noise",
		{},
		{{"dimension", Vector2{2, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"position", Vector2{2, 1}},
		 {"position_unit", EnumValue{0}},
		 {"scale", Vector2{2, 1}},
		 {"scale_mapped", true},
		 {"progress_mapped", true},
		 {"detail_mapped", true},
		 {"level_in", Vector2{2, 1}},
		 {"seed", 17.25},
		 {"detail", 1.24},
		 {"attribute_color_depth", EnumValue{5}}}
	);
	INFO(diagnostic.Message << "; " << direct.Message);
	REQUIRE(direct.Ok);
	SamePixels(linked, direct.Output());

	Document progress = WaveletSourceGraph(true);
	Plan progressPlan;
	REQUIRE(Compile(progress, progressPlan, diagnostic) == Status::Ok);
	Image projectedRange;
	REQUIRE(Evaluate(progress, progressPlan, "out", {}, projectedRange, diagnostic) == Status::Ok);
	const auto expected = RunNode(
		"pc.wavelet_noise",
		{},
		{{"dimension", Vector2{2, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"position", Vector2{2, 1}},
		 {"position_unit", EnumValue{0}},
		 {"scale", Vector2{2, 1}},
		 {"scale_mapped", true},
		 {"progress", Vector2{2, 1}},
		 {"progress_mapped", true},
		 {"detail_mapped", true},
		 {"level_in", Vector2{2, 1}},
		 {"seed", 17.25},
		 {"detail", 1.24},
		 {"attribute_color_depth", EnumValue{5}}}
	);
	INFO("progress getter: " << expected.Port << ": " << expected.Message);
	REQUIRE(expected.Ok);
	SamePixels(projectedRange, expected.Output());
}

TEST_CASE("Wavelet Noise quotes selected work before observers or outputs", "[source_wavelet_noise]") {
	const auto *entry = FindCatalogueEntry("pc.wavelet_noise");
	const auto executor = detail::FindExecutor("pc.wavelet_noise");
	REQUIRE(entry);
	REQUIRE(executor);
	Node node{"generator", "pc.wavelet_noise", "", {}, {{"seed", 17.25}}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	SetDefaults(context);
	std::vector<ElementValue> dimensions(2000, Vector2{4, 4});
	SetValue(context, "dimension", ArrayValue{ValueType::Vector2, std::move(dimensions)});
	SetValue(context, "dimension_unit", EnumValue{0});
	SetValue(context, "scale_mapped", true);
	SetValue(context, "progress_mapped", true);
	SetValue(context, "detail_mapped", true);
	SetValue(context, "detail", 1.24);
	SetValue(context, "seed", 17.25);
	SetValue(context, "attribute_array_process", EnumValue{0});
	context.InputProvenanceResolved = true;
	for (const auto &[port, value] : context.Values)
		context.ValueViews.emplace_back(port, &value);
	context.ProcessorCount = 2000;
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

TEST_CASE(
	"Wavelet Noise source rows follow Loop, Hold, Expand, and inverse schedules", "[source_wavelet_noise]"
) {
	const auto *entry = FindCatalogueEntry("pc.wavelet_noise");
	const auto executor = detail::FindExecutor("pc.wavelet_noise");
	REQUIRE(entry);
	REQUIRE(executor);
	const Image uvA = FloatPixel({.25, .2, 0, .6});
	const Image uvB = FloatPixel({.75, .8, 0, .4});
	ImageArray uvRows;
	uvRows.Images = {uvA, uvB};
	uvRows.Items = {{size_t{0}}, {size_t{1}}};
	for (int64_t mode = 0; mode < 4; ++mode) {
		Node node{"generator", "pc.wavelet_noise", "", {}, {{"seed", 17.25}}};
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		SetDefaults(context);
		SetValue(context, "dimension", ArrayValue{ValueType::Vector2, {Vector2{4, 3}, Vector2{5, 3}}});
		SetValue(context, "dimension_unit", EnumValue{0});
		SetValue(context, "position", Vector2{.13, -.19});
		SetValue(context, "position_unit", EnumValue{0});
		SetValue(context, "scale", Vector2{4, 7});
		SetValue(context, "scale_mapped", true);
		SetValue(context, "progress_mapped", true);
		SetValue(context, "detail_mapped", true);
		SetValue(context, "attribute_color_depth", EnumValue{5});
		SetValue(context, "seed", ArrayValue{ValueType::Scalar, {17.25, -17.25}});
		SetValue(context, "progress", ArrayValue{ValueType::Vector2, {Vector2{.2, .2}, Vector2{.8, .8}}});
		SetValue(
			context,
			"detail",
			ArrayValue{ValueType::Vector2, {Vector2{.8, .8}, Vector2{1.4, 1.4}, Vector2{1.8, 1.8}}}
		);
		SetValue(context, "rotation", ArrayValue{ValueType::Scalar, {-15.0, 33.0}});
		SetValue(context, "attribute_array_process", EnumValue{mode});
		context.ImageArrays.emplace_back("uv_map", &uvRows);
		context.InputProvenanceResolved = true;
		for (const auto &[port, value] : context.Values)
			context.ValueViews.emplace_back(port, &value);
		size_t observed = 0;
		const bool ran = detail::RunProcessorBatch(context, executor, Observe, &observed);
		INFO(mode << ": " << context.FailurePort << ": " << context.FailureMessage);
		REQUIRE(ran);
		REQUIRE(context.OutputImageArrays.size() == 1);
		const auto &images = context.OutputImageArrays.front().second.Images;
		const size_t expectedRows = mode < 2 ? 3 : 96;
		REQUIRE(images.size() == expectedRows);
		CHECK(observed == expectedRows);
		for (size_t row = 0; row < images.size(); ++row) {
			CAPTURE(mode, row);
			// Inverse schedules use the mirrored suffix of all 15 physical source slots.
			const size_t dimensionIndex = mode == 0	  ? row % 2
										  : mode == 1 ? std::min(row, size_t{1})
										  : mode == 2 ? row / 48
													  : SourceInverseIndex(row, 0);
			const size_t seedIndex = mode == 0	 ? row % 2
									 : mode == 1 ? std::min(row, size_t{1})
									 : mode == 2 ? row / 24 % 2
												 : SourceInverseIndex(row, 3);
			const size_t progressIndex = mode == 0	 ? row % 2
										 : mode == 1 ? std::min(row, size_t{1})
										 : mode == 2 ? row / 12 % 2
													 : SourceInverseIndex(row, 4);
			const size_t detailIndex = mode == 0   ? row % 3
									   : mode == 1 ? row
									   : mode == 2 ? row / 4 % 3
												   : SourceInverseIndex(row, 5);
			const size_t rotationIndex = mode == 0	 ? row % 2
										 : mode == 1 ? std::min(row, size_t{1})
										 : mode == 2 ? row / 2 % 2
													 : SourceInverseIndex(row, 9);
			const size_t uvIndex = mode == 0   ? row % 2
								   : mode == 1 ? std::min(row, size_t{1})
								   : mode == 2 ? row % 2
											   : SourceInverseIndex(row, 11);
			const auto expected = RunNode(
				"pc.wavelet_noise",
				{{"uv_map", uvIndex == 0 ? &uvA : &uvB}},
				{{"dimension", dimensionIndex == 0 ? Vector2{4, 3} : Vector2{5, 3}},
				 {"dimension_unit", EnumValue{0}},
				 {"position", Vector2{.13, -.19}},
				 {"position_unit", EnumValue{0}},
				 {"scale", Vector2{4, 7}},
				 {"scale_mapped", true},
				 {"progress", progressIndex == 0 ? Vector2{.2, .2} : Vector2{.8, .8}},
				 {"progress_mapped", true},
				 {"detail",
				  detailIndex == 0	 ? Vector2{.8, .8}
				  : detailIndex == 1 ? Vector2{1.4, 1.4}
									 : Vector2{1.8, 1.8}},
				 {"detail_mapped", true},
				 {"seed", seedIndex == 0 ? 17.25 : -17.25},
				 {"rotation", rotationIndex == 0 ? -15.0 : 33.0},
				 {"uv_mix", 1.0},
				 {"attribute_color_depth", EnumValue{5}}}
			);
			INFO(expected.Port << ": " << expected.Message);
			REQUIRE(expected.Ok);
			CHECK(images[row].Width == expected.Output().Width);
			CHECK(images[row].Height == expected.Output().Height);
			CHECK(images[row].Format == expected.Output().Format);
			CHECK(images[row].Pixels == expected.Output().Pixels);
		}
	}
}

TEST_CASE(
	"Wavelet Noise mask overflow refuses the selected batch before observation", "[source_wavelet_noise]"
) {
	const auto *entry = FindCatalogueEntry("pc.wavelet_noise");
	const auto executor = detail::FindExecutor("pc.wavelet_noise");
	REQUIRE(entry);
	REQUIRE(executor);
	const Image uv{1, 1, std::vector<uint8_t>(16), 0, SurfaceFormat::RGBA32Float};
	Image hugeUv = uv;
	Image mask = uv;
	REQUIRE(StoreSurfacePixel(hugeUv, 0, 0, {.5, .5, 0, 3.0e38}));
	REQUIRE(StoreSurfacePixel(mask, 0, 0, {10, 10, 10, 1}));
	Node node{"generator", "pc.wavelet_noise", "", {}, {{"seed", 17.25}}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.Images.emplace_back("uv_map", &hugeUv);
	context.Images.emplace_back("mask", &mask);
	SetDefaults(context);
	SetValue(context, "scale_mapped", true);
	SetValue(context, "progress_mapped", true);
	SetValue(context, "detail_mapped", true);
	SetValue(context, "detail", 1.24);
	SetValue(context, "seed", 17.25);
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

TEST_CASE("Wavelet Noise refuses byte exhaustion before observers or outputs", "[source_wavelet_noise]") {
	const auto *entry = FindCatalogueEntry("pc.wavelet_noise");
	const auto executor = detail::FindExecutor("pc.wavelet_noise");
	REQUIRE(entry);
	REQUIRE(executor);
	Node node{"generator", "pc.wavelet_noise", "", {}, {{"seed", 17.25}}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = 1;
	SetDefaults(context);
	SetValue(context, "dimension", Vector2{1, 1});
	SetValue(context, "dimension_unit", EnumValue{0});
	SetValue(context, "scale_mapped", true);
	SetValue(context, "progress_mapped", true);
	SetValue(context, "detail_mapped", true);
	SetValue(context, "detail", 1.24);
	SetValue(context, "seed", 17.25);
	SetValue(context, "attribute_color_depth", EnumValue{5});
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
