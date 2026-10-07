#include "../src/ProcessorBatch.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.source_tile_transform")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Image Labels2x2() {
		return {2, 2, {10, 0, 0, 255, 20, 0, 0, 255, 30, 0, 0, 255, 40, 0, 0, 255}, 0};
	}
	Image FloatImage(uint32_t width, uint32_t height, SurfacePixel pixel) {
		Image image{
			width, height, std::vector<uint8_t>(size_t(width) * height * 16), 0, SurfaceFormat::RGBA32Float
		};
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x)
				REQUIRE(StoreSurfacePixel(image, x, y, pixel));
		return image;
	}
	uint8_t Red(const Image &image, uint32_t x, uint32_t y) {
		return image.Pixels[(size_t(y) * image.Width + x) * 4];
	}
	uint8_t Alpha(const Image &image, uint32_t x, uint32_t y) {
		return image.Pixels[(size_t(y) * image.Width + x) * 4 + 3];
	}
	Document TileGraph() {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"source", "image.captured", "", {}, {{"source_id", std::string("source")}}},
			{"tile",
			 "pc.tile",
			 "",
			 {},
			 {{"scaling_type", EnumValue{0}}, {"dimension", Vector2{4, 4}}, {"dimension_unit", EnumValue{0}}}}
		};
		document.Links = {{"source", "image", "tile", "surface_in"}};
		document.Outputs = {{"out", "tile", "surface_out"}};
		return document;
	}
	AtlasValue SurfaceAtlas(Image image) {
		AtlasValue atlas;
		auto &data = atlas.Data.emplace();
		data.Kind = AtlasKind::SurfaceAtlas;
		data.Surface.Data = std::move(image);
		data.Dimension = {1, 1};
		return atlas;
	}
	std::vector<uint8_t> Reds(const Image &image) {
		std::vector<uint8_t> reds;
		reds.reserve(size_t(image.Width) * image.Height);
		for (size_t offset = 0; offset < image.Pixels.size(); offset += 4)
			reds.push_back(image.Pixels[offset]);
		return reds;
	}
}

TEST_CASE(
	"Tile shader keeps raw output dimensions and excludes the rounded rectangle edge", "[source_2d][tile]"
) {
	const auto source = Labels2x2();
	const auto map = FloatImage(1, 1, {.45, .75, 0, 1});
	const auto run = RunNode(
		"pc.tile",
		{{"surface_in", &source}, {"uv_map", &map}},
		{{"dimension", Vector2{2.5, 3.5}}, {"dimension_unit", EnumValue{0}}}
	);
	REQUIRE(run.Ok);
	CHECK(run.Output().Width == 2);
	CHECK(run.Output().Height == 4);
	CHECK(Red(run.Output(), 0, 0) == 20);
	CHECK(Alpha(run.Output(), 0, 3) == 0);
	const auto zero = RunNode(
		"pc.tile", {{"surface_in", &source}}, {{"dimension", Vector2{}}, {"dimension_unit", EnumValue{0}}}
	);
	REQUIRE(zero.Ok);
	CHECK(Alpha(zero.Output(), 0, 0) == 0);
}

TEST_CASE(
	"Tile reference spacing retains one preview-row raw size across processor arrays", "[source_2d][tile]"
) {
	const auto source = Labels2x2();
	auto document = TileGraph();
	document.Nodes[1].Values = {{"scaling_type", EnumValue{1}}, {"spacing", Vector2{.375, 0}}};
	document.Junctions = {
		{"amounts", "", ValueType::Array, ArrayValue{ValueType::Vector2, {Vector2{2, 2}, Vector2{3, 2}}}}
	};
	document.Links.push_back({"amounts", "value", "tile", "amount"});
	const std::array<RequestImageSource, 1> sources{{{"source", source}}};
	EvaluationRequest request;
	request.ImageSources = sources;
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	ImageArray firstPreview;
	const auto firstStatus = EvaluateArray(document, plan, "out", request, firstPreview, diagnostic);
	INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
	REQUIRE(firstStatus == Status::Ok);
	REQUIRE(firstPreview.Images.size() == 2);
	CHECK(firstPreview.Images[0].Width == 4);
	CHECK(firstPreview.Images[1].Width == 6);
	CHECK(Alpha(firstPreview.Images[0], 3, 0) == 255);
	CHECK(Alpha(firstPreview.Images[1], 3, 0) == 255);
	document.Nodes[1].SourceProperties = {{"preview_index", int64_t(1)}};
	Document restored;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	REQUIRE(restored == document);
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	ImageArray secondPreview;
	REQUIRE(EvaluateArray(restored, plan, "out", request, secondPreview, diagnostic) == Status::Ok);
	CHECK(Alpha(secondPreview.Images[0], 3, 0) == 0);
	CHECK(Alpha(secondPreview.Images[1], 3, 0) == 0);
	restored.Nodes[1].SourceProperties = {{"preview_index", int64_t(2)}};
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	ImageArray refused;
	CHECK(EvaluateArray(restored, plan, "out", request, refused, diagnostic) == Status::InvalidValue);
	CHECK(diagnostic.Port == "preview_index");
	CHECK(refused.Images.empty());
}

