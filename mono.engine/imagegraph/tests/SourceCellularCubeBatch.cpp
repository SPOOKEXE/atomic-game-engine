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

TEST_SUITE_ID("engine.imagegraph.source_cellular_cube_batch")
using namespace engine::imagegraph;
namespace {
	Document CellularCubeGraph() {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {Node{
			"generator",
			"pc.cellular_cube",
			"",
			{},
			{{"dimension", Vector2{8, 6}},
			 {"dimension_unit", EnumValue{0}},
			 {"shape", EnumValue{0}},
			 {"rotation", Vector3{30, 45, 0}},
			 {"scale", 1.0},
			 {"axis_2", EnumValue{0}},
			 {"position_2", .23},
			 {"axis", EnumValue{0}},
			 {"seed", 17.25},
			 {"rotation_2", Vector3{11, -17, 23}},
			 {"scale_2", Vector3{1.3, .7, 1.1}},
			 {"noise_scale", 3.25},
			 {"iteration", int64_t{2}},
			 {"position", Vector3{.17, -.31, .23}},
			 {"level", Vector2{-.2, 1.3}},
			 {"attribute_color_depth", EnumValue{5}}}
		}};
		document.Outputs = {{"surface", "generator", "surface_out"}, {"cross", "generator", "cross_section"}};
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
	void SetNodeValue(Node &node, std::string_view port, Value value) {
		for (auto &authored : node.Values)
			if (authored.Port == port) {
				authored.Data = std::move(value);
				return;
			}
		node.Values.push_back({std::string(port), std::move(value)});
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

TEST_CASE("Cellular Cube animation and inherited instances cover both outputs", "[source_cellular_cube]") {
	auto document = CellularCubeGraph();
	document.Nodes[0].SourceAnimatedInputs = {"seed"};
	document.Keyframes = {
		{"generator", "seed", 0, 17.25}, {"generator", "seed", 1, 31.5}, {"generator", "seed", 2, 31.5}
	};
	Node instance{"copy", "pc.cellular_cube", "", {}, {}};
	instance.InstanceBase = "generator";
	document.Nodes.push_back(std::move(instance));
	document.Outputs.push_back({"copy_surface", "copy", "surface_out"});
	document.Outputs.push_back({"copy_cross", "copy", "cross_section"});
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	REQUIRE(restored == document);
	for (uint64_t tick : {uint64_t{0}, uint64_t{1}}) {
		Image surface, cross, copySurface, copyCross;
		REQUIRE(Draw(restored, "surface", tick, surface, diagnostic) == Status::Ok);
		REQUIRE(Draw(restored, "cross", tick, cross, diagnostic) == Status::Ok);
		REQUIRE(Draw(restored, "copy_surface", tick, copySurface, diagnostic) == Status::Ok);
		REQUIRE(Draw(restored, "copy_cross", tick, copyCross, diagnostic) == Status::Ok);
		CHECK(copySurface.Pixels == surface.Pixels);
		CHECK(copyCross.Pixels == cross.Pixels);
		if (tick == 0) {
			const Image firstSurface = surface, firstCross = cross;
			REQUIRE(Draw(restored, "surface", 1, surface, diagnostic) == Status::Ok);
			REQUIRE(Draw(restored, "cross", 1, cross, diagnostic) == Status::Ok);
			CHECK(firstSurface.Pixels != surface.Pixels);
			CHECK(firstCross.Pixels != cross.Pixels);
		}
	}
}

TEST_CASE(
	"Cellular Cube array schedules retain both outputs and source slot selection", "[source_cellular_cube]"
) {
	const auto *entry = FindCatalogueEntry("pc.cellular_cube");
	const auto executor = detail::FindExecutor("pc.cellular_cube");
	REQUIRE(entry);
	REQUIRE(executor);
	const std::array<std::string_view, 14> nativeSlots{
		{"dimension",
		 "shape",
		 "rotation",
		 "scale",
		 "axis_2",
		 "position_2",
		 "axis",
		 "seed",
		 "rotation_2",
		 "scale_2",
		 "noise_scale",
		 "iteration",
		 "position",
		 "level"}
	};
	for (size_t slot = 0; slot < nativeSlots.size(); ++slot) {
		const auto *input = FindCatalogueInput(*entry, nativeSlots[slot]);
		REQUIRE(input);
		CHECK(input->SourceIndex == int64_t(slot));
	}
	for (int64_t mode = 0; mode < 4; ++mode) {
		Node node{"generator", "pc.cellular_cube", "", {}, {{"seed", 17.25}}};
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		SetDefaults(context);
		SetValue(context, "dimension", ArrayValue{ValueType::Vector2, {Vector2{4, 3}, Vector2{5, 3}}});
		SetValue(context, "dimension_unit", EnumValue{0});
		SetValue(context, "axis_2", ArrayValue{ValueType::Enum, {EnumValue{0}, EnumValue{2}}});
		SetValue(context, "position_2", ArrayValue{ValueType::Scalar, {.2, .7}});
		SetValue(context, "seed", ArrayValue{ValueType::Scalar, {17.25, 31.5}});
		SetValue(context, "iteration", int64_t{2});
		SetValue(context, "shape", EnumValue{0});
		SetValue(context, "rotation", Vector3{0, 0, 0});
		SetValue(context, "axis", EnumValue{1});
		SetValue(context, "rotation_2", Vector3{11, -17, 23});
		SetValue(context, "scale_2", Vector3{1.3, .7, 1.1});
		SetValue(context, "noise_scale", 3.25);
		SetValue(context, "position", Vector3{.17, -.31, .23});
		SetValue(context, "level", Vector2{-.2, 1.3});
		SetValue(context, "attribute_color_depth", EnumValue{5});
		SetValue(context, "attribute_array_process", EnumValue{mode});
		context.InputProvenanceResolved = true;
		for (const auto &[port, value] : context.Values)
			context.ValueViews.emplace_back(port, &value);
		size_t observed = 0;
		const bool ran = detail::RunProcessorBatch(context, executor, Observe, &observed);
		INFO(mode << ":" << context.FailurePort << ":" << context.FailureMessage);
		REQUIRE(ran);
		REQUIRE(context.OutputImageArrays.size() == 2);
		const auto findRows = [&](std::string_view port) -> const ImageArray & {
			for (const auto &[id, images] : context.OutputImageArrays)
				if (id == port) return images;
			static const ImageArray empty;
			return empty;
		};
		const ImageArray &surfaces = findRows("surface_out");
		const ImageArray &crossSections = findRows("cross_section");
		const size_t count = mode < 2 ? 2 : 16;
		REQUIRE(surfaces.Images.size() == count);
		REQUIRE(crossSections.Images.size() == count);
		CHECK(observed == count);
		for (size_t row = 0; row < count; ++row) {
			size_t dimensionIndex, crossAxisIndex, crossPositionIndex, seedIndex;
			if (mode == 0 || mode == 1) {
				dimensionIndex = crossAxisIndex = crossPositionIndex = seedIndex = row % 2;
			} else if (mode == 2) {
				dimensionIndex = row / 8;
				crossAxisIndex = row / 4 % 2;
				crossPositionIndex = row / 2 % 2;
				seedIndex = row % 2;
			} else {
				dimensionIndex = row % 2;
				crossAxisIndex = crossPositionIndex = row % 2;
				seedIndex = row / 2 % 2;
			}
			Document scalar = CellularCubeGraph();
			SetNodeValue(scalar.Nodes[0], "dimension", dimensionIndex == 0 ? Vector2{4, 3} : Vector2{5, 3});
			SetNodeValue(scalar.Nodes[0], "dimension_unit", EnumValue{0});
			SetNodeValue(scalar.Nodes[0], "shape", EnumValue{0});
			SetNodeValue(scalar.Nodes[0], "rotation", Vector3{0, 0, 0});
			SetNodeValue(scalar.Nodes[0], "axis_2", EnumValue{crossAxisIndex == 0 ? 0 : 2});
			SetNodeValue(scalar.Nodes[0], "position_2", crossPositionIndex == 0 ? .2 : .7);
			SetNodeValue(scalar.Nodes[0], "axis", EnumValue{1});
			SetNodeValue(scalar.Nodes[0], "seed", seedIndex == 0 ? 17.25 : 31.5);
			SetNodeValue(scalar.Nodes[0], "iteration", int64_t{2});
			SetNodeValue(scalar.Nodes[0], "attribute_array_process", EnumValue{mode});
			Image expectedSurface, expectedCross;
			Diagnostic diagnostic;
			REQUIRE(Draw(scalar, "surface", 0, expectedSurface, diagnostic) == Status::Ok);
			REQUIRE(Draw(scalar, "cross", 0, expectedCross, diagnostic) == Status::Ok);
			INFO("array mode=" << mode << " row=" << row);
			SamePixels(surfaces.Images[row], expectedSurface);
			SamePixels(crossSections.Images[row], expectedCross);
		}
	}
}

TEST_CASE("Cellular Cube rejects cumulative work before observers or outputs", "[source_cellular_cube]") {
	const auto *entry = FindCatalogueEntry("pc.cellular_cube");
	const auto executor = detail::FindExecutor("pc.cellular_cube");
	REQUIRE(entry);
	REQUIRE(executor);
	Node node{"generator", "pc.cellular_cube", "", {}, {{"seed", 17.25}}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	SetDefaults(context);
	std::vector<ElementValue> dimensions(250, Vector2{3, 3});
	SetValue(context, "dimension", ArrayValue{ValueType::Vector2, std::move(dimensions)});
	SetValue(context, "dimension_unit", EnumValue{0});
	SetValue(context, "seed", 17.25);
	SetValue(context, "iteration", int64_t{1});
	SetValue(context, "attribute_array_process", EnumValue{0});
	context.InputProvenanceResolved = true;
	for (const auto &[port, value] : context.Values)
		context.ValueViews.emplace_back(port, &value);
	context.ProcessorCount = 250;
	size_t observed = 0;
	CHECK_FALSE(detail::RunProcessorBatch(context, executor, Observe, &observed));
	INFO(context.FailurePort << ":" << context.FailureMessage);
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.FailurePort == "surface_out");
	CHECK(observed == 0);
	CHECK(context.OutputImages.empty());
	CHECK(context.OutputImageArrays.empty());
}

TEST_CASE("Cellular Cube reserves both outputs before observers or allocation", "[source_cellular_cube]") {
	const auto *entry = FindCatalogueEntry("pc.cellular_cube");
	const auto executor = detail::FindExecutor("pc.cellular_cube");
	REQUIRE(entry);
	REQUIRE(executor);
	Node node{"generator", "pc.cellular_cube", "", {}, {{"seed", 17.25}}};
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
	INFO(context.FailurePort << ":" << context.FailureMessage);
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(observed == 0);
	CHECK(context.OutputImages.empty());
	CHECK(context.OutputImageArrays.empty());
}
