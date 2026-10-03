#include "TimelineEaseEditor.hpp"

#include "ImageGraphDocumentEdit.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

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
