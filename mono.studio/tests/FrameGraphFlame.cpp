// Flame row indexing and the production panel's clipped/full draw parity.
#include "FrameGraphFlame.hpp"

#include <engine/testing/Suite.hpp>
#include <engine/ui/Theme.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <imgui.h>
#include <imgui_internal.h>
#include <numeric>
#include <studio/Config.hpp>
#include <studio/Editor.hpp>

TEST_SUITE_ID("studio.framegraphflame")
TEST_DEPENDS("engine.core.framegraph")

namespace studio {
	struct ToolsProbe {
		using FlameCache = Editor::FrameGraphView::FlameCache;
		static bool FlameInitialise(Editor &editor) {
			engine::ui::InterfaceSettings settings;
			settings.DisplayWidth = 1280;
			settings.DisplayHeight = 720;
			editor.ShowFrameGraph = true;
			editor.FrameGraphState.Paused = true;
			editor.FrameGraphState.Mode = DiagnosticAggregation::Average;
			return editor.Interface.Initialise(editor.Renderer, nullptr, settings);
		}
		static void FlameSeed(Editor &editor) {
			auto &view = editor.FrameGraphState;
			view.Spans.clear();
			view.FrameMilliseconds = 12;
			view.PublishedFrames = 5;
			for (uint32_t world = 0; world < 8; world++) {
				const uint32_t parent = static_cast<uint32_t>(view.Spans.size());
				view.Spans.push_back(
					{.Name = "world",
					 .Milliseconds = 10,
					 .SelfMilliseconds = 1,
					 .Category = engine::core::ProfileCategory::ECS,
					 .Owner = engine::core::ProfileOwner::Studio}
				);
				for (uint32_t child = 0; child < 12; child++) {
					view.Spans.push_back(
						{.Name = "stage",
						 .Depth = 1,
						 .Parent = parent,
						 .StartMilliseconds = static_cast<float>(child) * .25f,
						 .Milliseconds = 2,
						 .SelfMilliseconds = .1f,
						 .Category = engine::core::ProfileCategory::Render,
						 .Owner = child % 2 == 0 ? engine::core::ProfileOwner::Studio
												 : engine::core::ProfileOwner::Engine}
					);
				}
				view.Spans.push_back(
					{.Name = "gpu query",
					 .Milliseconds = .125f * static_cast<float>(world + 1),
					 .Category = engine::core::ProfileCategory::Gpu,
					 .Owner = engine::core::ProfileOwner::Studio,
					 .Reported = true,
					 .Occurrences = world + 2}
				);
			}
			view.DisplayDirty = true;
		}
		static uint64_t FlameDraw(Editor &editor, float width, float height, float scroll) {
			editor.Interface.Begin(1.0f / 60.0f);
			ImGui::GetIO().IniFilename = nullptr;
			ImGui::SetNextWindowPos({20, 20}, ImGuiCond_Always);
			ImGui::SetNextWindowSize({width, height}, ImGuiCond_Always);
			ImGui::SetNextWindowScroll({0, scroll});
			editor.DrawFrameGraph();
			editor.Interface.End();
			return editor.Interface.Signature();
		}
		static auto &FlameCurrent(Editor &editor) {
			auto &view = editor.FrameGraphState;
			return view.FocusRoot == engine::core::FrameGraph::NO_PARENT ? view.Flame : view.FocusedFlame;
		}
		static void FlameClipAgain(Editor &editor) {
			for (auto &pass : FlameCurrent(editor).Passes)
				pass.Prepared = false;
		}
		static void FlameFullTraversal(Editor &editor) {
			// Keep the prepared row stamp, then paint all original indices through
			// the same production intersection guard and hover logic.
			for (auto &pass : FlameCurrent(editor).Passes) {
				pass.Visible = pass.OriginalIndices;
			}
		}
		static ImVec2 FlameBarCentre() {
			const auto *window = ImGui::FindWindowByName("Frame Graph");
			REQUIRE(window != nullptr);
			const auto colour = frame_graph_detail::FlameCategoryPalette(
				engine::ui::AccentColour()
			)[static_cast<size_t>(engine::core::ProfileCategory::ECS)];
			const auto &vertices = window->DrawList->VtxBuffer;
			for (int index = 0; index + 3 < vertices.Size; index++) {
				if (vertices[index].col != colour || vertices[index + 2].col != colour) continue;
				const ImVec2 upper = vertices[index].pos;
				const ImVec2 lower = vertices[index + 2].pos;
				const ImVec2 centre((upper.x + lower.x) * .5f, (upper.y + lower.y) * .5f);
				if (lower.x > upper.x && lower.y > upper.y && window->ClipRect.Contains(centre))
					return centre;
			}
			FAIL("no visible measured ECS flame bar");
			return {};
		}
		static bool FlameFocused(const Editor &editor) {
			return editor.FrameGraphState.FocusRoot != engine::core::FrameGraph::NO_PARENT;
		}
		static void FlameFilter(Editor &editor, engine::core::ProfileOwner owner) {
			editor.FrameGraphState.OwnerFilter = owner;
			editor.FrameGraphState.DisplayDirty = true;
		}
		static void FlameFocus(Editor &editor, uint32_t root) {
			auto &view = editor.FrameGraphState;
			view.FocusRoot = root;
			if (root == engine::core::FrameGraph::NO_PARENT) return;
			FocusDiagnosticSpans(view.DisplaySpans, root, view.FocusedSpans, view.FocusedSourceIndices);
			view.FocusedDisplayRows = LayoutDiagnosticRows(view.FocusedSpans, view.FocusedRows);
			const auto &selected =
				view.OwnerFilter == engine::core::ProfileOwner::All ? view.Spans : view.FilteredSpans;
			frame_graph_detail::BuildFlameCache(
				view.FocusedSpans,
				view.FocusedRows,
				view.FocusedSourceIndices,
				selected.size(),
				view.FocusedDisplayRows,
				view.FocusedFlame
			);
		}
		static void FlameResume(Editor &editor) {
			editor.FrameGraphState.Paused = false;
			editor.FrameGraphState.Interval = 0;
		}
	};
}

