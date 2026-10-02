#include <engine/imagegraphio/PxcxEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>

TEST_SUITE_ID("engine.imagegraphio.pxcxkeystructure")
TEST_DEPENDS("engine.imagegraphio.pxcxedit")

using namespace engine::imagegraph;
using namespace engine::imagegraphio;

namespace {
	PxcxImport Imported(int loopRange = -1) {
		engine::bake::PxcxArchive source;
		source.MetadataNumber = 121092;
		source.MetadataText = "1.22.10.201";
		source.GraphJson =
			R"({"animator":{"frames_total":30,"playback":0,"framerate":30},"nodes":[{"id":"number","type":"Node_Number_Simple","x":1,"y":2,"inputs":[{"anim":true,"r":[[[0,2,"first-marker"],20,[0,1],[0,0],0,0,true,0,4294967295,"first-tail"],[[1,8,0,"last-marker"],80,[0,1],[0,0],0,0,false,{"typ":"linear","spd":0,"future":{"keep":9}},123,"last-tail"]],"future":{"keep":"input"}}]}],"future":{"keep":"project"}})";
		if (loopRange >= 0) {
			const auto start = source.GraphJson.find("\"anim\":true");
			source.GraphJson.insert(start, "\"loop_range\":" + std::to_string(loopRange) + ",");
		}
		source.GraphJson.push_back('\0');
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(source, bytes, failure));
		engine::bake::PxcxArchive checked;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		PxcxImport imported;
		REQUIRE(ImportPxcxImageGraph(checked, imported, failure));
		REQUIRE(imported.Graph.Keyframes.size() == 2);
		return imported;
	}
	Keyframe Key(double time, double value) {
		Keyframe key;
		key.NodeId = "number";
		key.Port = "value";
		key.Data = value;
		key.Interpolation = "source";
		key.Ease = KeyframeEase{};
		FrameTime frame;
		REQUIRE(SplitFrameTime(time, frame));
		REQUIRE(SetFrameTime(key, frame));
		return key;
	}
	PxcxImport Edited(const PxcxImport &source, std::span<const PxcxEdit> edits) {
		std::vector<std::byte> bytes;
		Diagnostic diagnostic;
		const bool written = WritePxcxEdits(source, source.Source.OriginalBytes, edits, bytes, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(written);
		engine::bake::PxcxArchive archive;
		std::string failure;
		REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
		PxcxImport result;
		REQUIRE(ImportPxcxImageGraph(archive, result, failure, source.Options));
		CHECK(result.Source.GraphJson.find(R"("last-tail")") != std::string::npos);
		CHECK(result.Source.GraphJson.find(R"("future":{"keep":9})") != std::string::npos);
		return result;
	}
	double Evaluated(const PxcxImport &imported, double frame) {
		Document graph = imported.Graph;
		graph.Outputs = {{"out", "number", "number"}};
		Diagnostic diagnostic;
		Plan plan;
		REQUIRE(Compile(graph, plan, diagnostic) == Status::Ok);
		FrameTime time;
		REQUIRE(SplitFrameTime(frame, time));
		EvaluationRequest request;
		REQUIRE(SetFrameTime(request, time));
		EvaluatedValue result;
		const auto status = EvaluateValue(graph, plan, "out", request, result, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		Document restored;
		REQUIRE(Read(Write(graph), restored, diagnostic) == Status::Ok);
		CHECK(restored == graph);
		Plan replay;
		REQUIRE(Compile(restored, replay, diagnostic) == Status::Ok);
		EvaluatedValue retained;
		REQUIRE(EvaluateValue(restored, replay, "out", request, retained, diagnostic) == Status::Ok);
		CHECK(retained.Data == result.Data);
		return std::get<double>(result.Data);
	}
}

TEST_CASE(
	"PXC key insertions preserve source order drivers kinds and all surviving record tails",
	"[imagegraphio][pxcx_key_structure]"
) {
	const auto original = Imported();
	for (const auto &[frame, value] :
		 std::array{std::pair{-2.0, -20.0}, std::pair{5.0, 50.0}, std::pair{12.0, 120.0}}) {
		auto key = Key(frame, value);
		key.Kind = KeyframeKind::Adder;
		key.SourceDriver = KeyframeLinearDriver{2};
		const std::array<PxcxEdit, 1> insert{PxcxKeyframeInsertEdit{"number", "value", key}};
		const auto edited = Edited(original, insert);
		REQUIRE(edited.Graph.Keyframes.size() == 3);
		CHECK(edited.Source.GraphJson.find("16777215]") != std::string::npos);
		CHECK(
			edited.Source.GraphJson.find(
				R"([[0,2,"first-marker"],20,[0,1],[0,0],0,0,true,0,4294967295,"first-tail"])"
			) != std::string::npos
		);
		CHECK(
			edited.Source.GraphJson.find(
				R"([[1,8,0,"last-marker"],80,[0,1],[0,0],0,0,false,{"typ":"linear","spd":0,"future":{"keep":9}},123,"last-tail"])"
			) != std::string::npos
		);
		CHECK(
			CompareFrameTime(
				GetFrameTime(edited.Graph.Keyframes[0]), GetFrameTime(edited.Graph.Keyframes[1])
			) < 0
		);
		CHECK(
			CompareFrameTime(
				GetFrameTime(edited.Graph.Keyframes[1]), GetFrameTime(edited.Graph.Keyframes[2])
			) < 0
		);
		CHECK(Evaluated(edited, frame) == (frame <= 0 ? value : value + frame * 2));
		const std::array<PxcxEdit, 1> remove{PxcxKeyframeDeleteEdit{"number", "value", GetFrameTime(key)}};
		const auto restored = Edited(edited, remove);
		CHECK(restored.Graph == original.Graph);
		CHECK(Evaluated(restored, 5) == 50);
		if (frame < 0) {
			CHECK(Evaluated(edited, -.5) == value);
			CHECK(Evaluated(edited, 0) == value);
			CHECK(Evaluated(edited, 1) == 12);
		}
	}
}

TEST_CASE(
	"PXC deleting to a lone key retains expanded metadata and its reachable driver",
	"[imagegraphio][pxcx_key_structure]"
) {
	const auto original = Imported();
	const std::array<PxcxEdit, 1> remove{PxcxKeyframeDeleteEdit{"number", "value", FrameTime{2}}};
	const auto lone = Edited(original, remove);
	REQUIRE(lone.Graph.Keyframes.size() == 1);
	CHECK(lone.Graph.Keyframes[0] == original.Graph.Keyframes[1]);
	CHECK(lone.Source.GraphJson.find("last-marker") != std::string::npos);
	CHECK(lone.Source.GraphJson.find("first-tail") == std::string::npos);
	CHECK(Evaluated(lone, 3) == 80);
	const std::array<PxcxEdit, 1> last{PxcxKeyframeDeleteEdit{"number", "value", FrameTime{8}}};
	const std::vector<std::byte> initial{std::byte{0x77}};
	auto bytes = initial;
	Diagnostic diagnostic;
	CHECK_FALSE(WritePxcxEdits(lone, lone.Source.OriginalBytes, last, bytes, diagnostic));
	CHECK(bytes == initial);
	CHECK(diagnostic.Message == "PXC last-key deletion requires transient source snapshot semantics");
}

TEST_CASE(
	"PXC key structure conflicts unsupported fractional maps and missing identities refuse atomically",
	"[imagegraphio][pxcx_key_structure]"
) {
	const auto original = Imported();
	const std::vector<std::byte> initial{std::byte{0x77}};
	Diagnostic diagnostic;
	for (const auto &edit : std::array<PxcxEdit, 4>{
			 PxcxKeyframeInsertEdit{"number", "value", Key(2, 9)},
			 PxcxKeyframeInsertEdit{"number", "value", Key(3.5, 9)},
			 PxcxKeyframeDeleteEdit{"number", "value", FrameTime{4}},
			 PxcxKeyframeDeleteEdit{"missing", "value", FrameTime{2}}
		 }) {
		auto bytes = initial;
		CHECK_FALSE(
			WritePxcxEdits(original, original.Source.OriginalBytes, std::span(&edit, 1), bytes, diagnostic)
		);
		CHECK(bytes == initial);
		CHECK(diagnostic.Code == Status::UnsupportedExecution);
	}
	const auto insertion = PxcxKeyframeInsertEdit{"number", "value", Key(5, 50)};
	const std::array<PxcxEdit, 2> conflict{
		insertion, PxcxKeyframeDeleteEdit{"number", "value", FrameTime{5}}
	};
	auto bytes = initial;
	CHECK_FALSE(WritePxcxEdits(original, original.Source.OriginalBytes, conflict, bytes, diagnostic));
	CHECK(bytes == initial);
	CHECK(diagnostic.Message == "PXC edits target the same key twice");
}

TEST_CASE(
	"PXC key structure work and retained loop range stay bounded atomically",
	"[imagegraphio][pxcx_key_structure]"
) {
	const std::vector<std::byte> sentinel{std::byte{0x77}};
	Diagnostic diagnostic;
	const auto ranged = Imported(1);
	const std::array<PxcxEdit, 1> deletion{PxcxKeyframeDeleteEdit{"number", "value", FrameTime{2}}};
	auto bytes = sentinel;
	CHECK_FALSE(WritePxcxEdits(ranged, ranged.Source.OriginalBytes, deletion, bytes, diagnostic));
	CHECK(bytes == sentinel);
	CHECK(diagnostic.Message == "PXC key structure would invalidate source loop range");
	const auto original = Imported();
	std::vector<PxcxEdit> insertions;
	for (int i = 0; i < 500; ++i)
		insertions.emplace_back(PxcxKeyframeInsertEdit{"number", "value", Key(20 + i, i)});
	bytes = sentinel;
	CHECK_FALSE(WritePxcxEdits(original, original.Source.OriginalBytes, insertions, bytes, diagnostic));
	CHECK(bytes == sentinel);
	CHECK(diagnostic.Message == "PXC key structure exceeds sequence work limit");
}

TEST_CASE(
	"PXC negative integer prefix inserts retain source nonpositive first-key hold",
	"[imagegraphio][pxcx_key_structure]"
) {
	const auto original = Imported();
	auto first = Key(-4, -40);
	auto latest = Key(-2, -20);
	latest.SourceDriver = KeyframeLinearDriver{2};
	const std::array<PxcxEdit, 2> insertions{
		PxcxKeyframeInsertEdit{"number", "value", latest}, PxcxKeyframeInsertEdit{"number", "value", first}
	};
	const auto edited = Edited(original, insertions);
	CHECK(Evaluated(edited, -1) == -40);
	CHECK(Evaluated(edited, 0) == -40);
	CHECK(Evaluated(edited, 1) == 12);
}
