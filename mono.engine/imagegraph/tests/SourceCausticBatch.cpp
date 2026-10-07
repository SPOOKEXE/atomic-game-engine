#include "../src/ProcessorBatch.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.source_caustic_batch")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Document CausticGraph() {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {Node{
			"generator",
			"pc.caustic",
			"",
			{},
			{{"dimension", Vector2{8, 8}},
			 {"dimension_unit", EnumValue{0}},
			 {"seed", 0.0},
			 {"progress", 0.0},
			 {"progress_mapped", true},
			 {"progress_map_range", Vector2{0, 0}},
			 {"intensity", 1.0},
			 {"intensity_mapped", true},
			 {"intensity_map_range", Vector2{1, 1}},
			 {"detail", int64_t{2}}}
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
	Image White() {
		return Image{1, 1, {255, 255, 255, 255}, 0};
	}
}

TEST_CASE("Caustic animation and inherited instances survive Format9", "[source_2d][caustic]") {
	auto document = CausticGraph();
	document.Nodes[0].SourceAnimatedInputs = {"seed", "progress"};
	document.Keyframes = {
		{"generator", "seed", 0, 0.0},
		{"generator", "seed", 1, 17.25},
		{"generator", "seed", 2, 17.25},
		{"generator", "progress", 0, 0.0},
		{"generator", "progress", 1, 0.0},
		{"generator", "progress", 2, 0.75}
	};
	Node instance{"copy", "pc.caustic", "", {}, {}};
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
		CHECK(inherited.Pixels == source.Pixels);
		CHECK(inherited.Format == source.Format);
	}
	Image first, seedAnimated, progressAnimated;
	REQUIRE(Draw(restored, "out", 0, first, diagnostic) == Status::Ok);
	REQUIRE(Draw(restored, "out", 1, seedAnimated, diagnostic) == Status::Ok);
	REQUIRE(Draw(restored, "out", 2, progressAnimated, diagnostic) == Status::Ok);
	CHECK(first.Pixels != seedAnimated.Pixels);
	CHECK(seedAnimated.Pixels != progressAnimated.Pixels);
}

