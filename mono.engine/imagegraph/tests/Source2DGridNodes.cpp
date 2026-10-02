#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.source_grid")
using namespace engine::imagegraph;
TEST_CASE(
	"Grid preserves independent source heightmap alpha and masks only the main surface",
	"[imagegraph][source_2d]"
) {
	const Image uv = imagegraph_test::MakeImage(1, 1, {128, 128, 0, 128});
	const Image mask = imagegraph_test::MakeImage(1, 1, {255, 0, 0, 128});
	const auto run = imagegraph_test::RunNode(
		"pc.grid",
		{{"uv_map", &uv}, {"mask", &mask}},
		{{"dimension", Vector2{4, 4}},
		 {"dimension_unit", EnumValue{0}},
		 {"seed", 1.},
		 {"grid_size", Vector2{2, 2}},
		 {"grid_size_unit", EnumValue{0}},
		 {"render_type", EnumValue{2}},
		 {"uv_mix", 0.},
		 {"mask_alpha_only", true}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	for (size_t i = 0; i < 16; ++i) {
		for (size_t c = 0; c < 3; ++c) {
			CHECK(run.Output().Pixels[i * 4 + c] == 128);
			CHECK(run.Output("heightmap").Pixels[i * 4 + c] == 128);
		}
		CHECK(run.Output().Pixels[i * 4 + 3] == 21);
		CHECK(run.Output("heightmap").Pixels[i * 4 + 3] == 255);
	}
}
TEST_CASE(
	"Accurate Grid remains usable and diagnoses only its source-unwritten heightmap",
	"[imagegraph][source_2d]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"grid",
		 "pc.grid",
		 "",
		 {},
		 {{"dimension", Vector2{4, 4}},
		  {"dimension_unit", EnumValue{0}},
		  {"seed", 1.},
		  {"grid_size", Vector2{2, 2}},
		  {"grid_size_unit", EnumValue{0}},
		  {"render_type", EnumValue{1}},
		  {"gap_width", 1.},
		  {"tile_color", Gradient{0, {{0, {255, 255, 255, 255}}}}}}}
	};
	document.Outputs = {{"colour", "grid", "surface_out"}, {"height", "grid", "heightmap"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image image;
	const auto colour = Evaluate(document, plan, "colour", EvaluationRequest{}, image, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(colour == Status::Ok);
	for (size_t y = 0; y < 4; ++y)
		for (size_t x = 0; x < 4; ++x) {
			CHECK(image.Pixels[(y * 4 + x) * 4] == (x % 2 == 0 && y % 2 == 0 ? 255 : 0));
			CHECK(image.Pixels[(y * 4 + x) * 4 + 3] == 255);
		}
	CHECK(
		Evaluate(document, plan, "height", EvaluationRequest{}, image, diagnostic) ==
		Status::UnsupportedExecution
	);
	CHECK(diagnostic.NodeId == "grid");
	CHECK(diagnostic.Port == "heightmap");
	document.Nodes.push_back({"filter", "pc.color_select", "", {}, {}});
	document.Links = {{"grid", "heightmap", "filter", "surface_in"}};
	document.Outputs = {{"downstream", "filter", "surface_out"}};
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK(
		Evaluate(document, plan, "downstream", EvaluationRequest{}, image, diagnostic) ==
		Status::UnsupportedExecution
	);
	CHECK(diagnostic.NodeId == "grid");
	CHECK(diagnostic.Port == "heightmap");
}

TEST_CASE(
	"Grid safe draw uses gray texture pixels through a real graph for all scalar formats",
	"[imagegraph][source_2d]"
) {
	for (int64_t depth = 6; depth <= 8; ++depth)
		for (int64_t mode = 0; mode < 5; ++mode) {
			Document document;
			document.FormatVersion = 9;
			document.Nodes = {
				{"gray",
				 "pc.solid",
				 "",
				 {},
				 {{"dimension", Vector2{1, 1}},
				  {"dimension_unit", EnumValue{0}},
				  {"color", Colour{64, 0, 0, 255}},
				  {"attribute_color_depth", EnumValue{depth}}}},
				{"grid",
				 "pc.grid",
				 "",
				 {},
				 {{"dimension", Vector2{2, 1}},
				  {"dimension_unit", EnumValue{0}},
				  {"render_type", EnumValue{mode}},
				  {"attribute_color_depth", EnumValue{3}},
				  {"grid_size", Vector2{0, 0}},
				  {"grid_size_unit", EnumValue{0}},
				  {"level_in", Vector2{0, 0}}}}
			};
			document.Links = {{"gray", "surface_out", "grid", "texture"}};
			document.Outputs = {{"colour", "grid", "surface_out"}, {"height", "grid", "heightmap"}};
			Plan plan;
			Diagnostic diagnostic;
			REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
			Image image;
			const auto status = Evaluate(document, plan, "colour", {}, image, diagnostic);
			INFO(diagnostic.Message);
			REQUIRE(status == Status::Ok);
			CHECK(image.Pixels == std::vector<uint8_t>{64, 64, 64, 255, 64, 64, 64, 255});
			const auto previous = image;
			CHECK(Evaluate(document, plan, "height", {}, image, diagnostic) == Status::UnsupportedExecution);
			CHECK(image == previous);
			CHECK(diagnostic.NodeId == "grid");
			CHECK(diagnostic.Port == "heightmap");
		}
}
TEST_CASE(
	"Mixed Grid processor rows retain their valid main array and refuse an incomplete height array",
	"[imagegraph][source_2d]"
) {
	Document document;
	document.FormatVersion = 9;
	Node modes{"modes", "pc.array", "", {}, {}, {}};
	modes.DynamicInputs = {
		{"input_0", ValueType::Integer, int64_t{2}}, {"input_1", ValueType::Integer, int64_t{1}}
	};
	document.Nodes = {
		modes,
		{"grid",
		 "pc.grid",
		 "",
		 {},
		 {{"dimension", Vector2{2, 2}},
		  {"dimension_unit", EnumValue{0}},
		  {"seed", 1.},
		  {"grid_size", Vector2{2, 2}},
		  {"grid_size_unit", EnumValue{0}},
		  {"tile_color", Gradient{0, {{0, {255, 255, 255, 255}}}}}}}
	};
	document.Links = {{"modes", "array", "grid", "render_type"}};
	document.Outputs = {{"colour", "grid", "surface_out"}, {"height", "grid", "heightmap"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	ImageArray images;
	const auto status = EvaluateArray(document, plan, "colour", {}, images, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(images.Images.size() == 2);
	CHECK(
		images.Images[0].Pixels ==
		std::vector<uint8_t>{128, 128, 128, 255, 128, 128, 128, 255, 128, 128, 128, 255, 128, 128, 128, 255}
	);
	CHECK(
		images.Images[1].Pixels ==
		std::vector<uint8_t>{255, 255, 255, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255}
	);
	const auto previous = images;
	CHECK(EvaluateArray(document, plan, "height", {}, images, diagnostic) == Status::UnsupportedExecution);
	CHECK(images.Images == previous.Images);
	CHECK(images.Items == previous.Items);
	CHECK(diagnostic.NodeId == "grid");
	CHECK(diagnostic.Port == "heightmap");
}

TEST_CASE(
	"Grid vectorizes mixed scalar and RGBA textures without contaminating valid main rows",
	"[imagegraph][source_2d]"
) {
	Document document;
	document.FormatVersion = 9;
	Node rows{"rows", "value.array", "", {}, {}, {}};
	for (int64_t depth = 6; depth <= 8; ++depth) {
		const auto id = "scalar_" + std::to_string(depth);
		document.Nodes.push_back(
			{id,
			 "pc.solid",
			 "",
			 {},
			 {{"dimension", Vector2{1, 1}},
			  {"dimension_unit", EnumValue{0}},
			  {"color", Colour{64, 0, 0, 255}},
			  {"attribute_color_depth", EnumValue{depth}}}}
		);
		rows.DynamicInputs.push_back({id, ValueType::Image, std::nullopt});
		document.Links.push_back({id, "surface_out", "rows", id});
	}
	document.Nodes.push_back(
		{"rgba",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension", Vector2{1, 1}},
		  {"dimension_unit", EnumValue{0}},
		  {"color", Colour{64, 0, 0, 255}},
		  {"attribute_color_depth", EnumValue{3}}}}
	);
	rows.DynamicInputs.push_back({"rgba", ValueType::Image, std::nullopt});
	document.Links.push_back({"rgba", "surface_out", "rows", "rgba"});
	document.Nodes.push_back(std::move(rows));
	document.Nodes.push_back(
		{"grid",
		 "pc.grid",
		 "",
		 {},
		 {{"dimension", Vector2{2, 2}},
		  {"dimension_unit", EnumValue{0}},
		  {"seed", 1.},
		  {"grid_size", Vector2{2, 2}},
		  {"grid_size_unit", EnumValue{0}},
		  {"attribute_color_depth", EnumValue{3}},
		  {"render_type", EnumValue{2}}}}
	);
	document.Links.push_back({"rows", "array", "grid", "texture"});
	document.Outputs = {{"colour", "grid", "surface_out"}, {"height", "grid", "heightmap"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	ImageArray images;
	const auto status = EvaluateArray(document, plan, "colour", {}, images, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(images.Images.size() == 4);
	for (size_t row = 0; row < 4; ++row)
		for (size_t pixel = 0; pixel < 4; ++pixel) {
			for (size_t channel = 0; channel < 3; ++channel)
				CHECK(images.Images[row].Pixels[pixel * 4 + channel] == (row == 3 ? 128 : 64));
			CHECK(images.Images[row].Pixels[pixel * 4 + 3] == 255);
		}
	const auto previous = images;
	CHECK(EvaluateArray(document, plan, "height", {}, images, diagnostic) == Status::UnsupportedExecution);
	CHECK(images.Images == previous.Images);
	CHECK(images.Items == previous.Items);
	CHECK(diagnostic.NodeId == "grid");
	CHECK(diagnostic.Port == "heightmap");
}

TEST_CASE("Grid unused level division refuses only Heightmap in colored mode", "[imagegraph][source_2d]") {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"grid",
		 "pc.grid",
		 "",
		 {},
		 {{"dimension", Vector2{2, 2}},
		  {"dimension_unit", EnumValue{0}},
		  {"seed", 1.},
		  {"grid_size", Vector2{2, 2}},
		  {"grid_size_unit", EnumValue{0}},
		  {"level_in", Vector2{0, 0}},
		  {"gap", 0.},
		  {"render_type", EnumValue{0}},
		  {"tile_color", Gradient{0, {{0, {255, 255, 255, 255}}}}}}}
	};
	document.Outputs = {{"colour", "grid", "surface_out"}, {"height", "grid", "heightmap"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image image;
	const auto status = Evaluate(document, plan, "colour", {}, image, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	CHECK(image.Pixels == std::vector<uint8_t>(16, 255));
	CHECK(Evaluate(document, plan, "height", {}, image, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.NodeId == "grid");
	CHECK(diagnostic.Port == "heightmap");
}

TEST_CASE(
	"Grid Height Map mode reports each undefined level output without publishing pixels",
	"[imagegraph][source_2d]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"grid",
		 "pc.grid",
		 "",
		 {},
		 {{"dimension", Vector2{2, 2}},
		  {"dimension_unit", EnumValue{0}},
		  {"seed", 1.},
		  {"level_in", Vector2{0, 0}},
		  {"render_type", EnumValue{2}}}}
	};
	document.Outputs = {{"colour", "grid", "surface_out"}, {"height", "grid", "heightmap"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const Image sentinel{1, 1, {1, 2, 3, 4}, 0};
	Image image = sentinel;
	for (const auto name : {"colour", "height"}) {
		CHECK(Evaluate(document, plan, name, {}, image, diagnostic) == Status::UnsupportedExecution);
		CHECK(image == sentinel);
		CHECK(diagnostic.NodeId == "grid");
		CHECK(diagnostic.Port == (std::string_view(name) == "colour" ? "surface_out" : "heightmap"));
	}
}

TEST_CASE(
	"Grid scalar safe draw retains hardware filtering while bypassing custom interpolation",
	"[imagegraph][source_2d]"
) {
	Image surface{2, 1, {0, 255}, 0, SurfaceFormat::R8Unorm};
	for (int64_t interpolation : {int64_t{1}, int64_t{3}}) {
		const auto run = imagegraph_test::RunNode(
			"pc.grid",
			{{"texture", &surface}},
			{{"dimension", Vector2{4, 1}},
			 {"dimension_unit", EnumValue{0}},
			 {"render_type", EnumValue{0}},
			 {"attribute_color_depth", EnumValue{3}},
			 {"interpolate", EnumValue{interpolation}}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		const std::array<uint8_t, 4> expected = interpolation == 3 ? std::array<uint8_t, 4>{0, 64, 191, 255}
																   : std::array<uint8_t, 4>{0, 0, 255, 255};
		for (size_t pixel = 0; pixel < 4; ++pixel) {
			for (size_t channel = 0; channel < 3; ++channel)
				CHECK(run.Output().Pixels[pixel * 4 + channel] == expected[pixel]);
			CHECK(run.Output().Pixels[pixel * 4 + 3] == 255);
		}
	}
}
