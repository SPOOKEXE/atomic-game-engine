#include "TimelineEaseEditor.hpp"

#include "ImageGraphDocumentEdit.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>

TEST_SUITE_ID("studio.timeline_ease_editor")
TEST_DEPENDS("studio.imagegraph")
namespace {
	using namespace engine::imagegraph;
	struct Timeline {
		ImGuiContext *PreviousContext = ImGui::GetCurrentContext();
		ImGuiContext *Context = ImGui::CreateContext();
		Document Doc;
		studio::ImageGraphHistory History;
		studio::TimelineKeyEditor Editor;
		studio::TimelineEaseEditor Ease;
		Diagnostic Error;
		FrameTime Cursor{5, .25, false};
		std::vector<ImVec2> Rows;
		std::vector<ImGuiID> RowIds;
		unsigned Changes = 0;
		uint64_t Revision = 0;
		bool Refuse = false;
		Timeline() {
			ImGui::SetCurrentContext(Context);
			auto &io = ImGui::GetIO();
			io.DisplaySize = {800, 600};
			io.DeltaTime = 1.f / 60;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
			Doc.FormatVersion = 9;
			Doc.Nodes = {
				{"number", "value.number", "", {}, {{"value", 0.0}}},
				{"other", "value.number", "", {}, {{"value", 0.0}}}
			};
			Doc.Outputs = {{"out", "number", "number"}};
			Doc.Keyframes = {
				{"number", "value", 1, 1.0, "linear"},
				{"number", "value", 3, 3.0, "linear"},
				{"number", "value", 7, 7.0, "linear"},
				{"other", "value", 3, 8.0, "linear"}
			};
			for (size_t i = 0; i < Doc.Keyframes.size(); ++i) {
				auto &key = Doc.Keyframes[i];
				key.Interpolation = "source";
				key.Ease = KeyframeEase{"bezier", "cut", {.25, -.7}, {.5, 1.2}};
				key.SourceKeyId = "pxc:key:" + std::to_string(i);
			}
			Doc.Tracks = {{"number", "value", "hold", -1}, {"other", "value", "hold", -1}};
			Doc.Keyframes[3].Subframe = .25;
			Doc.Keyframes[0].SourceDriver = KeyframeLinearDriver{2};
		}
		~Timeline() {
			ImGui::SetCurrentContext(Context);
			ImGui::DestroyContext(Context);
			ImGui::SetCurrentContext(PreviousContext);
		}
		void Frame() {
			ImGui::SetCurrentContext(Context);
			ImGui::NewFrame();
			ImGui::SetNextWindowPos({20, 20});
			ImGui::SetNextWindowSize({700, 500});
			ImGui::Begin("Timeline", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
			Ease.Draw(Doc, Revision, Editor, Error, [&] {
				bool unchanged = false;
				const bool accepted = studio::ApplyImageGraphDocumentEdit(
					Doc,
					History,
					[&](Document &doc) { return Ease.PrepareCommit(doc, Error) && !Refuse; },
					&unchanged
				);
				if (accepted) {
					++Changes;
					++Revision;
				}
				return accepted || unchanged;
			});
			Rows.clear();
			RowIds.clear();
			for (const auto &key : Doc.Keyframes) {
				(void)Editor.DrawRow(key);
				const auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
				Rows.push_back({(a.x + b.x) * .5f, (a.y + b.y) * .5f});
				RowIds.push_back(Context->LastItemData.ID);
			}
			ImGui::End();
			ImGui::Render();
		}
		void Click(size_t row, bool shift = false) {
			auto &io = ImGui::GetIO();
			io.AddKeyEvent(ImGuiMod_Shift, shift);
			io.AddMousePosEvent(Rows.at(row).x, Rows.at(row).y);
			Frame();
			io.AddMouseButtonEvent(0, true);
			Frame();
			io.AddMouseButtonEvent(0, false);
			Frame();
			io.AddKeyEvent(ImGuiMod_Shift, false);
			Frame();
		}
		void Key(ImGuiKey key) {
			auto &io = ImGui::GetIO();
			io.AddKeyEvent(key, true);
			Frame();
			io.AddKeyEvent(key, false);
			Frame();
		}
		bool Focus(const char *label) {
			for (unsigned step = 0; step < 40; ++step) {
				auto *window = Context->OpenPopupStack.empty() ? ImGui::FindWindowByName("Timeline")
															   : Context->OpenPopupStack.back().Window;
				if (window && Context->NavWindow == window && Context->NavId == window->GetID(label))
					return true;
				Key(ImGuiKey_Tab);
			}
			return false;
		}
		void Button(const char *label) {
			REQUIRE(Focus(label));
			Key(ImGuiKey_Space);
			Frame();
		}
		void Whole(const char *value) {
			REQUIRE(Focus("Width delta"));
			Key(ImGuiKey_Enter);
			auto &io = ImGui::GetIO();
			io.AddKeyEvent(ImGuiMod_Ctrl, true);
			Key(ImGuiKey_A);
			io.AddKeyEvent(ImGuiMod_Ctrl, false);
			Frame();
			io.AddInputCharactersUTF8(value);
			Frame();
			Key(ImGuiKey_Enter);
		}
	};
	Keyframe EaseKey(std::string node, uint64_t tick, Value data, std::string id) {
		Keyframe key{std::move(node), "center", tick, std::move(data), "source", KeyframeEase{}};
		key.Ease = KeyframeEase{"bezier", "cut", {.25, -.7}, {.55, 1.2}};
		key.SourceKeyId = std::move(id);
		key.SourceDriver = KeyframeLinearDriver{.125};
		key.Kind = KeyframeKind::Adder;
		return key;
	}
	Document MixedSourceDocument() {
		Document document;
		document.FormatVersion = 10;
		Node owner{"owner", "pc.mirror_polar", {}, {}, {{"center", Vector2{.2, .3}}}};
		owner.SourceAnimatedInputs = {"center"};
		SourceSeparatedVec2Animator axes;
		axes.Port = "center";
		axes.Axes[0].Keys = {EaseKey("owner", 0, .25, "x0"), EaseKey("owner", 4, .75, "x4")};
		axes.Axes[1].Keys = {EaseKey("owner", 0, 2.0, "y0"), EaseKey("owner", 4, 4.0, "y4")};
		owner.SourceSeparatedVec2Animators.emplace().Inputs.push_back(std::move(axes));
		Node alias = owner;
		alias.Id = "alias";
		alias.InstanceBase = "owner";
		alias.SourceSeparatedVec2Animators = {};
		document.Nodes = {
			{"source",
			 "image.solid",
			 {},
			 {},
			 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{}}}},
			{"alias_source",
			 "image.solid",
			 {},
			 {},
			 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{}}}},
			std::move(owner),
			std::move(alias)
		};
		document.Links = {
			{"source", "image", "owner", "surface_in"}, {"alias_source", "image", "alias", "surface_in"}
		};
		document.Keyframes = {
			EaseKey("owner", 0, Vector2{.2, .3}, "combined-owner-0"),
			EaseKey("owner", 4, Vector2{.7, .8}, "combined-owner-4"),
			EaseKey("alias", 0, Vector2{.2, .3}, {}),
			EaseKey("alias", 4, Vector2{.7, .8}, {})
		};
		document.Tracks = {{"owner", "center", "hold", -1}, {"alias", "center", "hold", -1}};
		document.SourceAnimators.emplace();
		GroupSubtypeBinding binding{
			"alias", "owner", GroupSubtypeAnimator::Animated, GroupSubtypeAnimator::Animated, "center"
		};
		binding.Axes = {GroupAxisStorage::Shared, "owner", "center", "owner", GroupSubtypeAnimator::Animated};
		document.SourceAnimators->Bindings.push_back(std::move(binding));
		document.Outputs = {{"out", "owner", "surface_out"}};
		return document;
	}
}
TEST_CASE(
	"Selected source key easing popup publishes one full-identity undo transaction", "[studio][timeline_ease]"
) {
	Timeline ui;
	ui.Frame();
	ui.Frame();
	ui.Click(0);
	ui.Click(3, true);
	const auto selection = ui.Editor.Selection;
	const auto before = ui.Doc;
	ui.Button("Ease keys");
	REQUIRE(ui.Ease.Active);
	ui.Whole("0.75");
	CHECK(ui.Doc == before);
	ui.Button("Apply");
	REQUIRE(ui.Changes == 1);
	CHECK_FALSE(ui.Ease.Active);
	CHECK(ui.Editor.Selection == selection);
	CHECK(ui.Doc.Keyframes[1] == before.Keyframes[1]);
	CHECK(ui.Doc.Keyframes[2] == before.Keyframes[2]);
	for (size_t index : {0u, 3u}) {
		auto expected = before.Keyframes[index];
		expected.Ease->In.X = 1;
		expected.Ease->Out.X = 1.25;
		expected.Ease->InType = expected.Ease->OutType = "bezier";
		CHECK(ui.Doc.Keyframes[index] == expected);
	}
	CHECK(ui.Doc.Tracks == before.Tracks);
	const auto after = ui.Doc;
	Document loaded;
	Diagnostic error;
	REQUIRE(Read(Write(after), loaded, error) == Status::Ok);
	CHECK(loaded == after);
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(ui.Doc == after);
}
TEST_CASE(
	"Selected easing popup preserves staged originals and selection on host refusal",
	"[studio][timeline_ease]"
) {
	Timeline ui;
	ui.Frame();
	ui.Frame();
	ui.Click(0);
	const auto before = ui.Doc;
	const auto selected = ui.Editor.Selection;
	ui.Button("Ease keys");
	ui.Whole("0.75");
	const auto originals = ui.Ease.Originals;
	ui.Refuse = true;
	ui.Button("Apply");
	CHECK(ui.Doc == before);
	CHECK(ui.Editor.Selection == selected);
	CHECK(ui.Ease.Originals == originals);
	CHECK(ui.Ease.Active);
	CHECK_FALSE(ui.History.CanUndo());
	ui.Refuse = false;
	ui.Button("Apply");
	CHECK(ui.Changes == 1);
	CHECK_FALSE(ui.Ease.Active);
}
TEST_CASE(
	"Selected easing guards stale keys, capture bytes and unaffected handle sides", "[studio][timeline_ease]"
) {
	Timeline ui;
	ui.Editor.Selection = {studio::TimelineKeyEditor::Identity(ui.Doc.Keyframes[0])};
	const auto before = ui.Doc;
	SECTION("native interpolation is not silently converted") {
		ui.Doc.Keyframes[0].Interpolation = "linear";
		ui.Doc.Keyframes[0].Ease.reset();
		const auto native = ui.Doc;
		CHECK_FALSE(ui.Ease.Begin(ui.Doc, ui.Editor, ui.Error));
		CHECK(ui.Error.Code == Status::InvalidValue);
		CHECK(ui.Doc == native);
		CHECK(ui.Ease.Originals.empty());
	}
	SECTION("byte refusal before staging") {
		CHECK_FALSE(ui.Ease.Begin(ui.Doc, ui.Editor, ui.Error, 1));
		CHECK(ui.Error.Code == Status::LimitExceeded);
		CHECK(ui.Ease.Originals.empty());
	}
	SECTION("stale source metadata is refused atomically") {
		REQUIRE(ui.Ease.Begin(ui.Doc, ui.Editor, ui.Error));
		ui.Doc.Keyframes[0].SourceKeyId = "changed-origin";
		const auto changed = ui.Doc;
		CHECK_FALSE(studio::ApplyImageGraphDocumentEdit(ui.Doc, ui.History, [&](Document &doc) {
			return ui.Ease.PrepareCommit(doc, ui.Error);
		}));
		CHECK(ui.Error.Code == Status::InvalidValue);
		CHECK(ui.Doc == changed);
		CHECK(ui.Ease.Originals[0] == before.Keyframes[0]);
	}
	SECTION("incoming-only reset retains outgoing type and vertical coordinates") {
		REQUIRE(ui.Ease.Begin(ui.Doc, ui.Editor, ui.Error));
		ui.Ease.Sides = 2;
		ui.Ease.Delta = -1;
		REQUIRE(ui.Ease.PrepareCommit(ui.Doc, ui.Error));
		auto expected = before.Keyframes[0];
		expected.Ease->In.X = 0;
		expected.Ease->InType = "linear";
		CHECK(ui.Doc.Keyframes[0] == expected);
		CHECK(ui.Doc.Keyframes[1] == before.Keyframes[1]);
	}
}

