// Flame row indexing and the production panel's clipped/full draw parity.
#include "FrameGraphFlame.hpp"

#include "FrameGraphTable.hpp"

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
#include <vector>

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
		static void TableSeed(Editor &editor) {
			auto &view = editor.FrameGraphState;
			view.Paused = true;
			view.OwnerFilter = engine::core::ProfileOwner::All;
			view.FrameMilliseconds = 10.0f;
			view.IdleMilliseconds = 0.0f;
			view.Spans = {
				{.Name = "world",
				 .Milliseconds = 6.0f,
				 .IdleMilliseconds = 1.0f,
				 .Owner = engine::core::ProfileOwner::Studio},
				{.Name = "stage alpha",
				 .Depth = 1,
				 .Parent = 0,
				 .Milliseconds = 3.0f,
				 .Owner = engine::core::ProfileOwner::Studio},
				{.Name = "script",
				 .Milliseconds = 3.0f,
				 .IdleMilliseconds = 2.0f,
				 .Owner = engine::core::ProfileOwner::Engine},
			};
			view.DisplayDirty = true;
		}
		static void TableFilterStudio(Editor &editor) {
			auto &view = editor.FrameGraphState;
			view.OwnerFilter = engine::core::ProfileOwner::Studio;
			view.DisplayDirty = true;
		}
		static void TableChangeStageTime(Editor &editor, float milliseconds) {
			auto &view = editor.FrameGraphState;
			view.Spans[1].Milliseconds = milliseconds;
			view.DisplayDirty = true;
		}
		static const std::vector<size_t> &TableOrder(const Editor &editor) {
			return editor.FrameGraphState.TableOrder;
		}
		static uint32_t TableParent(const Editor &editor, size_t index) {
			return editor.FrameGraphState.Spans[index].Parent;
		}
		static uint32_t TableDepth(const Editor &editor, size_t index) {
			return editor.FrameGraphState.Spans[index].Depth;
		}
		static ImVec2 TableHeaderPosition(int column) {
			ImGuiWindow *window = ImGui::FindWindowByName("Frame Graph");
			REQUIRE(window != nullptr);
			const ImGuiID tableId = window->GetID("##spans");
			ImGuiTable *table = ImGui::GetCurrentContext()->Tables.GetByKey(tableId);
			REQUIRE(table != nullptr);
			REQUIRE(column >= 0);
			REQUIRE(column < table->ColumnsCount);
			const ImGuiTableColumn &tableColumn = table->Columns[column];
			const float headerHeight = table->InstanceDataFirst.LastTopHeadersRowHeight;
			return ImVec2(
				(tableColumn.MinX + tableColumn.MaxX) * 0.5f, table->OuterRect.Min.y + headerHeight * 0.5f
			);
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
		static void TableClick(Editor &editor, int column) {
			const ImVec2 position = TableHeaderPosition(column);
			ImGui::GetIO().AddMousePosEvent(position.x, position.y);
			FlameDraw(editor, 1000, 600, 0);
			ImGui::GetIO().AddMouseButtonEvent(0, true);
			FlameDraw(editor, 1000, 600, 0);
			ImGui::GetIO().AddMouseButtonEvent(0, false);
			FlameDraw(editor, 1000, 600, 0);
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
	"frame graph table sorts displayed values without changing captured spans", "[studio][diagnostics][table]"
) {
	using engine::core::ProfileCategory;
	std::array<studio::DiagnosticSpan, 6> spans{{
		{.Name = "zeta", .Depth = 1, .Parent = 1, .Milliseconds = 5, .IdleMilliseconds = 1},
		{.Name = "alpha", .Milliseconds = 4},
		{.Name = "alpha", .Milliseconds = 2, .IdleMilliseconds = 1},
		{.Name = "gpu", .Milliseconds = -2, .Category = ProfileCategory::Gpu},
		{.Name = "reported", .Milliseconds = 20, .Reported = true},
		{.Name = "idle", .Milliseconds = 3, .IdleMilliseconds = 2},
	}};
	std::vector<size_t> order;
	using studio::frame_graph_table_detail::BuildOrder;
	using studio::frame_graph_table_detail::Sort;
	constexpr float frameBusy = 10.0f;

	const Sort nameAscending{0, ImGuiSortDirection_Ascending};
	BuildOrder(spans, frameBusy, &nameAscending, order);
	CHECK(order == std::vector<size_t>{1, 2, 3, 5, 4, 0});

	const Sort busyDescending{1, ImGuiSortDirection_Descending};
	BuildOrder(spans, frameBusy, &busyDescending, order);
	CHECK(order == std::vector<size_t>{4, 0, 1, 2, 5, 3});
	const Sort busyAscending{1, ImGuiSortDirection_Ascending};
	BuildOrder(spans, frameBusy, &busyAscending, order);
	CHECK(order == std::vector<size_t>{3, 2, 5, 0, 1, 4});
	const Sort idleDescending{2, ImGuiSortDirection_Descending};
	BuildOrder(spans, frameBusy, &idleDescending, order);
	CHECK(order == std::vector<size_t>{5, 0, 2, 1, 3, 4});
	const Sort idleAscending{2, ImGuiSortDirection_Ascending};
	BuildOrder(spans, frameBusy, &idleAscending, order);
	CHECK(order == std::vector<size_t>{1, 3, 4, 0, 2, 5});

	const Sort shareDescending{3, ImGuiSortDirection_Descending};
	BuildOrder(spans, frameBusy, &shareDescending, order);
	CHECK(order == std::vector<size_t>{0, 1, 2, 5, 3, 4});
	const Sort shareAscending{3, ImGuiSortDirection_Ascending};
	BuildOrder(spans, frameBusy, &shareAscending, order);
	CHECK(order == std::vector<size_t>{2, 5, 0, 1, 3, 4});

	BuildOrder(spans, frameBusy, nullptr, order);
	CHECK(order == std::vector<size_t>{0, 1, 2, 3, 4, 5});
	CHECK(spans[0].Name == "zeta");
	CHECK(spans[0].Parent == 1);
	CHECK(spans[0].Depth == 1);
	CHECK(spans[1].Name == "alpha");
	CHECK_FALSE(studio::frame_graph_table_detail::Share(spans[3], frameBusy).has_value());
	CHECK_FALSE(studio::frame_graph_table_detail::Share(spans[4], frameBusy).has_value());
	CHECK(studio::frame_graph_table_detail::BusyMilliseconds(spans[3]) == 0.0f);
	CHECK(studio::frame_graph_table_detail::BusyMilliseconds(spans[0]) == 4.0f);
	CHECK(studio::frame_graph_table_detail::Share(spans[1], 0.0f).value() == 0.0f);
}

TEST_CASE("frame graph table headers cycle row sorting while paused", "[studio][diagnostics][table][input]") {
	const auto previous = studio::ConfigRoot();
	const auto scratch = std::filesystem::temp_directory_path() / "studio-framegraph-table-sort";
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
	studio::ToolsProbe::TableSeed(editor);
	for (size_t warm = 0; warm < 3; warm++) {
		studio::ToolsProbe::FlameDraw(editor, 1000, 600, 0);
	}
	CHECK(studio::ToolsProbe::TableOrder(editor) == std::vector<size_t>{0, 1, 2});

	studio::ToolsProbe::TableClick(editor, 0);
	CHECK(studio::ToolsProbe::TableOrder(editor) == std::vector<size_t>{2, 1, 0});
	studio::ToolsProbe::FlameDraw(editor, 1000, 600, 0);
	CHECK(studio::ToolsProbe::TableOrder(editor) == std::vector<size_t>{2, 1, 0});

	studio::ToolsProbe::TableClick(editor, 1);
	CHECK(studio::ToolsProbe::TableOrder(editor) == std::vector<size_t>{0, 1, 2});
	studio::ToolsProbe::TableClick(editor, 1);
	CHECK(studio::ToolsProbe::TableOrder(editor) == std::vector<size_t>{2, 1, 0});
	studio::ToolsProbe::TableClick(editor, 1);
	CHECK(studio::ToolsProbe::TableOrder(editor) == std::vector<size_t>{0, 1, 2});
	studio::ToolsProbe::TableFilterStudio(editor);
	studio::ToolsProbe::FlameDraw(editor, 1000, 600, 0);
	CHECK(studio::ToolsProbe::TableOrder(editor) == std::vector<size_t>{0, 1});
	studio::ToolsProbe::TableChangeStageTime(editor, 9.0f);
	studio::ToolsProbe::FlameDraw(editor, 1000, 600, 0);
	CHECK(studio::ToolsProbe::TableOrder(editor) == std::vector<size_t>{0, 1});
	studio::ToolsProbe::TableClick(editor, 1);
	CHECK(studio::ToolsProbe::TableOrder(editor) == std::vector<size_t>{1, 0});
	CHECK(studio::ToolsProbe::TableParent(editor, 1) == 0);
	CHECK(studio::ToolsProbe::TableDepth(editor, 1) == 1);
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
