#include "../src/ImageGraphSdfAdapter.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("client.imagegraph.sdf_adapter")
TEST_DEPENDS("engine.imagegraph.document")

namespace {
	using namespace engine::imagegraph;
	Document SdfScene() {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"sphere", "pc.rm_primitive", "", {}, {{"shape", EnumValue{1}}, {"radius", .7}}},
			{"render",
			 "pc.rm_render",
			 "",
			 {},
			 {{"dimension", Vector2{32, 24}},
			  {"dimension_unit", EnumValue{0}},
			  {"attribute_texture_size", 1024.0},
			  {"camera_scale", 1.0}}}
		};
		document.Links = {{"sphere", "sdf_object", "render", "sdf_object"}};
		document.Outputs = {{"out", "render", "surface_out"}};
		document.Keyframes = {
			{"render", "camera_scale", 0, 1.0, "linear"}, {"render", "camera_scale", 10, 2.0, "linear"}
		};
		return document;
	}
}

TEST_CASE(
	"SDF adapter captures an owned object and animated renderer controls", "[client][imagegraph][sdf_adapter]"
) {
	auto document = SdfScene();
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	engine::render::imagegraph::SourceSdfRequest first, second;
	const bool built = client::detail::BuildSdfRequest(
		document, plan, document.Nodes.back(), "surface_out", 0, 7, false, first, diagnostic
	);
	INFO(diagnostic.Message);
	REQUIRE(built);
	REQUIRE(
		client::detail::BuildSdfRequest(
			document, plan, document.Nodes.back(), "surface_out", 10, 7, true, second, diagnostic
		)
	);
	CHECK(first.Width == 32);
	CHECK(first.Height == 24);
	CHECK(first.Format == engine::assets::TextureFormat::RGBA8_LINEAR);
	CHECK(second.Format == engine::assets::TextureFormat::RGBA8);
	CHECK(first.CameraScale == 1);
	CHECK(second.CameraScale == 2);
	REQUIRE(first.Object.Data);
	REQUIRE(second.Object.Data);
	REQUIRE(first.Object.Data->Shapes.size() == 1);
	CHECK(first.Object.Data->Shapes.front().Shape == 101);
	CHECK(first.Object.Data->Shapes.front().Radius == .7);
	CHECK(first.Object.Data.operator->() != second.Object.Data.operator->());
	CHECK(first.TextureAtlasSize == 1024);
	const auto saved = first;
	CHECK_FALSE(
		client::detail::BuildSdfRequest(
			document, plan, document.Nodes.back(), "missing", 0, 7, false, first, diagnostic
		)
	);
	CHECK(diagnostic.Code == Status::UnknownPort);
	CHECK(first == saved);
	document.Nodes.back().Values.push_back({"fov", -1.0});
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK_FALSE(
		client::detail::BuildSdfRequest(
			document, plan, document.Nodes.back(), "surface_out", 0, 7, false, first, diagnostic
		)
	);
	CHECK(diagnostic.Code == Status::InvalidValue);
	CHECK(first == saved);
}

