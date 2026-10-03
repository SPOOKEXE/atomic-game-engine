#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <client/ImageGraphRuntime.hpp>
#include <filesystem>
#include <fstream>

TEST_SUITE_ID("client.imagegraph_frame_cache")
TEST_CASE(
	"Client keyless Cache Array seeks populate the explicitly sampled prefix and retain output on refusal",
	"[client][source_frame_cache]"
) {
	using namespace engine::imagegraph;
	Document d;
	d.FormatVersion = 9;
	d.Timeline = TimelineSettings{6, 0, 5, "loop", 24};
	d.Nodes = {
		{"solid",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{63, 17, 0, 255}}}},
		{"cache",
		 "pc.cache_array",
		 "",
		 {},
		 {{"start_frame", int64_t{-1}}, {"stop_frame", int64_t{-1}}, {"step", int64_t{1}}}},
		{"select",
		 "pc.sequence_anim",
		 "",
		 {},
		 {{"speed", 0.}, {"sequence", ArrayValue{ValueType::Scalar, {2.}}}}}
	};
	d.Links = {{"solid", "image", "cache", "surface_in"}, {"cache", "cache_array", "select", "surface_in"}};
	d.Outputs = {{"out", "select", "surface_out"}};
	const auto directory = std::filesystem::temp_directory_path() / "atomic-frame-cache-client";
	std::filesystem::remove_all(directory);
	std::filesystem::create_directories(directory / "imagegraphs");
	struct Cleanup {
		std::filesystem::path Directory;
		~Cleanup() {
			std::filesystem::remove_all(Directory);
		}
	} cleanup{directory};
	const engine::core::Name graph("frame-cache"), out("out");
	{
		std::ofstream file(client::ImageGraphDocumentPath(directory, graph));
		file << Write(d);
	}
	const auto result = client::LoadImageGraphFrame(directory, graph, out, 2, 7);
	INFO(result.Diagnostic.Message);
	REQUIRE(result.Status == Status::Ok);
	CHECK(result.Animated);
	REQUIRE(result.Image.Pixels.size() == 4);
	CHECK(result.Image.Pixels == std::vector<uint8_t>{63, 17, 0, 255});
	const auto repeat = client::LoadImageGraphFrame(directory, graph, out, 2, 7);
	CHECK(repeat.Status == Status::Ok);
	CHECK(repeat.Image == result.Image);
	const auto reset = client::LoadImageGraphFrame(directory, graph, out, 0, 7);
	CHECK(reset.Status == Status::InvalidOutput);
	CHECK(reset.Image.Pixels.empty());
	d.Nodes[1].SourceProperties = {{"cache_group", ArrayValue{ValueType::Text, {std::string("solid")}}}};
	{
		std::ofstream file(client::ImageGraphDocumentPath(directory, graph));
		file << Write(d);
	}
	const auto denied = client::LoadImageGraphFrame(directory, graph, out, 2, 7);
	CHECK(denied.Status == Status::UnsupportedExecution);
	CHECK(result.Image.Pixels == std::vector<uint8_t>{63, 17, 0, 255});
}
