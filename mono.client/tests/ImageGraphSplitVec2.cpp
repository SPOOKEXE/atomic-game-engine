#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <client/ImageGraphRuntime.hpp>
#include <filesystem>
#include <fstream>

TEST_SUITE_ID("client.imagegraph_split_vec2")
TEST_DEPENDS("engine.imagegraph.source_separated_vec2")

TEST_CASE(
	"Split Mirror axes schedule static storage and replay explicitly animated seeks", "[client][mirror_axes]"
) {
	using namespace engine::imagegraph;
	Document document;
	document.FormatVersion = 9;
	document.Project = ProjectSettings{};
	document.Project->SurfaceWidth = document.Project->SurfaceHeight = 4;
	document.Timeline = TimelineSettings{3, 0, 2, "loop", 30};
	document.Nodes = {
		{"source",
		 "image.checker",
		 "",
		 {},
		 {{"width", int64_t{4}},
		  {"height", int64_t{4}},
		  {"size", .5},
		  {"diagonal", true},
		  {"color_1", Colour{10, 20, 30, 255}},
		  {"color_2", Colour{50, 60, 70, 255}}}},
		{"mirror",
		 "pc.mirror_polar",
		 "",
		 {},
		 {{"center", Vector2{.9, .9}}, {"scale", Vector2{0, 0}}, {"interpolate", EnumValue{1}}}}
	};
	auto &axes = document.Nodes.back()
					 .SourceSeparatedVec2Animators.emplace()
					 .Inputs.emplace_back("center", std::array<SourceScalarAnimator, 2>{})
					 .Axes;
	axes[0].Keys = {
		{"mirror", "center", 0, 0., "source", KeyframeEase{}},
		{"mirror", "center", 2, .25, "source", KeyframeEase{}}
	};
	axes[1].Keys = {{"mirror", "center", 0, 0., "source", KeyframeEase{}}};
	document.Links = {{"source", "image", "mirror", "surface_in"}};
	document.Outputs = {{"out", "mirror", "surface_out"}};
	bool animated = false;
	SECTION("Unspecified source mode keeps static axes and still schedules authored storage") {
		REQUIRE(document.Nodes.back().SourceAnimatedInputs.empty());
	}
	SECTION("Explicit source animation changes sampled pixels and reconstructs seeks") {
		animated = true;
		document.Nodes.back().SourceAnimatedInputs = {"center"};
	}
	REQUIRE(document.Keyframes.empty());
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const auto directory = std::filesystem::temp_directory_path() / "atomic-client-split-vec2";
	std::filesystem::remove_all(directory);
	std::filesystem::create_directories(directory / "imagegraphs");
	struct Cleanup {
		std::filesystem::path Directory;
		~Cleanup() {
			std::filesystem::remove_all(Directory);
		}
	} cleanup{directory};
	const engine::core::Name graph("split-mirror"), output("out");
	{
		std::ofstream file(client::ImageGraphDocumentPath(directory, graph));
		file << Write(document);
		REQUIRE(file.good());
	}
	const auto first = client::LoadImageGraphFrame(directory, graph, output, 0);
	INFO(first.Diagnostic.Message);
	REQUIRE(first.Status == Status::Ok);
	CHECK(first.Animated);
	const auto later = client::LoadImageGraphFrame(directory, graph, output, 2);
	INFO(later.Diagnostic.Message);
	REQUIRE(later.Status == Status::Ok);
	CHECK(later.Animated);
	REQUIRE(first.Image.Pixels.size() == 64);
	REQUIRE(later.Image.Pixels.size() == 64);
	for (size_t i = 0; i < 64; i += 4) {
		CHECK(first.Image.Pixels[i] == 10);
		CHECK(first.Image.Pixels[i + 1] == 20);
		CHECK(first.Image.Pixels[i + 2] == 30);
		CHECK(first.Image.Pixels[i + 3] == 255);
		CHECK(later.Image.Pixels[i] == (animated ? 50 : 10));
		CHECK(later.Image.Pixels[i + 1] == (animated ? 60 : 20));
		CHECK(later.Image.Pixels[i + 2] == (animated ? 70 : 30));
		CHECK(later.Image.Pixels[i + 3] == 255);
	}
	CHECK((first.Image.Hash != later.Image.Hash) == animated);
	const auto repeat = client::LoadImageGraphFrame(directory, graph, output, 2);
	REQUIRE(repeat.Status == Status::Ok);
	CHECK(repeat.Image == later.Image);
	const auto reset = client::LoadImageGraphFrame(directory, graph, output, 0);
	REQUIRE(reset.Status == Status::Ok);
	CHECK(reset.Image == first.Image);
}
