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

TEST_SUITE_ID("engine.imagegraph.source_simplex_cube")
using namespace engine::imagegraph;
using imagegraph_test::NodeRun;
using namespace complex_generator_test;
namespace {
	NodeRun SimplexCube(
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
		const CatalogueEntry *entry = FindCatalogueEntry("pc.simplex_cube");
		const auto executor = detail::FindExecutor("pc.simplex_cube");
		REQUIRE(entry);
		REQUIRE(executor);
		Node node{"node", "pc.simplex_cube", "", {}, {}};
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
		Document document = complex_generator_test::Graph("pc.simplex_cube", {4, 4});
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

TEST_CASE("Simplex Cube writes both ray-marched and cross-plane outputs", "[source_simplex_cube]") {
	const NodeRun run = SimplexCube();
	RequireSuccess(run);
	REQUIRE(run.Images.size() == 2);
	CHECK(run.Output("surface_out").Width == 8);
	CHECK(run.Output("surface_out").Height == 6);
	CHECK(run.Output("cross_section").Width == 8);
	CHECK(run.Output("cross_section").Height == 6);
	CHECK(run.Output("surface_out").Format == SurfaceFormat::RGBA32Float);
	CHECK(run.Output("cross_section").Format == SurfaceFormat::RGBA32Float);
}

TEST_CASE("Simplex Cube marks ray misses and clears fractional uncovered pixels", "[source_simplex_cube]") {
	const NodeRun miss = SimplexCube();
	RequireSuccess(miss);
	CHECK(Pixel(miss.Output("surface_out"), 0, 0)[3] == 0.0);
	CHECK(Pixel(miss.Output("cross_section"), 0, 0)[3] == 1.0);
	const NodeRun fractional = SimplexCube({{"dimension", Vector2{2.5, 1.5}}});
	RequireSuccess(fractional);
	CHECK(fractional.Output("surface_out").Width == 2);
	CHECK(fractional.Output("surface_out").Height == 2);
	CheckPixel(fractional.Output("surface_out"), 0, 1, {0, 0, 0, 0});
	CheckPixel(fractional.Output("cross_section"), 0, 1, {0, 0, 0, 0});
}

TEST_CASE("Simplex Cube uses the constructor's four iteration default", "[source_simplex_cube]") {
	Document defaults = Graph();
	Set(defaults, "dimension", Vector2{8, 6});
	Document explicitFour = defaults;
	Set(explicitFour, "iteration", int64_t{4});
	CHECK(DrawPort(defaults, "surface_out") == DrawPort(explicitFour, "surface_out"));
	CHECK(DrawPort(defaults, "cross_section") == DrawPort(explicitFour, "cross_section"));
	CheckPixel(
		DrawPort(defaults, "surface_out"),
		3,
		2,
		{.46620213985443115, .46620213985443115, .46620213985443115, 1}
	);
	CheckPixel(
		DrawPort(defaults, "cross_section"),
		3,
		2,
		{.4529331922531128, .4529331922531128, .4529331922531128, 1}
	);
}

TEST_CASE("Simplex Cube writes both outputs in each supported color format", "[source_simplex_cube]") {
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
		const NodeRun run = SimplexCube({{"attribute_color_depth", EnumValue{depth}}});
		RequireSuccess(run);
		CHECK(run.Output("surface_out").Format == formats[size_t(depth - 2)]);
		CHECK(run.Output("cross_section").Format == formats[size_t(depth - 2)]);
	}
}

TEST_CASE("Simplex Cube nonpositive iterations skip seed and noise transforms", "[source_simplex_cube]") {
	for (int64_t iteration : {int64_t{0}, int64_t{-2}}) {
		const NodeRun run = SimplexCube(
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
	"Simplex Cube rejects missing seed, equal level bounds, and nonfinite inputs", "[source_simplex_cube]"
) {
	const NodeRun missingSeed = SimplexCube({}, false);
	CHECK_FALSE(missingSeed.Ok);
	CHECK(missingSeed.Code == Status::UnsupportedExecution);
	CHECK(missingSeed.Port == "seed");
	CHECK(missingSeed.Images.empty());
	const NodeRun equalLevel = SimplexCube({{"level", Vector2{.5, .5}}});
	CHECK_FALSE(equalLevel.Ok);
	CHECK(equalLevel.Code == Status::UnsupportedExecution);
	CHECK(equalLevel.Port == "level");
	CHECK(equalLevel.Images.empty());
	const NodeRun nonfiniteSeed = SimplexCube({{"seed", std::numeric_limits<double>::infinity()}});
	CHECK_FALSE(nonfiniteSeed.Ok);
	CHECK(nonfiniteSeed.Code == Status::InvalidValue);
	CHECK(nonfiniteSeed.Port == "seed");
	CHECK(nonfiniteSeed.Images.empty());
	const NodeRun nonfiniteScale = SimplexCube({{"noise_scale", std::numeric_limits<double>::infinity()}});
	CHECK_FALSE(nonfiniteScale.Ok);
	CHECK(nonfiniteScale.Code == Status::InvalidValue);
	CHECK(nonfiniteScale.Port == "noise_scale");
	CHECK(nonfiniteScale.Images.empty());
	const NodeRun unrepresentableLattice =
		SimplexCube({{"noise_scale", 1.0}, {"position", Vector3{2e9, 0, 0}}});
	CHECK_FALSE(unrepresentableLattice.Ok);
	CHECK(unrepresentableLattice.Code == Status::UnsupportedExecution);
	CHECK(unrepresentableLattice.Port == "noise_scale");
	CHECK(unrepresentableLattice.Images.empty());
	const NodeRun hashPhaseOverflow =
		SimplexCube({{"seed", 3e38}, {"noise_scale", 1.0}, {"position", Vector3{0, 0, 1e5}}});
	CHECK_FALSE(hashPhaseOverflow.Ok);
	CHECK(hashPhaseOverflow.Code == Status::InvalidValue);
	CHECK(hashPhaseOverflow.Port == "seed");
	CHECK(hashPhaseOverflow.Images.empty());
	const NodeRun zeroGradient = SimplexCube(
		{{"seed", 17.25},
		 {"noise_scale", 0.0},
		 {"iteration", int64_t{1}},
		 {"position", Vector3{5396.2666015625, -927.7335205078125, 4903.2666015625}}}
	);
	CHECK_FALSE(zeroGradient.Ok);
	CHECK(zeroGradient.Code == Status::UnsupportedExecution);
	CHECK(zeroGradient.Port == "seed");
	CHECK(zeroGradient.Images.empty());
	const NodeRun consumedSkewOverflow =
		SimplexCube({{"iteration", int64_t{2}}, {"position", Vector3{3e38, 3e38, 3e38}}});
	CHECK_FALSE(consumedSkewOverflow.Ok);
	CHECK(consumedSkewOverflow.Code == Status::InvalidValue);
	CHECK(consumedSkewOverflow.Port == "noise_scale");
	CHECK(consumedSkewOverflow.Images.empty());
	const NodeRun unrepresentableHalf =
		SimplexCube({{"level", Vector2{0, 1e-10}}, {"attribute_color_depth", EnumValue{4}}});
	CHECK_FALSE(unrepresentableHalf.Ok);
	CHECK(unrepresentableHalf.Code == Status::InvalidValue);
	CHECK(unrepresentableHalf.Port == "surface_out");
	CHECK(unrepresentableHalf.Images.empty());
}

TEST_CASE(
	"Simplex Cube source surface getters convert to Vec3, dimensions, and numeric controls",
	"[source_simplex_cube]"
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

TEST_CASE("Simplex Cube numeric source arrays pad and truncate Vec3 values", "[source_simplex_cube]") {
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

TEST_CASE("Simplex Cube matches independent binary32 simplex and ray samples", "[source_simplex_cube]") {
	struct Golden {
		std::string_view Name;
		uint32_t X, Y;
		std::vector<std::pair<std::string_view, Value>> Overrides;
		SurfacePixel Surface;
		SurfacePixel Cross;
	};
	const std::array<Golden, 54> references{
		{{"shape-0-axis-0-cross-0",
		  3,
		  2,
		  {},
		  {0.4692020118236542, 0.4692020118236542, 0.4692020118236542, 1},
		  {0.4508849084377289, 0.4508849084377289, 0.4508849084377289, 1}},
		 {"shape-0-axis-0-cross-1",
		  3,
		  2,
		  {{"axis_2", EnumValue{1}}},
		  {0.4692020118236542, 0.4692020118236542, 0.4692020118236542, 1},
		  {0.4766269028186798, 0.4766269028186798, 0.4766269028186798, 1}},
		 {"shape-0-axis-0-cross-2",
		  3,
		  2,
		  {{"axis_2", EnumValue{2}}},
		  {0.4692020118236542, 0.4692020118236542, 0.4692020118236542, 1},
		  {0.35606321692466736, 0.35606321692466736, 0.35606321692466736, 1}},
		 {"shape-0-axis-1-cross-0",
		  3,
		  2,
		  {{"axis", EnumValue{1}}},
		  {0.3546108901500702, 0.3546108901500702, 0.3546108901500702, 1},
		  {0.4508849084377289, 0.4508849084377289, 0.4508849084377289, 1}},
		 {"shape-0-axis-1-cross-1",
		  3,
		  2,
		  {{"axis", EnumValue{1}}, {"axis_2", EnumValue{1}}},
		  {0.3546108901500702, 0.3546108901500702, 0.3546108901500702, 1},
		  {0.4766269028186798, 0.4766269028186798, 0.4766269028186798, 1}},
		 {"shape-0-axis-1-cross-2",
		  3,
		  2,
		  {{"axis", EnumValue{1}}, {"axis_2", EnumValue{2}}},
		  {0.3546108901500702, 0.3546108901500702, 0.3546108901500702, 1},
		  {0.35606321692466736, 0.35606321692466736, 0.35606321692466736, 1}},
		 {"shape-0-axis-2-cross-0",
		  3,
		  2,
		  {{"axis", EnumValue{2}}},
		  {0.4800400733947754, 0.4800400733947754, 0.4800400733947754, 1},
		  {0.4508849084377289, 0.4508849084377289, 0.4508849084377289, 1}},
		 {"shape-0-axis-2-cross-1",
		  3,
		  2,
		  {{"axis", EnumValue{2}}, {"axis_2", EnumValue{1}}},
		  {0.4800400733947754, 0.4800400733947754, 0.4800400733947754, 1},
		  {0.4766269028186798, 0.4766269028186798, 0.4766269028186798, 1}},
		 {"shape-0-axis-2-cross-2",
		  3,
		  2,
		  {{"axis", EnumValue{2}}, {"axis_2", EnumValue{2}}},
		  {0.4800400733947754, 0.4800400733947754, 0.4800400733947754, 1},
		  {0.35606321692466736, 0.35606321692466736, 0.35606321692466736, 1}},
		 {"shape-1-axis-0-cross-0",
		  3,
		  2,
		  {{"shape", EnumValue{1}}},
		  {0.41309666633605957, 0.41309666633605957, 0.41309666633605957, 1},
		  {0.4508849084377289, 0.4508849084377289, 0.4508849084377289, 1}},
		 {"shape-1-axis-0-cross-1",
		  3,
		  2,
		  {{"shape", EnumValue{1}}, {"axis_2", EnumValue{1}}},
		  {0.41309666633605957, 0.41309666633605957, 0.41309666633605957, 1},
		  {0.4766269028186798, 0.4766269028186798, 0.4766269028186798, 1}},
		 {"shape-1-axis-0-cross-2",
		  3,
		  2,
		  {{"shape", EnumValue{1}}, {"axis_2", EnumValue{2}}},
		  {0.41309666633605957, 0.41309666633605957, 0.41309666633605957, 1},
		  {0.35606321692466736, 0.35606321692466736, 0.35606321692466736, 1}},
		 {"shape-1-axis-1-cross-0",
		  3,
		  2,
		  {{"shape", EnumValue{1}}, {"axis", EnumValue{1}}},
		  {0.2851957082748413, 0.2851957082748413, 0.2851957082748413, 1},
		  {0.4508849084377289, 0.4508849084377289, 0.4508849084377289, 1}},
		 {"shape-1-axis-1-cross-1",
		  3,
		  2,
		  {{"shape", EnumValue{1}}, {"axis", EnumValue{1}}, {"axis_2", EnumValue{1}}},
		  {0.2851957082748413, 0.2851957082748413, 0.2851957082748413, 1},
		  {0.4766269028186798, 0.4766269028186798, 0.4766269028186798, 1}},
		 {"shape-1-axis-1-cross-2",
		  3,
		  2,
		  {{"shape", EnumValue{1}}, {"axis", EnumValue{1}}, {"axis_2", EnumValue{2}}},
		  {0.2851957082748413, 0.2851957082748413, 0.2851957082748413, 1},
		  {0.35606321692466736, 0.35606321692466736, 0.35606321692466736, 1}},
		 {"shape-1-axis-2-cross-0",
		  3,
		  2,
		  {{"shape", EnumValue{1}}, {"axis", EnumValue{2}}},
		  {0.4261661469936371, 0.4261661469936371, 0.4261661469936371, 1},
		  {0.4508849084377289, 0.4508849084377289, 0.4508849084377289, 1}},
		 {"shape-1-axis-2-cross-1",
		  3,
		  2,
		  {{"shape", EnumValue{1}}, {"axis", EnumValue{2}}, {"axis_2", EnumValue{1}}},
		  {0.4261661469936371, 0.4261661469936371, 0.4261661469936371, 1},
		  {0.4766269028186798, 0.4766269028186798, 0.4766269028186798, 1}},
		 {"shape-1-axis-2-cross-2",
		  3,
		  2,
		  {{"shape", EnumValue{1}}, {"axis", EnumValue{2}}, {"axis_2", EnumValue{2}}},
		  {0.4261661469936371, 0.4261661469936371, 0.4261661469936371, 1},
		  {0.35606321692466736, 0.35606321692466736, 0.35606321692466736, 1}},
		 {"shape-0-pixel-0-0",
		  0,
		  0,
		  {},
		  {0.6058517694473267, 0.6058517694473267, 0.6058517694473267, 0},
		  {0.5160998702049255, 0.5160998702049255, 0.5160998702049255, 1}},
		 {"shape-0-pixel-7-5",
		  7,
		  5,
		  {},
		  {0.3734724223613739, 0.3734724223613739, 0.3734724223613739, 0},
		  {0.4842495918273926, 0.4842495918273926, 0.4842495918273926, 1}},
		 {"shape-0-pixel-4-3",
		  4,
		  3,
		  {},
		  {0.5022157430648804, 0.5022157430648804, 0.5022157430648804, 1},
		  {0.5228356122970581, 0.5228356122970581, 0.5228356122970581, 1}},
		 {"shape-1-pixel-0-0",
		  0,
		  0,
		  {{"shape", EnumValue{1}}},
		  {0.6058517694473267, 0.6058517694473267, 0.6058517694473267, 0},
		  {0.5160998702049255, 0.5160998702049255, 0.5160998702049255, 1}},
		 {"shape-1-pixel-7-5",
		  7,
		  5,
		  {{"shape", EnumValue{1}}},
		  {0.3734724223613739, 0.3734724223613739, 0.3734724223613739, 0},
		  {0.4842495918273926, 0.4842495918273926, 0.4842495918273926, 1}},
		 {"shape-1-pixel-4-3",
		  4,
		  3,
		  {{"shape", EnumValue{1}}},
		  {0.4182366132736206, 0.4182366132736206, 0.4182366132736206, 1},
		  {0.5228356122970581, 0.5228356122970581, 0.5228356122970581, 1}},
		 {"camera-identity",
		  3,
		  2,
		  {{"rotation", Vector3{0, 0, 0}}},
		  {0.5165746808052063, 0.5165746808052063, 0.5165746808052063, 1},
		  {0.4508849084377289, 0.4508849084377289, 0.4508849084377289, 1}},
		 {"camera-compound",
		  3,
		  2,
		  {{"rotation", Vector3{-37, 71, 123}}},
		  {0.512337327003479, 0.512337327003479, 0.512337327003479, 1},
		  {0.4508849084377289, 0.4508849084377289, 0.4508849084377289, 1}},
		 {"camera-negative-scale",
		  3,
		  2,
		  {{"scale", -1.2}},
		  {0.372476190328598, 0.372476190328598, 0.372476190328598, 1},
		  {0.4508849084377289, 0.4508849084377289, 0.4508849084377289, 1}},
		 {"object-identity",
		  3,
		  2,
		  {{"rotation_2", Vector3{0, 0, 0}}, {"scale_2", Vector3{1, 1, 1}}},
		  {0.5403516888618469, 0.5403516888618469, 0.5403516888618469, 1},
		  {0.4072997272014618, 0.4072997272014618, 0.4072997272014618, 1}},
		 {"object-signed-scale",
		  3,
		  2,
		  {{"scale_2", Vector3{-1.3, 0, 2.1}}},
		  {0.47884342074394226, 0.47884342074394226, 0.47884342074394226, 1},
		  {0.5597502589225769, 0.5597502589225769, 0.5597502589225769, 1}},
		 {"negative-seed",
		  3,
		  2,
		  {{"seed", -17.25}},
		  {0.40526559948921204, 0.40526559948921204, 0.40526559948921204, 1},
		  {0.4711048901081085, 0.4711048901081085, 0.4711048901081085, 1}},
		 {"negative-noise-scale",
		  3,
		  2,
		  {{"noise_scale", -3.25}},
		  {0.3640487492084503, 0.3640487492084503, 0.3640487492084503, 1},
		  {0.42450281977653503, 0.42450281977653503, 0.42450281977653503, 1}},
		 {"zero-noise-scale",
		  3,
		  2,
		  {{"noise_scale", 0}},
		  {0.4431844651699066, 0.4431844651699066, 0.4431844651699066, 1},
		  {0.4431844651699066, 0.4431844651699066, 0.4431844651699066, 1}},
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
		  {0.45547404885292053, 0.45547404885292053, 0.45547404885292053, 1},
		  {0.497690886259079, 0.497690886259079, 0.497690886259079, 1}},
		 {"four-iterations",
		  3,
		  2,
		  {{"iteration", int64_t{4}}},
		  {0.46620213985443115, 0.46620213985443115, 0.46620213985443115, 1},
		  {0.4529331922531128, 0.4529331922531128, 0.4529331922531128, 1}},
		 {"cross-outside-cube",
		  3,
		  2,
		  {{"position_2", -1.7}},
		  {0.4692020118236542, 0.4692020118236542, 0.4692020118236542, 1},
		  {0.5111658573150635, 0.5111658573150635, 0.5111658573150635, 1}},
		 {"cross-fractional-dimension",
		  1,
		  1,
		  {{"dimension", Vector2{3.6, 2.6}}},
		  {0.39744722843170166, 0.39744722843170166, 0.39744722843170166, 1},
		  {0.6497149467468262, 0.6497149467468262, 0.6497149467468262, 1}},
		 {"fractional-edge-covered",
		  3,
		  2,
		  {{"dimension", Vector2{3.6, 2.6}}},
		  {0.42616188526153564, 0.42616188526153564, 0.42616188526153564, 0},
		  {0.419236421585083, 0.419236421585083, 0.419236421585083, 1}},
		 {"reversed-levels",
		  3,
		  2,
		  {{"level", Vector2{1.3, -0.2}}},
		  {0.5307979583740234, 0.5307979583740234, 0.5307979583740234, 1},
		  {0.5491151213645935, 0.5491151213645935, 0.5491151213645935, 1}},
		 {"zero-ortho-scale",
		  3,
		  2,
		  {{"scale", 0}},
		  {0.40313395857810974, 0.40313395857810974, 0.40313395857810974, 1},
		  {0.4508849084377289, 0.4508849084377289, 0.4508849084377289, 1}},
		 {"zero-object-scale",
		  3,
		  2,
		  {{"scale_2", Vector3{0, 0, 0}}},
		  {0.4431844651699066, 0.4431844651699066, 0.4431844651699066, 1},
		  {0.4431844651699066, 0.4431844651699066, 0.4431844651699066, 1}},
		 {"fractional-uncovered", 0, 1, {{"dimension", Vector2{2.5, 1.5}}}, {0, 0, 0, 0}, {0, 0, 0, 0}},
		 {"constructor-controls",
		  3,
		  2,
		  {{"position", Vector3{0, 0, 0}},
		   {"rotation_2", Vector3{0, 0, 0}},
		   {"scale_2", Vector3{1, 1, 1}},
		   {"noise_scale", 8},
		   {"iteration", int64_t{4}},
		   {"level", Vector2{0, 1}},
		   {"position_2", 0}},
		  {0.48855528235435486, 0.48855528235435486, 0.48855528235435486, 1},
		  {0.3792034983634949, 0.3792034983634949, 0.3792034983634949, 1}},
		 {"wrapped-cell-sum-positive",
		  3,
		  2,
		  {{"position", Vector3{1000000000.0, 1000000000.0, 1000000000.0}},
		   {"noise_scale", 0},
		   {"iteration", int64_t{1}}},
		  {0.46666666865348816, 0.46666666865348816, 0.46666666865348816, 1},
		  {0.46666666865348816, 0.46666666865348816, 0.46666666865348816, 1}},
		 {"wrapped-cell-sum-negative",
		  3,
		  2,
		  {{"position", Vector3{-1000000000.0, -1000000000.0, -1000000000.0}},
		   {"noise_scale", 0},
		   {"iteration", int64_t{1}}},
		  {0.46666666865348816, 0.46666666865348816, 0.46666666865348816, 1},
		  {0.46666666865348816, 0.46666666865348816, 0.46666666865348816, 1}},
		 {"wrapped-intermediate-sum",
		  3,
		  2,
		  {{"position", Vector3{1000000000.0, 1000000000.0, -1000000000.0}},
		   {"noise_scale", 0},
		   {"iteration", int64_t{1}}},
		  {0.46666666865348816, 0.46666666865348816, 0.46666666865348816, 1},
		  {0.46666666865348816, 0.46666666865348816, 0.46666666865348816, 1}},
		 {"rank-xyz",
		  3,
		  2,
		  {{"position", Vector3{0.3, 0.2, 0.1}}, {"noise_scale", 0}, {"iteration", int64_t{1}}},
		  {0.26489970088005066, 0.26489970088005066, 0.26489970088005066, 1},
		  {0.26489970088005066, 0.26489970088005066, 0.26489970088005066, 1}},
		 {"rank-xzy",
		  3,
		  2,
		  {{"position", Vector3{0.3, 0.1, 0.2}}, {"noise_scale", 0}, {"iteration", int64_t{1}}},
		  {0.2654954493045807, 0.2654954493045807, 0.2654954493045807, 1},
		  {0.2654954493045807, 0.2654954493045807, 0.2654954493045807, 1}},
		 {"rank-zxy",
		  3,
		  2,
		  {{"position", Vector3{0.2, 0.1, 0.3}}, {"noise_scale", 0}, {"iteration", int64_t{1}}},
		  {0.26002946496009827, 0.26002946496009827, 0.26002946496009827, 1},
		  {0.26002946496009827, 0.26002946496009827, 0.26002946496009827, 1}},
		 {"rank-zyx",
		  3,
		  2,
		  {{"position", Vector3{0.1, 0.2, 0.3}}, {"noise_scale", 0}, {"iteration", int64_t{1}}},
		  {0.2539675533771515, 0.2539675533771515, 0.2539675533771515, 1},
		  {0.2539675533771515, 0.2539675533771515, 0.2539675533771515, 1}},
		 {"rank-yzx",
		  3,
		  2,
		  {{"position", Vector3{0.1, 0.3, 0.2}}, {"noise_scale", 0}, {"iteration", int64_t{1}}},
		  {0.2533719539642334, 0.2533719539642334, 0.2533719539642334, 1},
		  {0.2533719539642334, 0.2533719539642334, 0.2533719539642334, 1}},
		 {"rank-yxz",
		  3,
		  2,
		  {{"position", Vector3{0.2, 0.3, 0.1}}, {"noise_scale", 0}, {"iteration", int64_t{1}}},
		  {0.25883805751800537, 0.25883805751800537, 0.25883805751800537, 1},
		  {0.25883805751800537, 0.25883805751800537, 0.25883805751800537, 1}},
		 {"rank-tie",
		  3,
		  2,
		  {{"position", Vector3{0.2, 0.2, 0.2}}, {"noise_scale", 0}, {"iteration", int64_t{1}}},
		  {0.2052443027496338, 0.2052443027496338, 0.2052443027496338, 1},
		  {0.2052443027496338, 0.2052443027496338, 0.2052443027496338, 1}}}
	};
	for (const auto &reference : references) {
		INFO(reference.Name);
		const NodeRun run = SimplexCube(reference.Overrides);
		RequireSuccess(run);
		CheckPixel(run.Output("surface_out"), reference.X, reference.Y, reference.Surface);
		CheckPixel(run.Output("cross_section"), reference.X, reference.Y, reference.Cross);
	}
}
