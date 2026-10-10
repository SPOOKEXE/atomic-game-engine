// The indexed Output list compared with its original full row submission.
#include <engine/core/Log.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/ui/Fonts.hpp>
#include <engine/ui/Theme.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <imgui.h>
#include <imgui_internal.h>
#include <studio/Config.hpp>
#include <studio/Editor.hpp>
#include <studio/Widgets.hpp>
#include <vector>

TEST_SUITE_ID("studio.output")
TEST_DEPENDS("engine.ui.interface")

namespace studio {
	struct ToolsProbe {
		static bool OutputInitialise(Editor &editor) {
			engine::ui::InterfaceSettings settings;
			settings.DisplayWidth = 1280;
			settings.DisplayHeight = 720;
			editor.ShowOutput = true;
			return editor.Interface.Initialise(editor.Renderer, nullptr, settings);
		}
		static void OutputAppend(Editor &editor, std::string text, engine::core::LogLevel level) {
			editor.Output.push_back({std::move(text), level, editor.NextOutputSerial++});
		}
		static void OutputSeed(Editor &editor) {
			for (size_t index = 0; index < 80; ++index) {
				std::string text = (index % 2 == 0 ? "needle " : "ordinary ") + std::to_string(index);
				if (index % 7 == 0) text += "\nsecond line\n\nlast\tcolumn";
				if (index == 30) text += std::string(64 * 1024, 'x');
				if (index == 42) text += "##hidden-id";
				const auto level = index % 3 == 0	? engine::core::LogLevel::Warning
								   : index % 3 == 1 ? engine::core::LogLevel::Error
													: engine::core::LogLevel::Info;
				OutputAppend(editor, std::move(text), level);
			}
		}
		static ImGuiWindow *OutputChild() {
			auto *window = ImGui::FindWindowByName("Output");
			REQUIRE(window != nullptr);
			for (auto *child : window->DC.ChildWindows) {
				if (child->ChildId == window->GetID("##lines")) return child;
			}
			FAIL("Output child was not submitted");
			return nullptr;
		}
		static void OutputDraw(Editor &editor, float scroll, bool full = false) {
			// Reuse the real child's identity, bounds and scrollbar state. Only
			// replace its contents with the pre-index full submission for parity.
			const auto *previous = full ? OutputChild() : nullptr;
			const ImVec2 childPos = previous ? previous->Pos : ImVec2{};
			const ImVec2 childSize = previous ? previous->Size : ImVec2{};
			editor.Interface.Begin(1.0f / 60.0f);
			ImGui::GetIO().IniFilename = nullptr;
			ImGui::SetNextWindowPos({20, 20}, ImGuiCond_Always);
			ImGui::SetNextWindowSize({980, 600}, ImGuiCond_Always);
			if (!full) {
				// Child scroll is applied by BeginChild on this frame.
				if (auto *window = ImGui::FindWindowByName("Output")) {
					for (auto *child : window->DC.ChildWindows)
						if (child->ChildId == window->GetID("##lines")) ImGui::SetScrollY(child, scroll);
				}
				editor.DrawOutput();
			} else {
				ImGui::Begin("Output");
				ImGui::SetCursorScreenPos(childPos);
				ImGui::SetNextWindowScroll({0, scroll});
				if (ImGui::BeginChild("##lines", childSize, ImGuiChildFlags_None)) {
					const engine::ui::ScopedFont font(
						engine::ui::Typeface::Monospace, engine::ui::TextSize::Small, editor.OutputZoom
					);
					size_t shown = 0;
					for (const auto &message : editor.Output) {
						const bool error = message.Level == engine::core::LogLevel::Error;
						const bool warning = message.Level == engine::core::LogLevel::Warning;
						if (error ? !editor.ShowErrors : warning ? !editor.ShowWarnings : !editor.ShowInfo)
							continue;
						int score = 0;
						if (!editor.OutputFilter.empty() &&
							!FuzzyMatch(editor.OutputFilter, message.Text, score))
							continue;
						++shown;
						const unsigned int colour = error	  ? engine::ui::ErrorColour()
													: warning ? engine::ui::WarningColour()
															  : 0;
						if (colour != 0) ImGui::PushStyleColor(ImGuiCol_Text, colour);
						ImGui::PushID(static_cast<int>(message.Serial));
						ImGui::Selectable(
							message.Text.c_str(),
							editor.OutputSelected(message.Serial),
							ImGuiSelectableFlags_AllowOverlap
						);
						ImGui::PopID();
						if (colour != 0) ImGui::PopStyleColor();
					}
					if (shown != editor.Output.size()) {
						ImGui::TextDisabled(
							"- %zu of %zu lines, %zu hidden by the filter",
							shown,
							editor.Output.size(),
							editor.Output.size() - shown
						);
					}
					if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1) ImGui::SetScrollHereY(1);
				}
				ImGui::EndChild();
				ImGui::End();
			}
			editor.Interface.End();
		}
		static void OutputFilterSet(Editor &editor, std::string filter, bool info = true) {
			editor.OutputFilter = std::move(filter);
			editor.ShowInfo = info;
		}
		static void OutputZoomSet(Editor &editor, float zoom) {
			editor.OutputZoom = zoom;
		}
		static void OutputClear(Editor &editor) {
			editor.Output.clear();
		}
		static void OutputTrim(Editor &editor) {
			editor.Output.pop_front();
		}
		static size_t OutputDisplayed(const Editor &editor) {
			return editor.OutputRows.Rows.size();
		}
		static ImVec2 OutputRowPoint(const Editor &editor, size_t visibleRow) {
			const auto *child = OutputChild();
			const auto &row = editor.OutputRows.Rows.at(visibleRow);
			return {
				child->InnerRect.Min.x + 40, editor.OutputRows.LayoutOrigin + row.Offset + row.Height * .5f
			};
		}
		static uint64_t OutputRowSerial(const Editor &editor, size_t visibleRow) {
			return editor.Output[editor.OutputRows.Rows.at(visibleRow).MessageIndex].Serial;
		}
		static uint64_t OutputSelectedHead(const Editor &editor) {
			return editor.OutputHead;
		}
		static size_t OutputCopy(Editor &editor) {
			return editor.CopyOutputSelection();
		}
	};
}

