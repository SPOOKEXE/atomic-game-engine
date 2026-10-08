#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraphio/PxcxImport.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraphio.pxcx_color_depth")
using namespace engine::imagegraphio;
namespace {
	engine::bake::PxcxArchive Archive(std::string_view depth) {
		engine::bake::PxcxArchive source;
		source.MetadataNumber = 121092;
		source.MetadataText = "1.22.10.201";
		source.GraphJson = R"({"attributes":{"surface_dimension":[8,3])";
		if (!depth.empty()) source.GraphJson += ",\"color_depth\":" + std::string(depth);
		source.GraphJson +=
			R"(},"nodes":[{"id":"group","type":"Node_Group","x":0,"y":0,"inputs":[],"attri":{"color_depth":5}}]})";
		source.GraphJson.push_back('\0');
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(source, bytes, failure));
		engine::bake::PxcxArchive checked;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		return checked;
	}
}
TEST_CASE(
	"PXC project depth and ordinary group depth project while source bytes remain exact",
	"[imagegraphio][color_depth]"
) {
	for (int64_t depth = 0; depth <= 6; ++depth) {
		const auto archive = Archive(std::to_string(depth));
		PxcxImport result;
		std::string failure;
		REQUIRE(ImportPxcxImageGraph(archive, result, failure));
		REQUIRE(result.Graph.Project);
		CHECK(result.Graph.Project->ColorDepth == depth);
		CHECK(result.Graph.FormatVersion == 11);
		CHECK(result.Source.OriginalBytes == archive.OriginalBytes);
		CHECK(result.Source.GraphJson == archive.GraphJson);
		REQUIRE(result.Graph.Groups.size() == 1);
		CHECK(result.Graph.Groups.front().Id == "group");
		CHECK(result.Graph.Groups.front().ColorDepth == 5);
		CHECK(result.Graph.Nodes.empty());
		REQUIRE(result.Source.Nodes.size() == 1);
		CHECK(result.Source.Nodes.front().Id == "group");
		CHECK(result.Source.Nodes.front().Type == "Node_Group");
		CHECK_FALSE(engine::imagegraph::HasNativeExecutor("pc.group"));
		CHECK(result.CatalogueNodes == 0);
		CHECK(result.NativeNodes == 0);
		engine::imagegraph::Document restored;
		engine::imagegraph::Diagnostic diagnostic;
		REQUIRE(
			engine::imagegraph::Read(engine::imagegraph::Write(result.Graph), restored, diagnostic) ==
			engine::imagegraph::Status::Ok
		);
		CHECK(restored == result.Graph);
	}
	PxcxImport result;
	std::string failure;
	REQUIRE(ImportPxcxImageGraph(Archive({}), result, failure));
	CHECK(result.Graph.Project->ColorDepth == 1);
}
TEST_CASE("Unrepresented source project depth refuses atomically", "[imagegraphio][color_depth]") {
	PxcxImport result;
	std::string failure;
	REQUIRE(ImportPxcxImageGraph(Archive("3"), result, failure));
	const auto original = engine::imagegraph::Write(result.Graph);
	const auto originalBytes = result.Source.OriginalBytes;
	for (std::string_view invalid : {"-1", "7", "1.5", "true", "\"rgba32f\""}) {
		CHECK_FALSE(ImportPxcxImageGraph(Archive(invalid), result, failure));
		CHECK(failure.find("color_depth") != std::string::npos);
		CHECK(engine::imagegraph::Write(result.Graph) == original);
		CHECK(result.Source.OriginalBytes == originalBytes);
	}
}
