#include "../src/ImageGraphDocumentEdit.hpp"
#include "../src/TimelineDopesheet.hpp"

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
