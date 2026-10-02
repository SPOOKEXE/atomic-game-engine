#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraphio/PxcxEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <iostream>

TEST_SUITE_ID("engine.imagegraphio.pxcxlastkey")
TEST_DEPENDS("engine.imagegraphio.pxcxkeystructure")
using namespace engine::imagegraph;
using namespace engine::imagegraphio;
namespace {
	PxcxImport Imported() {
		engine::bake::PxcxArchive source;
		source.MetadataNumber = 121092;
		source.MetadataText = "1.22.10.201";
		source.GraphJson =
			R"({"animator":{"frames_total":30,"playback":1,"framerate":30},"nodes":[{"id":"number","type":"Node_Number_Simple","x":0,"y":0,"inputs":[{"anim":true,"r":[[[1,0,0,"removed-marker"],5,[0,1],[0,0],0,0,true,{"typ":"linear","spd":0.5},16777215,"removed-tail"]],"future":"retained-input"}]}],"future":"retained-project"})";
		source.GraphJson.push_back('\0');
		std::string failure;
		std::vector<std::byte> bytes;
		const bool written = engine::bake::WritePxcx(source, bytes, failure);
		INFO(failure);
		REQUIRE(written);
		engine::bake::PxcxArchive checked;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		PxcxImport result;
		REQUIRE(ImportPxcxImageGraph(checked, result, failure));
		return result;
	}
	PxcxImport Edited(const PxcxImport &source, const PxcxEdit &edit) {
		const std::array edits{edit};
		std::vector<std::byte> bytes;
		Diagnostic diagnostic;
		const bool written = WritePxcxEdits(source, source.Source.OriginalBytes, edits, bytes, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(written);
		engine::bake::PxcxArchive checked;
		std::string failure;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		PxcxImport result;
		REQUIRE(ImportPxcxImageGraph(checked, result, failure, source.Options));
		Document restored;
		REQUIRE(Read(Write(result.Graph), restored, diagnostic) == Status::Ok);
		CHECK(restored == result.Graph);
		return result;
	}
}
TEST_CASE(
	"PXC last-key deletion captures lone raw driver at explicit project frame",
	"[imagegraphio][pxcx_last_key]"
) {
	const auto source = Imported();
	REQUIRE(source.Graph.Nodes.front().Type == "pc.number_simple");
	const auto result = Edited(source, PxcxKeyframeDeleteEdit{"number", "value", {}, FrameTime{3}});
	REQUIRE(result.Graph.Keyframes.size() == 1);
	CHECK(result.Graph.Keyframes.front().Port == "value");
	CHECK(result.Graph.Keyframes.front().Tick == 0);
	CHECK(result.Graph.Keyframes.front().Kind == KeyframeKind::Normal);
	CHECK(result.Graph.Keyframes.front().Data == Value{6.5});
	CHECK_FALSE(result.Graph.Keyframes.front().SourceDriver);
	CHECK(result.Graph.Tracks.size() == 1);
	Document graph = result.Graph;
	graph.Outputs = {{"out", "number", "number"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(graph, plan, diagnostic) == Status::Ok);
	EvaluatedValue value;
	REQUIRE(EvaluateValue(graph, plan, "out", {}, value, diagnostic) == Status::Ok);
	CHECK(std::get<double>(value.Data) == 6.5);
	CHECK(result.Source.GraphJson.find(R"("r":{"d":6.5})") != std::string::npos);
	CHECK(result.Source.GraphJson.find(R"("anim":false)") != std::string::npos);
	CHECK(result.Source.GraphJson.find(R"("future":"retained-input")") != std::string::npos);
	CHECK(result.Source.GraphJson.find(R"("future":"retained-project")") != std::string::npos);
	CHECK(source.Source.GraphJson.find("removed-tail") != std::string::npos);
	CHECK(result.Source.GraphJson.find("removed-tail") == std::string::npos);
}
TEST_CASE(
	"PXC last-key capture requires exact explicit clock and fails atomically", "[imagegraphio][pxcx_last_key]"
) {
	const auto source = Imported();
	for (PxcxKeyframeDeleteEdit edit :
		 {PxcxKeyframeDeleteEdit{"number", "value", {}},
		  PxcxKeyframeDeleteEdit{"number", "value", {1}, FrameTime{3}},
		  PxcxKeyframeDeleteEdit{"number", "value", {}, FrameTime{0, 1}}}) {
		const std::array<PxcxEdit, 1> edits{edit};
		const auto sentinel = source.Source.OriginalBytes;
		auto output = sentinel;
		Diagnostic diagnostic;
		CHECK_FALSE(WritePxcxEdits(source, sentinel, edits, output, diagnostic));
		CHECK(output == sentinel);
	}
}

TEST_CASE(
	"PXC gradient last-key reset cascades only to its animated hidden range", "[imagegraphio][pxcx_last_key]"
) {
	CHECK_FALSE(HasNativeExecutor("pc.gradient"));
	for (bool rangeAnimated : {false, true}) {

		// Source Node_Gradient declares gradient1, map15, hidden range16. The entire
		// control vector is supplied so the importer can verify the real node mapping.
		std::array<std::string, 26> inputs;
		inputs.fill(R"({"r":{"d":-4}})");
		const auto value = [](std::string data) { return "{\"r\":{\"d\":" + data + "}}"; };
		const std::string identity = "[0,1,0,0,1,0,0,0,0,0,0.3333333333333333,0.3333333333333333,-0."
									 "3333333333333333,-0.3333333333333333,1,1,0,0]";
		inputs[0] = R"({"r":{"d":[1,1]},"attri":{"use_project_dimension":1}})";
		inputs[1] =
			R"({"anim":true,"r":[[[0,0],"{\"keys\":[{\"time\":0,\"value\":4278190080},{\"time\":1,\"value\":4294967295}],\"type\":0}",[0,1],[0,0],0,0,true,0]],"future":"gradient-input"})";
		inputs[2] = value("0");
		inputs[3] = value("90");
		inputs[4] = value("0.5");
		inputs[5] = value("0");
		inputs[6] = value("[0.5,0.5]");
		inputs[7] = value("0");
		inputs[9] = value("1");
		inputs[14] = value("true");
		inputs[17] = value("[1,1]");
		inputs[19] = value("1");
		inputs[20] = value(identity);
		inputs[21] = value("0");
		inputs[22] = value("[0,1,0,0,1,0,0,0,0,0,0.3333333333333333,0,-0.3333333333333333,0,1,0,0,0]");
		inputs[23] = value("[0,1]");
		inputs[24] = value(identity);
		inputs[25] = value("[0,1]");
		inputs[16] =
			std::string("{\"anim\":") + (rangeAnimated ? "true" : "false") +
			R"(,"r":[[[0,0],[0,0,1,0],[0,1],[0,0],0,0,true,{"typ":"linear","spd":0.5}]],"future":"range-input"})";
		engine::bake::PxcxArchive archive;
		archive.MetadataNumber = 121092;
		archive.MetadataText = "1.22.10.201";
		archive.GraphJson =
			R"({"animator":{"frames_total":30,"playback":1,"framerate":30},"nodes":[{"id":"gradient","type":"Node_Gradient","x":0,"y":0,"inputs":[)";
		for (size_t index = 0; index < inputs.size(); ++index) {
			if (index) archive.GraphJson += ',';
			archive.GraphJson += inputs[index];
		}
		archive.GraphJson += "]}]}";
		archive.GraphJson.push_back('\0');
		std::vector<std::byte> bytes;
		std::string failure;
		const bool written = engine::bake::WritePxcx(archive, bytes, failure);
		INFO(failure);
		REQUIRE(written);
		engine::bake::PxcxArchive checked;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		PxcxImport source;
		REQUIRE(ImportPxcxImageGraph(checked, source, failure));
		REQUIRE(source.Graph.Nodes.front().Type == "pc.gradient");
		CHECK(
			source.Graph.Nodes.front().SourceStaticInputs ==
			(rangeAnimated ? std::vector<std::string>{} : std::vector<std::string>{"gradient_map_range"})
		);
		CHECK(
			source.Graph.Nodes.front().SourceAnimatedInputs ==
			(rangeAnimated ? std::vector<std::string>{"gradient_map_range"} : std::vector<std::string>{})
		);
		Document modeRoundTrip;
		Diagnostic modeDiagnostic;
		REQUIRE(Read(Write(source.Graph), modeRoundTrip, modeDiagnostic) == Status::Ok);
		CHECK(modeRoundTrip == source.Graph);
		Document invalidMode = source.Graph;
		invalidMode.Nodes.front().SourceStaticInputs = {"gradient"};
		invalidMode.Nodes.front().SourceAnimatedInputs.clear();
		CHECK(Migrate(invalidMode, modeDiagnostic) == Status::InvalidValue);
		Plan invalidModePlan;
		CHECK(Compile(invalidMode, invalidModePlan, modeDiagnostic) == Status::InvalidValue);
		if (!rangeAnimated) {
			std::string absentModeGraph = archive.GraphJson;
			const std::string modeField = R"("anim":false,)";
			const auto modeOffset = absentModeGraph.find(modeField);
			REQUIRE(modeOffset != std::string::npos);
			absentModeGraph.erase(modeOffset, modeField.size());
			engine::bake::PxcxArchive absentModeArchive = archive;
			absentModeArchive.GraphJson = std::move(absentModeGraph);
			std::vector<std::byte> absentModeBytes;
			REQUIRE(engine::bake::WritePxcx(absentModeArchive, absentModeBytes, failure));
			engine::bake::PxcxArchive absentModeChecked;
			REQUIRE(engine::bake::ReadPxcx(absentModeBytes, absentModeChecked, failure));
			PxcxImport absentMode;
			REQUIRE(ImportPxcxImageGraph(absentModeChecked, absentMode, failure));
			CHECK(absentMode.Graph.Nodes.front().SourceStaticInputs.empty());
			CHECK(absentMode.Graph.Nodes.front().SourceAnimatedInputs.empty());
			std::string malformedModeGraph = archive.GraphJson;
			const std::string validMode = R"("anim":false)";
			const auto validModeOffset = malformedModeGraph.find(validMode);
			REQUIRE(validModeOffset != std::string::npos);
			malformedModeGraph.replace(validModeOffset, validMode.size(), R"("anim":null)");
			engine::bake::PxcxArchive malformedModeArchive = archive;
			malformedModeArchive.GraphJson = std::move(malformedModeGraph);
			std::vector<std::byte> malformedModeBytes;
			REQUIRE(engine::bake::WritePxcx(malformedModeArchive, malformedModeBytes, failure));
			engine::bake::PxcxArchive malformedModeChecked;
			REQUIRE(engine::bake::ReadPxcx(malformedModeBytes, malformedModeChecked, failure));
			PxcxImport malformedMode;
			CHECK_FALSE(ImportPxcxImageGraph(malformedModeChecked, malformedMode, failure));
		}
		const auto result = Edited(source, PxcxKeyframeDeleteEdit{"gradient", "gradient", {}, FrameTime{3}});
		CHECK(
			result.Graph.Nodes.front().SourceStaticInputs == std::vector<std::string>{"gradient_map_range"}
		);
		CHECK(result.Graph.Nodes.front().SourceAnimatedInputs.empty());
		const auto gradientKey =
			std::find_if(result.Graph.Keyframes.begin(), result.Graph.Keyframes.end(), [](const auto &key) {
				return key.Port == "gradient";
			});
		REQUIRE(gradientKey != result.Graph.Keyframes.end());
		CHECK(gradientKey->Tick == 0);
		CHECK(gradientKey->Kind == KeyframeKind::Normal);
		CHECK_FALSE(gradientKey->SourceDriver);
		const auto range =
			std::find_if(result.Graph.Keyframes.begin(), result.Graph.Keyframes.end(), [](const auto &key) {
				return key.NodeId == "gradient" && key.Port == "gradient_map_range";
			});
		REQUIRE(range != result.Graph.Keyframes.end());
		const Vector4 expectedRange = rangeAnimated ? Vector4{1.5, 1.5, 2.5, 1.5} : Vector4{0, 0, 1, 0};
		CHECK(std::get<Vector4>(range->Data) == expectedRange);
		CHECK(range->SourceDriver.has_value() == !rangeAnimated);
		Document graph = result.Graph;
		graph.Outputs = {{"gradient_output", "gradient", "surface_out"}};
		Plan plan;
		Diagnostic diagnostic;
		const auto compiled = Compile(graph, plan, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(compiled == Status::Ok);
		EvaluationRequest clock;
		clock.Tick = 11;
		EvaluationSnapshot snapshot;
		REQUIRE(
			EvaluateNodeInputs(graph, plan, "gradient", clock, snapshot, diagnostic) == Status::Ok
		);
		const auto resolved =
			std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &value) {
				return value.Port == "gradient_map_range";
			});
		REQUIRE(resolved != snapshot.Values().end());
		CHECK(resolved->Data == Value{expectedRange});
		CHECK(result.Source.GraphJson.find(R"("future":"range-input")") != std::string::npos);
		if (!rangeAnimated) CHECK(result.Source.GraphJson.find(inputs[16]) != std::string::npos);
	}
}
