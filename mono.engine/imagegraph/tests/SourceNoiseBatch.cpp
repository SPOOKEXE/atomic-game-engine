#include "../src/ProcessorBatch.hpp"
#include "NodeHarness.hpp"

#include <engine/imagegraph/NoiseField.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.source_noise_batch")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Document NoiseGraph() {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"generator",
			 "pc.noise",
			 "",
			 {},
			 {{"dimension", Vector2{4, 3}}, {"dimension_unit", EnumValue{0}}, {"seed", 17.0}}}
		};
		document.Outputs = {{"out", "generator", "surface_out"}};
		return document;
	}
	void Set(Document &document, std::string port, Value value) {
		for (auto &stored : document.Nodes[0].Values)
			if (stored.Port == port) {
				stored.Data = std::move(value);
				return;
			}
		document.Nodes[0].Values.push_back({std::move(port), std::move(value)});
	}
	Node Array(std::string id, ValueType type, Value first, Value second) {
		Node node{std::move(id), "pc.array", "", {}, {}, {}};
		node.DynamicInputs = {{"input_0", type, std::move(first)}, {"input_1", type, std::move(second)}};
		return node;
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

TEST_CASE("Noise array schedules preserve generated rows through Format9", "[source_2d][noise]") {
	for (int64_t schedule = 0; schedule < 4; ++schedule) {
		auto document = NoiseGraph();
		Set(document, "attribute_array_process", EnumValue{schedule});
		document.Nodes.push_back(Array("dimensions", ValueType::Vector2, Vector2{3, 2}, Vector2{2, 3}));
		document.Nodes.push_back(Array("seeds", ValueType::Scalar, 17.0, 31.0));
		document.Links = {
			{"dimensions", "array", "generator", "dimension"}, {"seeds", "array", "generator", "seed"}
		};
		Document restored;
		Diagnostic diagnostic;
		REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
		REQUIRE(restored == document);
		Plan plan;
		REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
		ImageArray output;
		const auto status = EvaluateArray(restored, plan, "out", {}, output, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		const std::array<std::pair<size_t, size_t>, 2> pairedRows{{{3, 2}, {2, 3}}};
		const std::array<std::pair<size_t, size_t>, 4> expandedRows =
			schedule == 2 ? std::array<std::pair<size_t, size_t>, 4>{{{3, 2}, {3, 2}, {2, 3}, {2, 3}}}
						  : std::array<std::pair<size_t, size_t>, 4>{{{3, 2}, {2, 3}, {3, 2}, {2, 3}}};
		const size_t count = schedule < 2 ? pairedRows.size() : expandedRows.size();
		REQUIRE(output.Images.size() == count);
		for (size_t i = 0; i < count; ++i) {
			const auto [width, height] = schedule < 2 ? pairedRows[i] : expandedRows[i];
			CHECK(output.Images[i].Width == width);
			CHECK(output.Images[i].Height == height);
			// grug source inverse reverses all eleven slot suffixes, including singleton controls.
			const size_t seedIndex = i % 2;
			const auto expected = RunNode(
				"pc.noise",
				{},
				{{"dimension", Vector2{double(width), double(height)}},
				 {"dimension_unit", EnumValue{0}},
				 {"seed", seedIndex == 0 ? 17.0 : 31.0}}
			);
			INFO(expected.Message);
			REQUIRE(expected.Ok);
			CHECK(output.Images[i].Format == expected.Output().Format);
			REQUIRE(output.Images[i].Pixels.size() == expected.Output().Pixels.size());
			for (size_t byte = 0; byte < output.Images[i].Pixels.size(); ++byte) {
				CAPTURE(schedule, i, seedIndex, byte);
				CHECK(unsigned(output.Images[i].Pixels[byte]) == unsigned(expected.Output().Pixels[byte]));
			}
		}
	}
}

TEST_CASE("Noise preflights cumulative batch work before output allocation", "[source_2d][noise]") {
	const auto *entry = FindCatalogueEntry("pc.noise");
	const auto executor = detail::FindExecutor("pc.noise");
	REQUIRE(entry);
	REQUIRE(executor);
	Node node{"generator", "pc.noise", "", {}, {{"seed", 17.0}}};
	EvaluationRequest request;
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	detail::NodeContext context(node, *entry, request, budget);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	SetDefaults(context);
	SetValue(context, "seed", 17.0);
	SetValue(context, "attribute_array_process", EnumValue{2});
	SetValue(context, "attribute_color_depth", EnumValue{3});
	SetValue(context, "dimension", ArrayValue{ValueType::Vector2, {Vector2{1, 1}, Vector2{512, 257}}});
	SetValue(context, "dimension_unit", EnumValue{0});
	// grug graph evaluation supplies borrowed original input views before selecting rows.
	for (const auto &[port, value] : context.Values)
		context.ValueViews.emplace_back(port, &value);
	context.ProcessorCount = 2;
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

TEST_CASE("Noise preflights output bytes before allocating a single image", "[source_2d][noise]") {
	const auto *entry = FindCatalogueEntry("pc.noise");
	const auto executor = detail::FindExecutor("pc.noise");
	REQUIRE(entry);
	REQUIRE(executor);
	Node node{"generator", "pc.noise", "", {}, {{"seed", 17.0}}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = sizeof(std::pair<std::string, Image>) + sizeof(AuthoredValue) +
						 sizeof(std::pair<std::string, ImageArray>) +
						 sizeof(std::pair<std::string_view, SourceSocketDomain>);
	SetDefaults(context);
	SetValue(context, "seed", 17.0);
	SetValue(context, "attribute_color_depth", EnumValue{3});
	context.InputProvenanceResolved = true;
	CHECK_FALSE(executor(context));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.FailurePort == "surface_out");
	CHECK(context.OutputImages.empty());
	CHECK(context.OutputImageArrays.empty());
}

TEST_CASE("Noise field output uses the generic image-generator raster fallback", "[source_2d][noise]") {
	const auto *entry = FindCatalogueEntry("pc.noise");
	const auto executor = detail::FindExecutor("pc.noise");
	REQUIRE(entry);
	REQUIRE(executor);
	Node node{"generator", "pc.noise", "", {}, {{"seed", 17.0}}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.NoiseFieldRequested = true;
	SetDefaults(context);
	SetValue(context, "seed", 17.0);
	context.InputProvenanceResolved = true;
	REQUIRE(executor(context));
	REQUIRE(context.OutputImages.size() == 1);
	const auto fieldOutput =
		std::find_if(context.OutputValues.begin(), context.OutputValues.end(), [](const auto &v) {
			return v.Port == "field";
		});
	REQUIRE(fieldOutput != context.OutputValues.end());
	const auto *field = std::get_if<NoiseFieldValue>(&fieldOutput->Data);
	REQUIRE(field);
	REQUIRE(field->Data);
	REQUIRE(field->Data->Raster);
	CHECK(field->Data->Raster->Pixels == context.OutputImages.front().second.Pixels);
	CHECK(field->Data->Raster->Format == context.OutputImages.front().second.Format);
}

TEST_CASE("Noise native persistence instances and keyframes reproduce source pixels", "[source_2d][noise]") {
	auto document = NoiseGraph();
	document.Nodes[0].SourceAnimatedInputs = {"seed"};
	document.Keyframes = {{"generator", "seed", 0, 3.0}, {"generator", "seed", 2, 29.0}};
	Node instance{"copy", "pc.noise", "", {}, {}};
	instance.InstanceBase = "generator";
	document.Nodes.push_back(std::move(instance));
	document.Outputs.push_back({"copy", "copy", "surface_out"});
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	REQUIRE(restored == document);
	for (const uint64_t tick : {uint64_t{0}, uint64_t{2}}) {
		Image source, inherited;
		REQUIRE(Draw(restored, "out", tick, source, diagnostic) == Status::Ok);
		REQUIRE(Draw(restored, "copy", tick, inherited, diagnostic) == Status::Ok);
		CHECK(inherited == source);
		if (tick == 0) {
			Image repeated;
			REQUIRE(Draw(restored, "out", tick, repeated, diagnostic) == Status::Ok);
			CHECK(repeated == source);
		}
	}
	Image first, last;
	REQUIRE(Draw(restored, "out", 0, first, diagnostic) == Status::Ok);
	REQUIRE(Draw(restored, "out", 2, last, diagnostic) == Status::Ok);
	CHECK(first.Pixels != last.Pixels);
}

TEST_CASE("Noise UV sampling refuses Atlas values", "[source_2d][noise]") {
	const Image uv{1, 1, {128, 64, 0, 255}, 0};
	for (const AtlasKind kind : {AtlasKind::Atlas, AtlasKind::SurfaceAtlas}) {
		AtlasValue atlas;
		auto &data = atlas.Data.emplace();
		data.Kind = kind;
		data.Surface.Data = uv;
		data.Dimension = {1, 1};
		const auto refused = RunNode("pc.noise", {}, {{"seed", 17.0}, {"uv_map", atlas}});
		CHECK_FALSE(refused.Ok);
		CHECK(refused.Code == Status::UnsupportedExecution);
		CHECK(refused.Port == "uv_map");
		CHECK(refused.Images.empty());
	}
	const auto missingSeed = RunNode("pc.noise", {}, {});
	CHECK_FALSE(missingSeed.Ok);
	CHECK(missingSeed.Code == Status::UnsupportedExecution);
	CHECK(missingSeed.Port == "seed");
}
