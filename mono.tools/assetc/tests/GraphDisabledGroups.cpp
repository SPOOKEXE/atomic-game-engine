#include <engine/assets/Texture.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <assetc/Bake.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

TEST_SUITE_ID("tools.assetc.graph_disabled_groups")

namespace {
	using namespace engine::imagegraph;
	namespace fs = std::filesystem;
	struct GraphScratch {
		fs::path Root;
		explicit GraphScratch(std::string_view name)
			: Root(fs::temp_directory_path() / ("assetc-group-" + std::string(name))) {
			std::error_code error;
			fs::remove_all(Root, error);
			REQUIRE(fs::create_directories(Root / "in", error));
		}
		~GraphScratch() {
			std::error_code error;
			fs::remove_all(Root, error);
		}
		GraphScratch(const GraphScratch &) = delete;
		GraphScratch &operator=(const GraphScratch &) = delete;
		void WriteGraph(const Document &document) const {
			const std::string source = Write(document);
			REQUIRE_FALSE(source.empty());
			std::ofstream file(Root / "in" / "selected.graph", std::ios::binary);
			REQUIRE(file.good());
			file.write(source.data(), static_cast<std::streamsize>(source.size()));
			REQUIRE(file.good());
		}
		assetc::Report Bake(float frameRate = 0) const {
			assetc::Settings settings;
			settings.Input = Root / "in";
			settings.Mipmaps = false;
			settings.FlipbookFps = frameRate;
			std::string failure;
			auto report = assetc::Bake(settings, failure);
			INFO(failure);
			REQUIRE(failure.empty());
			REQUIRE(report.Assets.size() == 1);
			return report;
		}
	};
	Node Solid(std::string id, Colour colour) {
		return {
			std::move(id),
			"image.solid",
			"group",
			{},
			{{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", colour}}
		};
	}
	Document GroupedImage(bool active = true, bool frames = false) {
		Document document;
		document.FormatVersion = 10;
		Group group{"group", "Group"};
		group.RenderActive = active;
		group.Ports = {{"result", "parent-value", PortDirection::Output, "control"}};
		document.Groups.push_back(std::move(group));
		document.Junctions = {{"parent-value", "group", ValueType::Any, std::nullopt}};
		document.Nodes = {Solid("red", {255, 0, 0, 255}), {"control", "pc.group_output", "group", {}, {}}};
		if (frames) {
			document.Nodes.push_back(Solid("blue", {0, 0, 255, 255}));
			Node array{"frames", "value.array", "group", {}, {}};
			array.DynamicInputs = {
				{"first", ValueType::Image, std::nullopt}, {"second", ValueType::Image, std::nullopt}
			};
			document.Nodes.push_back(std::move(array));
			document.Links = {
				{"red", "image", "frames", "first"},
				{"blue", "image", "frames", "second"},
				{"frames", "array", "control", "value"}
			};
		} else
			document.Links = {{"red", "image", "control", "value"}};
		document.Links.push_back({"control", "value", "parent-value", "value"});
		document.Outputs = {{"selected", "control", "value"}};
		return document;
	}
	engine::assets::TextureData ReadTexture(const assetc::Report &report) {
		INFO(report.Assets.front().Failure);
		REQUIRE(report.Failures == 0);
		REQUIRE_FALSE(report.Assets.front().Payload.empty());
		engine::core::ByteReader reader(report.Assets.front().Payload);
		engine::assets::TextureData texture;
		REQUIRE(engine::assets::Texture::Read(reader, texture));
		return texture;
	}
	std::array<uint8_t, 4> Pixel(const engine::assets::TextureData &texture, size_t index) {
		REQUIRE(index * 4 + 4 <= texture.Pixels.size());
		return {
			std::to_integer<uint8_t>(texture.Pixels[index * 4]),
			std::to_integer<uint8_t>(texture.Pixels[index * 4 + 1]),
			std::to_integer<uint8_t>(texture.Pixels[index * 4 + 2]),
			std::to_integer<uint8_t>(texture.Pixels[index * 4 + 3])
		};
	}
}

TEST_CASE("Asset bake reads the scheduled grouped image output", "[assetc][imagegraph][group_render]") {
	const GraphScratch scratch("enabled-image");
	scratch.WriteGraph(GroupedImage());
	const auto report = scratch.Bake();
	const auto texture = ReadTexture(report);
	CHECK(texture.Width == 1);
	CHECK(texture.Height == 1);
	CHECK(Pixel(texture, 0) == (std::array<uint8_t, 4>{255, 0, 0, 255}));
	CHECK(report.Assets.front().Output == "selected.atex");
}

TEST_CASE(
	"Asset bake reads an image array held by a generic group socket", "[assetc][imagegraph][group_render]"
) {
	const GraphScratch scratch("enabled-array");
	scratch.WriteGraph(GroupedImage(true, true));
	const auto texture = ReadTexture(scratch.Bake(12));
	CHECK(texture.Width == 2);
	CHECK(texture.Height == 2);
	CHECK(texture.FlipbookFrames == 2);
	CHECK(texture.FlipbookFrameRate == 12);
	CHECK(Pixel(texture, 0) == (std::array<uint8_t, 4>{255, 0, 0, 255}));
	CHECK(Pixel(texture, 1) == (std::array<uint8_t, 4>{0, 0, 255, 255}));
	CHECK(Pixel(texture, 2) == (std::array<uint8_t, 4>{0, 0, 0, 0}));
}

TEST_CASE(
	"Asset bake refuses a cold disabled image group without scheduling its producer",
	"[assetc][imagegraph][group_render]"
) {
	const GraphScratch scratch("disabled-image");
	scratch.WriteGraph(GroupedImage(false));
	const auto report = scratch.Bake();
	CHECK(report.Failures == 1);
	CHECK_FALSE(report.Assets.front().Failure.empty());
	CHECK(report.Assets.front().Payload.empty());
	CHECK(report.OutputBytes == 0);
	CHECK(report.Assets.front().Output.empty());
}

TEST_CASE(
	"Asset bake rejects grouped scalar output without inventing image pixels",
	"[assetc][imagegraph][group_render]"
) {
	const GraphScratch scratch("scalar");
	auto document = GroupedImage(false);
	document.Nodes.front() = {"red", "pc.number_simple", "group", {}, {{"value", 73.0}}};
	document.Links.front().FromPort = "number";
	scratch.WriteGraph(document);
	const auto report = scratch.Bake();
	CHECK(report.Failures == 1);
	CHECK_FALSE(report.Assets.front().Failure.empty());
	CHECK(report.Assets.front().Payload.empty());
	CHECK(report.OutputBytes == 0);
}