TEST_CASE(
	"flame buckets retain original pass order and selected GPU sample identity",
	"[studio][diagnostics][flame]"
) {
	using engine::core::ProfileCategory;
	const std::array<studio::DiagnosticSpan, 6> spans{{
		{.Name = "cpu", .Depth = 0, .Milliseconds = 3},
		{.Name = "gpu first", .Milliseconds = 2, .Category = ProfileCategory::Gpu, .Occurrences = 7},
		{.Name = "cpu child", .Depth = 1, .Milliseconds = 1},
		{.Name = "gpu tied", .Milliseconds = 2, .Category = ProfileCategory::Gpu, .Occurrences = 99},
		{.Name = "gap", .Depth = 1, .Milliseconds = 1},
		{.Name = "gap", .Depth = 2, .Milliseconds = 1},
	}};
	const std::array<uint32_t, 6> rows{2, 1, 4, 0, 4, 0};
	studio::ToolsProbe::FlameCache cache;
	studio::frame_graph_detail::BuildFlameCache(spans, rows, {}, 4, 5, cache);
	CHECK(cache.CpuRows == 5);
	CHECK(cache.GpuRows == 2);
	CHECK(cache.GpuMaximumMilliseconds == 2);
	CHECK(cache.GpuMaximumSamples == 7);
	CHECK(cache.HasOverlap);
	for (auto &pass : cache.Passes)
		studio::frame_graph_detail::PrepareFlameRows(pass, 0, 10, 0, 100);
	CHECK(cache.Passes[0].Visible == std::vector<uint32_t>{4, 5});
	CHECK(cache.Passes[1].Visible == std::vector<uint32_t>{0, 2});
	CHECK(cache.Passes[2].Visible == std::vector<uint32_t>{1, 3});
	// Focused indices name full display sources. Classification uses that
	// mapping, rather than the position within the focused list.
	const std::array<uint32_t, 6> sources{5, 1, 4, 3, 0, 2};
	studio::frame_graph_detail::BuildFlameCache(spans, rows, sources, 4, 5, cache);
	for (auto &pass : cache.Passes)
		studio::frame_graph_detail::PrepareFlameRows(pass, 0, 10, 0, 100);
	CHECK(cache.Passes[0].Visible == std::vector<uint32_t>{0, 2});
	CHECK(cache.Passes[1].Visible == std::vector<uint32_t>{4, 5});
	for (auto &pass : cache.Passes) {
		studio::frame_graph_detail::PrepareFlameRows(pass, 0, 10, 20, 20);
		CHECK(pass.Visible.empty());
		studio::frame_graph_detail::PrepareFlameRows(pass, 0, 10, 40, 10);
		CHECK(pass.Visible.empty());
		studio::frame_graph_detail::PrepareFlameRows(pass, 0, 10, -20, 100);
		CHECK(pass.Visible == pass.OriginalIndices);
	}
	studio::frame_graph_detail::BuildFlameCache({}, {}, {}, 0, 0, cache);
	for (auto &pass : cache.Passes) {
		studio::frame_graph_detail::PrepareFlameRows(pass, 0, 10, 0, 100);
		CHECK(pass.Visible.empty());
	}
}

