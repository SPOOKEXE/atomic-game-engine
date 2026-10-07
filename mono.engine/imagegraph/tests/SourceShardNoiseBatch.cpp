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

TEST_SUITE_ID("engine.imagegraph.source_shard_noise_batch")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Document ShardNoiseGraph() {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"field",
			 "pc.noise",
			 "",
			 {},
			 {{"dimension", Vector2{4, 3}}, {"dimension_unit", EnumValue{0}}, {"seed", 7.0}}},
			{"generator",
			 "pc.shard_noise",
			 "",
			 {},
			 {{"dimension", Vector2{4, 3}},
			  {"dimension_unit", EnumValue{0}},
			  {"position_unit", EnumValue{0}},
			  {"seed", 17.0},
			  {"progress", 0.0},
			  {"progress_mapped", true},
			  {"progress_map_range", Vector2{0, 100}},
			  {"sharpness", 1.0},
			  {"sharpness_mapped", true},
			  {"sharpness_map_range", Vector2{0.5, 1.5}},
			  {"scale", Vector2{4, 4}},
			  {"scale_mapped", true},
			  {"scale_map_range", Vector2{2, 8}},
			  {"level_in", Vector2{0, 1}},
			  {"level_out", Vector2{0, 1}}}}
		};
		document.Links = {
			{"field", "surface_out", "generator", "progress_map"},
			{"field", "surface_out", "generator", "sharpness_map"},
			{"field", "surface_out", "generator", "scale_map"}
		};
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