TEST_CASE(
	"Tile admits cumulative work and later malformed rows before any output observer", "[source_2d][tile]"
) {
	const auto source = Labels2x2();
	const auto *entry = FindCatalogueEntry("pc.tile");
	REQUIRE(entry);
	Node node{"tile", "pc.tile", "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.Images = {{"surface_in", &source}};
	for (const auto &input : entry->Inputs)
		if (const auto value = CatalogueDefault(input)) context.Values.emplace_back(input.Id, *value);
	for (auto &[port, value] : context.Values) {
		if (port == "dimension")
			value = ArrayValue{ValueType::Vector2, {Vector2{800, 800}, Vector2{800, 800}}};
		if (port == "dimension_unit") value = EnumValue{0};
	}
	context.InputProvenanceResolved = true;
	Status expected = Status::LimitExceeded;
	std::string_view failedPort = "surface_out";
	SECTION("complete batch exceeds work") {}
	SECTION("later row has zero scale") {
		for (auto &[port, value] : context.Values) {
			if (port == "dimension") value = ArrayValue{ValueType::Vector2, {Vector2{4, 4}, Vector2{4, 4}}};
			if (port == "scale") value = ArrayValue{ValueType::Vector2, {Vector2{1, 1}, Vector2{0, 1}}};
		}
		expected = Status::InvalidValue;
		failedPort = "scale";
	}
	size_t observed = 0;
	const auto observer = [](detail::NodeContext &, void *state) {
		++*static_cast<size_t *>(state);
		return true;
	};
	const auto executor = detail::FindExecutor("pc.tile");
	REQUIRE(executor);
	CHECK_FALSE(detail::RunProcessorBatch(context, executor, observer, &observed));
	CHECK(context.FailureCode == expected);
	CHECK(context.FailurePort == failedPort);
	CHECK(observed == 0);
	CHECK(context.OutputImages.empty());
	CHECK_FALSE(context.TileReferenceDimension.has_value());
}

TEST_CASE("Tile rejects derived overflow and honors requested output depth", "[source_2d][tile]") {
	const auto source = Labels2x2();
	const auto bad = RunNode(
		"pc.tile",
		{{"surface_in", &source}},
		{{"dimension", Vector2{2, 2}},
		 {"dimension_unit", EnumValue{0}},
		 {"scale", Vector2{std::numeric_limits<float>::denorm_min(), 1}}}
	);
	CHECK_FALSE(bad.Ok);
	CHECK(bad.Code == Status::InvalidValue);
	CHECK(bad.Images.empty());
	const auto floating = RunNode(
		"pc.tile",
		{{"surface_in", &source}},
		{{"dimension", Vector2{2, 2}},
		 {"dimension_unit", EnumValue{0}},
		 {"attribute_color_depth", EnumValue{5}}}
	);
	REQUIRE(floating.Ok);
	CHECK(floating.Output().Format == SurfaceFormat::RGBA32Float);
}

TEST_CASE("Tile repeats a labelled surface at fixed pixel dimensions", "[source_2d][tile]") {
	const Image source = Labels2x2();
	const auto run = RunNode(
		"pc.tile",
		{{"surface_in", &source}},
		{{"scaling_type", EnumValue{0}},
		 {"dimension", Vector2{4, 4}},
		 {"dimension_unit", EnumValue{0}},
		 {"spacing", Vector2{0, 0}},
		 {"spacing_unit", EnumValue{0}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(run.Output().Width == 4);
	CHECK(run.Output().Height == 4);
	CHECK(
		Reds(run.Output()) ==
		std::vector<uint8_t>{10, 20, 10, 20, 30, 40, 30, 40, 10, 20, 10, 20, 30, 40, 30, 40}
	);

	const auto relative = RunNode(
		"pc.tile",
		{{"surface_in", &source}},
		{{"scaling_type", EnumValue{1}}, {"amount", Vector2{2, 2}}, {"dimension_unit", EnumValue{0}}}
	);
	INFO(relative.Message);
	REQUIRE(relative.Ok);
	CHECK(relative.Output().Width == 4);
	CHECK(relative.Output().Height == 4);
	CHECK(Reds(relative.Output()) == Reds(run.Output()));
}

TEST_CASE("Tile flip grid and quarter polar patterns orient repeated labels", "[source_2d][tile]") {
	const auto source = Labels2x2();
	const auto flipped = RunNode(
		"pc.tile",
		{{"surface_in", &source}},
		{{"pattern", EnumValue{1}}, {"dimension", Vector2{4, 4}}, {"dimension_unit", EnumValue{0}}}
	);
	INFO(flipped.Message);
	REQUIRE(flipped.Ok);
	CHECK(
		Reds(flipped.Output()) ==
		std::vector<uint8_t>{10, 20, 20, 10, 30, 40, 40, 30, 30, 40, 40, 30, 10, 20, 20, 10}
	);

	const auto polar = RunNode(
		"pc.tile",
		{{"surface_in", &source}},
		{{"pattern", EnumValue{2}}, {"dimension", Vector2{4, 4}}, {"dimension_unit", EnumValue{0}}}
	);
	INFO(polar.Message);
	REQUIRE(polar.Ok);
	CHECK(Red(polar.Output(), 2, 0) == 20);
	CHECK(Red(polar.Output(), 3, 0) == 40);
	CHECK(
		polar.Output().Pixels != RunNode(
									 "pc.tile",
									 {{"surface_in", &source}},
									 {{"dimension", Vector2{4, 4}}, {"dimension_unit", EnumValue{0}}}
								 )
									 .Output()
									 .Pixels
	);
}

TEST_CASE("Tile alternates row and column shifts and leaves spacing gaps transparent", "[source_2d][tile]") {
	const auto source = Labels2x2();
	const auto shiftedRows = RunNode(
		"pc.tile",
		{{"surface_in", &source}},
		{{"shift_axis", EnumValue{0}},
		 {"shift", .5},
		 {"spacing_unit", EnumValue{0}},
		 {"dimension", Vector2{4, 4}},
		 {"dimension_unit", EnumValue{0}}}
	);
	INFO(shiftedRows.Message);
	REQUIRE(shiftedRows.Ok);
	CHECK(Red(shiftedRows.Output(), 0, 0) == 10);
	CHECK(Red(shiftedRows.Output(), 0, 2) == 20);
	const auto shiftedColumns = RunNode(
		"pc.tile",
		{{"surface_in", &source}},
		{{"shift_axis", EnumValue{1}},
		 {"shift", .5},
		 {"dimension", Vector2{4, 4}},
		 {"dimension_unit", EnumValue{0}}}
	);
	REQUIRE(shiftedColumns.Ok);
	CHECK(Red(shiftedColumns.Output(), 2, 0) == 30);

	const auto gaps = RunNode(
		"pc.tile",
		{{"surface_in", &source}},
		{{"spacing", Vector2{1, 0}},
		 {"spacing_unit", EnumValue{0}},
		 {"dimension", Vector2{4, 4}},
		 {"dimension_unit", EnumValue{0}}}
	);
	INFO(gaps.Message);
	REQUIRE(gaps.Ok);
	CHECK(Alpha(gaps.Output(), 0, 0) == 255);
	CHECK(Alpha(gaps.Output(), 2, 0) == 0);
	CHECK(Red(gaps.Output(), 3, 0) == 10);
}

TEST_CASE("Tile applies position, scale, and rotation in source order", "[source_2d][tile]") {
	const auto source = Labels2x2();
	const auto position = RunNode(
		"pc.tile",
		{{"surface_in", &source}},
		{{"posiiton", Vector2{1, 0}},
		 {"posiiton_unit", EnumValue{0}},
		 {"dimension", Vector2{4, 4}},
		 {"dimension_unit", EnumValue{0}}}
	);
	INFO(position.Message);
	REQUIRE(position.Ok);
	CHECK(Red(position.Output(), 0, 0) == 20);

	const auto scale = RunNode(
		"pc.tile",
		{{"surface_in", &source}},
		{{"scale", Vector2{2, 1}}, {"dimension", Vector2{4, 4}}, {"dimension_unit", EnumValue{0}}}
	);
	REQUIRE(scale.Ok);
	CHECK(Red(scale.Output(), 0, 0) == 10);
	CHECK(Red(scale.Output(), 1, 0) == 10);
	CHECK(Red(scale.Output(), 2, 0) == 20);
	CHECK(Red(scale.Output(), 3, 0) == 20);

	const auto rotation = RunNode(
		"pc.tile",
		{{"surface_in", &source}},
		{{"dimension", Vector2{2, 2}}, {"dimension_unit", EnumValue{0}}, {"rotation", 90.}}
	);
	INFO(rotation.Message);
	REQUIRE(rotation.Ok);
	CHECK(Reds(rotation.Output()) == std::vector<uint8_t>{20, 40, 10, 30});
}

TEST_CASE("Tile UV map flips green and scales alpha even when UV mix is zero", "[source_2d][tile]") {
	const auto source = Labels2x2();
	const auto neutral = FloatImage(1, 1, {.5, 1, .25, .4});
	const auto zeroMix = RunNode(
		"pc.tile",
		{{"surface_in", &source}, {"uv_map", &neutral}},
		{{"dimension", Vector2{2, 2}}, {"dimension_unit", EnumValue{0}}, {"uv_mix", 0.}}
	);
	INFO(zeroMix.Message);
	REQUIRE(zeroMix.Ok);
	CHECK(zeroMix.Output().Pixels[0] == source.Pixels[0]);
	CHECK(zeroMix.Output().Pixels[4] == source.Pixels[4]);
	CHECK(Alpha(zeroMix.Output(), 0, 0) == 102);

	const auto greenHigh = FloatImage(1, 1, {.5, .75, .25, 1});
	const auto greenLow = FloatImage(1, 1, {.5, .25, .25, 1});
	const auto flipped = RunNode(
		"pc.tile",
		{{"surface_in", &source}, {"uv_map", &greenHigh}},
		{{"dimension", Vector2{2, 2}}, {"dimension_unit", EnumValue{0}}, {"uv_mix", 1.}}
	);
	const auto unflipped = RunNode(
		"pc.tile",
		{{"surface_in", &source}, {"uv_map", &greenLow}},
		{{"dimension", Vector2{2, 2}}, {"dimension_unit", EnumValue{0}}, {"uv_mix", 1.}}
	);
	REQUIRE(flipped.Ok);
	REQUIRE(unflipped.Ok);
	CHECK(flipped.Output().Pixels != unflipped.Output().Pixels);
}

TEST_CASE("Tile keeps typed floating HDR and normalized output formats", "[source_2d][tile]") {
	for (SurfaceFormat format :
		 {SurfaceFormat::RGBA8Unorm, SurfaceFormat::RGBA16Float, SurfaceFormat::RGBA32Float}) {
		const auto layout = CheckedSurfaceLayout(2, 2, format, Limits::MaximumEvaluationBytes);
		REQUIRE(layout);
		Image source{2, 2, std::vector<uint8_t>(size_t(layout->Bytes)), 0, format};
		REQUIRE(StoreSurfacePixel(
			source,
			0,
			0,
			format == SurfaceFormat::RGBA8Unorm ? SurfacePixel{.25, .5, .75, 1} : SurfacePixel{2, 1.5, .75, 1}
		));
		const auto run = RunNode(
			"pc.tile",
			{{"surface_in", &source}},
			{{"dimension", Vector2{2, 2}}, {"dimension_unit", EnumValue{0}}}
		);
		INFO("format " << unsigned(format) << ": " << run.Message);
		REQUIRE(run.Ok);
		CHECK(run.Output().Format == format);
		SurfacePixel output{};
		REQUIRE(LoadSurfacePixel(run.Output(), 0, 0, output));
		if (format != SurfaceFormat::RGBA8Unorm)
			CHECK(output[0] == 2);
		else
			CHECK(run.Output().Pixels == source.Pixels);
	}
}

TEST_CASE(
	"Tile clamps and rounds fixed dimensions and refuses undefined sampling controls", "[source_2d][tile]"
) {
	const auto source = Labels2x2();
	const auto rounded = RunNode(
		"pc.tile",
		{{"surface_in", &source}},
		{{"dimension", Vector2{2.5, 3.5}}, {"dimension_unit", EnumValue{0}}}
	);
	INFO(rounded.Message);
	REQUIRE(rounded.Ok);
	CHECK(rounded.Output().Width == 2);
	CHECK(rounded.Output().Height == 4);
	const auto clamped = RunNode(
		"pc.tile", {{"surface_in", &source}}, {{"dimension", Vector2{0, 0}}, {"dimension_unit", EnumValue{0}}}
	);
	REQUIRE(clamped.Ok);
	CHECK(clamped.Output().Width == 1);
	CHECK(clamped.Output().Height == 1);

	const auto badScale = RunNode("pc.tile", {{"surface_in", &source}}, {{"scale", Vector2{0, 1}}});
	CHECK_FALSE(badScale.Ok);
	CHECK(badScale.Port == "scale");
	CHECK(badScale.Images.empty());
	const auto badSpacing = RunNode(
		"pc.tile", {{"surface_in", &source}}, {{"spacing", Vector2{-2, 0}}, {"spacing_unit", EnumValue{0}}}
	);
	CHECK_FALSE(badSpacing.Ok);
	CHECK(badSpacing.Port == "spacing");
	CHECK(badSpacing.Images.empty());
}

TEST_CASE("Tile refuses raw main and auxiliary Atlas bindings", "[source_2d][tile]") {
	const auto source = Labels2x2();
	const auto main = RunNode(
		"pc.tile",
		{},
		{{"surface_in", SurfaceAtlas(source)}, {"dimension", Vector2{4, 4}}, {"dimension_unit", EnumValue{0}}}
	);
	const auto plain = RunNode(
		"pc.tile", {{"surface_in", &source}}, {{"dimension", Vector2{4, 4}}, {"dimension_unit", EnumValue{0}}}
	);
	INFO(main.Message);
	CHECK_FALSE(main.Ok);
	CHECK(main.Code == Status::UnsupportedExecution);
	CHECK(main.Port == "surface_in");
	CHECK(main.Images.empty());
	REQUIRE(plain.Ok);
	const auto raw =
		RunNode("pc.tile", {{"surface_in", &source}}, {{"uv_map", SurfaceAtlas(source)}, {"uv_mix", 1.}});
	CHECK_FALSE(raw.Ok);
	CHECK(raw.Code == Status::UnsupportedExecution);
	CHECK(raw.Port == "uv_map");
}

TEST_CASE("Tile linked arrays expand patterns and animated rotation round-trips", "[source_2d][tile]") {
	const auto source = Labels2x2();
	auto document = TileGraph();
	ArrayValue patterns{ValueType::Enum, {EnumValue{0}, EnumValue{1}}};
	document.Junctions = {{"patterns", "", ValueType::Array, std::move(patterns)}};
	document.Links.push_back({"patterns", "value", "tile", "pattern"});
	document.Nodes[1].Values.push_back({"attribute_array_process", EnumValue{2}});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const std::array<RequestImageSource, 1> sources{{{"source", source}}};
	EvaluationRequest request;
	request.ImageSources = sources;
	ImageArray output;
	const auto status = EvaluateArray(document, plan, "out", request, output, diagnostic);
	INFO(diagnostic.NodeId << ":" << diagnostic.Port << ": " << diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(output.Images.size() == 2);
	CHECK(Reds(output.Images[0]) != Reds(output.Images[1]));
	CHECK(
		Reds(output.Images[0]) == Reds(RunNode(
										   "pc.tile",
										   {{"surface_in", &source}},
										   {{"dimension", Vector2{4, 4}}, {"dimension_unit", EnumValue{0}}}
								  ).Output())
	);

	auto animated = TileGraph();
	animated.Nodes[1].SourceAnimatedInputs = {"rotation"};
	animated.Keyframes = {{"tile", "rotation", 0, 0.}, {"tile", "rotation", 1, 90.}};
	Document restored;
	REQUIRE(Read(Write(animated), restored, diagnostic) == Status::Ok);
	REQUIRE(restored == animated);
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	request.Tick = 0;
	Image first;
	REQUIRE(Evaluate(restored, plan, "out", request, first, diagnostic) == Status::Ok);
	request.Tick = 1;
	Image later;
	REQUIRE(Evaluate(restored, plan, "out", request, later, diagnostic) == Status::Ok);
	CHECK(Reds(first) != Reds(later));
}