namespace {
	struct OutputConfiguration {
		std::filesystem::path Previous = studio::ConfigRoot();
		OutputConfiguration() {
			const auto scratch = std::filesystem::temp_directory_path() / "studio-output-parity";
			std::filesystem::create_directories(scratch);
			studio::SetConfigRoot(scratch);
		}
		~OutputConfiguration() {
			studio::SetConfigRoot(Previous);
		}
	};
	struct OutputFixture {
		OutputConfiguration Configuration;
		studio::Editor Editor;
		OutputFixture() {
			REQUIRE(studio::ToolsProbe::OutputInitialise(Editor));
			studio::ToolsProbe::OutputSeed(Editor);
		}
	};

	void CheckOutputParity(studio::Editor &editor, float scroll) {
		for (int warm = 0; warm < 4; ++warm)
			studio::ToolsProbe::OutputDraw(editor, scroll);
		const auto *child = studio::ToolsProbe::OutputChild();
		const ImVec2 content = child->ContentSize;
		const ImVec2 cursorMaximum = child->DC.CursorMaxPos;
		const ImVec2 maximum = child->ScrollMax;
		const float target = child->ScrollTarget.y;
		const std::vector<ImDrawVert> vertices(
			child->DrawList->VtxBuffer.begin(), child->DrawList->VtxBuffer.end()
		);
		const std::vector<ImDrawIdx> indices(
			child->DrawList->IdxBuffer.begin(), child->DrawList->IdxBuffer.end()
		);
		studio::ToolsProbe::OutputDraw(editor, scroll, true);
		child = studio::ToolsProbe::OutputChild();
		CHECK(child->DC.CursorMaxPos.x == cursorMaximum.x);
		CHECK(child->DC.CursorMaxPos.y == cursorMaximum.y);
		CHECK(child->ContentSize.x == content.x);
		CHECK(child->ContentSize.y == content.y);
		CHECK(child->ScrollMax.x == maximum.x);
		CHECK(child->ScrollMax.y == maximum.y);
		CHECK(child->ScrollTarget.y == target);
		CHECK(
			std::equal(
				vertices.begin(),
				vertices.end(),
				child->DrawList->VtxBuffer.begin(),
				child->DrawList->VtxBuffer.end(),
				[](const auto &a, const auto &b) {
					return a.pos.x == b.pos.x && a.pos.y == b.pos.y && a.uv.x == b.uv.x && a.uv.y == b.uv.y &&
						   a.col == b.col;
				}
			)
		);
		CHECK(
			std::equal(
				indices.begin(),
				indices.end(),
				child->DrawList->IdxBuffer.begin(),
				child->DrawList->IdxBuffer.end()
			)
		);
	}
}