TEST_CASE(
	"row-clipped flame painting matches full traversal across panel changes",
	"[studio][diagnostics][flame][input]"
) {
	const auto previous = studio::ConfigRoot();
	const auto scratch = std::filesystem::temp_directory_path() / "studio-flame-parity";
	std::filesystem::create_directories(scratch);
	studio::SetConfigRoot(scratch);
	struct Restore {
		std::filesystem::path Previous;
		~Restore() {
			studio::SetConfigRoot(Previous);
		}
	} restore{previous};
	studio::Editor editor;
	REQUIRE(studio::ToolsProbe::FlameInitialise(editor));
	studio::ToolsProbe::FlameSeed(editor);
	const auto parity = [&](float width, float height, float scroll) {
		for (size_t warm = 0; warm < 3; warm++)
			studio::ToolsProbe::FlameDraw(editor, width, height, scroll);
		studio::ToolsProbe::FlameClipAgain(editor);
		const uint64_t clipped = studio::ToolsProbe::FlameDraw(editor, width, height, scroll);
		studio::ToolsProbe::FlameFullTraversal(editor);
		CHECK(studio::ToolsProbe::FlameDraw(editor, width, height, scroll) == clipped);
	};
	parity(1200, 650, 0);
	const ImVec2 bar = studio::ToolsProbe::FlameBarCentre();
	ImGui::GetIO().AddMousePosEvent(bar.x, bar.y);
	parity(1200, 650, 0);
	ImGui::GetIO().AddMouseButtonEvent(0, true);
	studio::ToolsProbe::FlameDraw(editor, 1200, 650, 0);
	ImGui::GetIO().AddMouseButtonEvent(0, false);
	studio::ToolsProbe::FlameDraw(editor, 1200, 650, 0);
	REQUIRE(studio::ToolsProbe::FlameFocused(editor));
	ImGui::GetIO().AddMousePosEvent(-100, -100);
	parity(1200, 650, 0);
	studio::ToolsProbe::FlameFocus(editor, engine::core::FrameGraph::NO_PARENT);
	const auto oldColours = engine::ui::GlobalColours();
	struct RestoreColours {
		engine::ui::ThemeColours Colours;
		~RestoreColours() {
			engine::ui::SetGlobalColours(Colours);
		}
	} restoreColours{oldColours};
	for (const unsigned int accent : {IM_COL32(255, 40, 60, 255), IM_COL32(40, 220, 130, 255)}) {
		auto colours = oldColours;
		colours[engine::ui::ThemeColour::Accent] = accent;
		engine::ui::SetGlobalColours(colours);
		parity(1200, 650, 0);
	}
	engine::ui::SetGlobalColours(oldColours);
	parity(800, 300, 190);
	parity(800, 300, 420);
	studio::ToolsProbe::FlameFocus(editor, 0);
	parity(900, 400, 0);
	studio::ToolsProbe::FlameFocus(editor, engine::core::FrameGraph::NO_PARENT);
	parity(900, 400, 260);
	studio::ToolsProbe::FlameFilter(editor, engine::core::ProfileOwner::Studio);
	parity(900, 400, 0);
	ImGui::GetStyle().FramePadding.y += 3;
	parity(720, 380, 120);
	studio::ToolsProbe::FlameFilter(editor, engine::core::ProfileOwner::All);
	studio::ToolsProbe::FlameResume(editor);
	const bool enabled = engine::core::FrameGraph::IsEnabled();
	engine::core::FrameGraph::SetEnabled(true);
	struct RestoreGraph {
		bool Enabled;
		~RestoreGraph() {
			engine::core::FrameGraph::SetEnabled(Enabled);
		}
	} restoreGraph{enabled};
	engine::core::FrameGraph::BeginFrame();
	engine::core::FrameGraph::Report("resumed", engine::core::ProfileCategory::Render, .1f);
	engine::core::FrameGraph::Report("gpu resumed", engine::core::ProfileCategory::Gpu, .025f);
	engine::core::FrameGraph::EndFrame();
	parity(1200, 650, 0);
}

TEST_CASE(
	"the flame palette keeps exact category rounding and refreshes on accent changes",
	"[studio][diagnostics][flame][theme]"
) {
	using engine::core::ProfileCategory;
	for (const unsigned int accent :
		 {IM_COL32(0, 0, 0, 255),
		  IM_COL32(70, 120, 210, 255),
		  IM_COL32(255, 255, 255, 255),
		  IM_COL32(0, 0, 0, 255)}) {
		const auto &palette = studio::frame_graph_detail::FlameCategoryPalette(accent);
		for (size_t index = 0; index < palette.size(); index++) {
			const auto category = static_cast<ProfileCategory>(index);
			CHECK(palette[index] == studio::frame_graph_detail::UncachedFlameColour(category, accent));
		}
		CHECK(palette[static_cast<size_t>(ProfileCategory::Idle)] == IM_COL32(70, 74, 86, 190));
		CHECK(
			studio::frame_graph_detail::UncachedFlameColour(static_cast<ProfileCategory>(255), accent) ==
			studio::frame_graph_detail::UncachedFlameColour(ProfileCategory::Engine, accent)
		);
	}
}