TEST_CASE("Selected easing popup cancels an authored revision change", "[studio][timeline_ease]") {
	Timeline ui;
	ui.Frame();
	ui.Frame();
	ui.Click(0);
	ui.Button("Ease keys");
	REQUIRE(ui.Ease.Active);
	const auto before = ui.Doc;
	const auto selected = ui.Editor.Selection;
	++ui.Revision;
	ui.Frame();
	CHECK_FALSE(ui.Ease.Active);
	CHECK(ui.Ease.Originals.empty());
	CHECK(ui.Doc == before);
	CHECK(ui.Editor.Selection == selected);
	CHECK(ui.Error.Code == Status::InvalidValue);
	CHECK_FALSE(ui.History.CanUndo());
}

TEST_CASE(
	"Selected easing edits mixed combined and captured scalar axes once",
	"[studio][timeline_ease][mirror_axes]"
) {
	auto document = MixedSourceDocument();
	const auto before = document;
	studio::TimelineKeyEditor selection;
	selection.Selection = {
		{"owner", "center", {0}, 0}, {"alias", "center", {0}, 1}, {"owner", "center", {0}, -1}
	};
	studio::TimelineEaseEditor ease;
	Diagnostic error;
	REQUIRE(ease.Begin(document, selection, error));
	CHECK(ease.OriginalAxes == std::vector<int8_t>{0, 1, -1});
	ease.Sides = 2;
	ease.Delta = .25;
	Document changed = document;
	REQUIRE(ease.PrepareCommit(changed, error));
	const auto changedOwner = std::find_if(changed.Nodes.begin(), changed.Nodes.end(), [](const auto &node) {
		return node.Id == "owner";
	});
	const auto beforeOwner = std::find_if(before.Nodes.begin(), before.Nodes.end(), [](const auto &node) {
		return node.Id == "owner";
	});
	REQUIRE(changedOwner != changed.Nodes.end());
	REQUIRE(beforeOwner != before.Nodes.end());
	REQUIRE(changedOwner->SourceSeparatedVec2Animators);
	const auto &afterAxes = changedOwner->SourceSeparatedVec2Animators->Inputs.front().Axes;
	const auto &beforeAxes = beforeOwner->SourceSeparatedVec2Animators->Inputs.front().Axes;
	auto expectedX = beforeAxes[0].Keys[0];
	expectedX.Ease->In.X = .5;
	CHECK(afterAxes[0].Keys[0].Ease->In.X == .5);
	CHECK(afterAxes[0].Keys[0].Ease->Out.X == beforeAxes[0].Keys[0].Ease->Out.X);
	CHECK(afterAxes[0].Keys[0].SourceKeyId == "x0");
	CHECK(afterAxes[0].Keys[0].SourceDriver == beforeAxes[0].Keys[0].SourceDriver);
	CHECK(afterAxes[0].Keys[0] == expectedX);
	CHECK(afterAxes[0].Keys[1] == beforeAxes[0].Keys[1]);
	auto expectedY = beforeAxes[1].Keys[0];
	expectedY.Ease->In.X = .5;
	CHECK(afterAxes[1].Keys[0].Ease->In.X == .5);
	CHECK(afterAxes[1].Keys[0].SourceKeyId == "y0");
	CHECK(afterAxes[1].Keys[0] == expectedY);
	CHECK(afterAxes[1].Keys[1] == beforeAxes[1].Keys[1]);
	CHECK_FALSE(changed.Nodes.back().SourceSeparatedVec2Animators);
	REQUIRE(changed.Keyframes.size() == before.Keyframes.size());
	for (const auto &original : before.Keyframes) {
		auto expectedCombined = original;
		if (original.Tick == 0) expectedCombined.Ease->In.X = .5;
		const auto edited =
			std::find_if(changed.Keyframes.begin(), changed.Keyframes.end(), [&](const auto &key) {
				return key.NodeId == original.NodeId && key.Port == original.Port &&
					   GetFrameTime(key) == GetFrameTime(original);
			});
		REQUIRE(edited != changed.Keyframes.end());
		CHECK(*edited == expectedCombined);
	}
	CHECK(changed.SourceAnimators->Bindings.front().Axes.Storage == GroupAxisStorage::Shared);

	studio::ImageGraphHistory history;
	REQUIRE(history.TryRecord(before, changed));
	document = changed;
	const auto accepted = document;
	REQUIRE(history.Undo(document));
	CHECK(document == before);
	REQUIRE(history.Redo(document));
	CHECK(document == accepted);
	Document reopened;
	REQUIRE(Read(Write(document), reopened, error) == Status::Ok);
	CHECK(reopened == document);
}

