#include "../src/ProcessorBatch.hpp"
#include "NodeHarness.hpp"

#include <engine/imagegraph/Surface.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.source_voronoi_extra_batch")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Document VoronoiExtraGraph() {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {Node{
			"generator",
			"pc.voronoi_extra",
			"",
			{},
			{{"dimension", Vector2{2, 2}},
			 {"dimension_unit", EnumValue{0}},
			 {"position", Vector2{.31, .17}},
			 {"position_unit", EnumValue{0}},
			 {"scale", Vector2{7.7, 6.3}},
			 {"seed", 17.0},
			 {"mode", EnumValue{1}},
			 {"progress", .37}}
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
	SurfacePixel Pixel(const Image &image, uint32_t x = 0, uint32_t y = 0) {
		SurfacePixel pixel{};
		REQUIRE(LoadSurfacePixel(image, x, y, pixel));
		return pixel;
	}
}

TEST_CASE("Extra Voronoi animation and inherited instances survive Format9", "[source_2d][voronoi_extra]") {
	auto document = VoronoiExtraGraph();
	document.Nodes[0].SourceAnimatedInputs = {"seed", "progress"};
	document.Keyframes = {
		{"generator", "seed", 0, 17.0},
		{"generator", "seed", 1, 31.0},
		{"generator", "seed", 2, 31.0},
		{"generator", "progress", 0, .37},
		{"generator", "progress", 1, .37},
		{"generator", "progress", 2, .83}
	};
	Node instance{"copy", "pc.voronoi_extra", "", {}, {}};
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
	Image first, seedAnimated, progressAnimated;
	REQUIRE(Draw(restored, "out", 0, first, diagnostic) == Status::Ok);
	REQUIRE(Draw(restored, "out", 1, seedAnimated, diagnostic) == Status::Ok);
	REQUIRE(Draw(restored, "out", 2, progressAnimated, diagnostic) == Status::Ok);
	CHECK(first.Pixels != seedAnimated.Pixels);
	CHECK(seedAnimated.Pixels != progressAnimated.Pixels);
}

TEST_CASE("Extra Voronoi source mode arrays retain hidden shader branches", "[source_2d][voronoi_extra]") {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {Node{
		"generator",
		"pc.voronoi_extra",
		"",
		{},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"position", Vector2{0, 0}},
		 {"position_unit", EnumValue{0}},
		 {"scale", Vector2{4, 4}},
		 {"seed", 17.25},
		 {"progress", 0.0},
		 {"parameter_a", 0.0},
		 {"rotation", 0.0},
		 {"level_in", Vector2{0, 1}},
		 {"level_out", Vector2{0, 1}},
		 {"tile", false},
		 {"attribute_color_depth", EnumValue{5}}}
	}};
	Node modeRows{"modes", "pc.array", "", {}, {{"type", EnumValue{0}}}};
	modeRows.DynamicInputs = {{"input_0", ValueType::Scalar, 2.0}, {"input_1", ValueType::Scalar, 3.0}};
	document.Nodes.push_back(std::move(modeRows));
	document.Links = {{"modes", "array", "generator", "mode"}};
	document.Outputs = {{"out", "generator", "surface_out"}};
	Plan plan;
	Diagnostic diagnostic;
	const Status compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.NodeId << ":" << diagnostic.Port << ": " << diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	ImageArray images;
	const Status status = EvaluateArray(document, plan, "out", {}, images, diagnostic);
	INFO(diagnostic.NodeId << ":" << diagnostic.Port << ": " << diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(images.Images.size() == 2);
	const SurfacePixel square = Pixel(images.Images[0]);
	const SurfacePixel defaultMode = Pixel(images.Images[1]);
	for (size_t channel = 0; channel < 3; ++channel) {
		CHECK(square[channel] == Catch::Approx(.5).margin(5e-5));
		CHECK(defaultMode[channel] == Catch::Approx(0).margin(1e-6));
	}
	CHECK(square[3] == Catch::Approx(1).margin(1e-6));
	CHECK(defaultMode[3] == Catch::Approx(1).margin(1e-6));
}

