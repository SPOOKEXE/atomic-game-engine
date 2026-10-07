#include "ComplexGeneratorFixture.hpp"
#include "NodeHarness.hpp"

#include <engine/imagegraph/Surface.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.source_cellular_cube")
using namespace engine::imagegraph;
using imagegraph_test::NodeRun;
using namespace complex_generator_test;
namespace {
	NodeRun CellularCube(
		const std::vector<std::pair<std::string_view, Value>> &overrides = {}, bool includeSeed = true
	) {
		std::vector<std::pair<std::string_view, Value>> values{
			{"dimension", Vector2{8, 6}},
			{"dimension_unit", EnumValue{0}},
			{"shape", EnumValue{0}},
			{"rotation", Vector3{30, 45, 0}},
			{"scale", 1.0},
			{"axis_2", EnumValue{0}},
			{"position_2", .23},
			{"axis", EnumValue{0}},
			{"rotation_2", Vector3{11, -17, 23}},
			{"scale_2", Vector3{1.3, .7, 1.1}},
			{"noise_scale", 3.25},
			{"iteration", int64_t{2}},
			{"position", Vector3{.17, -.31, .23}},
			{"level", Vector2{-.2, 1.3}},
			{"attribute_color_depth", EnumValue{5}}
		};
		if (includeSeed) values.emplace_back("seed", 17.25);
		for (const auto &[port, value] : overrides) {
			const auto found = std::find_if(values.begin(), values.end(), [&](const auto &item) {
				return item.first == port;
			});
			if (found == values.end())
				values.emplace_back(port, value);
			else
				found->second = value;
		}
		NodeRun run;
		const CatalogueEntry *entry = FindCatalogueEntry("pc.cellular_cube");
		const auto executor = detail::FindExecutor("pc.cellular_cube");
		REQUIRE(entry);
		REQUIRE(executor);
		Node node{"node", "pc.cellular_cube", "", {}, {}};
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		for (const CatalogueInput &input : entry->Inputs) {
			const auto given = std::find_if(values.begin(), values.end(), [&](const auto &item) {
				return item.first == input.Id;
			});
			if (given != values.end())
				context.Values.emplace_back(input.Id, given->second);
			else if (const auto fallback = CatalogueDefault(input)) {
				context.Values.emplace_back(input.Id, *fallback);
				context.CatalogueDefaultInputs.emplace_back(input.Id);
			}
		}
		context.InputProvenanceResolved = true;
		run.Ok = executor(context) && context.FailureCode == Status::Ok;
		run.Code = context.FailureCode;
		run.Message = context.FailureMessage;
		run.Port = context.FailurePort;
		run.Images = std::move(context.OutputImages);
		run.Values = std::move(context.OutputValues);
		return run;
	}
	SurfacePixel Pixel(const Image &image, uint32_t x = 0, uint32_t y = 0) {
		SurfacePixel pixel{};
		REQUIRE(LoadSurfacePixel(image, x, y, pixel));
		return pixel;
	}
	void CheckPixel(const Image &image, uint32_t x, uint32_t y, SurfacePixel expected, double margin = 5e-5) {
		const auto actual = Pixel(image, x, y);
		for (size_t channel = 0; channel < 4; ++channel)
			CHECK(actual[channel] == Catch::Approx(expected[channel]).margin(margin));
	}
	void RequireSuccess(const NodeRun &run) {
		INFO(run.Port << ": " << run.Message);
		REQUIRE(run.Ok);
	}
	Document Graph() {
		Document document = complex_generator_test::Graph("pc.cellular_cube", {4, 4});
		Set(document, "dimension_unit", EnumValue{0});
		Set(document, "shape", EnumValue{0});
		Set(document, "rotation", Vector3{30, 45, 0});
		Set(document, "scale", 1.0);
		Set(document, "axis", EnumValue{0});
		Set(document, "axis_2", EnumValue{0});
		Set(document, "position_2", .23);
		Set(document, "rotation_2", Vector3{11, -17, 23});
		Set(document, "scale_2", Vector3{1.3, .7, 1.1});
		Set(document, "noise_scale", 3.25);
		Set(document, "position", Vector3{.17, -.31, .23});
		Set(document, "level", Vector2{-.2, 1.3});
		Set(document, "seed", 17.25);
		Set(document, "attribute_color_depth", EnumValue{5});
		document.Outputs.push_back({"cross", "generator", "cross_section"});
		return document;
	}
	Image DrawPort(Document document, std::string_view port) {
		Image image;
		Diagnostic diagnostic;
		document.Outputs[0].Port = std::string(port);
		Plan plan;
		const auto compiled = Compile(document, plan, diagnostic);
		REQUIRE(compiled == Status::Ok);
		const auto status = Evaluate(document, plan, "out", {}, image, diagnostic);
		INFO(diagnostic.NodeId + ":" + diagnostic.Port + ":" + diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return image;
	}
	ImageArray DrawArrayPort(Document document, std::string_view port) {
		document.Outputs[0].Port = std::string(port);
		Plan plan;
		Diagnostic diagnostic;
		const auto compiled = Compile(document, plan, diagnostic);
		REQUIRE(compiled == Status::Ok);
		ImageArray images;
		const auto status = EvaluateArray(document, plan, "out", {}, images, diagnostic);
		INFO(diagnostic.NodeId + ":" + diagnostic.Port + ":" + diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return images;
	}
}

TEST_CASE("Cellular Cube writes both ray-marched and cross-plane outputs", "[source_cellular_cube]") {
	const NodeRun run = CellularCube();
	RequireSuccess(run);
	REQUIRE(run.Images.size() == 2);
	CHECK(run.Output("surface_out").Width == 8);
	CHECK(run.Output("surface_out").Height == 6);
	CHECK(run.Output("cross_section").Width == 8);
	CHECK(run.Output("cross_section").Height == 6);
	CHECK(run.Output("surface_out").Format == SurfaceFormat::RGBA32Float);
	CHECK(run.Output("cross_section").Format == SurfaceFormat::RGBA32Float);
}

TEST_CASE(
	"Cellular Cube preserves sampled RGB on a ray miss and clears uncovered fractional pixels",
	"[source_cellular_cube]"
) {
	const NodeRun miss = CellularCube();
	RequireSuccess(miss);
	const SurfacePixel surface = Pixel(miss.Output("surface_out"), 0, 0);
	CHECK(surface[0] == Catch::Approx(.3977099657).margin(5e-5));
	CHECK(surface[3] == 0.0);
	CheckPixel(miss.Output("cross_section"), 0, 0, {.2525976002, .2525976002, .2525976002, 1});
	CHECK(Pixel(miss.Output("cross_section"), 0, 0)[3] == 1.0);
	const NodeRun fractional = CellularCube({{"dimension", Vector2{2.5, 1.5}}});
	RequireSuccess(fractional);
	CHECK(fractional.Output("surface_out").Width == 2);
	CHECK(fractional.Output("surface_out").Height == 2);
	CheckPixel(fractional.Output("surface_out"), 0, 1, {0, 0, 0, 0});
	CheckPixel(fractional.Output("cross_section"), 0, 1, {0, 0, 0, 0});
}

TEST_CASE("Cellular Cube uses the constructor's one iteration default", "[source_cellular_cube]") {
	Document defaults = Graph();
	Set(defaults, "dimension", Vector2{8, 6});
	Document explicitOne = defaults;
	Set(explicitOne, "iteration", int64_t{1});
	CHECK(DrawPort(defaults, "surface_out") == DrawPort(explicitOne, "surface_out"));
	CHECK(DrawPort(defaults, "cross_section") == DrawPort(explicitOne, "cross_section"));
	CheckPixel(DrawPort(defaults, "surface_out"), 3, 2, {.615770936, .615770936, .615770936, 1});
	CheckPixel(DrawPort(defaults, "cross_section"), 3, 2, {.5239584446, .5239584446, .5239584446, 1});
}

TEST_CASE("Cellular Cube writes both outputs in each supported color format", "[source_cellular_cube]") {
	constexpr std::array<SurfaceFormat, 7> formats{
		SurfaceFormat::RGBA4Unorm,
		SurfaceFormat::RGBA8Unorm,
		SurfaceFormat::RGBA16Float,
		SurfaceFormat::RGBA32Float,
		SurfaceFormat::R8Unorm,
		SurfaceFormat::R16Float,
		SurfaceFormat::R32Float
	};
	for (int64_t depth = 2; depth <= 8; ++depth) {
		const NodeRun run = CellularCube({{"attribute_color_depth", EnumValue{depth}}});
		RequireSuccess(run);
		CHECK(run.Output("surface_out").Format == formats[size_t(depth - 2)]);
		CHECK(run.Output("cross_section").Format == formats[size_t(depth - 2)]);
	}
}

TEST_CASE("Cellular Cube nonpositive iterations skip seed and noise transforms", "[source_cellular_cube]") {
	for (int64_t iteration : {int64_t{0}, int64_t{-2}}) {
		const NodeRun run = CellularCube(
			{{"iteration", iteration},
			 {"noise_scale", std::numeric_limits<double>::infinity()},
			 {"position", Vector3{std::numeric_limits<double>::infinity(), 0, 0}},
			 {"rotation_2", Vector3{0, std::numeric_limits<double>::infinity(), 0}},
			 {"scale_2", Vector3{0, 0, std::numeric_limits<double>::infinity()}}},
			false
		);
		RequireSuccess(run);
		CheckPixel(run.Output("cross_section"), 0, 0, {.1333333403, .1333333403, .1333333403, 1});
	}
}

TEST_CASE(
	"Cellular Cube rejects missing seed, equal level bounds, and nonfinite inputs", "[source_cellular_cube]"
) {
	const NodeRun missingSeed = CellularCube({}, false);
	CHECK_FALSE(missingSeed.Ok);
	CHECK(missingSeed.Code == Status::UnsupportedExecution);
	CHECK(missingSeed.Port == "seed");
	CHECK(missingSeed.Images.empty());
	const NodeRun equalLevel = CellularCube({{"level", Vector2{.5, .5}}});
	CHECK_FALSE(equalLevel.Ok);
	CHECK(equalLevel.Code == Status::UnsupportedExecution);
	CHECK(equalLevel.Port == "level");
	CHECK(equalLevel.Images.empty());
	const NodeRun nonfiniteSeed = CellularCube({{"seed", std::numeric_limits<double>::infinity()}});
	CHECK_FALSE(nonfiniteSeed.Ok);
	CHECK(nonfiniteSeed.Code == Status::InvalidValue);
	CHECK(nonfiniteSeed.Port == "seed");
	CHECK(nonfiniteSeed.Images.empty());
	const NodeRun nonfiniteScale = CellularCube({{"noise_scale", std::numeric_limits<double>::infinity()}});
	CHECK_FALSE(nonfiniteScale.Ok);
	CHECK(nonfiniteScale.Code == Status::InvalidValue);
	CHECK(nonfiniteScale.Port == "noise_scale");
	CHECK(nonfiniteScale.Images.empty());
	const NodeRun consumedLastPosition =
		CellularCube({{"iteration", int64_t{2}}, {"position", Vector3{3e38, 3e38, 3e38}}});
	CHECK_FALSE(consumedLastPosition.Ok);
	CHECK(consumedLastPosition.Code == Status::InvalidValue);
	CHECK(consumedLastPosition.Port == "noise_scale");
	CHECK(consumedLastPosition.Images.empty());
	const NodeRun unrepresentableHalf =
		CellularCube({{"level", Vector2{0, 1e-10}}, {"attribute_color_depth", EnumValue{4}}});
	CHECK_FALSE(unrepresentableHalf.Ok);
	CHECK(unrepresentableHalf.Code == Status::InvalidValue);
	CHECK(unrepresentableHalf.Port == "surface_out");
	CHECK(unrepresentableHalf.Images.empty());
}

TEST_CASE(
	"Cellular Cube source surface getters convert to Vec3, dimensions, and numeric controls",
	"[source_cellular_cube]"
) {
	for (const std::string_view output : {"surface_out", "cross_section"}) {
		auto linked = Graph();
		linked.Nodes.push_back(Solid("size", {2, 1}, {0, 0, 0, 0}));
		linked.Links = {{"size", "surface_out", "generator", "position"}};
		auto expected = Graph();
		Set(expected, "position", Vector3{2, 1, 0});
		CHECK(DrawPort(linked, output) == DrawPort(expected, output));
	}
	{
		auto linked = Graph();
		linked.Nodes.push_back(Solid("size", {2, 1}, {0, 0, 0, 0}));
		linked.Links = {{"size", "surface_out", "generator", "dimension"}};
		auto expected = Graph();
		Set(expected, "dimension", Vector2{2, 1});
		for (const std::string_view output : {"surface_out", "cross_section"})
			CHECK(DrawPort(linked, output) == DrawPort(expected, output));
	}
	{
		auto linked = Graph();
		linked.Nodes.push_back(Solid("size", {2, 1}, {0, 0, 0, 0}));
		linked.Links = {{"size", "surface_out", "generator", "level"}};
		auto expected = Graph();
		Set(expected, "level", Vector2{2, 1});
		for (const std::string_view output : {"surface_out", "cross_section"})
			CHECK(DrawPort(linked, output) == DrawPort(expected, output));
	}
	{
		auto linked = Graph();
		linked.Nodes.push_back(Solid("size", {2, 1}, {0, 0, 0, 0}));
		linked.Links = {{"size", "surface_out", "generator", "noise_scale"}};
		for (const std::string_view output : {"surface_out", "cross_section"}) {
			const ImageArray actual = DrawArrayPort(linked, output);
			REQUIRE(actual.Images.size() == 2);
			for (size_t row = 0; row < 2; ++row) {
				auto expected = Graph();
				Set(expected, "noise_scale", row == 0 ? 2.0 : 1.0);
				CHECK(actual.Images[row] == DrawPort(expected, output));
			}
		}
	}
}

TEST_CASE("Cellular Cube numeric source arrays pad and truncate Vec3 values", "[source_cellular_cube]") {
	struct TupleCase {
		std::array<double, 4> Components;
		size_t Count;
		Vector3 Expected;
	};
	const std::array<TupleCase, 2> cases{
		{{{.17, -.31, .23, 0}, 2, {.17, -.31, 0}}, {{.17, -.31, .23, 9.0}, 4, {.17, -.31, .23}}}
	};
	for (const auto &test : cases) {
		auto linked = Graph();
		Node tuple{"tuple", "pc.array", "", {}, {}};
		for (size_t i = 0; i < test.Count; ++i)
			tuple.DynamicInputs.push_back(
				{"input_" + std::to_string(i), ValueType::Scalar, test.Components[i]}
			);
		linked.Nodes.push_back(std::move(tuple));
		linked.Links = {{"tuple", "array", "generator", "position"}};
		auto expected = Graph();
		Set(expected, "position", test.Expected);
		for (const std::string_view output : {"surface_out", "cross_section"})
			CHECK(DrawPort(linked, output) == DrawPort(expected, output));
	}
}

TEST_CASE("Cellular Cube matches independent binary32 cell and ray samples", "[source_cellular_cube]") {
	struct Golden {
		std::string_view Name;
		uint32_t X, Y;
		std::vector<std::pair<std::string_view, Value>> Overrides;
		SurfacePixel Surface;
		SurfacePixel Cross;
	};
	const std::array<Golden, 46> references{
		{{"shape-0-axis-0-cross-0",
		  3,
		  2,
		  {},
		  {0.5377126336097717, 0.5377126336097717, 0.5377126336097717, 1},
		  {0.5595377087593079, 0.5595377087593079, 0.5595377087593079, 1}},
		 {"shape-0-axis-0-cross-1",
		  3,
		  2,
		  {{"axis_2", EnumValue{1}}},
		  {0.5377126336097717, 0.5377126336097717, 0.5377126336097717, 1},
		  {0.5662088394165039, 0.5662088394165039, 0.5662088394165039, 1}},
		 {"shape-0-axis-0-cross-2",
		  3,
		  2,
		  {{"axis_2", EnumValue{2}}},
		  {0.5377126336097717, 0.5377126336097717, 0.5377126336097717, 1},
		  {0.34434986114501953, 0.34434986114501953, 0.34434986114501953, 1}},
		 {"shape-0-axis-1-cross-0",
		  3,
		  2,
		  {{"axis", EnumValue{1}}},
		  {0.4879355728626251, 0.4879355728626251, 0.4879355728626251, 1},
		  {0.5595377087593079, 0.5595377087593079, 0.5595377087593079, 1}},
		 {"shape-0-axis-1-cross-1",
		  3,
		  2,
		  {{"axis", EnumValue{1}}, {"axis_2", EnumValue{1}}},
		  {0.4879355728626251, 0.4879355728626251, 0.4879355728626251, 1},
		  {0.5662088394165039, 0.5662088394165039, 0.5662088394165039, 1}},
		 {"shape-0-axis-1-cross-2",
		  3,
		  2,
		  {{"axis", EnumValue{1}}, {"axis_2", EnumValue{2}}},
		  {0.4879355728626251, 0.4879355728626251, 0.4879355728626251, 1},
		  {0.34434986114501953, 0.34434986114501953, 0.34434986114501953, 1}},
		 {"shape-0-axis-2-cross-0",
		  3,
		  2,
		  {{"axis", EnumValue{2}}},
		  {0.566719114780426, 0.566719114780426, 0.566719114780426, 1},
		  {0.5595377087593079, 0.5595377087593079, 0.5595377087593079, 1}},
		 {"shape-0-axis-2-cross-1",
		  3,
		  2,
		  {{"axis", EnumValue{2}}, {"axis_2", EnumValue{1}}},
		  {0.566719114780426, 0.566719114780426, 0.566719114780426, 1},
		  {0.5662088394165039, 0.5662088394165039, 0.5662088394165039, 1}},
		 {"shape-0-axis-2-cross-2",
		  3,
		  2,
		  {{"axis", EnumValue{2}}, {"axis_2", EnumValue{2}}},
		  {0.566719114780426, 0.566719114780426, 0.566719114780426, 1},
		  {0.34434986114501953, 0.34434986114501953, 0.34434986114501953, 1}},
		 {"shape-1-axis-0-cross-0",
		  3,
		  2,
		  {{"shape", EnumValue{1}}},
		  {0.5180279016494751, 0.5180279016494751, 0.5180279016494751, 1},
		  {0.5595377087593079, 0.5595377087593079, 0.5595377087593079, 1}},
		 {"shape-1-axis-0-cross-1",
		  3,
		  2,
		  {{"shape", EnumValue{1}}, {"axis_2", EnumValue{1}}},
		  {0.5180279016494751, 0.5180279016494751, 0.5180279016494751, 1},
		  {0.5662088394165039, 0.5662088394165039, 0.5662088394165039, 1}},
		 {"shape-1-axis-0-cross-2",
		  3,
		  2,
		  {{"shape", EnumValue{1}}, {"axis_2", EnumValue{2}}},
		  {0.5180279016494751, 0.5180279016494751, 0.5180279016494751, 1},
		  {0.34434986114501953, 0.34434986114501953, 0.34434986114501953, 1}},
		 {"shape-1-axis-1-cross-0",
		  3,
		  2,
		  {{"shape", EnumValue{1}}, {"axis", EnumValue{1}}},
		  {0.5676290392875671, 0.5676290392875671, 0.5676290392875671, 1},
		  {0.5595377087593079, 0.5595377087593079, 0.5595377087593079, 1}},
		 {"shape-1-axis-1-cross-1",
		  3,
		  2,
		  {{"shape", EnumValue{1}}, {"axis", EnumValue{1}}, {"axis_2", EnumValue{1}}},
		  {0.5676290392875671, 0.5676290392875671, 0.5676290392875671, 1},
		  {0.5662088394165039, 0.5662088394165039, 0.5662088394165039, 1}},
		 {"shape-1-axis-1-cross-2",
		  3,
		  2,
		  {{"shape", EnumValue{1}}, {"axis", EnumValue{1}}, {"axis_2", EnumValue{2}}},
		  {0.5676290392875671, 0.5676290392875671, 0.5676290392875671, 1},
		  {0.34434986114501953, 0.34434986114501953, 0.34434986114501953, 1}},
		 {"shape-1-axis-2-cross-0",
		  3,
		  2,
		  {{"shape", EnumValue{1}}, {"axis", EnumValue{2}}},
		  {0.4761192500591278, 0.4761192500591278, 0.4761192500591278, 1},
		  {0.5595377087593079, 0.5595377087593079, 0.5595377087593079, 1}},
		 {"shape-1-axis-2-cross-1",
		  3,
		  2,
		  {{"shape", EnumValue{1}}, {"axis", EnumValue{2}}, {"axis_2", EnumValue{1}}},
		  {0.4761192500591278, 0.4761192500591278, 0.4761192500591278, 1},
		  {0.5662088394165039, 0.5662088394165039, 0.5662088394165039, 1}},
		 {"shape-1-axis-2-cross-2",
		  3,
		  2,
		  {{"shape", EnumValue{1}}, {"axis", EnumValue{2}}, {"axis_2", EnumValue{2}}},
		  {0.4761192500591278, 0.4761192500591278, 0.4761192500591278, 1},
		  {0.34434986114501953, 0.34434986114501953, 0.34434986114501953, 1}},
		 {"shape-0-pixel-0-0",
		  0,
		  0,
		  {},
		  {0.3977099657058716, 0.3977099657058716, 0.3977099657058716, 0},
		  {0.2525976002216339, 0.2525976002216339, 0.2525976002216339, 1}},
		 {"shape-0-pixel-7-5",
		  7,
		  5,
		  {},
		  {0.35397279262542725, 0.35397279262542725, 0.35397279262542725, 0},
		  {0.44881269335746765, 0.44881269335746765, 0.44881269335746765, 1}},
		 {"shape-0-pixel-4-3",
		  4,
		  3,
		  {},
		  {0.5421051383018494, 0.5421051383018494, 0.5421051383018494, 1},
		  {0.46385276317596436, 0.46385276317596436, 0.46385276317596436, 1}},
		 {"shape-1-pixel-0-0",
		  0,
		  0,
		  {{"shape", EnumValue{1}}},
		  {0.3977099657058716, 0.3977099657058716, 0.3977099657058716, 0},
		  {0.2525976002216339, 0.2525976002216339, 0.2525976002216339, 1}},
		 {"shape-1-pixel-7-5",
		  7,
		  5,
		  {{"shape", EnumValue{1}}},
		  {0.35397279262542725, 0.35397279262542725, 0.35397279262542725, 0},
		  {0.44881269335746765, 0.44881269335746765, 0.44881269335746765, 1}},
		 {"shape-1-pixel-4-3",
		  4,
		  3,
		  {{"shape", EnumValue{1}}},
		  {0.5337063074111938, 0.5337063074111938, 0.5337063074111938, 1},
		  {0.46385276317596436, 0.46385276317596436, 0.46385276317596436, 1}},
		 {"camera-identity",
		  3,
		  2,
		  {{"rotation", Vector3{0, 0, 0}}},
		  {0.4908049404621124, 0.4908049404621124, 0.4908049404621124, 1},
		  {0.5595377087593079, 0.5595377087593079, 0.5595377087593079, 1}},
		 {"camera-compound",
		  3,
		  2,
		  {{"rotation", Vector3{-37, 71, 123}}},
		  {0.3450787365436554, 0.3450787365436554, 0.3450787365436554, 1},
		  {0.5595377087593079, 0.5595377087593079, 0.5595377087593079, 1}},
		 {"camera-negative-scale",
		  3,
		  2,
		  {{"scale", -1.2}},
		  {0.4406561851501465, 0.4406561851501465, 0.4406561851501465, 1},
		  {0.5595377087593079, 0.5595377087593079, 0.5595377087593079, 1}},
		 {"object-identity",
		  3,
		  2,
		  {{"rotation_2", Vector3{0, 0, 0}}, {"scale_2", Vector3{1, 1, 1}}},
		  {0.3189649283885956, 0.3189649283885956, 0.3189649283885956, 1},
		  {0.6517267823219299, 0.6517267823219299, 0.6517267823219299, 1}},
		 {"object-signed-scale",
		  3,
		  2,
		  {{"scale_2", Vector3{-1.3, 0, 2.1}}},
		  {0.5462983250617981, 0.5462983250617981, 0.5462983250617981, 1},
		  {0.39857086539268494, 0.39857086539268494, 0.39857086539268494, 1}},
		 {"negative-seed",
		  3,
		  2,
		  {{"seed", -17.25}},
		  {0.5348010063171387, 0.5348010063171387, 0.5348010063171387, 1},
		  {0.6185762286186218, 0.6185762286186218, 0.6185762286186218, 1}},
		 {"negative-noise-scale",
		  3,
		  2,
		  {{"noise_scale", -3.25}},
		  {0.44286593794822693, 0.44286593794822693, 0.44286593794822693, 1},
		  {0.5490548014640808, 0.5490548014640808, 0.5490548014640808, 1}},
		 {"zero-noise-scale",
		  3,
		  2,
		  {{"noise_scale", 0}},
		  {0.670796811580658, 0.670796811580658, 0.670796811580658, 1},
		  {0.670796811580658, 0.670796811580658, 0.670796811580658, 1}},
		 {"zero-iteration",
		  3,
		  2,
		  {{"iteration", int64_t{0}}},
		  {0.13333334028720856, 0.13333334028720856, 0.13333334028720856, 1},
		  {0.13333334028720856, 0.13333334028720856, 0.13333334028720856, 1}},
		 {"negative-iteration",
		  3,
		  2,
		  {{"iteration", int64_t{-1}}},
		  {0.13333334028720856, 0.13333334028720856, 0.13333334028720856, 1},
		  {0.13333334028720856, 0.13333334028720856, 0.13333334028720856, 1}},
		 {"one-iteration",
		  3,
		  2,
		  {{"iteration", int64_t{1}}},
		  {0.6157709360122681, 0.6157709360122681, 0.6157709360122681, 1},
		  {0.5239584445953369, 0.5239584445953369, 0.5239584445953369, 1}},
		 {"four-iterations",
		  3,
		  2,
		  {{"iteration", int64_t{4}}},
		  {0.5272524952888489, 0.5272524952888489, 0.5272524952888489, 1},
		  {0.5401087999343872, 0.5401087999343872, 0.5401087999343872, 1}},
		 {"cross-outside-cube",
		  3,
		  2,
		  {{"position_2", -1.7}},
		  {0.5377126336097717, 0.5377126336097717, 0.5377126336097717, 1},
		  {0.4981994330883026, 0.4981994330883026, 0.4981994330883026, 1}},
		 {"cross-fractional-dimension",
		  1,
		  1,
		  {{"dimension", Vector2{3.6, 2.6}}},
		  {0.37765130400657654, 0.37765130400657654, 0.37765130400657654, 1},
		  {0.24359802901744843, 0.24359802901744843, 0.24359802901744843, 1}},
		 {"fractional-edge-covered",
		  3,
		  2,
		  {{"dimension", Vector2{3.6, 2.6}}},
		  {0.5533249974250793, 0.5533249974250793, 0.5533249974250793, 0},
		  {0.4573897123336792, 0.4573897123336792, 0.4573897123336792, 1}},
		 {"reversed-levels",
		  3,
		  2,
		  {{"level", Vector2{1.3, -0.2}}},
		  {0.4622873067855835, 0.4622873067855835, 0.4622873067855835, 1},
		  {0.44046223163604736, 0.44046223163604736, 0.44046223163604736, 1}},
		 {"zero-ortho-scale",
		  3,
		  2,
		  {{"scale", 0}},
		  {0.585148811340332, 0.585148811340332, 0.585148811340332, 1},
		  {0.5595377087593079, 0.5595377087593079, 0.5595377087593079, 1}},
		 {"zero-object-scale",
		  3,
		  2,
		  {{"scale_2", Vector3{0, 0, 0}}},
		  {0.670796811580658, 0.670796811580658, 0.670796811580658, 1},
		  {0.670796811580658, 0.670796811580658, 0.670796811580658, 1}},
		 {"unused-final-position-overflow",
		  3,
		  2,
		  {{"position", Vector3{3e+38, 3e+38, 3e+38}}, {"iteration", int64_t{1}}},
		  {0.13333334028720856, 0.13333334028720856, 0.13333334028720856, 1},
		  {0.13333334028720856, 0.13333334028720856, 0.13333334028720856, 1}},
		 {"fractional-uncovered", 0, 1, {{"dimension", Vector2{2.5, 1.5}}}, {0, 0, 0, 0}, {0, 0, 0, 0}},
		 {"constructor-controls",
		  3,
		  2,
		  {{"dimension", Vector2{8, 6}},
		   {"shape", EnumValue{0}},
		   {"rotation", Vector3{30, 45, 0}},
		   {"scale", 1.0},
		   {"axis", EnumValue{0}},
		   {"position", Vector3{0, 0, 0}},
		   {"rotation_2", Vector3{0, 0, 0}},
		   {"scale_2", Vector3{1, 1, 1}},
		   {"noise_scale", 8.0},
		   {"iteration", int64_t{1}},
		   {"level", Vector2{0, 1}},
		   {"axis_2", EnumValue{0}},
		   {"position_2", 0.0}},
		  {.5230411887168884, .5230411887168884, .5230411887168884, 1},
		  {.8628775477409363, .8628775477409363, .8628775477409363, 1}},
		 {"zero-hash-factor",
		  3,
		  2,
		  {{"seed", -318.3099}},
		  {.41799604892730713, .41799604892730713, .41799604892730713, 1},
		  {.4570040702819824, .4570040702819824, .4570040702819824, 1}}}
	};
	for (const auto &reference : references) {
		INFO(reference.Name);
		const NodeRun run = CellularCube(reference.Overrides);
		RequireSuccess(run);
		CheckPixel(run.Output("surface_out"), reference.X, reference.Y, reference.Surface);
		CheckPixel(run.Output("cross_section"), reference.X, reference.Y, reference.Cross);
	}
}
