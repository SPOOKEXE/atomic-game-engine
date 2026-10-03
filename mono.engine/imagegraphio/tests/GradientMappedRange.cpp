#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>

TEST_SUITE_ID("engine.imagegraphio.gradient_mapped_range")
using namespace engine::imagegraph;
using namespace engine::imagegraphio;
namespace {
	constexpr std::array<std::pair<std::string_view, size_t>, 4> Controls{
		{{"angle", 3}, {"radius", 4}, {"shift", 5}, {"scale", 9}}
	};
	PxcxImport ImportGradientMapped(size_t index, bool mapped = true) {
		engine::bake::PxcxArchive source;
		source.MetadataNumber = 121092;
		source.MetadataText = "1.22.10.201";
		source.GraphJson =
			R"({"nodes":[{"id":"mapped","x":0,"y":0,"type":"Node_Gradient","future_node":19,"inputs":[)";
		for (size_t i = 0; i <= index; ++i) {
			if (i) source.GraphJson += ',';
			if (i == index)
				source.GraphJson +=
					mapped ? R"({"anim":false,"r":{"d":[0.5,2.5]},"attri":{"mapped":true,"future":7}})"
						   : R"({"anim":false,"r":{"d":1.5},"attri":{"mapped":false,"future":7}})";
			else if (i == 0)
				source.GraphJson += R"({"r":{"d":[1,1]},"attri":{"use_project_dimension":0}})";
			else
				source.GraphJson += "{}";
		}
		source.GraphJson += "]}]}";
		source.GraphJson.push_back('\0');
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(source, bytes, failure));
		engine::bake::PxcxArchive checked;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		PxcxImport imported;
		REQUIRE(ImportPxcxImageGraph(checked, imported, failure));
		return imported;
	}
	AuthoredValue &Control(Document &d, std::string_view port) {
		auto value = std::find_if(d.Nodes[0].Values.begin(), d.Nodes[0].Values.end(), [&](const auto &v) {
			return v.Port == port;
		});
		REQUIRE(value != d.Nodes[0].Values.end());
		return *value;
	}
	PxcxImport Reimport(const std::vector<std::byte> &bytes) {
		engine::bake::PxcxArchive checked;
		std::string failure;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		PxcxImport replay;
		REQUIRE(ImportPxcxImageGraph(checked, replay, failure));
		return replay;
	}
} // namespace
TEST_CASE(
	"Gradient mapped Rotation Float and Slider pairs edit exact source slots", "[gradient_mapped_range]"
) {
	for (const auto &[port, index] : Controls) {
		auto imported = ImportGradientMapped(index);
		REQUIRE(imported.Graph.Nodes[0].Type == "pc.gradient");
		const auto rangePort = std::string(port) + "_map_range";
		const auto togglePort = std::string(port) + "_mapped";
		CHECK(Control(imported.Graph, togglePort).Data == Value{true});
		CHECK(Control(imported.Graph, rangePort).Data == Value{Vector2{.5, 2.5}});
		Diagnostic diagnostic;
		Document desired;
		REQUIRE(Read(Write(imported.Graph), desired, diagnostic) == Status::Ok);
		std::vector<std::byte> bytes;
		REQUIRE(WritePxcxProjection(imported, desired, {}, bytes, diagnostic));
		REQUIRE(bytes == imported.Source.OriginalBytes);
		Control(desired, rangePort).Data = Vector2{1.5, 3.5};
		const auto written = WritePxcxProjection(imported, desired, {}, bytes, diagnostic);
		INFO(diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(written);
		auto replay = Reimport(bytes);
		CHECK(replay.Graph.Nodes[0].Values == desired.Nodes[0].Values);
		CHECK(replay.Source.GraphJson.find("\"future\":7") != std::string::npos);
		CHECK(replay.Source.GraphJson.find("\"future_node\":19") != std::string::npos);
		Document executable = replay.Graph;
		executable.Outputs = {{"image", "mapped", "surface_out"}};
		Plan plan;
		REQUIRE(Compile(executable, plan, diagnostic) == Status::Ok);
		Image image;
		const auto status = Evaluate(executable, plan, "image", {}, image, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		REQUIRE(image.Width == 1);
		REQUIRE(image.Height == 1);
		const auto gray = port == "shift" ? uint8_t{255} : uint8_t{128};
		CHECK(image.Pixels == std::vector<uint8_t>{gray, gray, gray, 255});
	}
}
TEST_CASE("Gradient mapped toggles switch exact compact source representations", "[gradient_mapped_range]") {
	for (const auto &[port, index] : Controls) {
		for (bool mapped : {false, true}) {
			auto imported = ImportGradientMapped(index, mapped);
			Document desired = ImportGradientMapped(index, !mapped).Graph;
			std::vector<std::byte> bytes;
			Diagnostic diagnostic;
			const auto written = WritePxcxProjection(imported, desired, {}, bytes, diagnostic);
			INFO(port << ":" << diagnostic.Message);
			REQUIRE(written);
			auto replay = Reimport(bytes);
			CHECK(replay.Graph == desired);
			if (mapped)
				CHECK(Control(replay.Graph, port).Data == Value{1.5});
			else
				CHECK(
					Control(replay.Graph, std::string(port) + "_map_range").Data == Value{Vector2{.5, 2.5}}
				);
			CHECK(replay.Source.GraphJson.find("\"future\":7") != std::string::npos);
		}
	}
}
TEST_CASE(
	"Gradient inverse refuses wrong mapped values atomically at each "
	"exact port",
	"[gradient_mapped_range]"
) {
	for (const auto &[port, index] : Controls) {
		const auto imported = ImportGradientMapped(index);
		for (bool badToggle : {false, true}) {
			Document desired = imported.Graph;
			const auto target = std::string(port) + (badToggle ? "_mapped" : "_map_range");
			Control(desired, target).Data = badToggle ? Value{int64_t{1}} : Value{Vector4{0, 1, 2, 3}};
			std::vector<std::byte> bytes{std::byte{42}};
			Diagnostic diagnostic;
			CHECK_FALSE(WritePxcxProjection(imported, desired, {}, bytes, diagnostic));
			CHECK(bytes == std::vector<std::byte>{std::byte{42}});
		}
	}
}
TEST_CASE(
	"Gradient inverse refuses disabled mapped mode with retained "
	"endpoint-only state",
	"[gradient_mapped_range]"
) {
	for (const auto &[port, index] : Controls) {
		auto imported = ImportGradientMapped(index);
		Document desired = imported.Graph;
		Control(desired, std::string(port) + "_mapped").Data = false;
		std::vector<std::byte> bytes{std::byte{43}};
		Diagnostic diagnostic;
		CHECK_FALSE(WritePxcxProjection(imported, desired, {}, bytes, diagnostic));
		CHECK(bytes == std::vector<std::byte>{std::byte{43}});
	}
}
