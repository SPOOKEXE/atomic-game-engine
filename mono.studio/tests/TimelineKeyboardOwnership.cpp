#include "ImageGraphDocumentEdit.hpp"
#include "ImageGraphHistoryKeys.hpp"
#include "TimelineDopesheet.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nodegraph/Editor.hpp>
#include <studio/ImageGraph.hpp>

TEST_SUITE_ID("studio.timeline_keyboard_ownership")
TEST_DEPENDS("studio.imagegraph")

namespace {
	using namespace engine::imagegraph;
	struct ComposerPanels {
		ImGuiContext *PreviousContext = ImGui::GetCurrentContext();
		ImGuiContext *Context = ImGui::CreateContext();
		Document Doc;
		studio::ImageGraphHistory History;
		studio::TimelineKeyEditor Keys;
		studio::TimelineDopesheet View;
		Diagnostic Error;
		uint64_t Revision = 0;
		unsigned Changes = 0, Attempts = 0;
		nodegraph::Graph Graph;
		nodegraph::Canvas Canvas;
		studio::ImageGraphCanvasIds Ids;
		nodegraph::NodeId Number = nodegraph::NO_NODE;
		unsigned GraphChanges = 0;
		ImVec2 GraphMin, GraphMax, CanvasMin, CanvasMax, TextCenter, OtherCenter;
		bool ShowTextInput = false, TextInputActive = false;
		bool OpenHistoryPopup = false, HistoryPopupOpen = false, OtherWindow = false;
		bool ComposerOwnsFocus = false, AnyItemActive = false, WantTextInput = false;
		char Text[32] = "editable";
		ComposerPanels() {
			ImGui::SetCurrentContext(Context);
			auto &io = ImGui::GetIO();
			io.DisplaySize = {1000, 600};
			io.DeltaTime = 1.f / 60;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
			Doc.FormatVersion = 9;
			Doc.Nodes = {{"number", "value.number", "", {}, {{"value", 0.0}}}};
			Doc.Outputs = {{"out", "number", "number"}};
			Doc.Keyframes = {
				{"number", "value", 1, 1.0, "linear"},
				{"number", "value", 3, 3.0, "linear"},
				{"number", "value", 7, 7.0, "linear"}
			};
			studio::RegisterImageGraphNodeTypes();
			std::string failure;
			REQUIRE(studio::LoadImageGraphCanvas(Doc, Graph, Ids, failure));
			Number = Ids.ToCanvas.at("number");
			Canvas.Select(Number);
			Canvas.Signals.Changed = [&] { ++GraphChanges; };
		}
		~ComposerPanels() {
			ImGui::SetCurrentContext(Context);
			ImGui::DestroyContext(Context);
			ImGui::SetCurrentContext(PreviousContext);
		}
		void Frame() {
			ImGui::SetCurrentContext(Context);
			ImGui::NewFrame();
			if (OtherWindow) {
				ImGui::SetNextWindowPos({20, 20});
				ImGui::SetNextWindowSize({160, 220});
				ImGui::Begin("OtherWindow", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
				ImGui::TextUnformatted("Other window owns focus");
				OtherCenter = {ImGui::GetWindowPos().x + 80, ImGui::GetWindowPos().y + 100};
				ImGui::End();
			}
			ImGui::SetNextWindowPos(OtherWindow ? ImVec2{200, 20} : ImVec2{20, 20});
			ImGui::SetNextWindowSize(OtherWindow ? ImVec2{780, 500} : ImVec2{900, 500});
			ImGui::Begin("ComposerPanels", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
			// Match ImageComposer: canvas child first, sibling inspector child second.
			ImGui::BeginChild("##image-graph-canvas", {350, 440});
			GraphMin = ImGui::GetCursorScreenPos();
			GraphMax = {GraphMin.x + 350, GraphMin.y + 440};
			Canvas.Draw(Graph);
			ImGui::EndChild();
			ImGui::SameLine();
			ImGui::BeginChild("##image-graph-inspector", {0, 440});
			View.Draw(Doc, Revision, Keys, {}, Error, [&] {
				++Attempts;
				const bool accepted = studio::ApplyImageGraphDocumentEdit(Doc, History, [&](Document &doc) {
					return View.PrepareCommit(doc, Keys, Error);
				});
				if (accepted) {
					View.PublishCommit(Keys);
					++Changes;
					++Revision;
				}
				return accepted;
			});
			CanvasMin = ImGui::GetItemRectMin();
			CanvasMax = ImGui::GetItemRectMax();
			if (ShowTextInput) {
				ImGui::InputText("Timeline text", Text, sizeof(Text));
				TextInputActive = ImGui::IsItemActive();
				const ImVec2 minimum = ImGui::GetItemRectMin(), maximum = ImGui::GetItemRectMax();
				TextCenter = {(minimum.x + maximum.x) * 0.5f, (minimum.y + maximum.y) * 0.5f};
			}
			if (OpenHistoryPopup) {
				ImGui::OpenPopup("history gate");
				OpenHistoryPopup = false;
			}
			HistoryPopupOpen = ImGui::BeginPopup("history gate");
			if (HistoryPopupOpen) {
				ImGui::TextUnformatted("Popup owns keyboard input");
				ImGui::EndPopup();
			}
			ImGui::EndChild();
			ComposerOwnsFocus = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
			AnyItemActive = ImGui::IsAnyItemActive();
			WantTextInput = ImGui::GetIO().WantTextInput;
			studio::detail::ApplyImageGraphHistoryKey([&](bool redo) {
				const bool changed = redo ? History.Redo(Doc) : History.Undo(Doc);
				if (!changed) return;
				std::string failure;
				REQUIRE(studio::LoadImageGraphCanvas(Doc, Graph, Ids, failure));
				Number = Ids.ToCanvas.at("number");
				++Revision;
			});
			ImGui::End();
			ImGui::Render();
		}
		void Mouse(ImVec2 point) {
			ImGui::GetIO().AddMousePosEvent(point.x, point.y);
			Frame();
		}
		void Down(ImVec2 point) {
			Mouse(point);
			ImGui::GetIO().AddMouseButtonEvent(0, true);
			Frame();
		}
		void Up() {
			ImGui::GetIO().AddMouseButtonEvent(0, false);
			Frame();
			Frame();
		}
		void Click(size_t index, bool shift = false) {
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, shift);
			Frame();
			Down(View.Markers.at(index).Position);
			Up();
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, false);
			Frame();
		}
		void Key(ImGuiKey key) {
			auto &io = ImGui::GetIO();
			io.AddKeyEvent(key, true);
			Frame();
			io.AddKeyEvent(key, false);
			Frame();
		}
		void HistoryKey(ImGuiKey key, bool shift = false) {
			auto &io = ImGui::GetIO();
			io.AddKeyEvent(ImGuiMod_Ctrl, true);
			io.AddKeyEvent(ImGuiMod_Shift, shift);
			io.AddKeyEvent(key, true);
			Frame();
			io.AddKeyEvent(key, false);
			io.AddKeyEvent(ImGuiMod_Ctrl, false);
			io.AddKeyEvent(ImGuiMod_Shift, false);
			Frame();
		}
	};
}

