#include <engine/imagegraphio/PxcxEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.imagegraphio.pxcxrawsourcegetter")
using namespace engine::imagegraph;
using namespace engine::imagegraphio;
namespace {
	std::vector<engine::imagegraph::Keyframe> KeyframesFor(
		const engine::imagegraph::Document &document, std::string_view nodeId, std::string_view port
	) {
		std::vector<engine::imagegraph::Keyframe> result;
		for (const auto &key : document.Keyframes)
			if (key.NodeId == nodeId && key.Port == port) result.push_back(key);
		return result;
	}
	bool HasCanonicalStaticInput(
		const engine::imagegraph::Document &document, std::string_view nodeId, std::string_view port
	) {
		const auto node = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &entry) {
			return entry.Id == nodeId;
		});
		if (node == document.Nodes.end() ||
			std::find(node->SourceStaticInputs.begin(), node->SourceStaticInputs.end(), port) ==
				node->SourceStaticInputs.end())
			return false;
		const auto keys = KeyframesFor(document, nodeId, port);
		const auto tracks =
			std::count_if(document.Tracks.begin(), document.Tracks.end(), [&](const auto &track) {
				return track.NodeId == nodeId && track.Port == port;
			});
		return keys.size() == 1 && GetFrameTime(keys.front()) == FrameTime{} &&
			   keys.front().Kind == KeyframeKind::Normal && tracks == 1;
	}
	PxcxImport Imported(std::string_view type, std::string_view inputs) {
		engine::bake::PxcxArchive source;
		source.MetadataNumber = 121092;
		source.MetadataText = "1.22.10.201";
		source.GraphJson =
			R"({"animator":{"frames_total":30,"playback":1,"framerate":30},"nodes":[{"id":"source","type":")" +
			std::string(type) + R"(","x":0,"y":0,"inputs":[)" + std::string(inputs) +
			R"(],"future":"node"}],"future":"project"})";
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
		REQUIRE(result.CatalogueNodes == 1);
		return result;
	}
	PxcxImport Edited(const PxcxImport &source, const PxcxEdit &edit) {
		std::array edits{edit};
		std::vector<std::byte> bytes;
		Diagnostic diagnostic;
		const bool written = WritePxcxEdits(source, source.Source.OriginalBytes, edits, bytes, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(written);
		engine::bake::PxcxArchive checked;
		std::string failure;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		PxcxImport result;
		REQUIRE(ImportPxcxImageGraph(checked, result, failure));
		return result;
	}
	const Value &Raw(const PxcxImport &source, std::string_view port) {
		for (const auto &value : source.Graph.Nodes.front().Values)
			if (value.Port == port) return value.Data;
		FAIL("raw property missing");
		static Value missing;
		return missing;
	}
	size_t ChannelSamples(const Value &value) {
		const auto &array = std::get<ArrayValue>(value);
		REQUIRE(array.Nested.size() == 1);
		return array.Nested.front().size();
	}
	Value Evaluated(const PxcxImport &source, bool audio) {
		Document graph = source.Graph;
		const std::array frames{AudioCaptureFrame{"mono", 0, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10}, 10}};
		EvaluationRequest request;
		if (audio) {
			// The native host supplies the recorded frame; no PXC asset or device is invented.
			graph.Nodes.push_back(
				{"capture", "image.audio_recording", "", {}, {{"source_id", std::string{"mono"}}}, {}}
			);
			graph.Links.push_back({"capture", "audio", "source", "audio_data"});
			graph.Outputs = {{"out", "source", "bit_array"}};
			request.AudioFrames = frames;
		} else
			graph.Outputs = {{"out", "source", "x"}};
		Document restored;
		Diagnostic diagnostic;
		REQUIRE(Read(Write(graph), restored, diagnostic) == Status::Ok);
		CHECK(restored == graph);
		Plan plan;
		const auto compiled = Compile(restored, plan, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(compiled == Status::Ok);
		EvaluatedValue result;
		const auto status = EvaluateValue(restored, plan, "out", request, result, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return result.Data;
	}
}
TEST_CASE(
	"PXC raw Int survives archive edits native persistence and source getter evaluation",
	"[imagegraphio][pxcx_raw_getter]"
) {
	const auto source = Imported(
		"Node_Audio_Window",
		R"({"r":{"d":-4}},{"r":{"d":6.5}},{"r":{"d":0}},{"r":{"d":0}},{"r":{"d":1}},{"r":{"d":false}})"
	);
	CHECK(Raw(source, "width") == Value{6.5});
	CHECK(ChannelSamples(Evaluated(source, true)) == 6);
	const auto replay = Edited(source, PxcxInputValueEdit{"source", "width", 7.5});
	CHECK(Raw(replay, "width") == Value{7.5});
	CHECK(ChannelSamples(Evaluated(replay, true)) == 8);
	std::vector<std::byte> bytes;
	std::string failure;
	REQUIRE(engine::bake::WritePxcx(source.Source, bytes, failure));
	CHECK(bytes == source.Source.OriginalBytes);
}
TEST_CASE(
	"PXC raw Bool fractions preserve source threshold through edit replay", "[imagegraphio][pxcx_raw_getter]"
) {
	for (double raw : {-.25, .25, .5, .75}) {
		const auto source = Imported(
			"Node_Vector2", R"({"r":{"d":5.5}},{"r":{"d":0}},{"r":{"d":)" + std::to_string(raw) + "}}"
		);
		CHECK(Raw(source, "integer") == Value{raw});
		CHECK(std::get<double>(Evaluated(source, false)) == (raw > .5 ? 6. : 5.5));
		const auto replay = Edited(source, PxcxInputValueEdit{"source", "integer", .75});
		CHECK(Raw(replay, "integer") == Value{.75});
		CHECK(std::get<double>(Evaluated(replay, false)) == 6.);
	}
}
TEST_CASE(
	"PXC last Int and Bool key deletion durably retains raw disabled animator snapshots",
	"[imagegraphio][pxcx_raw_getter]"
) {
	const auto integer = Imported(
		"Node_Audio_Window",
		R"({"r":{"d":-4}},{"anim":true,"r":[[[0,0],5,[0,1],[0,0],0,0,true,{"typ":"linear","spd":0.5}]]},{"r":{"d":0}},{"r":{"d":0}},{"r":{"d":1}},{"r":{"d":false}})"
	);
	const auto captured = Edited(integer, PxcxKeyframeDeleteEdit{"source", "width", {}, FrameTime{3}});
	const auto disabledKey =
		std::find_if(captured.Graph.Keyframes.begin(), captured.Graph.Keyframes.end(), [](const auto &key) {
			return key.Port == "width";
		});
	REQUIRE(disabledKey != captured.Graph.Keyframes.end());
	CHECK(disabledKey->Tick == 0);
	CHECK(disabledKey->Kind == KeyframeKind::Normal);
	CHECK(disabledKey->Data == Value{6.5});
	CHECK_FALSE(disabledKey->SourceDriver);
	CHECK(Raw(captured, "width") == Value{6.5});
	CHECK(ChannelSamples(Evaluated(captured, true)) == 6);
	const auto integralCaptured =
		Edited(integer, PxcxKeyframeDeleteEdit{"source", "width", {}, FrameTime{2}});
	CHECK(Raw(integralCaptured, "width") == Value{6.0});
	CHECK(ChannelSamples(Evaluated(integralCaptured, true)) == 6);
	const auto boolean = Imported(
		"Node_Vector2",
		R"({"r":{"d":5.5}},{"r":{"d":0}},{"anim":true,"r":[[[0,0],false,[0,1],[0,0],0,0,true,{"typ":"linear","spd":0.25}]]})"
	);
	const auto booleanCaptured =
		Edited(boolean, PxcxKeyframeDeleteEdit{"source", "integer", {}, FrameTime{3}});
	const auto disabledBooleanKey = std::find_if(
		booleanCaptured.Graph.Keyframes.begin(), booleanCaptured.Graph.Keyframes.end(), [](const auto &key) {
			return key.Port == "integer";
		}
	);
	REQUIRE(disabledBooleanKey != booleanCaptured.Graph.Keyframes.end());
	CHECK(disabledBooleanKey->Tick == 0);
	CHECK(disabledBooleanKey->Kind == KeyframeKind::Normal);
	CHECK(disabledBooleanKey->Data == Value{.75});
	CHECK_FALSE(disabledBooleanKey->SourceDriver);
	CHECK(Raw(booleanCaptured, "integer") == Value{.75});
	CHECK(std::get<double>(Evaluated(booleanCaptured, false)) == 6.);
	const auto integralBooleanCaptured =
		Edited(boolean, PxcxKeyframeDeleteEdit{"source", "integer", {}, FrameTime{4}});
	CHECK(Raw(integralBooleanCaptured, "integer") == Value{1.0});
	CHECK(std::get<double>(Evaluated(integralBooleanCaptured, false)) == 6.);
	CHECK(booleanCaptured.Source.GraphJson.find(R"("future":"node")") != std::string::npos);
	CHECK(booleanCaptured.Source.GraphJson.find(R"("future":"project")") != std::string::npos);
}

TEST_CASE(
	"PXC integral raw source reals survive explicit edit replay without changing native representation",
	"[imagegraphio][pxcx_raw_integral]"
) {
	for (bool integer : {true, false}) {
		DYNAMIC_SECTION("integer getter " << integer) {
			const auto source =
				integer
					? Imported(
						  "Node_Audio_Window",
						  R"({"r":{"d":-4}},{"r":{"d":5}},{"r":{"d":0}},{"r":{"d":0}},{"r":{"d":1}},{"r":{"d":false}})"
					  )
					: Imported("Node_Vector2", R"({"r":{"d":5.5}},{"r":{"d":0}},{"r":{"d":false}})");
			const std::string port = integer ? "width" : "integer";
			const double raw = integer ? 6.0 : 1.0;
			CHECK(Raw(source, port) == (integer ? Value{int64_t{5}} : Value{false}));
			const std::array<PxcxEdit, 1> edits{PxcxInputValueEdit{"source", port, raw}};
			std::vector<std::byte> bytes{std::byte{0x37}};
			const auto sentinel = bytes;
			Diagnostic diagnostic;
			const bool written =
				WritePxcxEdits(source, source.Source.OriginalBytes, edits, bytes, diagnostic);
			INFO(port);
			INFO(diagnostic.Message);
			if (!written) CHECK(bytes == sentinel);
			REQUIRE(written);
			engine::bake::PxcxArchive checked;
			std::string failure;
			REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
			PxcxImport replay;
			REQUIRE(ImportPxcxImageGraph(checked, replay, failure));
			CHECK(Raw(replay, port) == Value{raw});
			if (integer)
				CHECK(ChannelSamples(Evaluated(replay, true)) == 6);
			else
				CHECK(std::get<double>(Evaluated(replay, false)) == 6.);
		}
	}
}

TEST_CASE(
	"PXC expanded raw Int and Bool real keys retain their authored numeric representation",
	"[imagegraphio][pxcx_raw_integral]"
) {
	for (bool integer : {true, false}) {
		DYNAMIC_SECTION("integer key " << integer) {
			const auto source =
				integer
					? Imported(
						  "Node_Audio_Window",
						  R"({"r":{"d":-4}},{"anim":true,"r":[[[0,0],5,[0,1],[0,0],0,0,true,{"typ":"linear","spd":0.5}]]},{"r":{"d":0}},{"r":{"d":0}},{"r":{"d":1}},{"r":{"d":false}})"
					  )
					: Imported(
						  "Node_Vector2",
						  R"({"r":{"d":5.5}},{"r":{"d":0}},{"anim":true,"r":[[[0,0],false,[0,1],[0,0],0,0,true,{"typ":"linear","spd":0.25}]]})"
					  );
			const std::string_view port = integer ? "width" : "integer";
			const auto sourceKeys = KeyframesFor(source.Graph, "source", port);
			REQUIRE(sourceKeys.size() == 1);
			auto key = sourceKeys.front();
			if (!integer) CHECK(HasCanonicalStaticInput(source.Graph, "source", "x"));
			const double raw = integer ? 6.0 : 1.0;
			key.Data = raw;
			const auto replay = Edited(source, PxcxKeyframeEdit{"source", key.Port, GetFrameTime(key), key});
			const auto replayKeys = KeyframesFor(replay.Graph, "source", port);
			REQUIRE(replayKeys.size() == 1);
			CHECK(replayKeys.front() == key);
			if (integer)
				CHECK(ChannelSamples(Evaluated(replay, true)) == 6);
			else
				CHECK(std::get<double>(Evaluated(replay, false)) == 6.);
		}
	}
}