TEST_CASE(
	"Output index preserves full submission pixels, extents and tail targets", "[studio][output][parity]"
) {
	OutputFixture fixture;
	auto &editor = fixture.Editor;
	for (const float scroll : {0.0f, 320.0f, 100000.0f})
		CheckOutputParity(editor, scroll);
	studio::ToolsProbe::OutputFilterSet(editor, "needle", false);
	CheckOutputParity(editor, 200);
	studio::ToolsProbe::OutputZoomSet(editor, 1.25f);
	ImGui::GetStyle().ItemSpacing.y = 3.5f;
	CheckOutputParity(editor, 321.5f);
	studio::ToolsProbe::OutputAppend(editor, "needle appended\nsecond line", engine::core::LogLevel::Error);
	CheckOutputParity(editor, 100000);
	studio::ToolsProbe::OutputTrim(editor);
	CheckOutputParity(editor, 0);
	studio::ToolsProbe::OutputClear(editor);
	CheckOutputParity(editor, 0);
	CHECK(studio::ToolsProbe::OutputDisplayed(editor) == 0);
}

TEST_CASE("Output mouse selection and copying keep filtered serial identity", "[studio][output][input]") {
	OutputFixture fixture;
	auto &editor = fixture.Editor;
	studio::ToolsProbe::OutputFilterSet(editor, "needle");
	for (int warm = 0; warm < 4; ++warm)
		studio::ToolsProbe::OutputDraw(editor, 0);
	const auto point = studio::ToolsProbe::OutputRowPoint(editor, 1);
	const auto serial = studio::ToolsProbe::OutputRowSerial(editor, 1);
	auto &io = ImGui::GetIO();
	io.AddMousePosEvent(point.x, point.y);
	studio::ToolsProbe::OutputDraw(editor, 0);
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	studio::ToolsProbe::OutputDraw(editor, 0);
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	studio::ToolsProbe::OutputDraw(editor, 0);
	REQUIRE(studio::ToolsProbe::OutputSelectedHead(editor) == serial);
	CHECK(studio::ToolsProbe::OutputCopy(editor) == 1);
	CHECK(std::string(ImGui::GetClipboardText()) == "needle 2");
	const auto next = studio::ToolsProbe::OutputRowPoint(editor, 3);
	io.AddKeyEvent(ImGuiMod_Shift, true);
	io.AddMousePosEvent(next.x, next.y);
	studio::ToolsProbe::OutputDraw(editor, 0);
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	studio::ToolsProbe::OutputDraw(editor, 0);
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	studio::ToolsProbe::OutputDraw(editor, 0);
	io.AddKeyEvent(ImGuiMod_Shift, false);
	CHECK(studio::ToolsProbe::OutputCopy(editor) == 3);
	CHECK(std::string(ImGui::GetClipboardText()) == "needle 2\nneedle 4\nneedle 6");
}