TEST_CASE(
	"cloud adapter captures the source scale multiplier and gradient", "[client][imagegraph][sdf_adapter]"
) {
	using namespace engine::render::imagegraph;
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"cloud",
		 "pc.rm_cloud",
		 "",
		 {},
		 {{"dimension", Vector2{8, 6}},
		  {"dimension_unit", EnumValue{0}},
		  {"scale", .5},
		  {"density", .7},
		  {"detail", int64_t{4}},
		  {"shape", EnumValue{1}}}}
	};
	document.Outputs = {{"out", "cloud", "surface_out"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	SourceSdfRequest request;
	INFO(diagnostic.Message);
	REQUIRE(
		client::detail::BuildSdfRequest(
			document, plan, document.Nodes[0], "surface_out", 0, 12, false, request, diagnostic
		)
	);
	CHECK(request.Mode == SourceRaymarchMode::Cloud);
	CHECK(request.Cloud.ObjectScale == 2);
	CHECK(request.Cloud.Density == .7f);
	CHECK(request.Cloud.Iteration == 4);
	CHECK(request.Cloud.Type == 1);
	CHECK(request.Cloud.ViewRange == std::array<float, 2>{0, 6});
	CHECK(request.Cloud.Gradient.Keys.size() == 2);
	CHECK_FALSE(request.Object.Data);
}

TEST_CASE(
	"scatter adapter distinguishes duplicate position sockets by source ID",
	"[client][imagegraph][sdf_adapter]"
) {
	using namespace engine::render::imagegraph;
	auto document = SdfScene();
	document.Keyframes.clear();
	auto &node = document.Nodes.back();
	node.Type = "pc.rm_render_scatter";
	node.Values.push_back({"position", Vector2{2, 3}});
	node.Values.push_back({"position_2", Vector3{4, 5, 6}});
	node.Values.push_back({"rotation", Vector3{7, 8, 9}});
	node.Values.push_back({"rot_random_min", Vector3{1, 2, 3}});
	node.Values.push_back({"rot_random_max", Vector3{10, 11, 12}});
	node.Values.push_back({"scale_range", Vector2{.5, 2}});
	node.Values.push_back({"seed", 17.0});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	SourceSdfRequest request;
	REQUIRE(
		client::detail::BuildSdfRequest(
			document, plan, node, "surface_out", 0, 12, false, request, diagnostic
		)
	);
	CHECK(request.Mode == SourceRaymarchMode::Scatter);
	CHECK(request.Scatter.Seed == 17);
	CHECK(request.Scatter.PositionOrigin == std::array<float, 2>{2, 3});
	CHECK(request.LightPosition == std::array<float, 3>{4, 5, 6});
	CHECK(request.Scatter.RotationOrigin == std::array<float, 3>{7, 8, 9});
	CHECK(request.Scatter.ObjectScale == std::array<float, 2>{.5, 2});
}

TEST_CASE(
	"terrain adapter packs the source atlas cells and preserves control defaults",
	"[client][imagegraph][sdf_adapter]"
) {
	using namespace engine::render::imagegraph;
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"height",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{255, 0, 0, 255}}}},
		{"color",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{0, 255, 0, 255}}}},
		{"terrain",
		 "pc.rm_terrain",
		 "",
		 {},
		 {{"dimension", Vector2{8, 8}},
		  {"dimension_unit", EnumValue{0}},
		  {"height", 2.0},
		  {"tile", false},
		  {"shadow", .4}}}
	};
	document.Links = {{"height", "image", "terrain", "surface"}, {"color", "image", "terrain", "texture"}};
	document.Outputs = {{"out", "terrain", "surface_out"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	SourceSdfRequest request;
	REQUIRE(
		client::detail::BuildSdfRequest(
			document, plan, document.Nodes.back(), "surface_out", 0, 1, false, request, diagnostic
		)
	);
	CHECK(request.Mode == SourceRaymarchMode::Terrain);
	CHECK(request.Terrain.Shape == 1);
	CHECK(request.Terrain.Tile == 0);
	CHECK(request.Terrain.Thickness == 2);
	CHECK(request.Terrain.Shadow == .4f);
	CHECK(request.Terrain.DepthIntensity == 1);
	CHECK(request.Terrain.SunPosition == std::array<float, 3>{.5, 1, .5});
	CHECK(request.Terrain.UseTexture);
	CHECK(request.TextureFiltering);
	REQUIRE(request.Terrain.Textures[0]);
	const auto &atlas = *request.Terrain.Textures[0];
	CHECK(atlas.Width == 4192);
	CHECK(atlas.Height == 1024);
	CHECK(request.Terrain.AtlasUvScale == std::array<float, 2>{1, 4192.f / 1024});
	CHECK(atlas.Pixels[0] == 255);
	CHECK(atlas.Pixels[1024 * 4 + 1] == 255);
	CHECK(atlas.Pixels[2048 * 4 + 3] == 0);
	CHECK(atlas.Pixels.size() < 32ull * 1024 * 1024);
}