TEST_CASE(
	"Composer sibling canvas does not consume a Dopesheet Delete gesture",
	"[studio][dopesheet_keyboard_ownership]"
) {
	ComposerPanels ui;
	ui.Frame();
	ui.Frame();
	const auto original = ui.Doc;
	const auto graphHash = ui.Graph.Hash(ui.Number);
	ui.Click(0);
	REQUIRE(ui.Keys.Selection.size() == 1);
	REQUIRE(ui.Canvas.Selection() == std::vector<nodegraph::NodeId>{ui.Number});
	// Clicking a marker stages a no-op drag; count only the following Delete.
	ui.Attempts = ui.Changes = 0;
	ui.Key(ImGuiKey_Delete);
	INFO(ui.Error.Message);
	CHECK(ui.GraphChanges == 0);
	CHECK(ui.Graph.Find(ui.Number) != nullptr);
	CHECK(ui.Graph.Hash(ui.Number) == graphHash);
	REQUIRE(ui.Attempts == 1);
	REQUIRE(ui.Changes == 1);
	REQUIRE(ui.Doc.Keyframes.size() == 2);
	CHECK(GetFrameTime(ui.Doc.Keyframes[0]) == FrameTime{3, 0, false});
	CHECK(ui.Doc.Nodes == original.Nodes);
	CHECK(ui.Doc.Outputs == original.Outputs);
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == original);
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(ui.Doc.Keyframes.size() == 2);
}

