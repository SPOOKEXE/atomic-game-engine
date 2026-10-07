#include "../src/AtlasPayload.hpp"
#include "ComplexGeneratorFixture.hpp"

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
#include <vector>

TEST_SUITE_ID("engine.imagegraph.source_shard_noise")
using namespace complex_generator_test;
using namespace engine::imagegraph;
using imagegraph_test::NodeRun;

namespace {
	Image FloatPixel(SurfacePixel pixel) {
		Image image{1, 1, std::vector<uint8_t>(16), 0, SurfaceFormat::RGBA32Float};
		REQUIRE(StoreSurfacePixel(image, 0, 0, pixel));
		return image;
	}
	SurfacePixel Pixel(const Image &image, uint32_t x = 0, uint32_t y = 0) {
		SurfacePixel pixel{};
		REQUIRE(LoadSurfacePixel(image, x, y, pixel));
		return pixel;
	}
	NodeRun RunShard(
		std::initializer_list<std::pair<std::string_view, Value>> extra = {},
		std::initializer_list<std::pair<std::string_view, const Image *>> images = {}
	) {
		std::vector<std::pair<std::string_view, Value>> values{
			{"dimension", Vector2{1, 1}},
			{"dimension_unit", EnumValue{0}},
			{"position", Vector2{0, 0}},
			{"position_unit", EnumValue{0}},
			{"seed", 17.25},
			{"rotation", 0.0},
			{"progress_mapped", true},
			{"progress_map_range", Vector2{0, 0}},
			{"sharpness_mapped", true},
			{"sharpness_map_range", Vector2{1, 1}},
			{"scale_mapped", true},
			{"scale_map_range", Vector2{4, 4}},
			{"level_in", Vector2{0, 1}},
			{"level_out", Vector2{0, 1}},
			{"uv_mix", 1.0},
			{"attribute_color_depth", EnumValue{5}}
		};
		for (const auto &[port, value] : extra) {
			const auto current = std::find_if(values.begin(), values.end(), [&](const auto &item) {
				return item.first == port;
			});
			if (current == values.end())
				values.emplace_back(port, value);
			else
				current->second = value;
		}
		NodeRun run;
		const CatalogueEntry *entry = FindCatalogueEntry("pc.shard_noise");
		const auto executor = detail::FindExecutor("pc.shard_noise");
		REQUIRE(entry);
		REQUIRE(executor);
		Node node{"node", "pc.shard_noise", "", {}, {}};
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		for (const auto &[port, image] : images)
			context.Images.emplace_back(port, image);
		for (const auto &input : entry->Inputs) {
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
		for (const auto &[port, value] : context.Values)
			context.ValueViews.emplace_back(port, &value);
		context.InputProvenanceResolved = true;
		run.Ok = executor(context) && context.FailureCode == Status::Ok;
		run.Code = context.FailureCode;
		run.Message = context.FailureMessage;
		run.Port = context.FailurePort;
		run.Images = std::move(context.OutputImages);
		run.Values = std::move(context.OutputValues);
		return run;
	}
	void CheckGray(const NodeRun &run, double expected, uint32_t x = 0, uint32_t y = 0, double alpha = 1) {
		INFO(run.Port << ": " << run.Message);
		REQUIRE(run.Ok);
		const auto pixel = Pixel(run.Output(), x, y);
		for (size_t channel = 0; channel < 3; ++channel)
			CHECK(pixel[channel] == Catch::Approx(expected).margin(5e-5));
		CHECK(pixel[3] == Catch::Approx(alpha).margin(1e-6));
	}
	Document Shard(Vector2 dimension = {4, 3}) {
		auto document = Graph("pc.shard_noise", dimension);
		Set(document, "progress_mapped", true);
		Set(document, "sharpness_mapped", true);
		Set(document, "scale_mapped", true);
		return document;
	}

	AtlasValue SurfaceAtlas() {
		AtlasValue atlas;
		auto &data = atlas.Data.emplace();
		data.Kind = AtlasKind::SurfaceAtlas;
		data.Surface.Data = {1, 1, {64, 128, 192, 255}, 0};
		data.Scale = {1, 1};
		data.Dimension = {1, 1};
		return atlas;
	}

	void CheckNativeFormats(const Document &document) {
		constexpr std::array formats{
			SurfaceFormat::RGBA4Unorm,
			SurfaceFormat::RGBA8Unorm,
			SurfaceFormat::RGBA16Float,
			SurfaceFormat::RGBA32Float,
			SurfaceFormat::R8Unorm,
			SurfaceFormat::R16Float,
			SurfaceFormat::R32Float
		};
		for (int64_t depth = 2; depth <= 8; ++depth) {
			auto current = document;
			Set(current, "attribute_color_depth", EnumValue{depth});
			const auto image = Draw(current);
			CHECK(image.Format == formats[size_t(depth - 2)]);
		}
	}
	ImageArray DrawArray(const Document &document) {
		Plan plan;
		Diagnostic diagnostic;
		const auto compiled = Compile(document, plan, diagnostic);
		INFO(diagnostic.NodeId + ":" + diagnostic.Port + ":" + diagnostic.Message);
		REQUIRE(compiled == Status::Ok);
		ImageArray images;
		const auto status = EvaluateArray(document, plan, "out", {}, images, diagnostic);
		INFO(diagnostic.NodeId + ":" + diagnostic.Port + ":" + diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return images;
	}
}

TEST_CASE(
	"Shard Noise matches independent float samples across controls and raw coordinates",
	"[source_shard_noise]"
) {
	constexpr std::array<std::pair<double, double>, 3> seeds{
		{{17.25, .6944754123687744}, {-17.25, .2502155900001526}, {10017.25, .6944754123687744}}
	};
	for (const auto &[seed, expected] : seeds)
		CheckGray(RunShard({{"seed", seed}}), expected);
	constexpr std::array<std::pair<double, double>, 3> sharpness{
		{{0, .5}, {.5, .5814668536186218}, {2, .7288069725036621}}
	};
	for (const auto &[sharp, expected] : sharpness)
		CheckGray(RunShard({{"sharpness", sharp}}), expected);

	const Image uv = FloatPixel({.3125, .3125, 0, 1});
	const auto offCenter = [&](double progress, double rotation, double sharp) {
		return RunShard(
			{{"dimension", Vector2{3, 2}},
			 {"position", Vector2{.1, -.2}},
			 {"scale", Vector2{9.25, 11.75}},
			 {"seed", -17.25},
			 {"progress", progress},
			 {"sharpness", sharp},
			 {"rotation", rotation}},
			{{"uv_map", &uv}}
		);
	};
	CheckGray(offCenter(37.5, 37, .65), .7575515508651733);
	CheckGray(offCenter(-37.5, 37, .65), .568216860294342);
	CheckGray(offCenter(37.5, -53, .65), .2448052167892456);
	const NodeRun reversedLevels = RunShard(
		{{"dimension", Vector2{3, 2}},
		 {"position", Vector2{.1, -.2}},
		 {"scale", Vector2{9.25, 11.75}},
		 {"seed", -17.25},
		 {"progress", 37.5},
		 {"sharpness", .65},
		 {"rotation", 37.0},
		 {"level_in", Vector2{.8, .2}},
		 {"level_out", Vector2{1.25, -.25}}},
		{{"uv_map", &uv}}
	);
	CheckGray(reversedLevels, 1.1438788175582886);

	const Image mapQuarter = FloatPixel({.25, .25, .25, 1});
	const NodeRun mappedControls = RunShard(
		{{"dimension", Vector2{3, 2}},
		 {"position", Vector2{.1, -.2}},
		 {"scale_map_range", Vector2{2, 6}},
		 {"progress_map_range", Vector2{10, 90}},
		 {"sharpness_map_range", Vector2{.25, 1.25}},
		 {"seed", -17.25},
		 {"rotation", 37.0}},
		{{"uv_map", &uv},
		 {"scale_map", &mapQuarter},
		 {"progress_map", &mapQuarter},
		 {"sharpness_map", &mapQuarter}}
	);
	CheckGray(mappedControls, .48468923568725586);
	CheckGray(offCenter(37.5, 37, .25), .5482778549194336);
	CheckGray(offCenter(37.5, 37, 1.25), .9170755743980408);

	const NodeRun fractional = RunShard(
		{{"dimension", Vector2{1.6, 2.6}},
		 {"scale", Vector2{5.5, 7.25}},
		 {"seed", 17.25},
		 {"progress", 25.0},
		 {"sharpness", .4},
		 {"level_in", Vector2{.2, .8}},
		 {"level_out", Vector2{-.25, 1.25}}}
	);
	CheckGray(fractional, .5682424306869507, 0, 1);

	const Image uvLow = FloatPixel({.25, .2, 0, .5});
	const Image uvHigh = FloatPixel({.25, .8, 0, .5});
	const auto uvCase = [&](const Image &map, double mix) {
		return RunShard(
			{{"scale", Vector2{3, 3}}, {"progress", 12.5}, {"sharpness", 1.0}, {"uv_mix", mix}},
			{{"uv_map", &map}}
		);
	};
	const NodeRun flippedLow = uvCase(uvLow, 1);
	const NodeRun flippedHigh = uvCase(uvHigh, 1);
	const NodeRun zeroMix = uvCase(uvLow, 0);
	CheckGray(flippedLow, .12112119793891907, 0, 0, .5);
	CheckGray(flippedHigh, .5164421200752258, 0, 0, .5);
	for (const NodeRun *run : {&flippedLow, &flippedHigh, &zeroMix}) {
		const auto pixel = Pixel(run->Output());
		CHECK(pixel[3] == Catch::Approx(.5).margin(1e-6));
	}
	const NodeRun plain = RunShard({{"scale", Vector2{3, 3}}, {"progress", 12.5}, {"sharpness", 1.0}});
	for (size_t channel = 0; channel < 3; ++channel)
		CHECK(Pixel(zeroMix.Output())[channel] == Catch::Approx(Pixel(plain.Output())[channel]).margin(1e-6));
}

TEST_CASE("Shard Noise mask path retains the shared RGBA8 staging boundary", "[source_shard_noise]") {
	auto document = Shard({1, 1});
	document.Nodes.push_back(Solid("mask", {1, 1}, {96, 96, 96, 96}));
	document.Links = {{"mask", "surface_out", "generator", "mask"}};
	const Image masked = Draw(document);
	const Image base = Draw(Shard({1, 1}));

	Set(document, "mask_alpha_only", true);
	const Image alphaOnly = Draw(document);
	CHECK(masked == alphaOnly);
	CHECK(masked.Format == SurfaceFormat::RGBA8Unorm);
	Image expected{1, 1, std::vector<uint8_t>(4), 0, SurfaceFormat::RGBA8Unorm};
	auto pixel = Pixel(base);
	const double maskChannel = 96. / 255.;
	pixel[3] *= maskChannel * maskChannel;
	REQUIRE(StoreSurfacePixel(expected, 0, 0, pixel));
	expected.Hash = SurfaceHash(expected);
	CHECK(masked == expected);

	CheckNativeFormats(Shard({1, 1}));
}

TEST_CASE("Shard Noise linked vector sources match authored position and scale", "[source_shard_noise]") {
	for (const std::string_view port : {"position", "scale"}) {
		auto linked = Shard({4, 3});
		Set(linked, "position_unit", EnumValue{0});
		linked.Nodes.push_back({"vector", "pc.vector2", "", {}, {{"x", .25}, {"y", -.5}}});
		linked.Links = {{"vector", "vector", "generator", std::string(port)}};

		auto direct = Shard({4, 3});
		Set(direct, "position_unit", EnumValue{0});
		Set(direct, std::string(port), Vector2{.25, -.5});
		CHECK(Draw(linked) == Draw(direct));
	}
}

TEST_CASE("Shard Noise resolves Surface and whole SurfaceArray vector inputs", "[source_shard_noise]") {
	for (const std::string_view port : {"position", "scale"}) {
		auto linked = Shard({4, 3});
		Set(linked, "position_unit", EnumValue{0});
		linked.Nodes.push_back(Solid("surface", {2, 1}, {255, 255, 255, 255}));
		linked.Links = {{"surface", "surface_out", "generator", std::string(port)}};
		auto direct = Shard({4, 3});
		Set(direct, std::string(port), Vector2{2, 1});
		Set(direct, "position_unit", EnumValue{0});
		CHECK(Draw(linked) == Draw(direct));

		auto linkedArray = Shard({4, 3});
		SurfaceRows(linkedArray, port);
		Set(linkedArray, "position_unit", EnumValue{0});
		auto wholeArray = Shard({4, 3});
		Set(wholeArray, std::string(port), Vector2{1, 1});
		Set(wholeArray, "position_unit", EnumValue{0});
		CHECK(Draw(linkedArray) == Draw(wholeArray));
	}
}

TEST_CASE("Shard Noise Dimension projects varying and equal SurfaceArray rows", "[source_shard_noise]") {
	auto surfaceRows = Shard({1, 1});
	SurfaceRows(surfaceRows, "dimension");
	Set(surfaceRows, "dimension_unit", EnumValue{0});
	auto valueRows = Shard({1, 1});
	valueRows.Nodes.push_back(Array("dimensions", ValueType::Vector2, Vector2{2, 1}, Vector2{3, 2}));
	valueRows.Links = {{"dimensions", "array", "generator", "dimension"}};
	Set(valueRows, "dimension_unit", EnumValue{0});
	const ImageArray surfaces = DrawArray(surfaceRows);
	const ImageArray values = DrawArray(valueRows);
	REQUIRE(surfaces.Images.size() == 2);
	REQUIRE(values.Images.size() == 2);
	CHECK(surfaces.Images == values.Images);

	auto equalSurfaces = Shard({1, 1});
	equalSurfaces.Nodes.push_back(Solid("first", {2, 1}, {255, 255, 255, 255}));
	equalSurfaces.Nodes.push_back(Solid("second", {2, 1}, {255, 255, 255, 255}));
	Node list{"surfaces", "value.array", "", {}, {}};
	list.DynamicInputs = {
		{"first", ValueType::Image, std::nullopt}, {"second", ValueType::Image, std::nullopt}
	};
	equalSurfaces.Nodes.push_back(std::move(list));
	equalSurfaces.Links = {
		{"first", "surface_out", "surfaces", "first"},
		{"second", "surface_out", "surfaces", "second"},
		{"surfaces", "array", "generator", "dimension"}
	};
	Set(equalSurfaces, "dimension_unit", EnumValue{0});
	auto scalar = Shard({1, 1});
	Set(scalar, "dimension", Vector2{2, 1});
	Set(scalar, "dimension_unit", EnumValue{0});
	CHECK(Draw(equalSurfaces) == Draw(scalar));
}

TEST_CASE("Shard Noise raw subpixel canvas clears its unshaded output", "[source_shard_noise]") {
	auto document = Graph("pc.shard_noise", {.2, .2});
	std::erase_if(document.Nodes[0].Values, [](const AuthoredValue &value) { return value.Port == "seed"; });
	const Image image = Draw(document);
	CHECK(image.Width == 1);
	CHECK(image.Height == 1);
	CHECK((image.Pixels == std::vector<uint8_t>{0, 0, 0, 0}));
}

TEST_CASE("Shard Noise refuses missing mappings, seed, and degenerate levels", "[source_shard_noise]") {
	for (const auto &[toggle, control] :
		 {std::pair<std::string_view, std::string_view>{"progress_mapped", "progress"},
		  {"sharpness_mapped", "sharpness"},
		  {"scale_mapped", "scale"}}) {
		auto document = Shard({1, 1});
		Set(document, std::string(toggle), false);
		Refuse(std::move(document), Status::UnsupportedExecution, control);
	}

	auto missingSeed = Shard({1, 1});
	std::erase_if(missingSeed.Nodes[0].Values, [](const AuthoredValue &value) {
		return value.Port == "seed";
	});
	Refuse(std::move(missingSeed), Status::UnsupportedExecution, "seed");

	auto equalLevels = Shard({1, 1});
	Set(equalLevels, "level_in", Vector2{.5, .5});
	Refuse(std::move(equalLevels), Status::UnsupportedExecution, "level_in");

	const NodeRun negativeSharpness = RunShard({{"sharpness", -.25}});
	CHECK_FALSE(negativeSharpness.Ok);
	CHECK(negativeSharpness.Code == Status::UnsupportedExecution);
	CHECK(negativeSharpness.Port == "sharpness");

	const NodeRun floatOverflow = RunShard({{"scale", Vector2{std::numeric_limits<double>::max(), 4}}});
	CHECK_FALSE(floatOverflow.Ok);
	CHECK(floatOverflow.Code == Status::InvalidValue);
	CHECK(floatOverflow.Port == "scale");

	ArrayValue nestedScale{ValueType::Scalar, {}};
	nestedScale.Nested = {{double{4}, double{4}}};
	const NodeRun nestedMappedScale = RunShard({{"scale", nestedScale}});
	CHECK_FALSE(nestedMappedScale.Ok);
	CHECK(nestedMappedScale.Code == Status::UnsupportedExecution);
	CHECK(nestedMappedScale.Port == "scale");

	const Image black = FloatPixel({0, 0, 0, 1});
	const Image white = FloatPixel({1, 1, 1, 1});
	const NodeRun positiveLow =
		RunShard({{"sharpness_map_range", Vector2{.5, -1}}}, {{"sharpness_map", &black}});
	const NodeRun directPositive = RunShard({{"sharpness", .5}});
	REQUIRE(positiveLow.Ok);
	REQUIRE(directPositive.Ok);
	CHECK(positiveLow.Output().Pixels == directPositive.Output().Pixels);
	const NodeRun negativeHigh =
		RunShard({{"sharpness_map_range", Vector2{.5, -1}}}, {{"sharpness_map", &white}});
	CHECK_FALSE(negativeHigh.Ok);
	CHECK(negativeHigh.Code == Status::UnsupportedExecution);
	CHECK(negativeHigh.Port == "sharpness");
}

TEST_CASE("Shard Noise rejects Atlas data on surface inputs", "[source_shard_noise]") {
	const auto atlas = SurfaceAtlas();
	REQUIRE(detail::ValidAtlasPayload(atlas));
	for (const std::string_view port : {"uv_map", "mask"}) {
		const NodeRun run = RunShard({{port, atlas}});
		INFO(port << ": " << run.Message);
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::UnsupportedExecution);
		CHECK(run.Port == port);
		CHECK(run.Images.empty());
	}
}
