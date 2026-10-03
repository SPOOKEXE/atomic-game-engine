#include "../src/TimelineDopesheet.hpp"

#include "../src/ImageGraphDocumentEdit.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("studio.timeline_dopesheet")
TEST_DEPENDS("studio.imagegraph")

namespace {
	using namespace engine::imagegraph;
	struct ScopedCurrentContext {
		ImGuiContext *PreviousContext = ImGui::GetCurrentContext();
		ImGuiContext *Context = ImGui::CreateContext();

		ScopedCurrentContext() {
			ImGui::SetCurrentContext(Context);
		}
		~ScopedCurrentContext() {
			ImGui::SetCurrentContext(Context);
			ImGui::DestroyContext(Context);
			ImGui::SetCurrentContext(PreviousContext);
		}
		ScopedCurrentContext(const ScopedCurrentContext &) = delete;
		ScopedCurrentContext &operator=(const ScopedCurrentContext &) = delete;
	};

	struct Sheet {
		ImGuiContext *PreviousContext = ImGui::GetCurrentContext();
		ImGuiContext *Context = ImGui::CreateContext();
		Document Doc;
		studio::ImageGraphHistory History;
		studio::TimelineKeyEditor Keys;
		studio::TimelineDopesheet View;
		Diagnostic Error;
		uint64_t Revision = 0;
		unsigned Changes = 0, Attempts = 0;
		ImVec2 CanvasMin, CanvasMax, TextCenter;
		bool ShowText = false, OpenGatePopup = false, CloseGatePopup = false, GatePopupOpen = false;
		char Text[32] = "editable";
		Sheet() {
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
		}
		~Sheet() {
			ImGui::SetCurrentContext(Context);
			ImGui::DestroyContext(Context);
			ImGui::SetCurrentContext(PreviousContext);
		}
		void Frame() {
			ImGui::SetCurrentContext(Context);
			ImGui::NewFrame();
			ImGui::SetNextWindowPos({20, 20});
			ImGui::SetNextWindowSize({900, 500});
			ImGui::Begin("Dopesheet", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
			if (OpenGatePopup) {
				ImGui::OpenPopup("Delete gate");
				OpenGatePopup = false;
			}
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
			if (ShowText) {
				ImGui::InputText("Text", Text, sizeof(Text));
				const auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
				TextCenter = {(a.x + b.x) * .5f, (a.y + b.y) * .5f};
			}
			GatePopupOpen = ImGui::BeginPopup("Delete gate");
			if (GatePopupOpen) {
				ImGui::TextUnformatted("Popup owns keyboard input");
				if (CloseGatePopup) {
					ImGui::CloseCurrentPopup();
					CloseGatePopup = false;
				}
				ImGui::EndPopup();
			}
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
		void DoubleClick(ImVec2 point) {
			Down(point);
			Up();
			Down(point);
			Up();
		}
		void Key(ImGuiKey key) {
			auto &io = ImGui::GetIO();
			io.AddKeyEvent(key, true);
			Frame();
			io.AddKeyEvent(key, false);
			Frame();
		}
	};
}

TEST_CASE(
	"dopesheet selects and restores its context when another is current", "[studio][timeline_dopesheet]"
) {
	ScopedCurrentContext existing;
	{
		Sheet ui;
		CHECK(ImGui::GetCurrentContext() == ui.Context);
		CHECK(ui.Context->IO.DisplaySize.x == 1000);
		CHECK(ui.Context->IO.DisplaySize.y == 600);
		ui.Frame();
	}
	CHECK(ImGui::GetCurrentContext() == existing.Context);
}

TEST_CASE(
	"dopesheet actual box selects key centers and drag commits one collision replacement",
	"[studio][timeline_dopesheet]"
) {
	Sheet ui;
	ui.Frame();
	ui.Frame();
	const auto before = ui.Doc;
	const auto first = ui.View.Markers[0].Position, last = ui.View.Markers[1].Position;
	ui.Down({first.x - 12, first.y - 10});
	ui.Mouse({last.x + 12, last.y + 10});
	ui.Up();
	REQUIRE(ui.Keys.Selection.size() == 2);
	CHECK_FALSE(ui.History.CanUndo());
	ui.Down(ui.View.Markers[0].Position);
	REQUIRE(ui.View.Dragging);
	const auto point = ui.View.Markers[0].Position;
	ui.Mouse({point.x + 6 * float(ui.View.PixelsPerFrame), point.y});
	CHECK(ui.Doc == before);
	ui.Up();
	INFO(ui.Error.Message);
	REQUIRE(ui.Changes == 1);
	REQUIRE(ui.Doc.Keyframes.size() == 2);
	CHECK(GetFrameTime(ui.Doc.Keyframes[0]) == FrameTime{7, 0, false});
	CHECK(GetFrameTime(ui.Doc.Keyframes[1]) == FrameTime{9, 0, false});
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
	CHECK_FALSE(ui.History.CanUndo());
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(ui.Doc.Keyframes.size() == 2);
}

TEST_CASE(
	"dopesheet double-clicking between keys selects and moves the adjacent pair in one undo",
	"[studio][timeline_dopesheet]"
) {
	Sheet ui;
	ui.Frame();
	ui.Frame();
	const auto before = ui.Doc;
	const auto first = ui.View.Markers[0].Position, second = ui.View.Markers[1].Position;
	ui.Click(2);
	auto &io = ImGui::GetIO();
	io.AddKeyEvent(ImGuiMod_Shift, true);
	ui.Frame();
	ui.DoubleClick({(first.x + second.x) * .5f, first.y});
	REQUIRE(ui.Keys.Selection.size() == 2);
	CHECK(ui.Keys.Selected(ui.Doc.Keyframes[0]));
	CHECK(ui.Keys.Selected(ui.Doc.Keyframes[1]));
	CHECK_FALSE(ui.Keys.Selected(ui.Doc.Keyframes[2]));
	CHECK(ui.Changes == 0);
	CHECK_FALSE(ui.History.CanUndo());
	io.AddKeyEvent(ImGuiMod_Shift, false);
	ui.Frame();

	const auto point = ui.View.Markers[0].Position;
	ui.Down(point);
	REQUIRE(ui.View.Dragging);
	ui.Mouse({point.x + float(ui.View.PixelsPerFrame), point.y});
	ui.Up();
	INFO(ui.Error.Message);
	REQUIRE(ui.Changes == 1);
	CHECK(GetFrameTime(ui.Doc.Keyframes[0]) == FrameTime{2, 0, false});
	CHECK(GetFrameTime(ui.Doc.Keyframes[1]) == FrameTime{4, 0, false});
	CHECK(GetFrameTime(ui.Doc.Keyframes[2]) == FrameTime{7, 0, false});
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
	CHECK_FALSE(ui.History.CanUndo());
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(GetFrameTime(ui.Doc.Keyframes[0]) == FrameTime{2, 0, false});
	CHECK(GetFrameTime(ui.Doc.Keyframes[1]) == FrameTime{4, 0, false});
	CHECK_FALSE(ui.History.CanRedo());
}

TEST_CASE(
	"dopesheet gap double-click ignores brackets under the labels and clipped header",
	"[studio][timeline_dopesheet]"
) {
	Sheet ui;
	ui.Frame();
	ui.Frame();
	const auto row = ui.View.Markers[0].Position.y;
	ui.View.PanX = -140;
	ui.Frame();
	ui.DoubleClick({ui.CanvasMin.x + 60, row});
	CHECK(ui.Keys.Selection.empty());
	CHECK(ui.Changes == 0);
	CHECK_FALSE(ui.History.CanUndo());

	ui.View.PanX = 0;
	ui.View.PanY = -24;
	ui.Frame();
	const float headerPoint = ui.CanvasMin.y + 20;
	const float frameGap = ui.CanvasMin.x + 140 + 60;
	ui.DoubleClick({frameGap, headerPoint});
	CHECK(ui.Keys.Selection.empty());
	CHECK(ui.Changes == 0);
	CHECK_FALSE(ui.History.CanUndo());
}

TEST_CASE(
	"dopesheet mouse drag Escape and stale originals leave authored keys untouched",
	"[studio][timeline_dopesheet]"
) {
	Sheet ui;
	ui.Frame();
	ui.Frame();
	const auto before = ui.Doc;
	ui.Down(ui.View.Markers[0].Position);
	auto point = ui.View.Markers[0].Position;
	ui.Mouse({point.x + 80, point.y});
	ui.Key(ImGuiKey_Escape);
	ui.Up();
	CHECK(ui.Doc == before);
	CHECK_FALSE(ui.History.CanUndo());
	CHECK_FALSE(ui.View.Dragging);
	ui.Down(ui.View.Markers[0].Position);
	point = ui.View.Markers[0].Position;
	ui.Doc.Keyframes[0].Kind = KeyframeKind::Adder;
	++ui.Revision;
	const auto changed = ui.Doc;
	ui.Mouse({point.x + 40, point.y});
	ui.Up();
	CHECK(ui.Doc == changed);
	CHECK(ui.Error.Code == Status::InvalidValue);
	CHECK_FALSE(ui.History.CanUndo());
}

TEST_CASE(
	"dopesheet exact Ctrl Alt scales about opposite endpoint with one undo", "[studio][timeline_dopesheet]"
) {
	Sheet ui;
	ui.Frame();
	ui.Frame();
	ui.Click(0);
	ui.Click(1, true);
	REQUIRE(ui.Keys.Selection.size() == 2);
	const auto before = ui.Doc;
	auto &io = ImGui::GetIO();
	io.AddKeyEvent(ImGuiMod_Ctrl, true);
	io.AddKeyEvent(ImGuiMod_Alt, true);
	ui.Frame();
	const auto point = ui.View.Markers[1].Position;
	ui.Down(point);
	REQUIRE(ui.View.Scaling);
	ui.Mouse({point.x + 4 * float(ui.View.PixelsPerFrame), point.y});
	ui.Up();
	INFO(ui.Error.Message);
	REQUIRE(ui.Changes == 1);
	REQUIRE(ui.Doc.Keyframes.size() == 2);
	CHECK(GetFrameTime(ui.Doc.Keyframes[0]) == FrameTime{1, 0, false});
	CHECK(GetFrameTime(ui.Doc.Keyframes[1]) == FrameTime{7, 0, false});
	CHECK(std::get<double>(ui.Doc.Keyframes[1].Data) == 3);
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
}

TEST_CASE(
	"dopesheet Alt drag clones without drivers and middle pan wheel zoom use real input",
	"[studio][timeline_dopesheet]"
) {
	Sheet ui;
	REQUIRE(studio::SetImageGraphKeyframeSourceDriver(ui.Doc, 0, KeyframeLinearDriver{2}, ui.Error));
	ui.Frame();
	ui.Frame();
	const auto before = ui.Doc;
	auto &io = ImGui::GetIO();
	io.AddKeyEvent(ImGuiMod_Alt, true);
	ui.Frame();
	const auto point = ui.View.Markers[0].Position;
	ui.Down(point);
	REQUIRE(ui.View.Copying);
	ui.Mouse({point.x + 4 * float(ui.View.PixelsPerFrame), point.y});
	ui.Up();
	INFO(ui.Error.Message);
	REQUIRE(ui.Changes == 1);
	REQUIRE(ui.Doc.Keyframes.size() == 4);
	CHECK(ui.Doc.Keyframes.front().SourceDriver == before.Keyframes.front().SourceDriver);
	const auto clone = std::find_if(ui.Doc.Keyframes.begin(), ui.Doc.Keyframes.end(), [](const auto &key) {
		return GetFrameTime(key) == FrameTime{5, 0, false};
	});
	REQUIRE(clone != ui.Doc.Keyframes.end());
	CHECK_FALSE(clone->SourceDriver);
	io.AddKeyEvent(ImGuiMod_Alt, false);
	ui.Frame();
	const auto oldZoom = ui.View.PixelsPerFrame;
	ui.Mouse({ui.CanvasMax.x - 40, ui.CanvasMin.y + 40});
	io.AddMouseWheelEvent(0, 1);
	ui.Frame();
	CHECK(ui.View.PixelsPerFrame > oldZoom);
	const auto oldPan = ui.View.PanX;
	io.AddMouseButtonEvent(2, true);
	ui.Frame();
	ui.Mouse({ui.CanvasMax.x - 20, ui.CanvasMin.y + 40});
	io.AddMouseButtonEvent(2, false);
	ui.Frame();
	CHECK(ui.View.PanX != oldPan);
	CHECK(ui.Changes == 1);
}

TEST_CASE(
	"dopesheet source mouse midpoint rounding and exact modifier masks are gesture specific",
	"[studio][timeline_dopesheet]"
) {
	Sheet ui;
	ui.Frame();
	ui.Frame();
	ui.Click(0);
	ui.Click(1, true);
	const auto before = ui.Doc;
	auto &io = ImGui::GetIO();
	io.AddKeyEvent(ImGuiMod_Ctrl, true);
	io.AddKeyEvent(ImGuiMod_Alt, true);
	io.AddKeyEvent(ImGuiMod_Super, true);
	ui.Frame();
	ui.Down(ui.View.Markers[1].Position);
	CHECK_FALSE(ui.View.Dragging);
	ui.Up();
	CHECK(ui.Doc == before);
	CHECK_FALSE(ui.History.CanUndo());
	io.AddKeyEvent(ImGuiMod_Ctrl, false);
	io.AddKeyEvent(ImGuiMod_Alt, false);
	io.AddKeyEvent(ImGuiMod_Super, false);
	ui.Frame();
	ui.Keys.Selection.clear();
	const auto point = ui.View.Markers[0].Position;
	ui.Down(point);
	ui.Mouse({point.x + 2.5f * float(ui.View.PixelsPerFrame), point.y});
	ui.Up();
	REQUIRE(ui.Changes == 1);
	REQUIRE(ui.Doc.Keyframes.size() == 2);
	CHECK(GetFrameTime(ui.Doc.Keyframes[0]) == FrameTime{3, 0, false});
	CHECK(std::get<double>(ui.Doc.Keyframes[0].Data) == 1);
}

TEST_CASE(
	"dopesheet scaling across fixed endpoint permits negative factors and clamps key positions",
	"[studio][timeline_dopesheet]"
) {
	Sheet ui;
	ui.Frame();
	ui.Frame();
	ui.Click(0);
	ui.Click(1, true);
	const auto before = ui.Doc;
	auto &io = ImGui::GetIO();
	io.AddKeyEvent(ImGuiMod_Ctrl, true);
	io.AddKeyEvent(ImGuiMod_Alt, true);
	ui.Frame();
	const auto point = ui.View.Markers[1].Position;
	ui.Down(point);
	REQUIRE(ui.View.Scaling);
	ui.Mouse({point.x - 3 * float(ui.View.PixelsPerFrame), point.y});
	ui.Up();
	INFO(ui.Error.Message);
	REQUIRE(ui.Changes == 1);
	REQUIRE(ui.Doc.Keyframes.size() == 3);
	CHECK(GetFrameTime(ui.Doc.Keyframes[0]) == FrameTime{});
	CHECK(std::get<double>(ui.Doc.Keyframes[0].Data) == 3);
	CHECK(GetFrameTime(ui.Doc.Keyframes[1]) == FrameTime{1, 0, false});
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
}

TEST_CASE("dopesheet history refusal retains authored key selection", "[studio][timeline_history54]") {
	Sheet ui;
	ui.History = studio::ImageGraphHistory(128, 1);
	ui.Frame();
	ui.Frame();
	const auto before = ui.Doc;
	const auto point = ui.View.Markers[0].Position;
	ui.Down(point);
	const auto selected = ui.Keys.Selection;
	REQUIRE(selected.size() == 1);
	ui.Mouse({point.x + 4 * float(ui.View.PixelsPerFrame), point.y});
	ui.Up();
	CHECK(ui.Doc == before);
	CHECK(ui.Changes == 0);
	CHECK_FALSE(ui.History.CanUndo());
	CHECK(ui.Keys.Selection == selected);
	CHECK_FALSE(ui.View.Dragging);
}

TEST_CASE(
	"dopesheet staged refusal preserves gesture and retry commits one undo", "[studio][timeline_history54]"
) {
	Sheet ui;
	ui.History = studio::ImageGraphHistory(128, 1);
	ui.Frame();
	ui.Frame();
	const auto before = ui.Doc;
	const auto point = ui.View.Markers[0].Position;
	ui.Down(point);
	ui.Mouse({point.x + 4 * float(ui.View.PixelsPerFrame), point.y});
	const auto selected = ui.Keys.Selection;
	const auto originals = ui.View.Originals;
	const auto destinations = ui.View.Destinations;
	Document staged = ui.Doc;
	REQUIRE(ui.View.PrepareCommit(staged, ui.Keys, ui.Error));
	REQUIRE_FALSE(ui.History.TryRecord(ui.Doc, staged));
	CHECK(ui.Doc == before);
	CHECK(ui.Keys.Selection == selected);
	CHECK(ui.View.Originals == originals);
	CHECK(ui.View.Destinations == destinations);
	CHECK(ui.View.Dragging);
	ui.View.Cancel();
	ui.Up();
	ui.History = studio::ImageGraphHistory{};
	ui.Down(ui.View.Markers[0].Position);
	const auto retry = ui.View.Markers[0].Position;
	ui.Mouse({retry.x + 4 * float(ui.View.PixelsPerFrame), retry.y});
	CHECK(ui.Doc == before);
	ui.Up();
	REQUIRE(ui.Changes == 1);
	const auto moved = ui.Doc;
	REQUIRE(moved != before);
	REQUIRE(ui.Keys.Selection.size() == 1);
	CHECK(ui.Keys.Selection[0].Time == FrameTime{5, 0, false});
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
	CHECK_FALSE(ui.History.CanUndo());
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(ui.Doc == moved);
	CHECK_FALSE(ui.History.CanRedo());
}

TEST_CASE("dopesheet refused Alt copy preserves redo and selection", "[studio][timeline_history54]") {
	Sheet ui;
	const auto before = ui.Doc;
	auto priorEdit = before;
	priorEdit.Nodes[0].Values[0].Data = 2.0;
	const auto bytes = std::max(Write(before).size(), Write(priorEdit).size());
	ui.History = studio::ImageGraphHistory(128, bytes);
	REQUIRE(ui.History.TryRecord(before, priorEdit));
	ui.Doc = priorEdit;
	REQUIRE(ui.History.Undo(ui.Doc));
	REQUIRE(ui.Doc == before);
	REQUIRE(ui.History.CanRedo());
	ui.Frame();
	ui.Frame();
	auto &io = ImGui::GetIO();
	io.AddKeyEvent(ImGuiMod_Alt, true);
	ui.Frame();
	const auto point = ui.View.Markers[0].Position;
	ui.Down(point);
	REQUIRE(ui.View.Copying);
	const auto selected = ui.Keys.Selection;
	ui.Mouse({point.x + 4 * float(ui.View.PixelsPerFrame), point.y});
	ui.Up();
	CHECK(ui.Doc == before);
	CHECK(ui.Changes == 0);
	CHECK(ui.Keys.Selection == selected);
	CHECK_FALSE(ui.View.Dragging);
	CHECK_FALSE(ui.View.Prepared);
	CHECK(ui.View.PreparedSelection.empty());
	CHECK_FALSE(ui.History.CanUndo());
	REQUIRE(ui.History.CanRedo());
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(ui.Doc == priorEdit);
}

TEST_CASE(
	"Dopesheet Delete removes the selected full identities in one undo transaction",
	"[studio][timeline_dopesheet][dopesheet_delete]"
) {
	Sheet ui;
	ui.Doc.Nodes.push_back({"other", "value.number", {}, {}, {{"value", 0.0}}});
	ui.Doc.Keyframes[1].NodeId = "other";
	ui.Doc.Keyframes[0].SourceKeyId = "first-source-key";
	ui.Doc.Keyframes[1].SourceKeyId = "other-source-key";
	ui.Doc.Keyframes[2].SourceKeyId = "untouched-source-key";
	ui.Doc.Tracks = {{"number", "value", "hold", -1}, {"other", "value", "hold", -1}};
	REQUIRE(studio::SetImageGraphKeyframeSourceDriver(ui.Doc, 2, KeyframeLinearDriver{2}, ui.Error));
	const auto before = ui.Doc;
	ui.Frame();
	ui.Frame();
	ui.Click(0);
	ui.Click(1, true);
	REQUIRE(ui.Keys.Selection.size() == 2);
	ui.Attempts = 0;
	ui.Changes = 0;
	ui.Key(ImGuiKey_Delete);
	INFO(ui.Error.Message);
	REQUIRE(ui.Attempts == 1);
	REQUIRE(ui.Changes == 1);
	CHECK(ui.Keys.Selection.empty());
	REQUIRE(ui.Doc.Keyframes.size() == 1);
	CHECK(ui.Doc.Keyframes.front() == before.Keyframes.back());
	CHECK(ui.Doc.Nodes == before.Nodes);
	CHECK(ui.Doc.Tracks == before.Tracks);
	CHECK(ui.Doc.Outputs == before.Outputs);
	const auto deleted = ui.Doc;
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
	CHECK_FALSE(ui.History.CanUndo());
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(ui.Doc == deleted);
	CHECK_FALSE(ui.History.CanRedo());
	Document restored;
	REQUIRE(Read(Write(ui.Doc), restored, ui.Error) == Status::Ok);
	CHECK(restored == deleted);
}

TEST_CASE(
	"Dopesheet shrinking Delete fits its history budget and replaces existing redo",
	"[studio][timeline_dopesheet][dopesheet_delete]"
) {
	Sheet ui;
	const auto before = ui.Doc;
	auto priorEdit = before;
	priorEdit.Nodes.front().Values.front().Data = 2.0;
	ui.History = studio::ImageGraphHistory(128, std::max(Write(before).size(), Write(priorEdit).size()));
	REQUIRE(ui.History.TryRecord(before, priorEdit));
	ui.Doc = priorEdit;
	REQUIRE(ui.History.Undo(ui.Doc));
	REQUIRE(ui.History.CanRedo());
	ui.Frame();
	ui.Frame();
	ui.Click(0);
	ui.Click(1, true);
	const auto selected = ui.Keys.Selection;
	REQUIRE(selected.size() == 2);
	ui.Attempts = 0;
	ui.Changes = 0;
	ui.Key(ImGuiKey_Delete);
	REQUIRE(ui.Attempts == 1);
	REQUIRE(ui.Changes == 1);
	REQUIRE(ui.Doc.Keyframes.size() == 1);
	CHECK(GetFrameTime(ui.Doc.Keyframes.front()) == FrameTime{7, 0, false});
	CHECK(ui.Keys.Selection.empty());
	CHECK_FALSE(ui.View.Dragging);
	CHECK_FALSE(ui.View.Prepared);
	CHECK(ui.View.Originals.empty());
	CHECK_FALSE(ui.History.CanRedo());
	const auto deleted = ui.Doc;
	CHECK(Write(deleted).size() < Write(before).size());
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
	CHECK_FALSE(ui.History.CanUndo());
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(ui.Doc == deleted);
}

TEST_CASE("Dopesheet Delete respects input and gesture ownership", "[studio][dopesheet_delete]") {
	Sheet ui;
	ui.Frame();
	ui.Frame();
	ui.Click(0);
	const auto before = ui.Doc;
	const auto selected = ui.Keys.Selection;
	ui.Attempts = ui.Changes = 0;
	SECTION("modified Delete belongs to another shortcut") {
		for (auto modifier : {ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiMod_Alt, ImGuiMod_Super}) {
			ImGui::GetIO().AddKeyEvent(modifier, true);
			ui.Frame();
			ui.Key(ImGuiKey_Delete);
			ImGui::GetIO().AddKeyEvent(modifier, false);
			ui.Frame();
		}
	}
	SECTION("an actual text field owns Delete while the sheet is hovered") {
		ui.ShowText = true;
		ui.Frame();
		ui.Down(ui.TextCenter);
		ui.Up();
		ui.Mouse(ui.View.Markers[0].Position);
		REQUIRE(ImGui::GetIO().WantTextInput);
		ui.Key(ImGuiKey_Delete);
		ui.Key(ImGuiKey_Escape);
		ui.ShowText = false;
		ui.Frame();
		ui.Frame();
	}
	SECTION("an actual popup owns the key") {
		ui.OpenGatePopup = true;
		ui.Frame();
		REQUIRE(ui.GatePopupOpen);
		ui.Key(ImGuiKey_Delete);
		ui.CloseGatePopup = true;
		ui.Frame();
		ui.Frame();
	}
	SECTION("an active key transfer owns the key") {
		REQUIRE(ui.Keys.Begin(ui.Doc, false, {}, ui.Error));
		ui.Key(ImGuiKey_Delete);
		REQUIRE(ui.Keys.Active);
		ui.Keys.Cancel();
	}
	SECTION("an active drag owns the key") {
		ui.Down(ui.View.Markers[0].Position);
		REQUIRE(ui.View.Dragging);
		ui.Key(ImGuiKey_Delete);
		REQUIRE(ui.View.Dragging);
		ui.Key(ImGuiKey_Escape);
		ui.Up();
	}
	CHECK(ui.Attempts == 0);
	CHECK(ui.Changes == 0);
	CHECK(ui.Doc == before);
	CHECK(ui.Keys.Selection == selected);
	CHECK_FALSE(ui.History.CanUndo());
	ui.Mouse(ui.View.Markers[0].Position);
	ui.Key(ImGuiKey_Delete);
	REQUIRE(ui.Attempts == 1);
	REQUIRE(ui.Changes == 1);
	CHECK(ui.Keys.Selection.empty());
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
}

TEST_CASE("Dopesheet deletion refuses changed pinned originals", "[studio][dopesheet_delete]") {
	Sheet ui;
	ui.Keys.Selection = {studio::TimelineKeyEditor::Identity(ui.Doc.Keyframes[0])};
	const auto selected = ui.Keys.Selection;
	REQUIRE(ui.View.BeginDeletion(ui.Doc, ui.Keys, ui.Error));
	const auto pinned = ui.View.Originals;
	ui.Doc.Keyframes[0].Data = 19.0;
	const auto changed = ui.Doc;
	CHECK_FALSE(studio::ApplyImageGraphDocumentEdit(ui.Doc, ui.History, [&](auto &document) {
		return ui.View.PrepareCommit(document, ui.Keys, ui.Error);
	}));
	CHECK(ui.Error.Code == Status::InvalidValue);
	CHECK(ui.Doc == changed);
	CHECK(ui.Keys.Selection == selected);
	CHECK(ui.View.Originals == pinned);
	CHECK(ui.View.Deleting);
	CHECK_FALSE(ui.History.CanUndo());
	ui.View.Cancel();
	CHECK_FALSE(ui.View.Deleting);
	CHECK(ui.View.Originals.empty());
}

TEST_CASE("Dopesheet deletion admits work before capturing a dense selection", "[studio][dopesheet_delete]") {
	Sheet ui;
	ui.Doc.Keyframes.clear();
	for (uint64_t tick = 0; tick < 10000; ++tick) {
		ui.Doc.Keyframes.push_back({"number", "value", tick, double(tick), "linear"});
		if (tick < 3000)
			ui.Keys.Selection.push_back(studio::TimelineKeyEditor::Identity(ui.Doc.Keyframes.back()));
	}
	const auto selected = ui.Keys.Selection;
	const auto before = ui.Doc;
	auto priorEdit = before;
	priorEdit.Nodes.front().Values.front().Data = 2.0;
	REQUIRE(ui.History.TryRecord(before, priorEdit));
	ui.Doc = priorEdit;
	REQUIRE(ui.History.Undo(ui.Doc));
	REQUIRE(ui.History.CanRedo());
	CHECK_FALSE(ui.View.BeginDeletion(ui.Doc, ui.Keys, ui.Error));
	CHECK(ui.Error.Code == Status::LimitExceeded);
	CHECK(ui.Doc == before);
	CHECK(ui.Keys.Selection == selected);
	CHECK(ui.View.Originals.empty());
	CHECK_FALSE(ui.View.Deleting);
	CHECK_FALSE(ui.History.CanUndo());
	REQUIRE(ui.History.CanRedo());
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(ui.Doc == priorEdit);
}