TEST_CASE("Caustic batch schedules use all native source slots", "[source_2d][caustic]") {
	const auto *entry = FindCatalogueEntry("pc.caustic");
	const auto executor = detail::FindExecutor("pc.caustic");
	REQUIRE(entry);
	REQUIRE(executor);
	const Image white = White();
	const Image uv{1, 1, {191, 73, 0, 255}, 0};
	ImageArray uvImages;
	uvImages.Images.push_back(uv);
	uvImages.Items.push_back({size_t{0}});
	ImageArray whiteImages;
	whiteImages.Images.push_back(white);
	whiteImages.Items.push_back({size_t{0}});
	const std::array<std::string_view, 12> nativeSlots{
		{"dimension",
		 "position",
		 "scale",
		 "seed",
		 "progress",
		 "detail",
		 "intensity",
		 "mask",
		 "uv_map",
		 "uv_mix",
		 "intensity_map",
		 "progress_map"}
	};
	for (size_t slot = 0; slot < nativeSlots.size(); ++slot) {
		const auto *input = FindCatalogueInput(*entry, nativeSlots[slot]);
		REQUIRE(input);
		CHECK(input->SourceIndex == int64_t(slot));
	}
	for (int64_t mode = 0; mode < 4; ++mode) {
		Node node{"generator", "pc.caustic", "", {}, {{"seed", 0.0}}};
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		SetDefaults(context);
		SetValue(context, "dimension", ArrayValue{ValueType::Vector2, {Vector2{1, 1}, Vector2{2, 1}}});
		SetValue(context, "dimension_unit", EnumValue{0});
		SetValue(context, "position", Vector2{0, 0});
		SetValue(context, "position_unit", EnumValue{0});
		SetValue(context, "scale", Vector2{0.5, 0.5});
		SetValue(context, "scale_unit", EnumValue{0});
		SetValue(context, "seed", ArrayValue{ValueType::Scalar, {0.0, 17.25}});
		SetValue(context, "progress", Vector2{0, 0});
		SetValue(context, "detail", int64_t{1});
		SetValue(context, "intensity", Vector2{1, 1});
		SetValue(context, "uv_mix", 1.0);
		SetValue(context, "attribute_array_process", EnumValue{mode});
		context.ImageArrays.emplace_back("uv_map", &uvImages);
		context.ImageArrays.emplace_back("intensity_map", &whiteImages);
		context.ImageArrays.emplace_back("progress_map", &whiteImages);
		SetValue(context, "progress_mapped", true);
		SetValue(context, "progress_map_range", Vector2{0, 0});
		SetValue(context, "intensity_mapped", true);
		SetValue(context, "intensity_map_range", Vector2{1, 1});
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
		// The source inverse reverses the full slot suffix lookup. With varying slots 0 and 3,
		// both indices are row % 2, so it repeats paired rows instead of reversing Cartesian order.
		const std::array<std::pair<size_t, size_t>, 4> expanded =
			mode == 2 ? std::array<std::pair<size_t, size_t>, 4>{{{0, 0}, {0, 1}, {1, 0}, {1, 1}}}
					  : std::array<std::pair<size_t, size_t>, 4>{{{0, 0}, {1, 1}, {0, 0}, {1, 1}}};
		const size_t count = mode < 2 ? paired.size() : expanded.size();
		REQUIRE(batch.Images.size() == count);
		CHECK(observed == count);
		for (size_t row = 0; row < count; ++row) {
			const auto [dimensionIndex, seedIndex] = mode < 2 ? paired[row] : expanded[row];
			const Vector2 dimension = dimensionIndex == 0 ? Vector2{1, 1} : Vector2{2, 1};
			const double seed = seedIndex == 0 ? 0.0 : 17.25;
			const auto expected = RunNode(
				"pc.caustic",
				{{"uv_map", &uv}, {"intensity_map", &white}, {"progress_map", &white}},
				{{"dimension", dimension},
				 {"dimension_unit", EnumValue{0}},
				 {"position", Vector2{0, 0}},
				 {"position_unit", EnumValue{0}},
				 {"scale", Vector2{0.5, 0.5}},
				 {"scale_unit", EnumValue{0}},
				 {"seed", seed},
				 {"progress", Vector2{0, 0}},
				 {"detail", int64_t{1}},
				 {"intensity", Vector2{1, 1}},
				 {"intensity_mapped", true},
				 {"intensity_map_range", Vector2{1, 1}},
				 {"progress_mapped", true},
				 {"progress_map_range", Vector2{0, 0}},
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

TEST_CASE("Caustic quotes selected batch work before publishing rows", "[source_2d][caustic]") {
	const auto *entry = FindCatalogueEntry("pc.caustic");
	const auto executor = detail::FindExecutor("pc.caustic");
	REQUIRE(entry);
	REQUIRE(executor);
	Node node{"generator", "pc.caustic", "", {}, {{"seed", 0.0}}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	SetDefaults(context);
	std::vector<ElementValue> dimensions(481, Vector2{2, 2});
	SetValue(context, "dimension", ArrayValue{ValueType::Vector2, std::move(dimensions)});
	SetValue(context, "dimension_unit", EnumValue{0});
	SetValue(context, "detail", int64_t{8});
	SetValue(context, "seed", 0.0);
	SetValue(context, "progress", Vector2{0, 0});
	SetValue(context, "intensity", Vector2{1, 1});
	SetValue(context, "progress_mapped", true);
	SetValue(context, "progress_map_range", Vector2{0, 0});
	SetValue(context, "intensity_mapped", true);
	SetValue(context, "intensity_map_range", Vector2{1, 1});
	SetValue(context, "attribute_array_process", EnumValue{0});
	context.InputProvenanceResolved = true;
	for (const auto &[port, value] : context.Values)
		context.ValueViews.emplace_back(port, &value);
	context.ProcessorCount = 481;
	size_t observed = 0;
	CHECK_FALSE(detail::RunProcessorBatch(context, executor, Observe, &observed));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.FailurePort == "surface_out");
	CHECK(context.FailureMessage.find("work") != std::string::npos);
	CHECK(observed == 0);
	CHECK(context.OutputImages.empty());
	CHECK(context.OutputImageArrays.empty());
}
