// Flame row indexing and the production panel's clipped/full draw parity.
#include "FrameGraphFlame.hpp"

#include "FrameGraphCaches.hpp"
#include "FrameGraphGeometry.hpp"
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
		static uint64_t GeometryDraw(
			Editor &editor, frame_graph_detail::FlameGeometry &cache, int prefix, bool split = false
		) {
			editor.Interface.Begin(1.0f / 60.0f);
			ImGui::GetIO().IniFilename = nullptr;
			ImGui::SetNextWindowPos({20, 20}, ImGuiCond_Always);
			ImGui::SetNextWindowSize({500, 300}, ImGuiCond_Always);
			ImGui::Begin("geometry fixture", nullptr, ImGuiWindowFlags_NoSavedSettings);
			auto &draw = *ImGui::GetWindowDrawList();
			for (int index = 0; index < prefix; index++)
				draw.AddRectFilled({30, 70}, {35, 75}, IM_COL32_WHITE);
			const auto key = frame_graph_detail::GeometryKey(
				draw,
				split ? 2 : 1,
				{40, 90},
				300,
				20,
				1,
				34,
				false,
				engine::ui::AccentColour(),
				engine::ui::BrightColour()
			);
			frame_graph_detail::PaintGeometry(cache, key, draw, [&] {
				draw.AddRectFilled({40, 90}, {340, 110}, engine::ui::AccentColour());
				if (split) draw.PushClipRect({40, 90}, {200, 110}, true);
				draw.AddText({43, 91}, IM_COL32_WHITE, "retained flame label");
				if (split) draw.PopClipRect();
			});
			ImGui::End();
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
		static uint64_t GeometryHits(Editor &editor) {
			const auto &cache = FlameCurrent(editor);
			return cache.Geometry[0] ? cache.Geometry[0]->Hits : 0;
		}
		static void RepaintGeometry(Editor &editor) {
			for (auto &geometry : FlameCurrent(editor).Geometry) {
				if (geometry) geometry->Ready = false;
			}
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
		static ImVec2
		FlameBarCentre(engine::core::ProfileCategory category = engine::core::ProfileCategory::ECS) {
			const auto *window = ImGui::FindWindowByName("Frame Graph");
			REQUIRE(window != nullptr);
			const auto colour = frame_graph_detail::FlameCategoryPalette(
				engine::ui::AccentColour()
			)[static_cast<size_t>(category)];
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
		static void CacheOpen(Editor &editor) {
			editor.FrameGraphState.ShowCascadedCaches = true;
			editor.ViewportPresentations.resize(1);
		}
		static void CacheRecord(Editor &editor, const engine::render::PresentationDamage &damage) {
			editor.ViewportPresentations[0].CacheProfile().Record(damage, true);
		}
		static uint64_t CacheSampledHits(const Editor &editor) {
			return editor.FrameGraphState.CacheProfile
				.Activities()[static_cast<size_t>(engine::render::PresentationCacheLayer::SceneImage)]
				.Hits;
		}
		static const std::vector<size_t> &CacheOrder(const Editor &editor, bool host) {
			return editor.FrameGraphState.CacheOrder[host ? 1 : 0];
		}
		static void CacheSortingSeed(Editor &editor) {
			CacheOpen(editor);
			auto &profile = editor.ViewportPresentations[0].CacheProfile();
			profile.Reset();
			engine::render::PresentationCacheApplicability applicable;
			applicable.Objects = false;
			profile.Record(
				{.Scene = true, .HostInterface = true, .Objects = true}, true, false, false, applicable
			);
			profile.Record({.Particles = true}, true);
			profile.Record({.Scene = true, .HostInterface = true}, true);
			editor.FrameGraphState.NextCacheProfile = 0;
		}
		static void CacheSortingMutation(Editor &editor) {
			engine::render::PresentationCacheApplicability applicable;
			applicable.Environment = false;
			for (size_t opportunity = 0; opportunity < 4; opportunity++)
				editor.ViewportPresentations[0].CacheProfile().Record({}, true, false, false, applicable);
		}
		static ImVec2 CacheHeaderPosition(bool host, int column) {
			ImGuiWindow *window = ImGui::FindWindowByName("Frame Graph");
			REQUIRE(window != nullptr);
			const ImGuiID group = window->GetID(host ? "host" : "viewport");
			const ImGuiID tableId = ImHashStr("cache activity", 0, group);
			ImGuiTable *table = ImGui::GetCurrentContext()->Tables.GetByKey(tableId);
			REQUIRE(table != nullptr);
			const auto &tableColumn = table->Columns[column];
			const float headerHeight = table->InstanceDataFirst.LastTopHeadersRowHeight;
			return {
				(tableColumn.MinX + tableColumn.MaxX) * 0.5f, table->OuterRect.Min.y + headerHeight * 0.5f
			};
		}
		static void CacheClick(Editor &editor, bool host, int column) {
			const ImVec2 position = CacheHeaderPosition(host, column);
			ImGui::GetIO().AddMousePosEvent(position.x, position.y);
			FlameDraw(editor, 1000, 700, 0);
			ImGui::GetIO().AddMouseButtonEvent(0, true);
			FlameDraw(editor, 1000, 700, 0);
			ImGui::GetIO().AddMouseButtonEvent(0, false);
			FlameDraw(editor, 1000, 700, 0);
		}
		static void CacheExpire(Editor &editor) {
			editor.FrameGraphState.NextCacheProfile = 0;
		}
		static uint32_t FlameFocusRoot(const Editor &editor) {
			return editor.FrameGraphState.FocusRoot;
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
		studio::ToolsProbe::RepaintGeometry(editor);
		CHECK(studio::ToolsProbe::FlameDraw(editor, width, height, scroll) == clipped);
	};
	parity(1200, 650, 0);
	const uint64_t hits = studio::ToolsProbe::GeometryHits(editor);
	const uint64_t cached = studio::ToolsProbe::FlameDraw(editor, 1200, 650, 0);
	CHECK(studio::ToolsProbe::GeometryHits(editor) == hits + 1);
	studio::ToolsProbe::RepaintGeometry(editor);
	CHECK(studio::ToolsProbe::FlameDraw(editor, 1200, 650, 0) == cached);
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
	const uint32_t parentFocus = studio::ToolsProbe::FlameFocusRoot(editor);
	const ImVec2 childBar = studio::ToolsProbe::FlameBarCentre(engine::core::ProfileCategory::Render);
	ImGui::GetIO().AddMousePosEvent(childBar.x, childBar.y);
	studio::ToolsProbe::FlameDraw(editor, 1200, 650, 0);
	ImGui::GetIO().AddMouseButtonEvent(0, true);
	studio::ToolsProbe::FlameDraw(editor, 1200, 650, 0);
	ImGui::GetIO().AddMouseButtonEvent(0, false);
	studio::ToolsProbe::FlameDraw(editor, 1200, 650, 0);
	CHECK(studio::ToolsProbe::FlameFocusRoot(editor) > parentFocus);
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

TEST_CASE(
	"cache display samples without interrupting hidden-page collection", "[studio][diagnostics][cache]"
) {
	struct View {
		engine::render::PresentationCacheProfile CacheProfile;
		uint64_t CacheProfileRevision = 0;
		double NextCacheProfile = 0;
		size_t SampledCacheViewport = SIZE_MAX;
	} view;
	engine::render::PresentationCacheProfile live;
	using engine::render::PresentationCacheLayer;
	using studio::frame_graph_detail::SampleCacheProfile;
	const size_t host = static_cast<size_t>(PresentationCacheLayer::HostInterface);
	const size_t scene = static_cast<size_t>(PresentationCacheLayer::SceneImage);
	// Timing page and hidden panel frames still accumulate, before any display sample.
	for (size_t frame = 0; frame < 128; frame++)
		live.Record({.HostInterface = frame % 4 == 0}, true);
	REQUIRE(SampleCacheProfile(view, live, 0, 10.0));
	CHECK(view.CacheProfile.Activities()[host].Writes == 32);
	CHECK(view.CacheProfile.Activities()[host].Hits == 96);
	CHECK(view.CacheProfile.Activities()[scene].Hits == 128);
	live.Record({.Scene = true, .Objects = true}, true);
	CHECK_FALSE(SampleCacheProfile(view, live, 0, 10.1));
	CHECK_FALSE(view.CacheProfile.LastDamage().Scene);
	CHECK(view.CacheProfile.Activities()[scene].Hits == 128);
	CHECK(SampleCacheProfile(view, live, 0, 10.25));
	CHECK(view.CacheProfile.LastDamage().Scene);
	CHECK(view.CacheProfile.LastDamage().Objects);
	CHECK(view.CacheProfile.Activities()[scene].Writes == 1);
	// Switching viewport is immediate; an ImGui time reset must not stall the view.
	CHECK(SampleCacheProfile(view, live, 1, 10.26));
	CHECK(SampleCacheProfile(view, live, 1, 0.0));
	live.Reset();
	view.NextCacheProfile = 0;
	CHECK(SampleCacheProfile(view, live, 1, 0.01));
	CHECK_FALSE(view.CacheProfile.HasObservations());
	CHECK(view.CacheProfile.Activities()[host].Writes == 0);
}

TEST_CASE(
	"cache page retains geometry while live decisions accumulate", "[studio][diagnostics][cache][input]"
) {
	const auto previous = studio::ConfigRoot();
	const auto scratch = std::filesystem::temp_directory_path() / "studio-cache-display";
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
	ImGui::GetIO().AddMousePosEvent(-100, -100);
	studio::ToolsProbe::CacheOpen(editor);
	studio::ToolsProbe::CacheRecord(editor, {});
	for (size_t warm = 0; warm < 3; warm++)
		studio::ToolsProbe::FlameDraw(editor, 1200, 650, 0);
	const uint64_t steady = studio::ToolsProbe::FlameDraw(editor, 1200, 650, 0);
	const uint64_t sampled = studio::ToolsProbe::CacheSampledHits(editor);
	for (size_t frame = 0; frame < 3; frame++) {
		studio::ToolsProbe::CacheRecord(editor, {});
		CHECK(studio::ToolsProbe::FlameDraw(editor, 1200, 650, 0) == steady);
		CHECK(studio::ToolsProbe::CacheSampledHits(editor) == sampled);
	}
	studio::ToolsProbe::CacheExpire(editor);
	CHECK(studio::ToolsProbe::FlameDraw(editor, 1200, 650, 0) != steady);
	CHECK(studio::ToolsProbe::CacheSampledHits(editor) == sampled + 3);
}

TEST_CASE(
	"retained flame indices rebase and command boundaries decline reuse", "[studio][diagnostics][flame]"
) {
	studio::Editor editor;
	REQUIRE(studio::ToolsProbe::FlameInitialise(editor));
	studio::frame_graph_detail::FlameGeometry cache;
	for (size_t warm = 0; warm < 3; warm++)
		studio::ToolsProbe::GeometryDraw(editor, cache, 1);
	REQUIRE(cache.Ready);
	const uint64_t hits = cache.Hits;
	const uint64_t rebased = studio::ToolsProbe::GeometryDraw(editor, cache, 300);
	CHECK(cache.Hits == hits + 1);
	cache.Ready = false;
	CHECK(studio::ToolsProbe::GeometryDraw(editor, cache, 300) == rebased);
	const uint64_t clipped = studio::ToolsProbe::GeometryDraw(editor, cache, 300, true);
	CHECK_FALSE(cache.Ready);
	CHECK(studio::ToolsProbe::GeometryDraw(editor, cache, 300, true) == clipped);
	CHECK_FALSE(cache.Ready);
}

TEST_CASE(
	"cache table headers sort sampled values and restore cascade order", "[studio][diagnostics][cache][input]"
) {
	const auto previous = studio::ConfigRoot();
	const auto scratch = std::filesystem::temp_directory_path() / "studio-cache-table-sort";
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
	studio::ToolsProbe::CacheSortingSeed(editor);
	for (size_t warm = 0; warm < 3; warm++)
		studio::ToolsProbe::FlameDraw(editor, 1000, 700, 0);
	const std::vector<size_t> cascade = studio::ToolsProbe::CacheOrder(editor, false);
	const std::vector<size_t> hostCascade = studio::ToolsProbe::CacheOrder(editor, true);
	REQUIRE(cascade == std::vector<size_t>{0, 1, 2, 3, 4, 5, 6, 7, 9, 10, 11});
	REQUIRE(hostCascade == std::vector<size_t>{8, 12, 13});
	for (const bool host : {false, true}) {
		for (int column = 0; column < 5; column++) {
			studio::ToolsProbe::CacheClick(editor, host, column);
			std::vector<size_t> expected;
			const auto &view = studio::ToolsProbe::CacheOrder(editor, host);
			// The production column preference picks ascending text and descending values first.
			const studio::frame_graph_detail::CacheSort descending{
				column, column < 2 ? ImGuiSortDirection_Ascending : ImGuiSortDirection_Descending
			};
			engine::render::PresentationCacheProfile profile;
			engine::render::PresentationCacheApplicability applicable;
			applicable.Objects = false;
			profile.Record(
				{.Scene = true, .HostInterface = true, .Objects = true}, true, false, false, applicable
			);
			profile.Record({.Particles = true}, true);
			profile.Record({.Scene = true, .HostInterface = true}, true);
			studio::frame_graph_detail::BuildCacheOrder(profile.Activities(), host, &descending, expected);
			CHECK(view == expected);
			studio::ToolsProbe::CacheClick(editor, host, column);
			const studio::frame_graph_detail::CacheSort ascending{
				column, column < 2 ? ImGuiSortDirection_Descending : ImGuiSortDirection_Ascending
			};
			studio::frame_graph_detail::BuildCacheOrder(profile.Activities(), host, &ascending, expected);
			CHECK(studio::ToolsProbe::CacheOrder(editor, host) == expected);
			studio::ToolsProbe::CacheClick(editor, host, column);
			CHECK(studio::ToolsProbe::CacheOrder(editor, host) == (host ? hostCascade : cascade));
		}
	}
	studio::ToolsProbe::CacheClick(editor, false, 2);
	studio::ToolsProbe::CacheExpire(editor);
	studio::ToolsProbe::FlameDraw(editor, 1000, 700, 0);
	const auto sampledOrder = studio::ToolsProbe::CacheOrder(editor, false);
	const auto position = [](const std::vector<size_t> &order, size_t layer) {
		return std::find(order.begin(), order.end(), layer) - order.begin();
	};
	CHECK(position(sampledOrder, 2) < position(sampledOrder, 0));
	studio::ToolsProbe::CacheSortingMutation(editor);
	studio::ToolsProbe::FlameDraw(editor, 1000, 700, 0);
	CHECK(studio::ToolsProbe::CacheOrder(editor, false) == sampledOrder);
	studio::ToolsProbe::CacheExpire(editor);
	studio::ToolsProbe::FlameDraw(editor, 1000, 700, 0);
	CHECK(
		position(studio::ToolsProbe::CacheOrder(editor, false), 0) <
		position(studio::ToolsProbe::CacheOrder(editor, false), 2)
	);
	CHECK(studio::ToolsProbe::CacheOrder(editor, true) == hostCascade);
}

TEST_CASE(
	"cache table value ordering keeps missing rates last and breaks ties by cascade",
	"[studio][diagnostics][cache]"
) {
	using engine::render::PresentationCacheActivity;
	using studio::frame_graph_detail::BuildCacheOrder;
	using studio::frame_graph_detail::CacheSort;
	std::array<PresentationCacheActivity, 14> rows{};
	rows[8] = {.Hits = 9, .Writes = 1, .Last = PresentationCacheActivity::Decision::Write};
	rows[12] = {.Hits = 2, .Writes = 8, .Last = PresentationCacheActivity::Decision::Hit};
	std::vector<size_t> order;
	const CacheSort hits{2, ImGuiSortDirection_Descending};
	BuildCacheOrder(rows, true, &hits, order);
	CHECK(order == std::vector<size_t>{8, 12, 13});
	const CacheSort writes{3, ImGuiSortDirection_Descending};
	BuildCacheOrder(rows, true, &writes, order);
	CHECK(order == std::vector<size_t>{12, 8, 13});
	const CacheSort rateAscending{4, ImGuiSortDirection_Ascending};
	BuildCacheOrder(rows, true, &rateAscending, order);
	CHECK(order == std::vector<size_t>{12, 8, 13});
	const CacheSort rateDescending{4, ImGuiSortDirection_Descending};
	BuildCacheOrder(rows, true, &rateDescending, order);
	CHECK(order == std::vector<size_t>{8, 12, 13});
	const CacheSort last{1, ImGuiSortDirection_Ascending};
	BuildCacheOrder(rows, true, &last, order);
	CHECK(order == std::vector<size_t>{12, 13, 8});
	const CacheSort names{0, ImGuiSortDirection_Ascending};
	BuildCacheOrder(rows, true, &names, order);
	CHECK(order == std::vector<size_t>{12, 8, 13});
	rows[12] = rows[8];
	BuildCacheOrder(rows, true, &hits, order);
	CHECK(order == std::vector<size_t>{8, 12, 13});
	BuildCacheOrder(rows, true, nullptr, order);
	CHECK(order == std::vector<size_t>{8, 12, 13});
}
