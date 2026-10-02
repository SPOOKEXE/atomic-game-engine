#include "../src/ImageGraphTransform3DAdapter.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <utility>
#include <vector>

TEST_SUITE_ID("client.imagegraph.transform_3d_adapter")
TEST_DEPENDS("engine.imagegraph.document")
TEST_DEPENDS("engine.render.imagegraph_transform_3d_gpu")

namespace {
	using namespace engine::imagegraph;

	Node Solid(std::string id, Colour colour) {
		return {
			std::move(id),
			"image.solid",
			"",
			{},
			{{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", colour}}
		};
	}

	Node Transform() {
		return {
			"transform",
			"image.transform_3d",
			"",
			{},
			{{"position", Vector3{}},
			 {"anchor", Vector3{}},
			 {"rotation", Quaternion{}},
			 {"scale", Vector3{1, 1, 1}},
			 {"texture_tiling", Vector2{1, 1}},
			 {"projection", EnumValue{1}},
			 {"fov", 45.0},
			 {"view_range", Vector2{.001, 10}},
			 {"depth_range", Vector2{0, 1}}}
		};
	}
}

TEST_CASE(
	"Transform Image 3D adapter captures linked surfaces and animated controls", "[client][imagegraph]"
) {
	using namespace engine::render::imagegraph;
	Document document;
	document.FormatVersion = 6;
	document.Nodes = {Solid("front", {12, 34, 56, 255}), Solid("back", {78, 90, 123, 255}), Transform()};
	document.Links = {
		{"front", "image", "transform", "surface"},
		{"back", "image", "transform", "back_surface"},
	};
	document.Outputs = {{"out", "transform", "rendered"}};
	document.Keyframes = {
		{"transform", "position", 0, Vector3{0, 0, 0}, "linear"},
		{"transform", "position", 10, Vector3{10, 20, 30}, "linear"},
		{"transform", "fov", 0, 45.0, "linear"},
		{"transform", "fov", 10, 90.0, "linear"},
	};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	TransformImage3DRequest request;
	const bool captured = client::detail::BuildTransformRequest(
		document, plan, document.Nodes.back(), 5, 77, true, request, diagnostic
	);
	INFO(
		"capture status=" << static_cast<int>(diagnostic.Code) << " node=" << diagnostic.NodeId
						  << " port=" << diagnostic.Port << " message=" << diagnostic.Message
	);
	REQUIRE(captured);
	CHECK(request.Front.Width == 1);
	CHECK(request.Front.Height == 1);
	CHECK(request.Front.Format == engine::assets::TextureFormat::RGBA8);
	CHECK(
		(request.Front.Pixels ==
		 std::vector<std::byte>{std::byte{12}, std::byte{34}, std::byte{56}, std::byte{255}})
	);
	CHECK(
		(request.Back.Pixels ==
		 std::vector<std::byte>{std::byte{78}, std::byte{90}, std::byte{123}, std::byte{255}})
	);
	CHECK((request.Position == std::array<float, 3>{5, 10, 15}));
	CHECK(request.FieldOfViewDegrees == 67.5f);
	CHECK(request.ColorSpace == TransformImage3DColorSpace::Display);
}
