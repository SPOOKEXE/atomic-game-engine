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

TEST_SUITE_ID("engine.imagegraph.source_scratch_noise_batch")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	constexpr std::array<size_t, 18> SOURCE_SLOT_LENGTHS{
		2, 2, 1, 1, 2, 1, 1, 1, 2, 1, 1, 1, 1, 3, 1, 1, 1, 1
	};
	size_t SourceInverseIndex(size_t row, size_t inputIndex) {
		std::array<size_t, SOURCE_SLOT_LENGTHS.size()> suffix{};
		size_t product = 1;
		for (size_t index = SOURCE_SLOT_LENGTHS.size(); index > 0; --index) {
			suffix[index - 1] = product;
			product *= SOURCE_SLOT_LENGTHS[index - 1];
		}
		return row / suffix[SOURCE_SLOT_LENGTHS.size() - 1 - inputIndex] % SOURCE_SLOT_LENGTHS[inputIndex];
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
	Document ScratchGraph() {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {Node{
			"generator",
			"pc.noise_scratch",
			"",
			{},
			{{"dimension", Vector2{8, 6}},
			 {"dimension_unit", EnumValue{0}},
			 {"position", Vector2{.13, -.19}},
			 {"position_unit", EnumValue{0}},
			 {"rotation", 23.0},
			 {"scale", Vector2{.7, .4}},
			 {"seed", 17.25},
			 {"thickness", .23},
			 {"thickness_mapped", true},
			 {"wavyness", .61},
			 {"wavyness_mapped", true},
			 {"softness", .42},
			 {"softness_mapped", true},
			 {"octaves", int64_t{3}},
			 {"octave_scale", .8},
			 {"octave_shift", Vector2{.3, -.2}},
			 {"octave_rotation", 30.0},
			 {"attribute_color_depth", EnumValue{5}}}
		}};
		document.Nodes[0].SourceAnimatedInputs = {"seed", "octave_scale"};
		document.Keyframes = {
			{"generator", "seed", 0, 17.25},
			{"generator", "seed", 1, -17.25},
			{"generator", "octave_scale", 0, .8},
			{"generator", "octave_scale", 1, 1.4}
		};
		Node inherited{"copy", "pc.noise_scratch", "", {}, {}};
		inherited.InstanceBase = "generator";
		document.Nodes.push_back(std::move(inherited));
		document.Outputs = {{"out", "generator", "surface_out"}, {"copy", "copy", "surface_out"}};
		return document;
	}
	Document ScratchGetterGraph() {
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
				"pc.noise_scratch",
				"",
				{},
				{{"dimension_unit", EnumValue{0}},
				 {"position_unit", EnumValue{0}},
				 {"seed", 17.25},
				 {"thickness_mapped", true},
				 {"wavyness", .61},
				 {"wavyness_mapped", true},
				 {"softness", .42},
				 {"softness_mapped", true},
				 {"octave_scale", .8},
				 {"octave_rotation", 30.0},
				 {"attribute_color_depth", EnumValue{5}}}
			}
		};
		document.Links = {
			{"source", "surface_out", "generator", "dimension"},
			{"source", "surface_out", "generator", "position"},
			{"source", "surface_out", "generator", "scale"},
			{"source", "surface_out", "generator", "octave_shift"},
			{"source", "surface_out", "generator", "thickness"},
			{"source", "surface_out", "generator", "octaves"}
		};
		document.Outputs = {{"out", "generator", "surface_out"}};
		return document;
	}
	Status Draw(
		const Document &document, std::string_view output, uint64_t tick, Image &image, Diagnostic &diagnostic
	) {
		Plan plan;
		const Status compiled = Compile(document, plan, diagnostic);
		if (compiled != Status::Ok) return compiled;
		EvaluationRequest request;
		request.Tick = tick;
		return Evaluate(document, plan, std::string(output), request, image, diagnostic);
	}
}

TEST_CASE("Scratch Noise animation and inherited instances survive Format9", "[source_scratch_noise]") {
	auto document = ScratchGraph();
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	REQUIRE(restored == document);
	for (const uint64_t tick : {uint64_t{0}, uint64_t{1}}) {
		Image source, copy;
		REQUIRE(Draw(restored, "out", tick, source, diagnostic) == Status::Ok);
		REQUIRE(Draw(restored, "copy", tick, copy, diagnostic) == Status::Ok);
		SamePixels(copy, source);
	}
	Image first, seedAnimated, scaleAnimated;
	REQUIRE(Draw(restored, "out", 0, first, diagnostic) == Status::Ok);
	REQUIRE(Draw(restored, "out", 1, seedAnimated, diagnostic) == Status::Ok);
	CHECK(first.Pixels != seedAnimated.Pixels);

	document.Keyframes[1].Data = 17.25;
	document.Keyframes[3].Data = 1.4;
	Document scaleOnly;
	REQUIRE(Read(Write(document), scaleOnly, diagnostic) == Status::Ok);
	REQUIRE(Draw(scaleOnly, "out", 0, first, diagnostic) == Status::Ok);
	REQUIRE(Draw(scaleOnly, "out", 1, scaleAnimated, diagnostic) == Status::Ok);
	CHECK(first.Pixels != scaleAnimated.Pixels);
}