TEST_CASE("Extra Voronoi off-center source-array mode 2 uses Square shader", "[source_2d][voronoi_extra]") {
	const auto *entry = FindCatalogueEntry("pc.voronoi_extra");
	const auto executor = detail::FindExecutor("pc.voronoi_extra");
	REQUIRE(entry);
	REQUIRE(executor);
	Image uv{1, 1, std::vector<uint8_t>(16), 0, SurfaceFormat::RGBA32Float};
	REQUIRE(StoreSurfacePixel(uv, 0, 0, {.3125, .3125, 0, 1}));
	Node node{"generator", "pc.voronoi_extra", "", {}, {{"seed", -17.25}}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	for (const auto &input : entry->Inputs)
		if (const auto value = CatalogueDefault(input)) context.Values.emplace_back(input.Id, *value);
	SetValue(context, "dimension", Vector2{3, 2});
	SetValue(context, "dimension_unit", EnumValue{0});
	SetValue(context, "position", Vector2{.1, -.2});
	SetValue(context, "position_unit", EnumValue{0});
	SetValue(context, "scale", Vector2{9.25, 11.75});
	SetValue(context, "seed", -17.25);
	SetValue(context, "mode", ArrayValue{ValueType::Enum, {EnumValue{2}}});
	SetValue(context, "progress", .375);
	SetValue(context, "parameter_a", .65);
	SetValue(context, "rotation", 37.0);
	SetValue(context, "tile", false);
	SetValue(context, "uv_mix", 1.0);
	SetValue(context, "attribute_color_depth", EnumValue{5});
	context.Images.emplace_back("uv_map", &uv);
	context.InputProvenanceResolved = true;
	for (const auto &[port, value] : context.Values)
		context.ValueViews.emplace_back(port, &value);
	size_t observed = 0;
	REQUIRE(detail::RunProcessorBatch(context, executor, Observe, &observed));
	REQUIRE(observed == 1);
	REQUIRE(context.OutputImages.size() == 1);
	const SurfacePixel pixel = Pixel(context.OutputImages.front().second);
	for (size_t channel = 0; channel < 3; ++channel)
		CHECK(pixel[channel] == Catch::Approx(.042911265045404434).margin(5e-5));

	Node fractionalNode{"fractional", "pc.voronoi_extra", "", {}, {{"seed", 17.25}}};
	detail::NodeContext fractional(fractionalNode, *entry, request);
	fractional.ByteBudget = Limits::MaximumEvaluationBytes;
	for (const auto &input : entry->Inputs)
		if (const auto value = CatalogueDefault(input)) fractional.Values.emplace_back(input.Id, *value);
	SetValue(fractional, "dimension", Vector2{1.6, 2.6});
	SetValue(fractional, "dimension_unit", EnumValue{0});
	SetValue(fractional, "position", Vector2{0, 0});
	SetValue(fractional, "position_unit", EnumValue{0});
	SetValue(fractional, "scale", Vector2{5.5, 7.25});
	SetValue(fractional, "seed", 17.25);
	SetValue(fractional, "mode", ArrayValue{ValueType::Enum, {EnumValue{2}}});
	SetValue(fractional, "progress", .25);
	SetValue(fractional, "parameter_a", -.4);
	SetValue(fractional, "tile", false);
	SetValue(fractional, "attribute_color_depth", EnumValue{5});
	SetValue(fractional, "level_in", Vector2{.2, .8});
	SetValue(fractional, "level_out", Vector2{-.25, 1.25});
	fractional.InputProvenanceResolved = true;
	for (const auto &[port, value] : fractional.Values)
		fractional.ValueViews.emplace_back(port, &value);
	observed = 0;
	REQUIRE(detail::RunProcessorBatch(fractional, executor, Observe, &observed));
	REQUIRE(observed == 1);
	REQUIRE(fractional.OutputImages.size() == 1);
	CHECK(fractional.OutputImages.front().second.Width == 2);
	CHECK(fractional.OutputImages.front().second.Height == 3);
	const SurfacePixel fractionalPixel = Pixel(fractional.OutputImages.front().second, 0, 1);
	for (size_t channel = 0; channel < 3; ++channel)
		CHECK(fractionalPixel[channel] == Catch::Approx(.1124168336391449).margin(5e-5));
}

TEST_CASE("Extra Voronoi refuses a Square sample without a winner", "[source_2d][voronoi_extra]") {
	const auto *entry = FindCatalogueEntry("pc.voronoi_extra");
	const auto executor = detail::FindExecutor("pc.voronoi_extra");
	REQUIRE(entry);
	REQUIRE(executor);
	Node node{"generator", "pc.voronoi_extra", "", {}, {{"seed", 17.25}}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	for (const auto &input : entry->Inputs)
		if (const auto value = CatalogueDefault(input)) context.Values.emplace_back(input.Id, *value);
	SetValue(context, "dimension", Vector2{1, 1});
	SetValue(context, "dimension_unit", EnumValue{0});
	SetValue(context, "position", Vector2{0, 0});
	SetValue(context, "position_unit", EnumValue{0});
	SetValue(context, "scale", Vector2{4, 4});
	SetValue(context, "seed", 17.25);
	SetValue(context, "mode", ArrayValue{ValueType::Enum, {EnumValue{2}}});
	SetValue(context, "parameter_a", 100.0);
	context.InputProvenanceResolved = true;
	for (const auto &[port, value] : context.Values)
		context.ValueViews.emplace_back(port, &value);
	size_t observed = 0;
	CHECK_FALSE(detail::RunProcessorBatch(context, executor, Observe, &observed));
	INFO(context.FailurePort << ":" << context.FailureMessage);
	CHECK(context.FailureCode == Status::UnsupportedExecution);
	CHECK(context.FailurePort == "parameter_a");
	CHECK(observed == 0);
	CHECK(context.OutputImages.empty());
	CHECK(context.OutputImageArrays.empty());
}

TEST_CASE("Extra Voronoi array schedules select native scalar and image rows", "[source_2d][voronoi_extra]") {
	const auto *entry = FindCatalogueEntry("pc.voronoi_extra");
	const auto executor = detail::FindExecutor("pc.voronoi_extra");
	REQUIRE(entry);
	REQUIRE(executor);
	const std::array<std::string_view, 14> nativeSlots{
		{"dimension",
		 "position",
		 "scale",
		 "seed",
		 "progress",
		 "mode",
		 "parameter_a",
		 "rotation",
		 "mask",
		 "uv_map",
		 "uv_mix",
		 "level_in",
		 "tile",
		 "level_out"}
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
		Node node{"generator", "pc.voronoi_extra", "", {}, {}};
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		SetDefaults(context);
		SetValue(context, "dimension", ArrayValue{ValueType::Vector2, {Vector2{1, 1}, Vector2{2, 1}}});
		SetValue(context, "dimension_unit", EnumValue{0});
		SetValue(context, "position", ArrayValue{ValueType::Vector2, {Vector2{.17, .31}, Vector2{.63, .28}}});
		SetValue(context, "position_unit", EnumValue{0});
		SetValue(context, "scale", Vector2{7.7, 6.3});
		SetValue(context, "seed", 17.0);
		SetValue(context, "mode", EnumValue{1});
		SetValue(context, "progress", ArrayValue{ValueType::Scalar, {.37, .83}});
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
			const size_t dimensionIndex = mode < 2 ? row : mode == 2 ? row / 8 : row % 2;
			const size_t positionIndex = mode < 2 ? row : mode == 2 ? row / 4 % 2 : row % 2;
			const size_t progressIndex = mode < 2 ? row : mode == 2 ? row / 2 % 2 : row % 2;
			const size_t uvIndex = mode < 2 ? row : mode == 2 ? row % 2 : row / 2 % 2;
			const Vector2 dimension = dimensionIndex == 0 ? Vector2{1, 1} : Vector2{2, 1};
			const Vector2 position = positionIndex == 0 ? Vector2{.17, .31} : Vector2{.63, .28};
			const double progress = progressIndex == 0 ? .37 : .83;
			const Image &uv = uvIndex == 0 ? uv0 : uv1;
			const auto expected = RunNode(
				"pc.voronoi_extra",
				{{"uv_map", &uv}},
				{{"dimension", dimension},
				 {"dimension_unit", EnumValue{0}},
				 {"position", position},
				 {"position_unit", EnumValue{0}},
				 {"scale", Vector2{7.7, 6.3}},
				 {"seed", 17.0},
				 {"mode", EnumValue{1}},
				 {"progress", progress},
				 {"uv_mix", 1.0}}
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
	"Extra Voronoi refuses cumulative work before observing or publishing", "[source_2d][voronoi_extra]"
) {
	const auto *entry = FindCatalogueEntry("pc.voronoi_extra");
	const auto executor = detail::FindExecutor("pc.voronoi_extra");
	REQUIRE(entry);
	REQUIRE(executor);
	Node node{"generator", "pc.voronoi_extra", "", {}, {{"seed", 17.25}}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	SetDefaults(context);
	std::vector<ElementValue> dimensions(70, Vector2{7, 7});
	SetValue(context, "dimension", ArrayValue{ValueType::Vector2, std::move(dimensions)});
	SetValue(context, "dimension_unit", EnumValue{0});
	SetValue(context, "seed", 17.25);
	SetValue(context, "mode", EnumValue{1});
	SetValue(context, "scale", Vector2{4, 4});
	SetValue(context, "tile", true);
	SetValue(context, "attribute_array_process", EnumValue{0});
	context.InputProvenanceResolved = true;
	for (const auto &[port, value] : context.Values)
		context.ValueViews.emplace_back(port, &value);
	context.ProcessorCount = 70;
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
