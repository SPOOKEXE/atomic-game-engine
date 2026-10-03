#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <client/ImageGraphRuntime.hpp>
#include <filesystem>
#include <fstream>

TEST_SUITE_ID("client.imagegraphsequenceanimation")
TEST_DEPENDS("engine.imagegraph.source_sequence_animation")

namespace {
	const engine::core::Name GRAPH("source-sequence-cadence");
	const engine::core::Name OUTPUT("final");
	struct GraphFile {
		std::filesystem::path Assets =
			std::filesystem::temp_directory_path() / "atomic-source-sequence-cadence";
		GraphFile() {
			std::filesystem::create_directories(Assets / "imagegraphs");
		}
		~GraphFile() {
			std::filesystem::remove_all(Assets);
		}
		void Store(const engine::imagegraph::Document &document) const {
			std::ofstream stream(client::ImageGraphDocumentPath(Assets, GRAPH), std::ios::binary);
			REQUIRE(stream);
			stream << engine::imagegraph::Write(document);
			REQUIRE(stream.good());
		}
	};
	engine::imagegraph::Document Sequence() {
		using namespace engine::imagegraph;
		Document document;
		document.FormatVersion = 9;
		document.Timeline = TimelineSettings{4, 0, 3, "loop", 24};
		document.Nodes = {
			{"red",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{2}}, {"height", int64_t{1}}, {"colour", Colour{255, 0, 0, 255}}}},
			{"blue",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{2}}, {"height", int64_t{1}}, {"colour", Colour{0, 0, 255, 255}}}},
			{"frames", "value.array", "", {}, {}},
			{"sequence",
			 "pc.sequence_anim",
			 "",
			 {},
			 {{"overflow", EnumValue{1}}, {"speed", 1.}, {"sequence", ArrayValue{ValueType::Scalar, {}}}}}
		};
		document.Nodes[2].DynamicInputs = {{"red", ValueType::Image, {}}, {"blue", ValueType::Image, {}}};
		document.Links = {
			{"red", "image", "frames", "red"},
			{"blue", "image", "frames", "blue"},
			{"frames", "array", "sequence", "surface_in"}
		};
		document.Outputs = {{"final", "sequence", "surface_out"}};
		return document;
	}
} // namespace
TEST_CASE(
	"Client samples keyless source sequences while retaining static "
	"graph cadence",
	"[client][imagegraph][source_sequence_animation]"
) {
	using namespace engine::imagegraph;
	GraphFile file;
	const auto document = Sequence();
	REQUIRE(document.Keyframes.empty());
	file.Store(document);
	const auto first = client::LoadImageGraphFrame(file.Assets, GRAPH, OUTPUT, 0);
	INFO(first.Diagnostic.Message);
	REQUIRE(first.Status == Status::Ok);
	CHECK(first.Animated);
	CHECK(first.Image.Pixels == std::vector<uint8_t>{255, 0, 0, 255, 255, 0, 0, 255});
	const auto second = client::LoadImageGraphFrame(file.Assets, GRAPH, OUTPUT, 1);
	INFO(second.Diagnostic.Message);
	REQUIRE(second.Status == Status::Ok);
	CHECK(second.Animated);
	CHECK(second.Image.Pixels == std::vector<uint8_t>{0, 0, 255, 255, 0, 0, 255, 255});
	const auto loop = client::LoadImageGraphFrame(file.Assets, GRAPH, OUTPUT, 2);
	REQUIRE(loop.Status == Status::Ok);
	CHECK(loop.Image == first.Image);
	const auto seek = client::LoadImageGraphFrame(file.Assets, GRAPH, OUTPUT, 1);
	REQUIRE(seek.Status == Status::Ok);
	CHECK(seek.Image == second.Image);
	auto staticDocument = document;
	staticDocument.Nodes.resize(1);
	staticDocument.Links.clear();
	staticDocument.Outputs = {{"final", "red", "image"}};
	file.Store(staticDocument);
	const auto staticFirst = client::LoadImageGraphFrame(file.Assets, GRAPH, OUTPUT, 0);
	const auto staticLater = client::LoadImageGraphFrame(file.Assets, GRAPH, OUTPUT, 3);
	INFO(staticLater.Diagnostic.Message);
	REQUIRE(staticFirst.Status == Status::Ok);
	REQUIRE(staticLater.Status == Status::Ok);
	CHECK_FALSE(staticFirst.Animated);
	CHECK_FALSE(staticLater.Animated);
	CHECK(staticFirst.Image == staticLater.Image);
	CHECK(staticLater.Image == first.Image);
}