TEST_CASE("Scratch Noise consumes Surface getters for controls and octaves", "[source_scratch_noise]") {
	const Document document = ScratchGetterGraph();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	ImageArray linked;
	REQUIRE(EvaluateArray(document, plan, "out", {}, linked, diagnostic) == Status::Ok);
	REQUIRE(linked.Images.size() == 2);
	const std::array<int64_t, 2> octaveRows{2, 1};
	for (size_t row = 0; row < linked.Images.size(); ++row) {
		CAPTURE(row);
		const auto direct = RunNode(
			"pc.noise_scratch",
			{},
			{{"dimension", Vector2{2, 1}},
			 {"dimension_unit", EnumValue{0}},
			 {"position", Vector2{2, 1}},
			 {"position_unit", EnumValue{0}},
			 {"scale", Vector2{2, 1}},
			 {"seed", 17.25},
			 {"thickness", Vector2{2, 1}},
			 {"thickness_mapped", true},
			 {"wavyness", .61},
			 {"wavyness_mapped", true},
			 {"softness", .42},
			 {"softness_mapped", true},
			 {"octaves", octaveRows[row]},
			 {"octave_scale", .8},
			 {"octave_shift", Vector2{2, 1}},
			 {"octave_rotation", 30.0},
			 {"attribute_color_depth", EnumValue{5}}}
		);
		INFO(diagnostic.Message << "; " << direct.Port << ": " << direct.Message);
		REQUIRE(direct.Ok);
		SamePixels(linked.Images[row], direct.Output());
	}
}

TEST_CASE("Scratch Noise input arrays follow all four source schedules", "[source_scratch_noise]") {
	const CatalogueEntry *entry = FindCatalogueEntry("pc.noise_scratch");
	const detail::Executor executor = detail::FindExecutor("pc.noise_scratch");
	REQUIRE(entry);
	REQUIRE(executor);
	const std::array<std::string_view, 18> slots{
		"dimension",
		"uv_map",
		"uv_mix",
		"mask",
		"seed",
		"thickness",
		"wavyness",
		"softness",
		"octaves",
		"octave_scale",
		"position",
		"rotation",
		"scale",
		"octave_shift",
		"octave_rotation",
		"thickness_map",
		"wavyness_map",
		"softness_map"
	};
	for (size_t index = 0; index < slots.size(); ++index) {
		const auto *input = FindCatalogueInput(*entry, slots[index]);
		REQUIRE(input);
		CHECK(input->SourceIndex == static_cast<int64_t>(index));
	}
	const Image uvA = FloatPixel({.25, .2, 0, .6});
	const Image uvB = FloatPixel({.75, .8, 0, .4});
	ImageArray uvRows;
	uvRows.Images = {uvA, uvB};
	uvRows.Items = {{size_t{0}}, {size_t{1}}};
	const std::array<Vector2, 2> dimensions{Vector2{2, 1}, Vector2{3, 1}};
	const std::array<double, 2> seeds{17.25, -17.25};
	const std::array<int64_t, 2> octaves{2, 3};
	const std::array<Vector2, 3> shifts{Vector2{0, 0}, Vector2{.3, -.2}, Vector2{.6, .1}};
	for (int64_t mode = 0; mode < 4; ++mode) {
		Node node{"generator", "pc.noise_scratch", "", {}, {}};
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		SetDefaults(context);
		SetValue(context, "dimension", ArrayValue{ValueType::Vector2, {dimensions[0], dimensions[1]}});
		SetValue(context, "dimension_unit", EnumValue{0});
		SetValue(context, "position", Vector2{.13, -.19});
		SetValue(context, "position_unit", EnumValue{0});
		SetValue(context, "rotation", 23.0);
		SetValue(context, "scale", Vector2{.7, .4});
		SetValue(context, "seed", ArrayValue{ValueType::Scalar, {seeds[0], seeds[1]}});
		SetValue(context, "thickness", .23);
		SetValue(context, "thickness_mapped", true);
		SetValue(context, "wavyness", .61);
		SetValue(context, "wavyness_mapped", true);
		SetValue(context, "softness", .42);
		SetValue(context, "softness_mapped", true);
		SetValue(context, "octaves", ArrayValue{ValueType::Integer, {octaves[0], octaves[1]}});
		SetValue(context, "octave_scale", .8);
		SetValue(context, "octave_shift", ArrayValue{ValueType::Vector2, {shifts[0], shifts[1], shifts[2]}});
		SetValue(context, "octave_rotation", 30.0);
		SetValue(context, "uv_mix", .25);
		SetValue(context, "attribute_color_depth", EnumValue{5});
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
		const size_t expectedRows = mode < 2 ? 3 : 48;
		REQUIRE(images.size() == expectedRows);
		CHECK(observed == expectedRows);
		for (size_t row = 0; row < images.size(); ++row) {
			CAPTURE(mode, row);
			const size_t dimensionIndex = mode == 0	  ? row % 2
										  : mode == 1 ? std::min(row, size_t{1})
										  : mode == 2 ? row / 24
													  : SourceInverseIndex(row, 0);
			const size_t uvIndex = mode == 0   ? row % 2
								   : mode == 1 ? std::min(row, size_t{1})
								   : mode == 2 ? row / 12 % 2
											   : SourceInverseIndex(row, 1);
			const size_t seedIndex = mode == 0	 ? row % 2
									 : mode == 1 ? std::min(row, size_t{1})
									 : mode == 2 ? row / 6 % 2
												 : SourceInverseIndex(row, 4);
			const size_t octaveIndex = mode == 0   ? row % 2
									   : mode == 1 ? std::min(row, size_t{1})
									   : mode == 2 ? row / 3 % 2
												   : SourceInverseIndex(row, 8);
			const size_t shiftIndex = mode == 0	  ? row % 3
									  : mode == 1 ? row
									  : mode == 2 ? row % 3
												  : SourceInverseIndex(row, 13);
			const auto expected = RunNode(
				"pc.noise_scratch",
				{{"uv_map", uvIndex == 0 ? &uvA : &uvB}},
				{{"dimension", dimensions[dimensionIndex]},
				 {"dimension_unit", EnumValue{0}},
				 {"position", Vector2{.13, -.19}},
				 {"position_unit", EnumValue{0}},
				 {"rotation", 23.0},
				 {"scale", Vector2{.7, .4}},
				 {"seed", seeds[seedIndex]},
				 {"thickness", .23},
				 {"thickness_mapped", true},
				 {"wavyness", .61},
				 {"wavyness_mapped", true},
				 {"softness", .42},
				 {"softness_mapped", true},
				 {"octaves", octaves[octaveIndex]},
				 {"octave_scale", .8},
				 {"octave_shift", shifts[shiftIndex]},
				 {"octave_rotation", 30.0},
				 {"uv_mix", .25},
				 {"attribute_color_depth", EnumValue{5}}}
			);
			INFO(expected.Port << ": " << expected.Message);
			REQUIRE(expected.Ok);
			SamePixels(images[row], expected.Output());
		}
	}
}

