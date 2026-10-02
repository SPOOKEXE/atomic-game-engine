#include "../src/TimelineKeyEditor.hpp"

#include "../src/ImageGraphDocumentEdit.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("studio.timeline_key_editor")
TEST_DEPENDS("studio.imagegraph")

namespace {
	using namespace engine::imagegraph;
	struct ScopedImGuiContext {
		ImGuiContext *PreviousContext = ImGui::GetCurrentContext();
		ImGuiContext *Context = ImGui::CreateContext();

		ScopedImGuiContext() {
			ImGui::SetCurrentContext(Context);
			auto &io = ImGui::GetIO();
			io.DisplaySize = {800, 600};
			io.DeltaTime = 1.f / 60;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
		}
		~ScopedImGuiContext() {
			ImGui::SetCurrentContext(Context);
			ImGui::DestroyContext(Context);
			ImGui::SetCurrentContext(PreviousContext);
		}
		ScopedImGuiContext(const ScopedImGuiContext &) = delete;
		ScopedImGuiContext &operator=(const ScopedImGuiContext &) = delete;
	};

	struct Timeline {
		ImGuiContext *PreviousContext = ImGui::GetCurrentContext();
		ImGuiContext *Context = ImGui::CreateContext();
		Document Doc;
		studio::ImageGraphHistory History;
		studio::TimelineKeyEditor Editor;
		Diagnostic Error;
		FrameTime Cursor{5, .25, false};
		std::vector<ImVec2> Rows;
		std::vector<ImGuiID> RowIds;
		unsigned Changes = 0;
		bool ReconcileSplit = false;
		size_t SplitCount = 1;
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
			Doc.Nodes = {{"number", "value.number", "", {}, {{"value", 0.0}}}};
			Doc.Outputs = {{"out", "number", "number"}};
			Doc.Keyframes = {
				{"number", "value", 1, 1.0, "linear"},
				{"number", "value", 3, 3.0, "linear"},
				{"number", "value", 7, 7.0, "linear"}
			};
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
			Editor.Draw(Doc, Cursor, Error, [&] {
				bool unchanged = false;
				const bool accepted = studio::ApplyImageGraphDocumentEdit(
					Doc,
					History,
					[&](Document &doc) {
						if (!Editor.PrepareCommit(doc, Error)) return false;
						return !ReconcileSplit ||
							   studio::SetImageGraphSplitOutputCount(doc, "split", SplitCount, Error, false);
					},
					&unchanged
				);
				if (accepted || unchanged) Editor.PublishCommit();
				if (accepted) ++Changes;
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
			REQUIRE(Focus("Whole frame"));
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

TEST_CASE("timeline repeated property rows select distinct full key identities", "[studio][timeline_keys]") {
	ScopedImGuiContext existingContext;
	{
		Timeline ui;
		ui.Frame();
		ui.Frame();
		REQUIRE(ui.RowIds.size() == 3);
		CHECK(ui.RowIds[0] != ui.RowIds[1]);
		CHECK(ui.RowIds[1] != ui.RowIds[2]);
		ui.Click(1);
		REQUIRE(ui.Editor.Selection.size() == 1);
		CHECK(ui.Editor.Selection[0].Time == FrameTime{3, 0, false});
		ui.Click(0, true);
		REQUIRE(ui.Editor.Selection.size() == 2);
		ui.Click(1, true);
		REQUIRE(ui.Editor.Selection.size() == 1);
		CHECK(ui.Editor.Selection[0].Time == FrameTime{1, 0, false});
		CHECK_FALSE(ui.History.CanUndo());
	}
	CHECK(ImGui::GetCurrentContext() == existingContext.Context);
}

TEST_CASE("timeline move popup resolves collision in one undo and redo", "[studio][timeline_keys]") {
	Timeline ui;
	ui.Frame();
	ui.Frame();
	const auto before = ui.Doc;
	ui.Click(0);
	ui.Click(1, true);
	ui.Button("Move keys");
	REQUIRE(ui.Editor.Active);
	ui.Whole("7");
	CHECK(ui.Doc == before);
	ui.Button("Apply");
	INFO(ui.Error.Message);
	REQUIRE_FALSE(ui.Editor.Active);
	REQUIRE(ui.Changes == 1);
	REQUIRE(ui.Doc.Keyframes.size() == 2);
	CHECK(GetFrameTime(ui.Doc.Keyframes[0]) == FrameTime{7, 0, false});
	CHECK(GetFrameTime(ui.Doc.Keyframes[1]) == FrameTime{9, 0, false});
	CHECK(std::get<double>(ui.Doc.Keyframes[0].Data) == 1);
	const auto after = ui.Doc;
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
	CHECK_FALSE(ui.History.CanUndo());
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(ui.Doc == after);
	Document parsed;
	REQUIRE(Read(Write(ui.Doc), parsed, ui.Error) == Status::Ok);
	CHECK(parsed == after);
}

TEST_CASE(
	"timeline copy paste popup cancels and applies without mutating copied keys", "[studio][timeline_keys]"
) {
	Timeline ui;
	ui.Frame();
	ui.Frame();
	const auto before = ui.Doc;
	ui.Click(0);
	ui.Click(1, true);
	ui.Button("Copy keys");
	REQUIRE(ui.Editor.Clipboard.size() == 2);
	CHECK(ui.Doc == before);
	ui.Button("Move keys");
	ui.Whole("8");
	ui.Button("Cancel");
	CHECK(ui.Doc == before);
	CHECK_FALSE(ui.History.CanUndo());
	ui.Button("Paste keys");
	ui.Key(ImGuiKey_Escape);
	ui.Frame();
	CHECK_FALSE(ui.Editor.Active);
	CHECK(ui.Doc == before);
	CHECK_FALSE(ui.History.CanUndo());
	ui.Button("Paste keys");
	ui.Button("Apply");
	INFO(ui.Error.Message);
	REQUIRE(ui.Changes == 1);
	CHECK(ui.Doc.Keyframes.size() == 5);
	CHECK(ui.Editor.Clipboard[0] == before.Keyframes[0]);
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
}

TEST_CASE("timeline staged full-key move refuses changed metadata", "[studio][timeline_keys]") {
	Timeline ui;
	ui.Frame();
	ui.Frame();
	ui.Click(0);
	ui.Button("Move keys");
	ui.Doc.Keyframes[0].Kind = KeyframeKind::Adder;
	const auto changed = ui.Doc;
	ui.Button("Apply");
	CHECK(ui.Editor.Active);
	CHECK(ui.Doc == changed);
	CHECK(ui.Error.Code == Status::InvalidValue);
	CHECK_FALSE(ui.History.CanUndo());
	ui.Button("Cancel");
	CHECK_FALSE(ui.Editor.Active);
}

TEST_CASE(
	"target paste popup chooses another property through measured mouse menu dispatch",
	"[studio][timeline_keys][target_paste]"
) {
	Timeline ui;
	ui.Doc.Nodes[0].Type = "pc.vector2";
	ui.Doc.Nodes[0].Values = {{"x", 0.0}, {"y", 0.0}};
	ui.Doc.Outputs[0].Port = "x";
	for (auto &key : ui.Doc.Keyframes)
		key.Port = "x";
	ui.Doc.Nodes.push_back({"destination", "pc.vector2", "", {}, {{"x", 0.0}, {"y", 0.0}}});
	ui.Frame();
	ui.Frame();
	const auto before = ui.Doc;
	ui.Click(0);
	ui.Click(1, true);
	ui.Button("Copy keys");
	ui.Button("Paste keys");
	REQUIRE(ui.Focus("Target"));
	const auto clickFocused = [&] {
		auto *window = ui.Context->NavWindow;
		REQUIRE(window != nullptr);
		// ImGui navigation rectangles use the scrolled content origin, not window->Pos.
		const auto rect = ImGui::WindowRectRelToAbs(window, window->NavRectRel[ui.Context->NavLayer]);
		const ImVec2 point{(rect.Min.x + rect.Max.x) * .5f, (rect.Min.y + rect.Max.y) * .5f};
		auto &io = ImGui::GetIO();
		io.AddMousePosEvent(point.x, point.y);
		ui.Frame();
		io.AddMouseButtonEvent(0, true);
		ui.Frame();
		io.AddMouseButtonEvent(0, false);
		ui.Frame();
		ui.Frame();
	};
	clickFocused();
	// Walk the actual menu rows; catalogue controls can precede the destination's x and y entries.
	size_t menuRows = 1;
	for (const auto &node : ui.Doc.Nodes) {
		if (const auto *schema = engine::imagegraph::FindSchema(node.Type))
			menuRows += schema->Properties.size();
		for (const auto &input : node.DynamicInputs)
			if (engine::imagegraph::IsAuthoredValueType(input.Type)) ++menuRows;
	}
	bool found = false;
	for (size_t step = 0; step <= menuRows; ++step) {
		auto *window = ui.Context->NavWindow;
		if (window && ui.Context->OpenPopupStack.size() == 2 &&
			ui.Context->NavId == window->GetID("destination.y")) {
			found = true;
			break;
		}
		ui.Key(ImGuiKey_DownArrow);
	}
	CAPTURE(ui.Context->OpenPopupStack.size(), ui.Context->NavId);
	INFO((ui.Context->NavWindow ? ui.Context->NavWindow->Name : "no navigation window"));
	REQUIRE(found);
	clickFocused();
	CHECK(ui.Editor.TargetNode == "destination");
	CHECK(ui.Editor.TargetPort == "y");
	CHECK(ui.Doc == before);
	ui.Button("Apply");
	INFO(ui.Error.Message);
	REQUIRE(ui.Changes == 1);
	REQUIRE(ui.Doc.Keyframes.size() == 5);
	const auto clone = std::find_if(ui.Doc.Keyframes.begin(), ui.Doc.Keyframes.end(), [](const auto &key) {
		return key.NodeId == "destination" && key.Port == "y" &&
			   GetFrameTime(key) == FrameTime{5, .25, false};
	});
	REQUIRE(clone != ui.Doc.Keyframes.end());
	CHECK(std::get<double>(clone->Data) == 1);
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
	CHECK_FALSE(ui.History.CanUndo());
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(ui.Doc.Keyframes.size() == 5);
}

TEST_CASE("timeline refused move Apply keeps popup and pinned keys", "[studio][timeline_history55]") {
	Timeline ui;
	ui.History = studio::ImageGraphHistory(128, 1);
	ui.Frame();
	ui.Frame();
	const auto before = ui.Doc;
	ui.Click(0);
	const auto selected = ui.Editor.Selection;
	ui.Button("Move keys");
	REQUIRE(ui.Editor.Active);
	const auto originals = ui.Editor.Originals;
	ui.Whole("5");
	ui.Button("Apply");
	CHECK(ui.Doc == before);
	CHECK(ui.Changes == 0);
	CHECK_FALSE(ui.History.CanUndo());
	CHECK(ui.Editor.Active);
	CHECK_FALSE(ui.Context->OpenPopupStack.empty());
	CHECK(ui.Editor.Selection == selected);
	CHECK(ui.Editor.Originals == originals);
	ui.History = studio::ImageGraphHistory{};
	ui.Button("Apply");
	REQUIRE(ui.Changes == 1);
	CHECK_FALSE(ui.Editor.Active);
	CHECK(ui.Context->OpenPopupStack.empty());
	const auto edited = ui.Doc;
	REQUIRE(edited != before);
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
	CHECK_FALSE(ui.History.CanUndo());
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(ui.Doc == edited);
	CHECK_FALSE(ui.History.CanRedo());
}

TEST_CASE(
	"timeline refused paste preserves redo clipboard and cancel selection", "[studio][timeline_history55]"
) {
	Timeline ui;
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
	ui.Click(0);
	const auto selected = ui.Editor.Selection;
	ui.Button("Copy keys");
	const auto clipboard = ui.Editor.Clipboard;
	REQUIRE(clipboard.size() == 1);
	ui.Button("Paste keys");
	const auto originals = ui.Editor.Originals;
	ui.Button("Apply");
	CHECK(ui.Doc == before);
	CHECK(ui.Changes == 0);
	CHECK(ui.Editor.Active);
	CHECK_FALSE(ui.Context->OpenPopupStack.empty());
	CHECK(ui.Editor.Selection == selected);
	CHECK(ui.Editor.Originals == originals);
	CHECK(ui.Editor.Clipboard == clipboard);
	CHECK_FALSE(ui.History.CanUndo());
	REQUIRE(ui.History.CanRedo());
	ui.Button("Cancel");
	CHECK_FALSE(ui.Editor.Active);
	CHECK(ui.Editor.Selection == selected);
	CHECK(ui.Editor.Clipboard == clipboard);
	CHECK(ui.Editor.PreparedSelection.empty());
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(ui.Doc == priorEdit);
}

TEST_CASE("timeline unchanged Apply closes popup without consuming undo", "[studio][timeline_history55]") {
	Timeline ui;
	ui.History = studio::ImageGraphHistory(128, 1);
	ui.Frame();
	ui.Frame();
	const auto before = ui.Doc;
	ui.Click(0);
	const auto selected = ui.Editor.Selection;
	ui.Button("Move keys");
	REQUIRE(ui.Editor.Active);
	ui.Button("Apply");
	CHECK(ui.Doc == before);
	CHECK(ui.Changes == 0);
	CHECK(ui.Editor.Selection == selected);
	CHECK_FALSE(ui.Editor.Active);
	CHECK(ui.Context->OpenPopupStack.empty());
	CHECK_FALSE(ui.History.CanUndo());
	CHECK_FALSE(ui.History.CanRedo());
}

TEST_CASE("timeline no-op follows actual split reconciliation admission", "[studio][timeline_history55]") {
	Timeline ui;
	Node split;
	split.Id = "split";
	split.Type = "pc.array_split";
	ui.Doc.Nodes.push_back(std::move(split));
	REQUIRE(studio::SetImageGraphSplitOutputCount(ui.Doc, "split", 1, ui.Error, false));
	ui.ReconcileSplit = true;
	const auto before = ui.Doc;
	ui.Frame();
	ui.Frame();
	ui.Click(0);
	const auto selected = ui.Editor.Selection;
	ui.Button("Move keys");
	const auto originals = ui.Editor.Originals;
	SECTION("rejected reconciliation preserves staged popup") {
		ui.SplitCount = Limits::MaximumArrayElements + 1;
		ui.Button("Apply");
		CHECK(ui.Doc == before);
		CHECK(ui.Changes == 0);
		CHECK(ui.Error.Code == Status::LimitExceeded);
		CHECK(ui.Editor.Active);
		CHECK_FALSE(ui.Context->OpenPopupStack.empty());
		CHECK(ui.Editor.Originals == originals);
		CHECK(ui.Editor.Selection == selected);
		CHECK_FALSE(ui.History.CanUndo());
		ui.Button("Cancel");
		CHECK(ui.Doc == before);
	}
	SECTION("successful reconciliation resize requires single undo") {
		ui.SplitCount = 3;
		ui.Button("Apply");
		REQUIRE(ui.Changes == 1);
		CHECK_FALSE(ui.Editor.Active);
		CHECK(ui.Context->OpenPopupStack.empty());
		REQUIRE(ui.Doc.Nodes.back().DynamicOutputs.size() == 2);
		const auto resized = ui.Doc;
		REQUIRE(ui.History.Undo(ui.Doc));
		CHECK(ui.Doc == before);
		CHECK_FALSE(ui.History.CanUndo());
		REQUIRE(ui.History.Redo(ui.Doc));
		CHECK(ui.Doc == resized);
	}
	SECTION("successful unchanged reconciliation closes without undo") {
		ui.SplitCount = 1;
		ui.Button("Apply");
		CHECK(ui.Doc == before);
		CHECK(ui.Changes == 0);
		CHECK_FALSE(ui.Editor.Active);
		CHECK(ui.Context->OpenPopupStack.empty());
		CHECK(ui.Editor.Selection == selected);
		CHECK_FALSE(ui.History.CanUndo());
	}
}