TEST_CASE(
	"Composer focused canvas retains its own Delete shortcut beside Dopesheet",
	"[studio][dopesheet_keyboard_ownership]"
) {
	ComposerPanels ui;
	ui.Frame();
	ui.Frame();
	ui.Keys.Selection = {{"number", "value", 1, 0, false}};
	const auto original = ui.Doc;
	ui.Down({ui.GraphMin.x + 300, ui.GraphMin.y + 400});
	ui.Up();
	ui.Canvas.Select(ui.Number);
	ui.Key(ImGuiKey_Delete);
	REQUIRE(ui.GraphChanges == 1);
	CHECK(ui.Graph.Find(ui.Number) == nullptr);
	CHECK(ui.Attempts == 0);
	CHECK(ui.Doc == original);
	CHECK(ui.Keys.Selection.size() == 1);
	CHECK_FALSE(ui.History.CanUndo());
}

TEST_CASE(
	"Composer keyboard history routes Undo and Redo through either owned panel",
	"[studio][dopesheet_keyboard_ownership]"
) {
	ComposerPanels ui;
	ui.Frame();
	ui.Frame();
	const Document original = ui.Doc;
	Document changed = original;
	changed.Keyframes[0].Data = 9.0;
	REQUIRE(ui.History.TryRecord(original, changed));
	ui.Doc = changed;
	++ui.Revision;
	ui.Click(0);
	REQUIRE(ui.Keys.Selection.size() == 1);
	REQUIRE(ui.ComposerOwnsFocus);

	ui.HistoryKey(ImGuiKey_Z);
	CHECK(ui.Doc == original);
	CHECK(ui.History.CanRedo());
	CHECK(ui.GraphChanges == 0);

	ui.Down({ui.GraphMin.x + 300, ui.GraphMin.y + 400});
	ui.Up();
	REQUIRE(ui.ComposerOwnsFocus);
	ui.HistoryKey(ImGuiKey_Z, true);
	CHECK(ui.Doc == changed);
	CHECK(ui.History.CanUndo());

	ui.Click(0);
	ui.HistoryKey(ImGuiKey_Z);
	CHECK(ui.Doc == original);
	CHECK(ui.History.CanRedo());
	ui.HistoryKey(ImGuiKey_Y);
	CHECK(ui.Doc == changed);
	CHECK(ui.History.CanUndo());
}

TEST_CASE(
	"Composer history shortcut leaves an active text field's document alone",
	"[studio][timeline_history_keys]"
) {
	ComposerPanels ui;
	ui.Frame();
	ui.Frame();
	const Document original = ui.Doc;
	Document changed = original;
	changed.Keyframes[0].Data = 9.0;
	REQUIRE(ui.History.TryRecord(original, changed));
	ui.Doc = changed;
	++ui.Revision;
	ui.ShowTextInput = true;
	ui.Frame();
	ui.Down(ui.TextCenter);
	ui.Up();
	REQUIRE(ui.TextInputActive);
	CHECK(ui.AnyItemActive);

	ui.HistoryKey(ImGuiKey_Z);
	CHECK(ui.Doc == changed);
	CHECK(ui.History.CanUndo());
}

TEST_CASE(
	"Composer history shortcut leaves an open popup's document alone", "[studio][timeline_history_keys]"
) {
	ComposerPanels ui;
	ui.Frame();
	ui.Frame();
	const Document original = ui.Doc;
	Document changed = original;
	changed.Keyframes[0].Data = 9.0;
	REQUIRE(ui.History.TryRecord(original, changed));
	ui.Doc = changed;
	++ui.Revision;
	ui.OpenHistoryPopup = true;
	ui.Frame();
	REQUIRE(ui.HistoryPopupOpen);
	REQUIRE(ui.ComposerOwnsFocus);
	CHECK_FALSE(ui.AnyItemActive);
	CHECK_FALSE(ui.WantTextInput);

	ui.HistoryKey(ImGuiKey_Z);
	CHECK(ui.Doc == changed);
	CHECK(ui.History.CanUndo());
}

TEST_CASE("Composer history shortcut ignores a different focused window", "[studio][timeline_history_keys]") {
	ComposerPanels ui;
	ui.OtherWindow = true;
	ui.Frame();
	ui.Frame();
	const Document original = ui.Doc;
	Document changed = original;
	changed.Keyframes[0].Data = 9.0;
	REQUIRE(ui.History.TryRecord(original, changed));
	ui.Doc = changed;
	++ui.Revision;
	ui.Down(ui.OtherCenter);
	ui.Up();
	REQUIRE_FALSE(ui.ComposerOwnsFocus);

	ui.HistoryKey(ImGuiKey_Z);
	CHECK(ui.Doc == changed);
	CHECK(ui.History.CanUndo());
}
