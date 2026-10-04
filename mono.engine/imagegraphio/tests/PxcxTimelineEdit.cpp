#include <engine/imagegraph/SourceTimeline.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

TEST_SUITE_ID("engine.imagegraphio.pxcx_timeline_edit")
TEST_DEPENDS("engine.imagegraphio.pxcxedit")

namespace {
	using namespace engine::imagegraph;
	using namespace engine::imagegraphio;
	using Json = nlohmann::ordered_json;
	PxcxImport TimelineSource(bool explicitBounds) {
		Json animator{
			{"frames_total", 12},
			{"playback", 0},
			{"framerate", 30},
			{"future_animator", {{"opaque", "keep"}}},
			{"frame_range_start", nullptr},
			{"frame_range_end", nullptr}
		};
		if (explicitBounds) {
			animator["frame_range_start"] = 3;
			animator["frame_range_end"] = 8;
		}
		engine::bake::PxcxArchive source;
		source.MetadataNumber = 121092;
		source.MetadataText = "1.22.10.201";
		source.GraphJson =
			Json{{"animator", animator}, {"nodes", Json::array()}, {"future_project", "keep"}}.dump() + '\0';
		std::string failure;
		std::vector<std::byte> bytes;
		REQUIRE(engine::bake::WritePxcx(source, bytes, failure));
		engine::bake::PxcxArchive checked;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		PxcxImport imported;
		REQUIRE(ImportPxcxImageGraph(checked, imported, failure));
		REQUIRE(imported.Graph.Timeline);
		return imported;
	}
	PxcxImport Reimport(std::span<const std::byte> bytes) {
		engine::bake::PxcxArchive checked;
		std::string failure;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		PxcxImport imported;
		REQUIRE(ImportPxcxImageGraph(checked, imported, failure));
		return imported;
	}
}
TEST_CASE(
	"PXC frame-count edit retains null bounds and opaque saved animator metadata",
	"[imagegraphio][artwork][timeline_edit]"
) {
	const auto original = TimelineSource(false);
	auto candidate = original.Graph;
	candidate.Timeline->Frames = 4;
	candidate.Timeline->Last = 3;
	Diagnostic diagnostic;
	std::vector<std::byte> bytes;
	const bool saved = WritePxcxProjection(original, candidate, {}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(saved);
	const auto imported = Reimport(bytes);
	CHECK(imported.Graph.Timeline == candidate.Timeline);
	const auto json = Json::parse(imported.Source.GraphJson.c_str());
	CHECK(json["animator"]["frames_total"] == 4);
	CHECK(json["animator"]["frame_range_start"].is_null());
	CHECK(json["animator"]["frame_range_end"].is_null());
	CHECK(json["animator"]["future_animator"]["opaque"] == "keep");
	CHECK(json["future_project"] == "keep");
	std::vector<std::byte> noOp;
	REQUIRE(WritePxcxProjection(imported, imported.Graph, {}, noOp, diagnostic));
	CHECK(noOp == bytes);
	CHECK(original.Graph.Timeline->Frames == 12);
}
TEST_CASE(
	"PXC explicit authoring range rate and playback edits reimport exactly",
	"[imagegraphio][artwork][timeline_edit]"
) {
	const auto original = TimelineSource(true);
	auto candidate = original.Graph;
	candidate.Timeline = TimelineSettings{20, 4, 12, "pingpong", 23.5};
	candidate.Timeline->SourceBounds = SourceAuthoringFrameBounds{
		{SourceFrameBoundPresence::Explicit, {5, 0, false}},
		{SourceFrameBoundPresence::Explicit, {13, 0, false}}
	};
	Diagnostic diagnostic;
	std::vector<std::byte> bytes;
	REQUIRE(WritePxcxProjection(original, candidate, {}, bytes, diagnostic));
	CHECK(Reimport(bytes).Graph.Timeline == candidate.Timeline);
	const auto json = Json::parse(Reimport(bytes).Source.GraphJson.c_str());
	CHECK(json["animator"]["frame_range_start"] == 5);
	CHECK(json["animator"]["frame_range_end"] == 13);
	CHECK(json["animator"]["playback"] == 2);
	CHECK(json["animator"]["framerate"] == 23.5);
}
TEST_CASE(
	"PXC inconsistent projection and unknown playback preserve prior bytes",
	"[imagegraphio][artwork][timeline_edit]"
) {
	const auto original = TimelineSource(true);
	Diagnostic diagnostic;
	std::vector<std::byte> bytes{std::byte{71}, std::byte{72}};
	const auto prior = bytes;
	for (const auto &timeline :
		 {TimelineSettings{2, 2, 1, "loop", 30},
		  TimelineSettings{12, 3, 3, "loop", 30},
		  TimelineSettings{12, 0, 11, "unknown", 30}}) {
		auto candidate = original.Graph;
		candidate.Timeline = timeline;
		CHECK_FALSE(WritePxcxProjection(original, candidate, {}, bytes, diagnostic));
		CHECK(bytes == prior);
		CHECK(original.Graph.Timeline->First == 2);
	}
}

TEST_CASE(
	"PXC reduced total retains explicit end beyond total and explicit normalization is a separate edit",
	"[imagegraphio][artwork][timeline_edit]"
) {
	auto source = TimelineSource(true);
	auto candidate = source.Graph;
	candidate.Timeline->SourceBounds->Start = {SourceFrameBoundPresence::Null, {}};
	candidate.Timeline->SourceBounds->End = {SourceFrameBoundPresence::Explicit, {12, 0, false}};
	candidate.Timeline->Frames = 2;
	Diagnostic diagnostic;
	REQUIRE(ProjectSourceTimelineWindow(*candidate.Timeline, diagnostic) == Status::Ok);
	std::vector<std::byte> bytes;
	REQUIRE(WritePxcxProjection(source, candidate, {}, bytes, diagnostic));
	const auto reloaded = Reimport(bytes);
	REQUIRE(reloaded.Graph.Timeline == candidate.Timeline);
	const auto json = Json::parse(reloaded.Source.GraphJson.c_str());
	CHECK(json["animator"]["frames_total"] == 2);
	CHECK(json["animator"]["frame_range_end"] == 12);
	CHECK(SourceTimelineLastFrame(*reloaded.Graph.Timeline) == 11.);
	auto normalized = reloaded.Graph;
	REQUIRE(NormalizeSourceTimelineBounds(*normalized.Timeline, diagnostic) == Status::Ok);
	REQUIRE(WritePxcxProjection(reloaded, normalized, {}, bytes, diagnostic));
	const auto stepped = Reimport(bytes);
	CHECK(stepped.Graph.Timeline == normalized.Timeline);
	CHECK(Json::parse(stepped.Source.GraphJson.c_str())["animator"]["frame_range_end"] == 2);
	CHECK(reloaded.Graph.Timeline->SourceBounds->End.Value.Tick == 12);
}
