#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <client/ImageGraphRuntime.hpp>
#include <filesystem>
#include <fstream>

TEST_SUITE_ID("client.imagegraphruntime.array")
TEST_DEPENDS("engine.imagegraph.document")

TEST_CASE("live array frame loading packs every ordered frame with authored timing", "[client][imagegraph]") {
	using namespace engine::imagegraph;
	const auto directory = std::filesystem::temp_directory_path() / "atomic-composer-array-frame-test";
	std::filesystem::create_directories(directory / "imagegraphs");
	const engine::core::Name graph("array-frame-test"), output("all");
	Document document;
	document.FormatVersion = 6;
	document.Timeline = TimelineSettings{};
	document.Timeline->FramesPerSecond = 8;
	document.Nodes = {
		{"red",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{255, 0, 0, 255}}}},
		{"blue",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{0, 0, 255, 255}}}},
		{"items", "value.array", "", {}, {}}
	};
	document.Nodes.back().DynamicInputs = {
		{"first", ValueType::Image, std::nullopt},
		{"second", ValueType::Image, std::nullopt},
		{"third", ValueType::Image, std::nullopt}
	};
	document.Links = {
		{"red", "image", "items", "first"},
		{"blue", "image", "items", "second"},
		{"red", "image", "items", "third"}
	};
	document.Outputs = {{"all", "items", "array"}};
	{
		std::ofstream file(client::ImageGraphDocumentPath(directory, graph));
		file << Write(document);
	}
	const auto frame = client::LoadImageGraphFrame(directory, graph, output, 0);
	INFO(frame.Diagnostic.Message);
	REQUIRE(frame.Status == Status::Ok);
	CHECK(frame.FlipbookSide == 2);
	CHECK(frame.FrameDurations == std::vector<float>{.125f, .125f, .125f});
	CHECK(frame.Image.Width == 2);
	CHECK(frame.Image.Height == 2);
	CHECK(
		frame.Image.Pixels == std::vector<uint8_t>{255, 0, 0, 255, 0, 0, 255, 255, 255, 0, 0, 255, 0, 0, 0, 0}
	);
	std::filesystem::remove_all(directory);
}

TEST_CASE("single-frame array loading preserves its sequence timing", "[client][imagegraph]") {
	using namespace engine::imagegraph;
	const auto directory = std::filesystem::temp_directory_path() / "atomic-composer-single-array-test";
	std::filesystem::create_directories(directory / "imagegraphs");
	const engine::core::Name graph("single-array-test"), output("all");
	Document document;
	document.FormatVersion = 6;
	document.Timeline = TimelineSettings{};
	document.Timeline->FramesPerSecond = 5;
	document.Nodes = {
		{"red",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{255, 0, 0, 255}}}},
		{"items", "value.array", "", {}, {}}
	};
	document.Nodes.back().DynamicInputs = {{"first", ValueType::Image, std::nullopt}};
	document.Links = {{"red", "image", "items", "first"}};
	document.Outputs = {{"all", "items", "array"}};
	{
		std::ofstream file(client::ImageGraphDocumentPath(directory, graph));
		file << Write(document);
	}
	const auto frame = client::LoadImageGraphFrame(directory, graph, output, 0);
	INFO(frame.Diagnostic.Message);
	REQUIRE(frame.Status == Status::Ok);
	CHECK(frame.FlipbookSide == 1);
	CHECK(frame.FrameDurations == std::vector<float>{.2f});
	CHECK(frame.Image.Pixels == std::vector<uint8_t>{255, 0, 0, 255});
	std::filesystem::remove_all(directory);
}
