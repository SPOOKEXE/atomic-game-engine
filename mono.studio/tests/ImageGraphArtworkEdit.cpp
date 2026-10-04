#include "ImageGraphArtworkEdit.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("studio.imagegraph_artwork_edit")
TEST_DEPENDS("engine.imagegraphio.source_artwork_edit")
namespace {
	using namespace engine::imagegraph;
	Document Source() {
		Document document;
		document.FormatVersion = 9;
		document.Timeline = TimelineSettings{12, 0, 11, "loop", 30};
		document.Nodes = {
			{"file", "pc.ora_file_read", {}, {20, 100}, {{"path", std::string("prepared.ora")}}}
		};
		return document;
	}
	GroupReplayState Bound(const Document &document) {
		GroupReplayState initial, prepared, bound;
		Diagnostic diagnostic;
		REQUIRE(RebindGroupReplay(document, initial, 1, prepared, diagnostic) == Status::Ok);
		REQUIRE(BindGroupReplay(document, {}, prepared, 1, bound, diagnostic) == Status::Ok);
		return bound;
	}
	HostNodeCapture Receipt(const Document &document) {
		HostNodeCapture capture;
		capture.Authored = document.Nodes[0];
		StructValue content, layer;
		content.Data.emplace();
		layer.Data.emplace();
		layer.Data->Fields = {{"name", std::string("Prepared Layer")}};
		ArrayValue layers;
		layers.ElementType = ValueType::Struct;
		layers.Elements.emplace_back(std::move(layer));
		content.Data->Fields.emplace_back("layerData", std::move(layers));
		capture.Outputs.push_back({"content", std::move(content)});
		return capture;
	}
}
TEST_CASE(
	"Prepared artwork publication records one history transition and publishes rebound owners",
	"[studio][artwork]"
) {
	using namespace engine::imagegraphio;
	Document document = Source();
	const auto original = document;
	auto replay = Bound(document);
	const auto receipt = Receipt(document);
	studio::ImageGraphHistory history;
	Diagnostic diagnostic;
	bool changed = false;
	REQUIRE(
		studio::detail::ApplyPreparedImageGraphArtwork(
			document,
			history,
			replay,
			receipt,
			{SourceArtworkAction::GenerateLayers, true, true, 1, 2, std::nullopt},
			diagnostic,
			changed
		)
	);
	REQUIRE(changed);
	CHECK(document.Nodes.size() == 2);
	CHECK(replay.AuthoringRevision() == 2);
	CHECK(history.CanUndo());
	const auto generated = document;
	REQUIRE(history.Undo(document));
	CHECK(document == original);
	CHECK_FALSE(history.CanUndo());
	REQUIRE(history.Redo(document));
	CHECK(document == generated);
	CHECK_FALSE(history.CanRedo());
	CHECK(receipt.Authored == original.Nodes[0]);
}
TEST_CASE(
	"Refused artwork history and stale receipts preserve document owner and existing history",
	"[studio][artwork]"
) {
	using namespace engine::imagegraphio;
	Document document = Source();
	const auto original = document;
	auto replay = Bound(document);
	const auto ownerBytes = replay.RetainedBytes();
	const auto receipt = Receipt(document);
	studio::ImageGraphHistory tinyHistory(4, 1);
	Diagnostic diagnostic;
	bool changed = true;
	CHECK_FALSE(
		studio::detail::ApplyPreparedImageGraphArtwork(
			document,
			tinyHistory,
			replay,
			receipt,
			{SourceArtworkAction::GenerateLayers, true, true, 1, 2, std::nullopt},
			diagnostic,
			changed
		)
	);
	CHECK_FALSE(changed);
	CHECK(document == original);
	CHECK(replay.AuthoringRevision() == 1);
	CHECK(replay.RetainedBytes() == ownerBytes);
	CHECK_FALSE(tinyHistory.CanUndo());
	auto stale = receipt;
	stale.Authored.Values[0].Data = std::string("changed.ora");
	studio::ImageGraphHistory history;
	CHECK_FALSE(
		studio::detail::ApplyPreparedImageGraphArtwork(
			document,
			history,
			replay,
			stale,
			{SourceArtworkAction::GenerateLayers, true, true, 1, 2, std::nullopt},
			diagnostic,
			changed
		)
	);
	CHECK(document == original);
	CHECK(replay.AuthoringRevision() == 1);
	CHECK_FALSE(history.CanUndo());
}