TEST_CASE("Scratch Noise admits whole work before observers or outputs", "[source_scratch_noise]") {
	const CatalogueEntry *entry = FindCatalogueEntry("pc.noise_scratch");
	const detail::Executor executor = detail::FindExecutor("pc.noise_scratch");
	REQUIRE(entry);
	REQUIRE(executor);
	Node node{"generator", "pc.noise_scratch", "", {}, {{"seed", 17.25}}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	SetDefaults(context);
	std::vector<ElementValue> dimensions(1000, Vector2{1, 1});
	SetValue(context, "dimension", ArrayValue{ValueType::Vector2, std::move(dimensions)});
	SetValue(context, "dimension_unit", EnumValue{0});
	SetValue(context, "seed", 17.25);
	SetValue(context, "thickness_mapped", true);
	SetValue(context, "wavyness_mapped", true);
	SetValue(context, "softness_mapped", true);
	SetValue(context, "octaves", int64_t{8});
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
	CHECK(context.FailureMessage.find("work") != std::string::npos);
	CHECK(observed == 0);
	CHECK(context.OutputImages.empty());
	CHECK(context.OutputImageArrays.empty());
}

TEST_CASE("Scratch Noise mask overflow refuses before publishing images", "[source_scratch_noise]") {
	const CatalogueEntry *entry = FindCatalogueEntry("pc.noise_scratch");
	const detail::Executor executor = detail::FindExecutor("pc.noise_scratch");
	REQUIRE(entry);
	REQUIRE(executor);
	const Image uv = FloatPixel({.5, .2, 0, 3.0e38});
	const Image mask = FloatPixel({10, 10, 10, 1});
	Node node{"generator", "pc.noise_scratch", "", {}, {{"seed", 17.25}}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.Images.emplace_back("uv_map", &uv);
	context.Images.emplace_back("mask", &mask);
	SetDefaults(context);
	SetValue(context, "dimension", Vector2{8, 6});
	SetValue(context, "dimension_unit", EnumValue{0});
	SetValue(context, "seed", 17.25);
	SetValue(context, "thickness_mapped", true);
	SetValue(context, "wavyness_mapped", true);
	SetValue(context, "softness_mapped", true);
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

TEST_CASE("Scratch Noise refuses byte exhaustion before observers or outputs", "[source_scratch_noise]") {
	const CatalogueEntry *entry = FindCatalogueEntry("pc.noise_scratch");
	const detail::Executor executor = detail::FindExecutor("pc.noise_scratch");
	REQUIRE(entry);
	REQUIRE(executor);
	Node node{"generator", "pc.noise_scratch", "", {}, {{"seed", 17.25}}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = 1;
	SetDefaults(context);
	SetValue(context, "dimension", Vector2{1, 1});
	SetValue(context, "dimension_unit", EnumValue{0});
	SetValue(context, "seed", 17.25);
	SetValue(context, "thickness_mapped", true);
	SetValue(context, "wavyness_mapped", true);
	SetValue(context, "softness_mapped", true);
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
