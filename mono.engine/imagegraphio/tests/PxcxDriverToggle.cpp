#include <engine/imagegraphio/PxcxImport.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraphio.pxcxdrivertoggle")
TEST_DEPENDS("engine.imagegraphio.pxcximport")

using namespace engine::imagegraph;
using namespace engine::imagegraphio;

namespace {
	PxcxImport Imported(bool active) {
		engine::bake::PxcxArchive source;
		source.MetadataNumber = 121092;
		source.MetadataText = "1.22.10.201";
		source.GraphJson =
			R"({"animator":{"frames_total":30,"playback":0,"framerate":30},"nodes":[{"id":"number","type":"Node_Number_Simple","x":1,"y":2,"inputs":[{"anim":)" +
			std::string(active ? "true" : "false") +
			R"(,"r":[[[0,0],5,[0,1],[0,0],0,0,true,{"typ":"linear","spd":1,"future":{"keep":"driver"}},4294967295,"retained-tail"]],"future":{"keep":"input"}}],"future":{"keep":"node"}}],"future":{"keep":"project"}})";
		source.GraphJson.push_back('\0');
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(source, bytes, failure));
		engine::bake::PxcxArchive checked;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		PxcxImport result;
		REQUIRE(ImportPxcxImageGraph(checked, result, failure));
		return result;
	}
	double Evaluated(const Document &graph) {
		Plan plan;
		Diagnostic diagnostic;
		const auto compiled = Compile(graph, plan, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(compiled == Status::Ok);
		EvaluationRequest request;
		request.Tick = 3;
		EvaluatedValue value;
		const auto evaluated = EvaluateValue(graph, plan, "result", request, value, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(evaluated == Status::Ok);
		REQUIRE(std::holds_alternative<double>(value.Data));
		return std::get<double>(value.Data);
	}
}

TEST_CASE(
	"PXC Float animation toggle controls the reachable single-key linear driver",
	"[imagegraphio][pxcx_driver_toggle]"
) {
	// NumberSimple.update -> Float.getValue -> getValueRecursive -> Float.__getAnimValue.
	// getAnim false returns first.value; true reaches animator and KeyDriver_Linear.apply.
	for (const bool active : {false, true}) {
		const auto imported = Imported(active);
		REQUIRE(imported.Graph.Nodes[0].Type == "pc.number_simple");
		REQUIRE(imported.CatalogueNodes == 1);
		REQUIRE(imported.Graph.Keyframes.size() == 1);
		CHECK(imported.Graph.Keyframes.front().SourceDriver == KeyframeSourceDriver{KeyframeLinearDriver{1}});
		CHECK(imported.Graph.Tracks.size() == 1);
		Document graph = imported.Graph;
		graph.Outputs = {{"result", "number", "number"}};
		const double expected = active ? 5 + 3 * 1 : 5;
		CHECK(Evaluated(graph) == expected);
		Document restored;
		Diagnostic diagnostic;
		REQUIRE(Read(Write(graph), restored, diagnostic) == Status::Ok);
		CHECK(restored == graph);
		CHECK(Evaluated(restored) == expected);
		std::vector<std::byte> exact;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(imported.Source, exact, failure));
		CHECK(exact == imported.Source.OriginalBytes);
		CHECK(imported.Source.GraphJson.find(R"("future":{"keep":"driver"})") != std::string::npos);
		CHECK(imported.Source.GraphJson.find(R"("retained-tail")") != std::string::npos);
	}
	auto inactive = Imported(false).Source.GraphJson;
	const auto active = Imported(true).Source.GraphJson;
	const auto position = inactive.find(R"("anim":false)");
	REQUIRE(position != std::string::npos);
	inactive.replace(position, std::string_view(R"("anim":false)").size(), R"("anim":true)");
	CHECK(inactive == active);
}
