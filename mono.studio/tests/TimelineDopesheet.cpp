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
		unsigned Changes = 0;
		ImVec2 CanvasMin, CanvasMax;
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
			View.Draw(Doc, Revision, Keys, {}, Error, [&] {
				bool accepted = false;
				if (studio::ApplyImageGraphDocumentEdit(Doc, History, [&](Document &doc) {
						accepted = View.Commit(doc, Keys, Error);
					})) {
					++Changes;
					++Revision;
				}
				return accepted;
			});
			CanvasMin = ImGui::GetItemRectMin();
			CanvasMax = ImGui::GetItemRectMax();
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
	};
}

TEST_CASE(
	"dopesheet selects and restores its context when another is current",
	"[studio][timeline_dopesheet]"
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
