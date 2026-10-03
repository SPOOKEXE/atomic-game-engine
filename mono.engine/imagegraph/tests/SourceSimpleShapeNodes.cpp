#include "NodeHarness.hpp"
#include "nodes/Processor.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.source_simple_shape")
using namespace engine::imagegraph;
namespace {
	Document ShapeGraph(std::string type = "pc.shape_ellipse", Vector2 size = {5, 5}) {
		Document doc;
		doc.FormatVersion = 9;
		doc.Nodes = {{"shape", type, "", {}, {{"dimension", size}, {"dimension_unit", EnumValue{0}}}}};
		doc.Outputs = {{"image", "shape", "surface_out"}};
		return doc;
	}
	Image ShapeImage(const Document &doc) {
		Plan plan;
		Diagnostic diagnostic;
		auto status = Compile(doc, plan, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		Image image;
		status = Evaluate(doc, plan, "image", image, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return image;
	}
	void ExpectBinary(const Image &image, const std::vector<std::string> &rows) {
		REQUIRE(image.Height == rows.size());
		REQUIRE(image.Format == SurfaceFormat::RGBA8Unorm);
		for (uint32_t y = 0; y < image.Height; y++) {
			REQUIRE(image.Width == rows[y].size());
			for (uint32_t x = 0; x < image.Width; x++)
				REQUIRE(
					detail::ReadPixel(image, x, y) ==
					(rows[y][x] == '1' ? detail::Rgba{1, 1, 1, 1} : detail::Rgba{})
				);
		}
	}
}
TEST_CASE(
	"Source simple shapes retain binary pixel center coverage and half boundary", "[source_simple_shape]"
) {
	const auto ellipse = ShapeImage(ShapeGraph());
	ExpectBinary(ellipse, {"01110", "11111", "11111", "11111", "01110"});
	ExpectBinary(ShapeImage(ShapeGraph("pc.shape_rectangle")), {"11111", "11111", "11111", "11111", "11111"});
	ExpectBinary(ShapeImage(ShapeGraph("pc.shape_half")), {"00000", "00000", "11111", "11111", "11111"});
	auto rectangle = ShapeGraph("pc.shape_rectangle");
	rectangle.Nodes[0].Values.push_back({"corner", 1.});
	REQUIRE(ShapeImage(rectangle).Pixels == ellipse.Pixels);
	const auto *entry = FindCatalogueEntry("pc.shape_ellipse");
	REQUIRE(entry);
	const auto *process = FindCatalogueInput(*entry, "attribute_process");
	REQUIRE(process);
	REQUIRE(CatalogueDefault(*process) == Value{false});
}
TEST_CASE(
	"Source rectangle output aspect and half transform ordering remain source specific",
	"[source_simple_shape]"
) {
	auto doc = ShapeGraph("pc.shape_rectangle", {4, 2});
	doc.Nodes[0].Values.push_back({"half_size", Vector2{.25, .5}});
	ExpectBinary(ShapeImage(doc), {"1111", "1111"});
	doc.Nodes[0].Type = "pc.shape_ellipse";
	ExpectBinary(ShapeImage(doc), {"0110", "0110"});
	doc = ShapeGraph("pc.shape_half", {5, 5});
	doc.Nodes[0].Values.push_back({"rotation", 90.});
	auto image = ShapeImage(doc);
	REQUIRE(detail::ReadPixel(image, 0, 0) == detail::Rgba{});
	REQUIRE(detail::ReadPixel(image, 4, 0) == detail::Rgba{1, 1, 1, 1});
	REQUIRE(detail::ReadPixel(image, 0, 4) == detail::Rgba{});
	REQUIRE(detail::ReadPixel(image, 4, 4) == detail::Rgba{1, 1, 1, 1});
	doc.Nodes[0].Values.push_back({"half_size", Vector2{-.5, .5}});
	image = ShapeImage(doc);
	REQUIRE(detail::ReadPixel(image, 0, 0) == detail::Rgba{1, 1, 1, 1});
	REQUIRE(detail::ReadPixel(image, 4, 0) == detail::Rgba{});
}
TEST_CASE(
	"Source simple shape colour and background retain RGBA without alpha multiplication",
	"[source_simple_shape]"
) {
	auto doc = ShapeGraph();
	doc.Project = ProjectSettings{};
	doc.Project->ColorDepth = 5;
	doc.Nodes[0].Values.push_back({"color", Colour{240, 20, 0, 128}});
	doc.Nodes[0].Values.push_back({"bg_color", Colour{0, 200, 0, 64}});
	const auto image = ShapeImage(doc);
	REQUIRE(image.Format == SurfaceFormat::RGBA8Unorm);
	REQUIRE(
		std::vector<uint8_t>(image.Pixels.begin() + 48, image.Pixels.begin() + 52) ==
		std::vector<uint8_t>{240, 20, 0, 128}
	);
	REQUIRE(
		std::vector<uint8_t>(image.Pixels.begin(), image.Pixels.begin() + 4) ==
		std::vector<uint8_t>{0, 200, 0, 64}
	);
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(doc), restored, diagnostic) == Status::Ok);
	REQUIRE(ShapeImage(restored).Pixels == image.Pixels);
}
TEST_CASE(
	"Source simple shape mask gates with background sampler and ignores mask texels", "[source_simple_shape]"
) {
	auto doc = ShapeGraph();
	doc.Nodes.push_back(
		{"mask",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{255, 255, 255, 255}}}}
	);
	doc.Nodes.push_back(
		{"bg",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{0, 0, 0, 255}}}}
	);
	doc.Links = {{"mask", "image", "shape", "mask"}, {"bg", "image", "shape", "bg"}};
	auto image = ShapeImage(doc);
	for (uint32_t y = 0; y < 5; y++)
		for (uint32_t x = 0; x < 5; x++)
			REQUIRE(detail::ReadPixel(image, x, y) == detail::Rgba{0, 0, 0, 1});
	doc.Nodes[2].Values.back().Data = Colour{0, 0, 255, 255};
	image = ShapeImage(doc);
	REQUIRE(detail::ReadPixel(image, 2, 2) == detail::Rgba{1, 1, 1, 1});
	REQUIRE(detail::ReadPixel(image, 0, 0) == detail::Rgba{0, 0, 1, 1});
	doc.Nodes[1].Values.back().Data = Colour{0, 0, 0, 0};
	doc.Nodes[0].Values.push_back({"mask_alpha_only", true});
	REQUIRE(ShapeImage(doc).Pixels == image.Pixels);
}
TEST_CASE(
	"Source simple shape numeric reference units and surface vector getters remain distinct",
	"[source_simple_shape]"
) {
	auto doc = ShapeGraph();
	doc.Nodes[0].Values.push_back({"half_size", Vector2{.49, .49}});
	doc.Nodes[0].Values.push_back({"half_size_unit", EnumValue{0}});
	doc.Junctions.push_back({"center", "", ValueType::Vector2, Vector2{.5, .5}});
	doc.Links = {{"center", "value", "shape", "center"}};
	auto image = ShapeImage(doc);
	REQUIRE(detail::ReadPixel(image, 2, 2) == detail::Rgba{1, 1, 1, 1});
	REQUIRE(detail::ReadPixel(image, 0, 0) == detail::Rgba{});
	doc.Nodes[0].Values.push_back({"center_unit", EnumValue{0}});
	image = ShapeImage(doc);
	REQUIRE(detail::ReadPixel(image, 0, 0) == detail::Rgba{1, 1, 1, 1});
	REQUIRE(detail::ReadPixel(image, 2, 2) == detail::Rgba{});
	doc.Nodes[0].Values.back().Data = EnumValue{1};
	doc.Nodes.push_back(
		{"size",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{2}}, {"height", int64_t{2}}, {"colour", Colour{255, 255, 255, 255}}}}
	);
	doc.Links = {{"size", "image", "shape", "center"}};
	doc.Nodes[0].Values[2].Data = Vector2{.6, .6};
	image = ShapeImage(doc);
	REQUIRE(detail::ReadPixel(image, 1, 1) == detail::Rgba{});
	// A raw (2,2) pixel centre with radius .8 covers the four adjacent centres, not an offscreen scaled
	// centre.
	doc.Nodes[0].Values[2].Data = Vector2{.8, .8};
	image = ShapeImage(doc);
	REQUIRE(detail::ReadPixel(image, 1, 1) == detail::Rgba{1, 1, 1, 1});
	REQUIRE(detail::ReadPixel(image, 2, 2) == detail::Rgba{1, 1, 1, 1});
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(doc), restored, diagnostic) == Status::Ok);
	REQUIRE(ShapeImage(restored).Pixels == image.Pixels);
}
TEST_CASE(
	"Source simple shape enabled array rows retain selected centres through persistence",
	"[source_simple_shape]"
) {
	auto doc = ShapeGraph();
	doc.Nodes[0].Values.insert(
		doc.Nodes[0].Values.end(),
		{{"attribute_process", true},
		 {"half_size", Vector2{.49, .49}},
		 {"half_size_unit", EnumValue{0}},
		 {"center_unit", EnumValue{0}}}
	);
	ArrayValue centers;
	centers.ElementType = ValueType::Scalar;
	centers.Nested = {{.5, .5}, {3.5, 3.5}};
	doc.Junctions.push_back({"centers", "", ValueType::Array, centers});
	doc.Links = {{"centers", "value", "shape", "center"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	ImageArray images;
	auto status = EvaluateArray(doc, plan, "image", EvaluationRequest{}, images, diagnostic);
	INFO(diagnostic.Port << ":" << diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(images.Images.size() == 2);
	REQUIRE(detail::ReadPixel(images.Images[0], 0, 0) == detail::Rgba{1, 1, 1, 1});
	REQUIRE(detail::ReadPixel(images.Images[1], 3, 3) == detail::Rgba{1, 1, 1, 1});
	REQUIRE(images.Images[0].Pixels != images.Images[1].Pixels);
	Document restored;
	REQUIRE(Read(Write(doc), restored, diagnostic) == Status::Ok);
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	ImageArray replay;
	REQUIRE(EvaluateArray(restored, plan, "image", EvaluationRequest{}, replay, diagnostic) == Status::Ok);
	REQUIRE(replay.Images.size() == 2);
	for (size_t i = 0; i < 2; i++)
		REQUIRE(replay.Images[i].Pixels == images.Images[i].Pixels);
	doc.Nodes[0].Values[2].Data = false;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	REQUIRE(
		EvaluateArray(doc, plan, "image", EvaluationRequest{}, replay, diagnostic) ==
		Status::UnsupportedExecution
	);
	REQUIRE(diagnostic.Port == "attribute_process");
}
TEST_CASE("Source simple shape singular and unuploaded states refuse atomically", "[source_simple_shape]") {
	const Image sentinel{1, 1, {5, 6, 7, 8}};
	Image image = sentinel;
	auto doc = ShapeGraph();
	doc.Nodes[0].Values.push_back({"half_size", Vector2{0, .5}});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(doc, plan, "image", image, diagnostic) == Status::UnsupportedExecution);
	REQUIRE(diagnostic.Port == "half_size");
	REQUIRE(image == sentinel);
	doc = ShapeGraph();
	doc.Nodes.push_back(
		{"mask",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{255, 255, 255, 255}}}}
	);
	doc.Links = {{"mask", "image", "shape", "mask"}};
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(doc, plan, "image", image, diagnostic) == Status::UnsupportedExecution);
	REQUIRE(diagnostic.Port == "mask");
	REQUIRE(image == sentinel);
	doc = ShapeGraph();
	doc.Nodes.push_back(
		{"path",
		 "pc.path",
		 "",
		 {},
		 {{"sample_path", .5}},
		 {{"anchor_0", ValueType::Array, ArrayValue{ValueType::Scalar, {0., 0., 0., 0., 0., 0., 0.}}},
		  {"anchor_1", ValueType::Array, ArrayValue{ValueType::Scalar, {1., 1., 0., 0., 0., 0., 0.}}}}}
	);
	doc.Junctions = {{"path_value", "", ValueType::Any, std::nullopt}};
	doc.Links = {{"path", "path_data", "path_value", "value"}, {"path_value", "value", "shape", "center"}};
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(doc, plan, "image", image, diagnostic) == Status::UnsupportedExecution);
	REQUIRE(diagnostic.Port == "center");
	REQUIRE(image == sentinel);
	doc = ShapeGraph();
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(doc, plan, "image", EvaluationRequest{}, image, diagnostic, 1) == Status::LimitExceeded);
	REQUIRE(image == sentinel);
}
TEST_CASE("Source simple shape work admission counts enabled processor rows", "[source_simple_shape]") {
	auto doc = ShapeGraph("pc.shape_rectangle", {2800, 1500});
	doc.Nodes[0].Values.push_back({"attribute_process", true});
	doc.Junctions = {{"corners", "", ValueType::Array, ArrayValue{ValueType::Scalar, {0., 1.}}}};
	doc.Links = {{"corners", "value", "shape", "corner"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	ImageArray output;
	output.Images = {{1, 1, {2, 3, 5, 7}}};
	const auto previous = output.Images;
	REQUIRE(
		EvaluateArray(doc, plan, "image", EvaluationRequest{}, output, diagnostic) == Status::LimitExceeded
	);
	REQUIRE(diagnostic.Port == "dimension");
	REQUIRE(output.Images == previous);
}
TEST_CASE(
	"Source simple shape Reference vectors use the first Dimension getter row", "[source_simple_shape]"
) {
	auto doc = ShapeGraph();
	doc.Nodes[0].Values.insert(
		doc.Nodes[0].Values.end(),
		{{"attribute_process", true}, {"half_size", Vector2{.49, .49}}, {"half_size_unit", EnumValue{0}}}
	);
	ArrayValue dimensions;
	dimensions.ElementType = ValueType::Scalar;
	dimensions.Nested = {{3., 3.}, {7., 7.}};
	doc.Junctions = {
		{"dimensions", "", ValueType::Array, dimensions}, {"center", "", ValueType::Vector2, Vector2{.5, .5}}
	};
	doc.Links = {{"dimensions", "value", "shape", "dimension"}, {"center", "value", "shape", "center"}};
	const auto evaluate = [&]() {
		Plan plan;
		Diagnostic diagnostic;
		auto status = Compile(doc, plan, diagnostic);
		INFO(diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		ImageArray images;
		status = EvaluateArray(doc, plan, "image", EvaluationRequest{}, images, diagnostic);
		INFO(diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		REQUIRE(images.Images.size() == 2);
		return images;
	};
	auto images = evaluate();
	REQUIRE(images.Images[0].Width == 3);
	REQUIRE(images.Images[1].Width == 7);
	for (const auto &image : images.Images)
		REQUIRE(detail::ReadPixel(image, 1, 1) == detail::Rgba{1, 1, 1, 1});
	REQUIRE(detail::ReadPixel(images.Images[1], 3, 3) == detail::Rgba{});
	doc.Nodes[0].Values[3].Data = Vector2{.5, .5};
	doc.Nodes[0].Values[4].Data = EnumValue{1};
	images = evaluate();
	for (const auto &image : images.Images) {
		size_t occupied = 0;
		for (uint32_t y = 0; y < image.Height; y++)
			for (uint32_t x = 0; x < image.Width; x++)
				occupied += detail::ReadPixel(image, x, y)[3] == 1;
		REQUIRE(occupied == 9);
	}
	doc.Nodes[0].Values[3].Data = Vector2{.8, .8};
	doc.Nodes[0].Values[4].Data = EnumValue{0};
	doc.Nodes.push_back(
		{"size",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{2}}, {"height", int64_t{2}}, {"colour", Colour{255, 255, 255, 255}}}}
	);
	doc.Links[1] = {"size", "image", "shape", "center"};
	images = evaluate();
	for (const auto &image : images.Images) {
		REQUIRE(detail::ReadPixel(image, 1, 1) == detail::Rgba{1, 1, 1, 1});
		REQUIRE(detail::ReadPixel(image, 2, 2) == detail::Rgba{1, 1, 1, 1});
	}
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(doc), restored, diagnostic) == Status::Ok);
	Plan plan;
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	ImageArray replay;
	REQUIRE(EvaluateArray(restored, plan, "image", EvaluationRequest{}, replay, diagnostic) == Status::Ok);
	REQUIRE(replay.Images.size() == 2);
	for (size_t i = 0; i < 2; i++)
		REQUIRE(replay.Images[i].Pixels == images.Images[i].Pixels);
}
TEST_CASE("Source shape Dimension surface arrays project before Reference units", "[source_simple_shape]") {
	auto doc = ShapeGraph();
	doc.Nodes[0].Values.insert(
		doc.Nodes[0].Values.end(),
		{{"attribute_process", true}, {"half_size", Vector2{.49, .49}}, {"half_size_unit", EnumValue{0}}}
	);
	ArrayValue dimensions;
	dimensions.ElementType = ValueType::Scalar;
	dimensions.Nested = {{3., 3.}, {7., 7.}};
	doc.Junctions = {
		{"dimensions", "", ValueType::Array, dimensions}, {"center", "", ValueType::Vector2, Vector2{.5, .5}}
	};
	doc.Nodes.push_back(
		{"dimension_images",
		 "pc.shape_rectangle",
		 "",
		 {},
		 {{"attribute_process", true}, {"dimension_unit", EnumValue{0}}}}
	);
	doc.Links = {
		{"dimensions", "value", "dimension_images", "dimension"},
		{"dimension_images", "surface_out", "shape", "dimension"},
		{"center", "value", "shape", "center"}
	};
	const auto evaluate = [&](const Document &source) {
		Plan plan;
		Diagnostic diagnostic;
		const auto compiled = Compile(source, plan, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(compiled == Status::Ok);
		ImageArray output;
		const auto status = EvaluateArray(source, plan, "image", EvaluationRequest{}, output, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return output;
	};
	auto images = evaluate(doc);
	REQUIRE(images.Images.size() == 2);
	REQUIRE(images.Images[0].Width == 3);
	REQUIRE(images.Images[1].Width == 7);
	for (const auto &image : images.Images) {
		REQUIRE(detail::ReadPixel(image, 1, 1) == detail::Rgba{1, 1, 1, 1});
		size_t occupied = 0;
		for (uint32_t y = 0; y < image.Height; y++)
			for (uint32_t x = 0; x < image.Width; x++)
				occupied += detail::ReadPixel(image, x, y)[3] == 1;
		REQUIRE(occupied == 1);
	}
	REQUIRE(detail::ReadPixel(images.Images[1], 3, 3) == detail::Rgba{});
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(doc), restored, diagnostic) == Status::Ok);
	const auto replay = evaluate(restored);
	REQUIRE(replay.Images.size() == 2);
	for (size_t i = 0; i < 2; i++)
		REQUIRE(replay.Images[i].Pixels == images.Images[i].Pixels);

	doc.Nodes[0].Values[3].Data = Vector2{.5, .5};
	doc.Nodes[0].Values[4].Data = EnumValue{1};
	images = evaluate(doc);
	for (const auto &image : images.Images) {
		size_t occupied = 0;
		for (uint32_t y = 0; y < image.Height; y++)
			for (uint32_t x = 0; x < image.Width; x++)
				occupied += detail::ReadPixel(image, x, y)[3] == 1;
		REQUIRE(occupied == 9);
	}
	doc.Nodes[0].Values[4].Data = EnumValue{0};
	// Surface-valued center bypasses Reference units even beside varying surface dimensions.
	doc.Nodes.push_back(
		{"center_image",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{2}}, {"height", int64_t{2}}, {"colour", Colour{255, 255, 255, 255}}}}
	);
	doc.Links.back() = {"center_image", "image", "shape", "center"};
	doc.Nodes[0].Values[3].Data = Vector2{.8, .8};
	images = evaluate(doc);
	REQUIRE(images.Images.size() == 2);
	for (const auto &image : images.Images) {
		REQUIRE(detail::ReadPixel(image, 1, 1) == detail::Rgba{1, 1, 1, 1});
		REQUIRE(detail::ReadPixel(image, 2, 2) == detail::Rgba{1, 1, 1, 1});
	}

	// Two equal-size source surfaces collapse to one Dimension vector before row scheduling.
	std::get<ArrayValue>(*doc.Junctions[0].Default).Nested = {{3., 3.}, {3., 3.}};
	const Image collapsed = ShapeImage(doc);
	REQUIRE(collapsed.Width == 3);
	REQUIRE(collapsed.Height == 3);
	ExpectBinary(collapsed, {"000", "011", "011"});
}