TEST_CASE(
	"Selected easing rejects stale scalar pins, invalid selectors and transaction refusal",
	"[studio][timeline_ease][mirror_axes]"
) {
	auto document = MixedSourceDocument();
	studio::TimelineKeyEditor selection;
	selection.Selection = {{"alias", "center", {0}, 1}};
	studio::TimelineEaseEditor ease;
	Diagnostic error;
	REQUIRE(ease.Begin(document, selection, error));
	ease.Delta = .5;
	const auto heldAxes = ease.Originals;
	auto owner = std::find_if(document.Nodes.begin(), document.Nodes.end(), [](const auto &node) {
		return node.Id == "owner";
	});
	REQUIRE(owner != document.Nodes.end());
	owner->SourceSeparatedVec2Animators->Inputs.front().Axes[1].Keys[0].Data = 9.0;
	const auto stale = document;
	CHECK_FALSE(ease.PrepareCommit(document, error));
	CHECK(error.Code == Status::InvalidValue);
	CHECK(document == stale);
	CHECK(ease.Originals == heldAxes);

	CHECK(ease.Active);
	ease.Cancel();
	auto fresh = MixedSourceDocument();
	REQUIRE(ease.Begin(fresh, selection, error));
	ease.Delta = .5;
	const auto before = fresh;
	CHECK_FALSE(ease.PrepareCommit(fresh, error, 1));
	CHECK(error.Code == Status::LimitExceeded);
	CHECK(fresh == before);

	ease.OriginalAxes.front() = 2;
	CHECK_FALSE(ease.PrepareCommit(fresh, error));
	CHECK(error.Code == Status::InvalidValue);
	CHECK(fresh == before);

	ease.OriginalAxes.front() = -2;
	auto uncaptured = fresh;
	uncaptured.SourceAnimators = {};
	const auto uncapturedBefore = uncaptured;
	CHECK_FALSE(ease.PrepareCommit(uncaptured, error));
	CHECK(error.Code == Status::InvalidValue);
	CHECK(uncaptured == uncapturedBefore);

	CHECK(ease.Active);
	ease.Cancel();
	auto limited = MixedSourceDocument();
	REQUIRE(ease.Begin(limited, selection, error));
	ease.Delta = .5;
	studio::ImageGraphHistory history(128, 1);
	CHECK_FALSE(studio::ApplyImageGraphDocumentEdit(limited, history, [&](Document &candidate) {
		return ease.PrepareCommit(candidate, error);
	}));
	CHECK(limited == MixedSourceDocument());
	CHECK_FALSE(history.CanUndo());
	CHECK(ease.Active);
}
