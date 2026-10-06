#include "../src/ImageGraphDocumentEdit.hpp"
#include "../src/KeyframeKindEditor.hpp"
#include "../src/TimelineDopesheet.hpp"
#include "../src/TimelineEaseEditor.hpp"

#include <engine/imagegraph/GroupReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("studio.timeline_source_aliases")
TEST_DEPENDS("studio.imagegraph")
namespace {
	using namespace engine::imagegraph;
	Document RetiredWriter() {
		Document document;
		document.FormatVersion = 10;
		document.Nodes = {
			{"owner", "pc.invert", {}, {}, {{"mix", .9}}}, {"alias", "pc.invert", {}, {}, {{"mix", .25}}}
		};
		document.Nodes[1].InstanceBase = "owner";
		for (auto &node : document.Nodes)
			node.SourceAnimatedInputs = {"mix"};
		Keyframe physical{"owner", "native:animator:0", 1, .25, "source", KeyframeEase{}};
		physical.SourceKeyId = "retained-key";
		physical.SourceDriver = KeyframeLinearDriver{.125};
		Keyframe alias = physical;
		alias.NodeId = "alias";
		alias.Port = "mix";
		alias.SourceKeyId.clear();
		document.Keyframes = {{"owner", "mix", 0, .9, "source", KeyframeEase{}}, alias};
		document.Tracks = {{"owner", "mix", "hold", -1}, {"alias", "mix", "hold", -1}};
		document.Outputs = {{"result", "owner", "surface_out"}};
		document.SourceAnimators.emplace();
		GroupSubtypeBinding binding{
			"alias", "owner", GroupSubtypeAnimator::Animated, GroupSubtypeAnimator::Animated, "mix"
		};
		binding.AnimatorPort = physical.Port;
		document.SourceAnimators->Bindings = {binding};
		DetachedSourceAnimator metadata;
		metadata.Id = physical.Port;
		metadata.OwnerId = "owner";
		metadata.OriginalPort = "mix";
		metadata.Writer = GroupSubtypeAnimator::Animated;
		metadata.Type = ValueType::Scalar;
		metadata.Track = AnimationTrack{"owner", physical.Port, "hold", -1};
		document.SourceAnimators->Detached = {metadata};
		document.SourceAnimators->DetachedValues = {{"owner", std::nullopt, {physical}, physical.Port}};
		return document;
	}
}
TEST_CASE(
	"Timeline aliases move, copy and undo the retained writer together", "[studio][timeline][source_aliases]"
) {
	auto document = RetiredWriter();
	const auto before = document;
	studio::ImageGraphHistory history;
	Diagnostic error;
	const std::vector<Keyframe> pinned{document.Keyframes[1]};
	const std::array<FrameTime, 1> moved{{{3, .25, true}}};
	REQUIRE(studio::ApplyImageGraphDocumentEdit(document, history, [&](Document &candidate) {
		return studio::RetimeImageGraphKeyframes(
			candidate, pinned, moved, false, error, Limits::MaximumEvaluationBytes, false
		);
	}));
	CHECK(GetFrameTime(document.SourceAnimators->DetachedValues[0].Keys[0]) == moved[0]);
	CHECK(document.SourceAnimators->DetachedValues[0].Keys[0].SourceKeyId == "retained-key");
	CHECK(document.Nodes == before.Nodes);
	CHECK(document.SourceAnimators->Bindings == before.SourceAnimators->Bindings);
	CHECK(document.SourceAnimators->Detached == before.SourceAnimators->Detached);
	CHECK(std::any_of(document.Keyframes.begin(), document.Keyframes.end(), [](const auto &key) {
		return key.NodeId == "owner" && std::get<double>(key.Data) == .9;
	}));
	const auto movedDocument = document;
	REQUIRE(history.Undo(document));
	CHECK(document == before);
	REQUIRE(history.Redo(document));
	CHECK(document == movedDocument);
	GroupReplayState restored;
	REQUIRE(RestoreSourceAnimatorBindings(document, {}, 2, restored, error) == Status::Ok);
	CHECK(restored.Bindings()[0].AnimatorPort == "native:animator:0");
	std::vector<Keyframe> copyPinned;
	for (const auto &key : document.Keyframes)
		if (key.NodeId == "alias") copyPinned.push_back(key);
	const std::array<FrameTime, 1> copied{{{7, .5, false}}};
	REQUIRE(studio::RetimeImageGraphKeyframes(document, copyPinned, copied, true, error));
	REQUIRE(document.SourceAnimators->DetachedValues[0].Keys.size() == 2);
	CHECK_FALSE(document.SourceAnimators->DetachedValues[0].Keys.back().SourceDriver);
	CHECK(document.SourceAnimators->DetachedValues[0].Keys.back().SourceKeyId.empty());
}
TEST_CASE(
	"Dopesheet deletion clears canonical aliases through one history transaction",
	"[studio][timeline][source_aliases]"
) {
	auto document = RetiredWriter();
	const auto before = document;
	studio::ImageGraphHistory history;
	studio::TimelineKeyEditor editor;
	studio::TimelineDopesheet sheet;
	Diagnostic error;
	editor.Selection = {studio::TimelineKeyEditor::Identity(document.Keyframes[1])};
	REQUIRE(sheet.BeginDeletion(document, editor, error));
	REQUIRE(studio::ApplyImageGraphDocumentEdit(document, history, [&](Document &candidate) {
		return sheet.PrepareCommit(candidate, editor, error);
	}));
	sheet.PublishCommit(editor);
	CHECK(editor.Selection.empty());
	CHECK(document.SourceAnimators->DetachedValues[0].Keys.empty());
	CHECK(document.Keyframes.size() == 1);
	CHECK(document.SourceAnimators->Detached == before.SourceAnimators->Detached);
	REQUIRE(history.Undo(document));
	CHECK(document == before);
}
namespace {
	Document MetadataWriters(bool retired = true) {
		auto document = RetiredWriter();
		auto second = document.SourceAnimators->DetachedValues[0].Keys[0];
		second.Tick = 5;
		second.Data = .75;
		second.SourceKeyId = "second-retained-key";
		second.SourceDriver.reset();
		document.SourceAnimators->DetachedValues[0].Keys.push_back(second);
		second.NodeId = "alias";
		second.Port = "mix";
		second.SourceKeyId.clear();
		document.Keyframes.push_back(second);
		Node sibling = document.Nodes[1];
		sibling.Id = "sibling";
		document.Nodes.push_back(sibling);
		auto binding = document.SourceAnimators->Bindings[0];
		binding.NodeId = sibling.Id;
		document.SourceAnimators->Bindings.push_back(binding);
		for (size_t index : {size_t{1}, size_t{2}}) {
			auto key = document.Keyframes[index];
			key.NodeId = "sibling";
			document.Keyframes.push_back(key);
		}
		document.Tracks.push_back({"sibling", "mix", "hold", -1});
		if (!retired) {
			document.Keyframes.erase(document.Keyframes.begin());
			for (auto key : document.SourceAnimators->DetachedValues[0].Keys) {
				key.Port = "mix";
				document.Keyframes.push_back(key);
			}
			for (auto &item : document.SourceAnimators->Bindings)
				item.AnimatorPort.clear();
			document.SourceAnimators->Detached.clear();
			document.SourceAnimators->DetachedValues.clear();
		}
		return document;
	}
	size_t MetadataIndex(const Document &document, std::string_view node = "alias", uint64_t tick = 1) {
		const auto found =
			std::find_if(document.Keyframes.begin(), document.Keyframes.end(), [&](const auto &key) {
				return key.NodeId == node && key.Tick == tick;
			});
		REQUIRE(found != document.Keyframes.end());
		return size_t(found - document.Keyframes.begin());
	}
	const Keyframe &MetadataCanonical(const Document &document, bool retired = true, uint64_t tick = 1) {
		if (!retired) return document.Keyframes[MetadataIndex(document, "owner", tick)];
		const auto &keys = document.SourceAnimators->DetachedValues[0].Keys;
		const auto found =
			std::find_if(keys.begin(), keys.end(), [&](const auto &key) { return key.Tick == tick; });
		REQUIRE(found != keys.end());
		return *found;
	}
	void MetadataValid(const Document &document) {
		Document restored;
		Diagnostic error;
		REQUIRE(Read(Write(document), restored, error) == Status::Ok);
		CHECK(restored == document);
		GroupReplayState replay;
		REQUIRE(RestoreSourceAnimatorBindings(restored, {}, 1, replay, error) == Status::Ok);
	}
}
TEST_CASE(
	"Key metadata controls edit captured physical and retired writers", "[studio][timeline][source_aliases]"
) {
	for (bool retired : {false, true}) {
		auto document = MetadataWriters(retired);
		const auto before = document;
		Diagnostic error;
		studio::KeyframeKindEditor kind;
		REQUIRE(kind.Begin(document, MetadataIndex(document)));
		REQUIRE(kind.Select(KeyframeKind::Adder));
		REQUIRE(kind.Commit(document, error));
		CHECK(MetadataCanonical(document, retired).Kind == KeyframeKind::Adder);
		CHECK(document.Keyframes[MetadataIndex(document, "sibling")].Kind == KeyframeKind::Adder);
		const KeyframeEase ease{"bezier", "cut", {.4, -.7}, {.6, 1.2}};
		REQUIRE(studio::SetImageGraphKeyframeEase(document, MetadataIndex(document), ease, error));
		CHECK(MetadataCanonical(document, retired).Ease == ease);
		CHECK(document.Keyframes[MetadataIndex(document, "sibling")].Ease == ease);
		REQUIRE(
			studio::SetImageGraphKeyframeSourceDriver(
				document, MetadataIndex(document), KeyframeSnapDriver{.25}, error
			)
		);
		CHECK(
			MetadataCanonical(document, retired).SourceDriver == KeyframeSourceDriver{KeyframeSnapDriver{.25}}
		);
		CHECK(
			document.Keyframes[MetadataIndex(document, "sibling")].SourceDriver ==
			MetadataCanonical(document, retired).SourceDriver
		);
		const auto driven = document;
		CHECK_FALSE(
			studio::SetImageGraphKeyframeInterpolation(document, MetadataIndex(document), "linear", error)
		);
		CHECK(document == driven);
		REQUIRE(
			studio::SetImageGraphKeyframeSourceDriver(document, MetadataIndex(document), std::nullopt, error)
		);
		REQUIRE(
			studio::SetImageGraphKeyframeInterpolation(document, MetadataIndex(document), "linear", error)
		);
		for (uint64_t tick : {1, 5}) {
			CHECK(MetadataCanonical(document, retired, tick).Interpolation == "linear");
			CHECK_FALSE(MetadataCanonical(document, retired, tick).Ease);
		}
		REQUIRE(
			studio::SetImageGraphKeyframeEase(document, MetadataIndex(document, "alias", 5), ease, error)
		);
		CHECK(MetadataCanonical(document, retired).Ease == KeyframeEase{});
		CHECK(MetadataCanonical(document, retired, 5).Ease == ease);
		REQUIRE(
			studio::SetImageGraphKeyframeSineDriver(
				document, MetadataIndex(document), KeyframeSineDriver{}, error
			)
		);
		CHECK(MetadataCanonical(document, retired).SineDriver == KeyframeSineDriver{});
		REQUIRE(
			studio::SetImageGraphKeyframeSineDriver(document, MetadataIndex(document), std::nullopt, error)
		);
		CHECK_FALSE(MetadataCanonical(document, retired).SineDriver);
		CHECK(MetadataCanonical(document, retired).SourceKeyId == "retained-key");
		CHECK(document.Nodes == before.Nodes);
		CHECK(document.SourceAnimators->Bindings == before.SourceAnimators->Bindings);
		CHECK(document.SourceAnimators->Detached == before.SourceAnimators->Detached);
		if (retired) CHECK(document.Keyframes[MetadataIndex(document, "owner", 0)] == before.Keyframes[0]);
		MetadataValid(document);
	}
}
TEST_CASE(
	"Shared easing width gestures deduplicate aliases and reject stale pins",
	"[studio][timeline][source_aliases]"
) {
	auto document = MetadataWriters();
	const auto before = document;
	studio::TimelineKeyEditor keys;
	keys.Selection = {
		studio::TimelineKeyEditor::Identity(document.Keyframes[MetadataIndex(document)]),
		studio::TimelineKeyEditor::Identity(document.Keyframes[MetadataIndex(document, "sibling")])
	};
	studio::TimelineEaseEditor ease;
	studio::ImageGraphHistory history;
	Diagnostic error;
	REQUIRE(ease.Begin(document, keys, error));
	ease.Delta = .5;
	REQUIRE(studio::ApplyImageGraphDocumentEdit(document, history, [&](Document &candidate) {
		return ease.PrepareCommit(candidate, error);
	}));
	CHECK(MetadataCanonical(document).Ease->In.X == .5);
	CHECK(MetadataCanonical(document).Ease->Out.X == .5);
	CHECK(
		MetadataCanonical(document).SourceDriver ==
		before.SourceAnimators->DetachedValues[0].Keys[0].SourceDriver
	);
	CHECK(MetadataCanonical(document).SourceKeyId == "retained-key");
	CHECK(document.Keyframes[MetadataIndex(document, "sibling")].Ease == MetadataCanonical(document).Ease);
	const auto accepted = document;
	REQUIRE(history.Undo(document));
	CHECK(document == before);
	REQUIRE(history.Redo(document));
	CHECK(document == accepted);
	ease.Cancel();
	REQUIRE(ease.Begin(document, keys, error));
	REQUIRE(
		studio::SetImageGraphKeyframeSourceDriver(
			document, MetadataIndex(document), KeyframeLinearDriver{.5}, error
		)
	);
	const auto changed = document;
	ease.Delta = .25;
	CHECK_FALSE(studio::ApplyImageGraphDocumentEdit(document, history, [&](Document &candidate) {
		return ease.PrepareCommit(candidate, error);
	}));
	CHECK(document == changed);
	CHECK(error.Code == Status::InvalidValue);
	MetadataValid(document);
}
TEST_CASE(
	"Captured key drafts admit borrowed owner overlap before cloning", "[studio][timeline][source_aliases]"
) {
	auto document = RetiredWriter();
	const auto before = document;
	Diagnostic error;
	const auto select = [](const auto &, size_t) { return true; };
	const auto change = [](auto &key, size_t) { key.Kind = KeyframeKind::Adder; };
	CHECK_FALSE(
		studio::EditCapturedImageGraphKeys(
			document, document.Keyframes, select, change, error, 0, Limits::MaximumEvaluationBytes
		)
	);
	CHECK(document == before);
	CHECK(error.Code == Status::LimitExceeded);
	CHECK_FALSE(
		studio::EditCapturedImageGraphKeys(
			document, document.Keyframes, select, change, error, Limits::MaximumEvaluationBytes
		)
	);
	CHECK(document == before);
}
