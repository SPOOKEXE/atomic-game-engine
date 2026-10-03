#include <engine/assets/Texture.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/SourceArgumentHost.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <assetc/Bake.hpp>
#include <filesystem>
#include <fstream>

TEST_SUITE_ID("tools.assetc.source_arguments")
TEST_DEPENDS("engine.imagegraph.source_argument")
TEST_CASE(
	"Tree bake uses one explicit argument table and defaults without a table", "[assetc][source_argument]"
) {
	using namespace engine::imagegraph;
	const auto root = std::filesystem::temp_directory_path() / "atomic-argument-tree-bake";
	std::filesystem::remove_all(root);
	std::filesystem::create_directories(root);
	struct Cleanup {
		std::filesystem::path Root;
		~Cleanup() {
			std::error_code error;
			std::filesystem::remove_all(Root, error);
		}
	} cleanup{root};
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"argument",
		 "pc.argument",
		 "",
		 {},
		 {{"tag", std::string{"width"}}, {"type", EnumValue{1}}, {"default_value", 2.}}},
		{"dimension", "pc.vector2", "", {}, {{"x", 2.0}, {"y", 1.0}}},
		{"solid",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension", Vector2{2, 1}},
		  {"dimension_unit", EnumValue{0}},
		  {"color", Colour{31, 47, 59, 255}},
		  {"attribute_color_depth", EnumValue{3}}}}
	};
	document.Links = {{"argument", "value", "dimension", "x"}, {"dimension", "vector", "solid", "dimension"}};
	document.Outputs = {{"image", "solid", "surface_out"}};
	{
		std::ofstream file(root / "argument.graph");
		file << Write(document);
	}
	assetc::Settings settings;
	settings.Input = root;
	settings.Mipmaps = false;
	const auto width = [&](SourceArgumentHost *host) {
		std::string failure;
		const auto report = assetc::Bake(settings, failure, host);
		INFO(failure);
		REQUIRE(failure.empty());
		REQUIRE(report.Failures == 0);
		REQUIRE(report.Assets.size() == 1);
		engine::core::ByteReader reader(report.Assets[0].Payload);
		engine::assets::TextureData texture;
		REQUIRE(engine::assets::Texture::Read(reader, texture));
		CHECK(texture.Height == 1);
		REQUIRE(texture.Pixels.size() == texture.Width * 4);
		CHECK(texture.Pixels[0] == std::byte{31});
		CHECK(texture.Pixels[1] == std::byte{47});
		CHECK(texture.Pixels[2] == std::byte{59});
		CHECK(texture.Pixels[3] == std::byte{255});
		return texture.Width;
	};
	CHECK(width(nullptr) == 2);
	SourceArgumentHost host;
	Diagnostic diagnostic;
	const std::string_view integers[] = {"width=3"};
	REQUIRE(
		host.PrepareOptions({{}, {}, integers, {}}, Limits::MaximumEvaluationBytes, diagnostic) == Status::Ok
	);
	CHECK(width(&host) == 3);
	const std::string_view malformed[] = {"width=3.5"};
	CHECK(
		host.PrepareOptions({{}, {}, malformed, {}}, Limits::MaximumEvaluationBytes, diagnostic) ==
		Status::InvalidValue
	);
	CHECK(width(&host) == 3);
}
