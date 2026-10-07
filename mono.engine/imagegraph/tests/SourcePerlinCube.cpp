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

TEST_SUITE_ID("engine.imagegraph.source_perlin_cube")
using namespace engine::imagegraph;
using imagegraph_test::NodeRun;
using namespace complex_generator_test;
namespace {
	NodeRun PerlinCube(
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
		const CatalogueEntry *entry = FindCatalogueEntry("pc.perlin_cube");
		const auto executor = detail::FindExecutor("pc.perlin_cube");
		REQUIRE(entry);
		REQUIRE(executor);
		Node node{"node", "pc.perlin_cube", "", {}, {}};
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
		Document document = complex_generator_test::Graph("pc.perlin_cube", {4, 4});
		complex_generator_test::Set(document, "dimension_unit", EnumValue{0});
		complex_generator_test::Set(document, "shape", EnumValue{0});
		complex_generator_test::Set(document, "rotation", Vector3{30, 45, 0});
		complex_generator_test::Set(document, "scale", 1.0);
		complex_generator_test::Set(document, "axis", EnumValue{0});
		complex_generator_test::Set(document, "axis_2", EnumValue{0});
		complex_generator_test::Set(document, "position_2", .23);
		complex_generator_test::Set(document, "rotation_2", Vector3{11, -17, 23});
		complex_generator_test::Set(document, "scale_2", Vector3{1.3, .7, 1.1});
		complex_generator_test::Set(document, "noise_scale", 3.25);
		complex_generator_test::Set(document, "iteration", int64_t{2});
		complex_generator_test::Set(document, "position", Vector3{.17, -.31, .23});
		complex_generator_test::Set(document, "level", Vector2{-.2, 1.3});
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

TEST_CASE(
	"Perlin Cube outputs both the ray-marched surface and independent cross plane", "[source_perlin_cube]"
) {
	constexpr std::array<double, 6> surfaceValues{
		.5178050399, .6267979145, .5521785617, .4722144604, .5522968173, .5420947671
	};
	constexpr std::array<double, 3> crossValues{.3882323503, .3480512202, .3736160994};
	for (int64_t shape = 0; shape < 2; ++shape)
		for (int64_t axis = 0; axis < 3; ++axis)
			for (int64_t crossAxis = 0; crossAxis < 3; ++crossAxis) {
				const NodeRun run = PerlinCube(
					{{"shape", EnumValue{shape}}, {"axis", EnumValue{axis}}, {"axis_2", EnumValue{crossAxis}}}
				);
				CAPTURE(shape, axis, crossAxis);
				RequireSuccess(run);
				REQUIRE(run.Images.size() == 2);
				CHECK(run.Output("surface_out").Width == 8);
				CHECK(run.Output("surface_out").Height == 6);
				CHECK(run.Output("cross_section").Width == 8);
				CHECK(run.Output("cross_section").Height == 6);
				CHECK(run.Output("surface_out").Format == SurfaceFormat::RGBA32Float);
				CHECK(run.Output("cross_section").Format == SurfaceFormat::RGBA32Float);
				CheckPixel(
					run.Output("surface_out"),
					3,
					2,
					{surfaceValues[size_t(shape * 3 + axis)],
					 surfaceValues[size_t(shape * 3 + axis)],
					 surfaceValues[size_t(shape * 3 + axis)],
					 1}
				);
				const double cross = crossValues[size_t(crossAxis)];
				CheckPixel(run.Output("cross_section"), 3, 2, {cross, cross, cross, 1});
			}
}

TEST_CASE("Perlin Cube surface and cross axes select their source coordinate lanes", "[source_perlin_cube]") {
	const NodeRun baseline = PerlinCube();
	RequireSuccess(baseline);
	for (int64_t axis = 1; axis < 3; ++axis) {
		const NodeRun surfaceAxis = PerlinCube({{"axis", EnumValue{axis}}});
		RequireSuccess(surfaceAxis);
		CHECK(surfaceAxis.Output("surface_out").Pixels != baseline.Output("surface_out").Pixels);
	}
	for (int64_t axis = 1; axis < 3; ++axis) {
		const NodeRun crossAxis = PerlinCube({{"axis_2", EnumValue{axis}}});
		RequireSuccess(crossAxis);
		CHECK(crossAxis.Output("cross_section").Pixels != baseline.Output("cross_section").Pixels);
	}
}

TEST_CASE(
	"Perlin Cube shape and camera affect the surface while object transform affects both outputs",
	"[source_perlin_cube]"
) {
	const NodeRun cube = PerlinCube({{"shape", EnumValue{0}}});
	const NodeRun sphere = PerlinCube({{"shape", EnumValue{1}}});
	const NodeRun camera = PerlinCube({{"rotation", Vector3{7, 21, -13}}});
	const NodeRun object =
		PerlinCube({{"rotation_2", Vector3{-19, 31, 47}}, {"scale_2", Vector3{.8, 1.4, .6}}});
	for (const NodeRun *run : {&cube, &sphere, &camera, &object})
		RequireSuccess(*run);
	CHECK(cube.Output("surface_out").Pixels != sphere.Output("surface_out").Pixels);
	CHECK(cube.Output("surface_out").Pixels != camera.Output("surface_out").Pixels);
	CHECK(cube.Output("cross_section").Pixels == sphere.Output("cross_section").Pixels);
	CHECK(cube.Output("cross_section").Pixels == camera.Output("cross_section").Pixels);
	CHECK(cube.Output("surface_out").Pixels != object.Output("surface_out").Pixels);
	CHECK(cube.Output("cross_section").Pixels != object.Output("cross_section").Pixels);
}

TEST_CASE("Perlin Cube keeps sampled RGB at ray misses", "[source_perlin_cube]") {
	for (const int64_t shape : {int64_t{0}, int64_t{1}}) {
		const NodeRun run = PerlinCube({{"shape", EnumValue{shape}}});
		RequireSuccess(run);
		CheckPixel(run.Output("surface_out"), 0, 0, {.3336636722, .3336636722, .3336636722, 0});
		CheckPixel(run.Output("cross_section"), 0, 0, {.4174546003, .4174546003, .4174546003, 1});
		CheckPixel(run.Output("surface_out"), 7, 5, {.2652512491, .2652512491, .2652512491, 0});
		CheckPixel(run.Output("cross_section"), 7, 5, {.4411796033, .4411796033, .4411796033, 1});
	}
	const NodeRun cubeCenter = PerlinCube({{"shape", EnumValue{0}}});
	const NodeRun sphereCenter = PerlinCube({{"shape", EnumValue{1}}});
	RequireSuccess(cubeCenter);
	RequireSuccess(sphereCenter);
	CheckPixel(cubeCenter.Output("surface_out"), 4, 3, {.7042066455, .7042066455, .7042066455, 1});
	CheckPixel(cubeCenter.Output("cross_section"), 4, 3, {.5395171046, .5395171046, .5395171046, 1});
	CheckPixel(sphereCenter.Output("surface_out"), 4, 3, {.7023547292, .7023547292, .7023547292, 1});
	CheckPixel(sphereCenter.Output("cross_section"), 4, 3, {.5395171046, .5395171046, .5395171046, 1});
}

TEST_CASE(
	"Perlin Cube retains noise color on misses and clears uncovered fractional pixels", "[source_perlin_cube]"
) {
	const NodeRun miss =
		PerlinCube({{"shape", EnumValue{1}}, {"rotation", Vector3{0, 0, 0}}, {"scale", 4.0}});
	RequireSuccess(miss);
	const auto surface = Pixel(miss.Output("surface_out"), 0, 0);
	CHECK(surface[3] == 0.0);
	CHECK(surface[0] != 0.0);
	const NodeRun fractional = PerlinCube({{"dimension", Vector2{2.5, 1.5}}, {"rotation", Vector3{0, 0, 0}}});
	RequireSuccess(fractional);
	CHECK(fractional.Output("surface_out").Width == 2);
	CHECK(fractional.Output("surface_out").Height == 2);
	CHECK(fractional.Output("cross_section").Width == 2);
	CHECK(fractional.Output("cross_section").Height == 2);
	CheckPixel(fractional.Output("surface_out"), 0, 1, {0, 0, 0, 0});
	CheckPixel(fractional.Output("cross_section"), 0, 1, {0, 0, 0, 0});
}

TEST_CASE(
	"Perlin Cube document getters project surfaces to vector and scalar controls", "[source_perlin_cube]"
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

TEST_CASE("Perlin Cube numeric source arrays pad and truncate Vec3 tuples", "[source_perlin_cube]") {
	struct TupleCase {
		std::array<double, 4> Components;
		size_t Count;
		Vector3 Expected;
	};
	const std::array<TupleCase, 2> cases{
		{{{.17, -.31, 0, 0}, 2, {.17, -.31, 0}}, {{.17, -.31, .23, 9.0}, 4, {.17, -.31, .23}}}
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

TEST_CASE("Perlin Cube writes both outputs in each supported color format", "[source_perlin_cube]") {
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
		const NodeRun run = PerlinCube({{"attribute_color_depth", EnumValue{depth}}});
		RequireSuccess(run);
		CHECK(run.Output("surface_out").Format == formats[size_t(depth - 2)]);
		CHECK(run.Output("cross_section").Format == formats[size_t(depth - 2)]);
	}
}

TEST_CASE("Perlin Cube nonpositive iteration skips seed and noise transforms", "[source_perlin_cube]") {
	for (int64_t iteration : {int64_t{0}, int64_t{-3}}) {
		const NodeRun run = PerlinCube(
			{{"iteration", iteration},
			 {"noise_scale", std::numeric_limits<double>::infinity()},
			 {"position", Vector3{std::numeric_limits<double>::infinity(), 0, 0}},
			 {"rotation_2", Vector3{0, std::numeric_limits<double>::infinity(), 0}},
			 {"scale_2", Vector3{0, 0, std::numeric_limits<double>::infinity()}}},
			false
		);
		RequireSuccess(run);
		CheckPixel(
			run.Output("cross_section"), 0, 0, {.13333334028720856, .13333334028720856, .13333334028720856, 1}
		);
	}
}

TEST_CASE("Perlin Cube diagnoses consumed seed, level, and float inputs", "[source_perlin_cube]") {
	const NodeRun missingSeed = PerlinCube({}, false);
	CHECK_FALSE(missingSeed.Ok);
	CHECK(missingSeed.Code == Status::UnsupportedExecution);
	CHECK(missingSeed.Port == "seed");
	const NodeRun equalLevel = PerlinCube({{"level", Vector2{.5, .5}}});
	CHECK_FALSE(equalLevel.Ok);
	CHECK(equalLevel.Code == Status::UnsupportedExecution);
	CHECK(equalLevel.Port == "level");
	const NodeRun nonfiniteSeed = PerlinCube({{"seed", std::numeric_limits<double>::infinity()}});
	CHECK_FALSE(nonfiniteSeed.Ok);
	CHECK(nonfiniteSeed.Code == Status::InvalidValue);
	CHECK(nonfiniteSeed.Port == "seed");
	CHECK(nonfiniteSeed.Images.empty());
	const NodeRun unrepresentableHalf =
		PerlinCube({{"level", Vector2{0, 1e-10}}, {"attribute_color_depth", EnumValue{4}}});
	CHECK_FALSE(unrepresentableHalf.Ok);
	CHECK(unrepresentableHalf.Code == Status::InvalidValue);
	CHECK(unrepresentableHalf.Port == "surface_out");
	CHECK(unrepresentableHalf.Images.empty());
}

TEST_CASE(
	"Perlin Cube matches independent camera, object, octave, and fractional references",
	"[source_perlin_cube]"
) {
	auto check = [](const NodeRun &run, uint32_t x, uint32_t y, SurfacePixel surface, SurfacePixel cross) {
		RequireSuccess(run);
		CheckPixel(run.Output("surface_out"), x, y, surface);
		CheckPixel(run.Output("cross_section"), x, y, cross);
	};
	check(
		PerlinCube({{"rotation", Vector3{0, 0, 0}}}),
		3,
		2,
		{.4147091806, .4147091806, .4147091806, 1},
		{.3882323503, .3882323503, .3882323503, 1}
	);
	check(
		PerlinCube({{"rotation", Vector3{-37, 71, 123}}}),
		3,
		2,
		{.5955045223, .5955045223, .5955045223, 1},
		{.3882323503, .3882323503, .3882323503, 1}
	);
	check(
		PerlinCube({{"scale", -1.2}}),
		3,
		2,
		{.6841916442, .6841916442, .6841916442, 1},
		{.3882323503, .3882323503, .3882323503, 1}
	);
	check(
		PerlinCube({{"rotation_2", Vector3{0, 0, 0}}, {"scale_2", Vector3{1, 1, 1}}}),
		3,
		2,
		{.3714922965, .3714922965, .3714922965, 1},
		{.4906947911, .4906947911, .4906947911, 1}
	);
	check(
		PerlinCube({{"scale_2", Vector3{-1.3, 0, 2.1}}}),
		3,
		2,
		{.4181127548, .4181127548, .4181127548, 1},
		{.5605949163, .5605949163, .5605949163, 1}
	);
	check(
		PerlinCube({{"seed", -17.25}}),
		3,
		2,
		{.4742952585, .4742952585, .4742952585, 1},
		{.5877014995, .5877014995, .5877014995, 1}
	);
	for (const auto &[noiseScale, expected] :
		 std::array<std::pair<double, double>, 2>{{{-3.25, .4717606306}, {0.0, .3199865818}}}) {
		const NodeRun run = PerlinCube({{"noise_scale", noiseScale}});
		check(
			run,
			3,
			2,
			{expected, expected, expected, 1},
			{noiseScale == 0 ? .3199865818 : .5680522323,
			 noiseScale == 0 ? .3199865818 : .5680522323,
			 noiseScale == 0 ? .3199865818 : .5680522323,
			 1}
		);
	}
	for (const auto &[iteration, expected] : std::array<std::pair<int64_t, double>, 4>{
			 {{0, .1333333403}, {-1, .1333333403}, {1, .6491907239}, {4, .4823480546}}
		 }) {
		const double cross = iteration == 0 || iteration == -1 ? .1333333403
							 : iteration == 1				   ? .4725705087
															   : .4396613836;
		check(
			PerlinCube({{"iteration", iteration}}),
			3,
			2,
			{expected, expected, expected, 1},
			{cross, cross, cross, 1}
		);
	}
	check(
		PerlinCube({{"position_2", -1.7}}),
		3,
		2,
		{.5178050399, .5178050399, .5178050399, 1},
		{.4046441019, .4046441019, .4046441019, 1}
	);
	check(
		PerlinCube({{"dimension", Vector2{3.6, 2.6}}}),
		1,
		1,
		{.5923324823, .5923324823, .5923324823, 1},
		{.5673570037, .5673570037, .5673570037, 1}
	);
	check(
		PerlinCube({{"dimension", Vector2{3.6, 2.6}}}),
		3,
		2,
		{.3767407835, .3767407835, .3767407835, 0},
		{.5169343352, .5169343352, .5169343352, 1}
	);
	check(
		PerlinCube({{"level", Vector2{1.3, -.2}}}),
		3,
		2,
		{.4821949005, .4821949005, .4821949005, 1},
		{.6117675900, .6117675900, .6117675900, 1}
	);
	check(
		PerlinCube({{"scale", 0.0}}),
		3,
		2,
		{.6062052846, .6062052846, .6062052846, 1},
		{.3882323503, .3882323503, .3882323503, 1}
	);
	check(
		PerlinCube({{"scale_2", Vector3{0, 0, 0}}}),
		3,
		2,
		{.3199865818, .3199865818, .3199865818, 1},
		{.3199865818, .3199865818, .3199865818, 1}
	);
	check(
		PerlinCube({{"iteration", int64_t{1}}, {"position", Vector3{3e38, 3e38, 3e38}}}),
		3,
		2,
		{.1333333403, .1333333403, .1333333403, 1},
		{.1333333403, .1333333403, .1333333403, 1}
	);
	const NodeRun usedFinalPosition =
		PerlinCube({{"iteration", int64_t{2}}, {"position", Vector3{3e38, 3e38, 3e38}}});
	CHECK_FALSE(usedFinalPosition.Ok);
	CHECK(usedFinalPosition.Code == Status::InvalidValue);
	CHECK(usedFinalPosition.Port == "noise_scale");
	CHECK(usedFinalPosition.Images.empty());
}