TEST_CASE("Shard noise animation and inherited instances survive Format9", "[source_2d][shard_noise]") {
	auto document = ShardNoiseGraph();
	document.Nodes[1].SourceAnimatedInputs = {"seed", "progress"};
	document.Keyframes = {
		{"generator", "seed", 0, 17.0},
		{"generator", "seed", 1, 31.0},
		{"generator", "progress", 0, 20.0},
		{"generator", "progress", 1, 80.0}
	};
	Node instance{"copy", "pc.shard_noise", "", {}, {}};
	instance.InstanceBase = "generator";
	document.Nodes.push_back(std::move(instance));
	document.Outputs.push_back({"copy", "copy", "surface_out"});
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	REQUIRE(restored == document);
	for (const uint64_t tick : {uint64_t{0}, uint64_t{1}}) {
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

TEST_CASE("Shard noise array schedules preserve source slot order", "[source_2d][shard_noise]") {
	const auto *entry = FindCatalogueEntry("pc.shard_noise");
	const auto executor = detail::FindExecutor("pc.shard_noise");
	REQUIRE(entry);
	REQUIRE(executor);
	const std::array<std::string_view, 15> sourceSlots{
		{"dimension",
		 "position",
		 "scale",
		 "seed",
		 "sharpness",
		 "progress",
		 "scale_map",
		 "sharpness_map",
		 "progress_map",
		 "rotation",
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
	const Image scaleMap = White(), sharpnessMap = White(), progressMap = White();
	for (int64_t mode = 0; mode < 4; ++mode) {
		Node node{"generator", "pc.shard_noise", "", {}, {}};
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		SetDefaults(context);
		SetValue(context, "dimension", ArrayValue{ValueType::Vector2, {Vector2{2, 2}, Vector2{3, 2}}});
		SetValue(context, "dimension_unit", EnumValue{0});
		SetValue(context, "position", ArrayValue{ValueType::Vector2, {Vector2{0, 0}, Vector2{1, 0}}});
		SetValue(context, "position_unit", EnumValue{0});
		SetValue(context, "scale", Vector2{4, 4});
		SetValue(context, "seed", ArrayValue{ValueType::Scalar, {17.0, 31.0}});
		SetValue(context, "sharpness", 1.0);
		SetValue(context, "sharpness_mapped", true);
		SetValue(context, "sharpness_map_range", Vector2{0.5, 1.5});
		SetValue(context, "progress", 0.0);
		SetValue(context, "progress_mapped", true);
		SetValue(context, "progress_map_range", Vector2{0, 100});
		SetValue(context, "scale_mapped", true);
		SetValue(context, "scale_map_range", Vector2{2, 8});
		SetValue(context, "rotation", ArrayValue{ValueType::Scalar, {0.0, 37.0}});
		SetValue(context, "uv_mix", 1.0);
		SetValue(context, "level_in", Vector2{0, 1});
		SetValue(context, "level_out", Vector2{0, 1});
		SetValue(context, "attribute_array_process", EnumValue{mode});
		context.Images = {
			{"scale_map", &scaleMap}, {"sharpness_map", &sharpnessMap}, {"progress_map", &progressMap}
		};
		context.InputProvenanceResolved = true;
		for (const auto &[port, value] : context.Values)
			context.ValueViews.emplace_back(port, &value);
		size_t observed = 0;
		const bool ran = detail::RunProcessorBatch(context, executor, Observe, &observed);
		INFO(mode << ":" << context.FailurePort << ":" << context.FailureMessage);
		REQUIRE(ran);
		REQUIRE(context.OutputImageArrays.size() == 1);
		const auto &batch = context.OutputImageArrays.front().second;
		const size_t count = mode < 2 ? 2 : 16;
		REQUIRE(batch.Images.size() == count);
		CHECK(observed == count);
		for (size_t row = 0; row < count; ++row) {
			CAPTURE(mode, row);
			size_t dimensionIndex, positionIndex, seedIndex, rotationIndex;
			if (mode < 2) {
				dimensionIndex = positionIndex = seedIndex = rotationIndex = row % 2;
			} else if (mode == 2) {
				dimensionIndex = row / 8;
				positionIndex = row / 4 % 2;
				seedIndex = row / 2 % 2;
				rotationIndex = row % 2;
			} else {
				dimensionIndex = positionIndex = seedIndex = row % 2;
				rotationIndex = row / 2 % 2;
			}
			const Vector2 dimension = dimensionIndex == 0 ? Vector2{2, 2} : Vector2{3, 2};
			const Vector2 position = positionIndex == 0 ? Vector2{0, 0} : Vector2{1, 0};
			const auto expected = RunNode(
				"pc.shard_noise",
				{{"scale_map", &scaleMap}, {"sharpness_map", &sharpnessMap}, {"progress_map", &progressMap}},
				{{"dimension", dimension},
				 {"dimension_unit", EnumValue{0}},
				 {"position", position},
				 {"position_unit", EnumValue{0}},
				 {"scale", Vector2{4, 4}},
				 {"seed", seedIndex == 0 ? 17.0 : 31.0},
				 {"sharpness", 1.0},
				 {"sharpness_mapped", true},
				 {"sharpness_map_range", Vector2{0.5, 1.5}},
				 {"progress", 0.0},
				 {"progress_mapped", true},
				 {"progress_map_range", Vector2{0, 100}},
				 {"scale_mapped", true},
				 {"scale_map_range", Vector2{2, 8}},
				 {"rotation", rotationIndex == 0 ? 0.0 : 37.0},
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
	"Shard noise preflights cumulative work before rows are observed or published", "[source_2d][shard_noise]"
) {
	const auto *entry = FindCatalogueEntry("pc.shard_noise");
	const auto executor = detail::FindExecutor("pc.shard_noise");
	REQUIRE(entry);
	REQUIRE(executor);
	Node node{"generator", "pc.shard_noise", "", {}, {{"seed", 17.0}}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	SetDefaults(context);
	std::vector<ElementValue> dimensions(70, Vector2{8, 8});
	SetValue(context, "dimension", ArrayValue{ValueType::Vector2, std::move(dimensions)});
	SetValue(context, "dimension_unit", EnumValue{0});
	SetValue(context, "seed", 17.0);
	SetValue(context, "scale_mapped", true);
	SetValue(context, "sharpness_mapped", true);
	SetValue(context, "progress_mapped", true);
	SetValue(context, "attribute_array_process", EnumValue{0});
	const Image map = White();
	context.Images = {{"scale_map", &map}, {"sharpness_map", &map}, {"progress_map", &map}};
	for (const auto &[port, value] : context.Values)
		context.ValueViews.emplace_back(port, &value);
	context.ProcessorCount = 70;
	context.InputProvenanceResolved = true;
	size_t observed = 0;
	CHECK_FALSE(detail::RunProcessorBatch(context, executor, Observe, &observed));
	INFO(context.FailureMessage);
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.FailurePort == "surface_out");
	CHECK(context.FailureMessage.find("work") != std::string::npos);
	CHECK(observed == 0);
	CHECK(context.OutputImages.empty());
	CHECK(context.OutputImageArrays.empty());
}
