#include <engine/imagegraph/SourceTimeline.hpp>
#include <engine/imagegraphio/SourceImageEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraphio.source_image_edit")
namespace {
	using namespace engine::imagegraph;
	using namespace engine::imagegraphio;
	Document ImageSource(std::string type = "pc.image_animated") {
		Document document;
		document.FormatVersion = 9;
		Node node;
		node.Id = "images";
		node.Type = std::move(type);
		document.Nodes.push_back(std::move(node));
		document.Timeline = TimelineSettings{};
		document.Timeline->Frames = 12;
		document.Timeline->Last = 11;
		document.Timeline->SourceBounds = SourceAuthoringFrameBounds{};
		document.Timeline->SourceBounds->End = {SourceFrameBoundPresence::Explicit, {12, 0, false}};
		return document;
	}
	GroupReplayState Bound(const Document &document, uint64_t revision = 7) {
		GroupReplayState previous, initialized, bound;
		Diagnostic diagnostic;
		REQUIRE(RebindGroupReplay(document, previous, revision, initialized, diagnostic) == Status::Ok);
		REQUIRE(BindGroupReplay(document, {}, initialized, revision, bound, diagnostic) == Status::Ok);
		return bound;
	}
	SourceImageFrameObservation Live(const Document &document) {
		SourceImageFrameObservation prepared;
		prepared.Controls.Authored = document.Nodes[0];
		prepared.Controls.Tick = 3;
		prepared.Controls.Subframe = .5;
		prepared.Controls.Inputs = {
			{"path", ArrayValue{ValueType::Text, {std::string("gone.png"), std::string("second.png")}}}
		};
		prepared.AuthoringRevision = 7;
		prepared.InputRevision = 11;
		prepared.Frames = {{1, 1, {12, 34, 56, 78}}, {2, 1, {200, 10, 40, 0, 255, 128, 64, 255}}};
		return prepared;
	}
	const Value &Property(const Document &document, std::string_view port) {
		for (const auto &property : document.Nodes[0].SourceProperties)
			if (property.Port == port) return property.Data;
		FAIL("source image property missing");
		return document.Nodes[0].SourceProperties.front().Data;
	}
	void Encode(SourceImageFrameObservation &prepared) {
		std::vector<engine::bake::SpriteCacheFrame> frames;
		for (const auto &image : prepared.Frames)
			frames.push_back({image.Width, image.Height, image.Pixels});
		std::string text, failure;
		REQUIRE(
			engine::bake::WriteSpriteCache(
				frames,
				engine::bake::SpriteCacheLayout::Rgba8TopDown,
				text,
				failure,
				8 * 1024 * 1024,
				prepared.Controls.Authored.Type == "pc.image" ? engine::bake::SpriteCacheShape::Sprite
															  : engine::bake::SpriteCacheShape::Array
			)
		);
		prepared.EncodedCache = std::move(text);
		prepared.CacheLayout = engine::bake::SpriteCacheLayout::Rgba8TopDown;
	}
}
TEST_CASE(
	"Match Length counts live images and retains source end beyond the new total",
	"[imagegraphio][image_cache]"
) {
	auto document = ImageSource();
	document.Nodes[0].SourceProperties = {
		{"cache_use", true}, {"cache_data", std::string("old three-frame cache")}
	};
	auto prepared = Live(document);
	auto replay = Bound(document);
	Document result;
	GroupReplayState resultReplay;
	Diagnostic diagnostic;
	bool changed = false;
	const auto previous = document;
	REQUIRE(
		ApplySourceImageEdit(
			document,
			prepared,
			replay,
			{SourceImageAction::MatchLength, 7, 8, 11},
			result,
			resultReplay,
			changed,
			diagnostic
		) == Status::Ok
	);
	REQUIRE(changed);
	REQUIRE(result.Timeline);
	CHECK(result.Timeline->Frames == 2);
	CHECK(result.Timeline->Last == 1);
	CHECK(SourceTimelineLastFrame(*result.Timeline) == 11.);
	CHECK(result.Timeline->SourceBounds == document.Timeline->SourceBounds);
	CHECK(result.Nodes == document.Nodes);
	CHECK(document == previous);
	CHECK(replay.AuthoringRevision() == 7);
	CHECK(resultReplay.AuthoringRevision() == 8);
	Document restored;
	REQUIRE(Read(Write(result), restored, diagnostic) == Status::Ok);
	CHECK(restored == result);
	prepared.Kind = SourceImageFrameKind::Cached;
	const auto accepted = result;
	CHECK(
		ApplySourceImageEdit(
			document,
			prepared,
			replay,
			{SourceImageAction::MatchLength, 7, 8, 11},
			result,
			resultReplay,
			changed,
			diagnostic
		) == Status::InvalidValue
	);
	CHECK(result == accepted);
	CHECK(resultReplay.AuthoringRevision() == 8);
	CHECK(changed);
}
TEST_CASE(
	"Cache admission and removal publish complete document and replay owners", "[imagegraphio][image_cache]"
) {
	auto document = ImageSource();
	auto prepared = Live(document);
	Encode(prepared);
	auto replay = Bound(document);
	Document result;
	GroupReplayState owner;
	Diagnostic diagnostic;
	bool changed = false;
	REQUIRE(
		ApplySourceImageEdit(
			document,
			prepared,
			replay,
			{SourceImageAction::Cache, 7, 8, 11},
			result,
			owner,
			changed,
			diagnostic
		) == Status::Ok
	);
	REQUIRE(changed);
	CHECK(std::get<bool>(Property(result, "cache_use")));
	CHECK(Property(result, "cache_data") == Value{*prepared.EncodedCache});
	CHECK(Property(result, "composer_sprite_cache_layout") == Value{std::string("rgba8-top-down")});
	CHECK(owner.AuthoringRevision() == 8);
	auto remove = prepared;
	remove.Controls.Authored = result.Nodes[0];
	remove.AuthoringRevision = 8;
	remove.Kind = SourceImageFrameKind::ControlsOnly;
	remove.Frames.clear();
	remove.EncodedCache.reset();
	remove.CacheLayout.reset();
	Document disabled;
	GroupReplayState disabledOwner;
	REQUIRE(
		ApplySourceImageEdit(
			result,
			remove,
			owner,
			{SourceImageAction::RemoveCache, 8, 9, 11},
			disabled,
			disabledOwner,
			changed,
			diagnostic
		) == Status::Ok
	);
	CHECK_FALSE(std::get<bool>(Property(disabled, "cache_use")));
	CHECK(Property(disabled, "cache_data") == Property(result, "cache_data"));
	CHECK(
		Property(disabled, "composer_sprite_cache_data_hash") ==
		Property(result, "composer_sprite_cache_data_hash")
	);
	CHECK(disabledOwner.AuthoringRevision() == 9);
	remove.Controls.Authored = disabled.Nodes[0];
	remove.AuthoringRevision = 9;
	const auto previous = result;
	REQUIRE(
		ApplySourceImageEdit(
			disabled,
			remove,
			disabledOwner,
			{SourceImageAction::RemoveCache, 9, 10, 11},
			result,
			owner,
			changed,
			diagnostic
		) == Status::Ok
	);
	CHECK_FALSE(changed);
	CHECK(result == previous);
	CHECK(owner.AuthoringRevision() == 8);
}
TEST_CASE(
	"Stale generation and aggregate capacity refusal preserve image action outputs",
	"[imagegraphio][image_cache]"
) {
	auto document = ImageSource();
	auto replay = Bound(document);
	auto prepared = Live(document);
	Encode(prepared);
	Document result = ImageSource("pc.image_sequence");
	const auto previous = result;
	GroupReplayState owner = Bound(result, 19);
	bool changed = true;
	Diagnostic diagnostic;
	for (const auto options :
		 {SourceImageEditOptions{SourceImageAction::Cache, 7, 8, 12},
		  SourceImageEditOptions{SourceImageAction::Cache, 6, 8, 11}}) {
		CHECK(
			ApplySourceImageEdit(document, prepared, replay, options, result, owner, changed, diagnostic) ==
			Status::InvalidValue
		);
		CHECK(result == previous);
		CHECK(owner.AuthoringRevision() == 19);
		CHECK(changed);
	}
	CHECK(
		ApplySourceImageEdit(
			document,
			prepared,
			replay,
			{SourceImageAction::Cache, 7, 8, 11},
			result,
			owner,
			changed,
			diagnostic,
			64
		) == Status::LimitExceeded
	);
	CHECK(result == previous);
	CHECK(owner.AuthoringRevision() == 19);
	CHECK(changed);
	prepared.Frames[0].Pixels.reserve(8 * 1024 * 1024);
	CHECK(
		ApplySourceImageEdit(
			document,
			prepared,
			replay,
			{SourceImageAction::Cache, 7, 8, 11},
			result,
			owner,
			changed,
			diagnostic,
			4 * 1024 * 1024
		) == Status::LimitExceeded
	);
	CHECK(result == previous);
	CHECK(owner.AuthoringRevision() == 19);
	prepared.Kind = SourceImageFrameKind::Cached;
	CHECK(
		ApplySourceImageEdit(
			document,
			prepared,
			replay,
			{SourceImageAction::Cache, 7, 8, 11},
			result,
			owner,
			changed,
			diagnostic
		) == Status::InvalidValue
	);
	CHECK(result == previous);
}
TEST_CASE(
	"An empty live animated source leaves Match Length and its outputs unchanged",
	"[imagegraphio][image_cache]"
) {
	auto document = ImageSource();
	auto prepared = Live(document);
	prepared.Frames.clear();
	auto replay = Bound(document);
	Document result = ImageSource("pc.image_sequence");
	const auto previous = result;
	GroupReplayState owner = Bound(result, 19);
	Diagnostic diagnostic;
	bool changed = true;
	REQUIRE(
		ApplySourceImageEdit(
			document,
			prepared,
			replay,
			{SourceImageAction::MatchLength, 7, 8, 11},
			result,
			owner,
			changed,
			diagnostic
		) == Status::Ok
	);
	CHECK_FALSE(changed);
	CHECK(result == previous);
	CHECK(owner.AuthoringRevision() == 19);
}
