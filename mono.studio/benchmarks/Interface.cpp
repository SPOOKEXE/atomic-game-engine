// The production toolbar and Preferences panel in a backend-free ImGui frame.
//
// This keeps NewFrame and Render visible as a separate floor. The inner clock
// brackets only the production member call, and heap totals sample one warmed
// frame without charging the counter reads to that panel's allocation delta.

#include <engine/core/FrameGraph.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Paths.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Bench.hpp>
#include <engine/world/Universe.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <imgui.h>
#include <imgui_internal.h>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <studio/Editor.hpp>
#include <studio/Plugins.hpp>
#include <vector>

TEST_SUITE_ID("studio.bench.interface")

using engine::core::HeapProfile;
using engine::core::HeapTotals;
using engine::testing::Consume;

namespace studio {
	// Editor already grants this private probe access for the headless tool suite.
	struct ToolsProbe {
		static void Prepare(Editor &editor, LoadedCppPlugin &plugin) {
			editor.Universe = std::make_unique<engine::world::Universe>();
			editor.Active = editor.Universe->Create({.Name = engine::core::Name("studio-interface-bench")});
			editor.SelectionWorld = editor.Active;

			plugin.Definition = MakeDefaultStudioPlugin();
			plugin.Manifest = plugin.Definition.Manifest;
			plugin.Running = true;
			plugin.Native = true;

			engine::world::WorldId world = editor.Active;
			plugin.Context = CppPluginContext{
				.Owner = &editor,
				.World = world,
				.Presentation = &plugin,
				.Bindings = &plugin.Bindings,
			};
			std::string error;
			if (!plugin.Definition.Open(plugin.Context, error)) {
				throw std::runtime_error(error);
			}

			editor.Plugins.push_back(&plugin);
			editor.ToolbarLayoutDirty = true;
			editor.ShowSettings = true;
		}

		static void PrepareContentWorlds(Editor &editor, size_t folders) {
			engine::scene::RegisterSceneClasses();
			editor.Universe = std::make_unique<engine::world::Universe>();
			const auto folderClass = engine::ecs::Classes::Find(engine::core::Name("Folder"));
			for (size_t index = 0; index < 10; ++index) {
				const auto world = editor.Universe->Create({
					.Name = engine::core::Name("studio-content-bench-" + std::to_string(index)),
				});
				editor.Universe->Enter(world, [&](engine::ecs::Store &store) {
					engine::scene::InstallServices(store);
					const auto workspace = engine::scene::WorkspaceOf(store);
					for (size_t child = 0; child < folders; ++child) {
						const auto folder = store.CreateInstance(folderClass);
						if (!store.SetParent(folder, workspace))
							throw std::runtime_error("content benchmark folder could not be parented");
					}
				});
			}
			editor.RequestShownContent();
			editor.RequestShownContent();
			if (editor.ContentScannedAtRevision.size() != 10 || !editor.WantedContent.empty())
				throw std::runtime_error("content benchmark demand did not settle");
		}

		static size_t ScanContent(Editor &editor) {
			editor.RequestShownContent();
			return editor.ContentScannedAtRevision.size();
		}

		static void DrawToolbar(Editor &editor) {
			editor.DrawToolbar();
		}

		static void DrawSettings(Editor &editor) {
			editor.DrawSettings();
		}

		static void DrawGeneralSettings(Editor &editor) {
			editor.DrawGeneralSettings();
		}

		static void DrawDefaultWorldSettings(Editor &editor) {
			editor.DrawDefaultWorldSettings();
		}

		static void DrawFrameGraph(Editor &editor) {
			editor.DrawFrameGraph();
		}

		static void PrepareFrameGraph(
			Editor &editor, DiagnosticAggregation mode = DiagnosticAggregation::Average, int interval = 1
		) {
			editor.ShowFrameGraph = true;
			editor.FrameGraphState.Interval = interval;
			editor.FrameGraphState.Mode = mode;
			editor.FrameGraphState.NextPublish = 0.0;
		}

		static void PauseFrameGraph(Editor &editor) {
			editor.FrameGraphState.Paused = true;
		}

		static size_t PublishedFrameGraphSpanCount(Editor &editor) {
			return editor.FrameGraphState.Spans.size();
		}

		static bool FrameGraphPaused(Editor &editor) {
			return editor.FrameGraphState.Paused;
		}

		static std::vector<DiagnosticSpan> PublishedFrameGraphSnapshot(Editor &editor) {
			return editor.FrameGraphState.Spans;
		}

		static bool
		PublishedFrameGraphSnapshotMatches(Editor &editor, const std::vector<DiagnosticSpan> &expected) {
			const auto &actual = editor.FrameGraphState.Spans;
			if (actual.size() != expected.size()) {
				return false;
			}
			for (size_t index = 0; index < actual.size(); index++) {
				const DiagnosticSpan &left = actual[index];
				const DiagnosticSpan &right = expected[index];
				if (left.Name != right.Name || left.Depth != right.Depth || left.Parent != right.Parent ||
					left.StartMilliseconds != right.StartMilliseconds ||
					left.Milliseconds != right.Milliseconds ||
					left.SelfMilliseconds != right.SelfMilliseconds ||
					left.IdleMilliseconds != right.IdleMilliseconds || left.Category != right.Category ||
					left.Owner != right.Owner || left.Reported != right.Reported ||
					left.Occurrences != right.Occurrences) {
					return false;
				}
			}
			return true;
		}

		static void Finish(Editor &editor, LoadedCppPlugin &plugin) noexcept {
			editor.Plugins.clear();
			if (plugin.Running && plugin.Definition.Close) {
				try {
					plugin.Definition.Close(plugin.Context);
				} catch (...) {}
			}
			plugin.Running = false;
			plugin.Bindings.Close();
		}
	};
}

namespace {
	using Clock = std::chrono::steady_clock;
	constexpr size_t FRAMES_PER_SAMPLE = 600;
	constexpr size_t WARM_FRAMES = 24;
	constexpr size_t BENCHMARK_WORLD_COUNT = 8;
	constexpr size_t DENSE_2048_PHASES_PER_WORLD = 256;

	struct PanelMetric {
		uint64_t MeanScopeNanoseconds = 0;
		uint64_t FrameAllocations = 0;
		uint64_t FrameAllocatedBytes = 0;
		bool HeapProfileEnabled = false;
	};

	struct WorkSpan {
		std::string_view Name;
		engine::core::ProfileCategory Category;
	};

	constexpr std::array<WorkSpan, 27> DENSE_WORK_SPANS{{
		{"input.poll", engine::core::ProfileCategory::Engine},
		{"input.route", engine::core::ProfileCategory::Engine},
		{"command.apply", engine::core::ProfileCategory::ECS},
		{"transform.propagate", engine::core::ProfileCategory::ECS},
		{"behavior.step", engine::core::ProfileCategory::Script},
		{"physics.broadphase", engine::core::ProfileCategory::Physics},
		{"physics.narrowphase", engine::core::ProfileCategory::Physics},
		{"physics.solve", engine::core::ProfileCategory::Physics},
		{"physics.publish", engine::core::ProfileCategory::Physics},
		{"scene.visibility", engine::core::ProfileCategory::ECS},
		{"scene.lod", engine::core::ProfileCategory::ECS},
		{"scene.resolve", engine::core::ProfileCategory::Assets},
		{"assets.lookup", engine::core::ProfileCategory::Assets},
		{"assets.decode", engine::core::ProfileCategory::Assets},
		{"assets.upload", engine::core::ProfileCategory::Assets},
		{"render.prepare", engine::core::ProfileCategory::Render},
		{"render.cull", engine::core::ProfileCategory::Render},
		{"render.pipeline", engine::core::ProfileCategory::Render},
		{"render.encode", engine::core::ProfileCategory::Render},
		{"render.submit", engine::core::ProfileCategory::Render},
		{"replication.diff", engine::core::ProfileCategory::Network},
		{"replication.encode", engine::core::ProfileCategory::Network},
		{"replication.send", engine::core::ProfileCategory::Network},
		{"audio.mix", engine::core::ProfileCategory::Engine},
		{"audio.submit", engine::core::ProfileCategory::Engine},
		{"ui.layout", engine::core::ProfileCategory::Render},
		{"ui.widgets", engine::core::ProfileCategory::Render},
	}};
	constexpr size_t DENSE_MINIMUM_SPANS = 1 + BENCHMARK_WORLD_COUNT * (1 + DENSE_WORK_SPANS.size() + 1 + 2);
	constexpr size_t DENSE_2048_MINIMUM_SPANS =
		1 + BENCHMARK_WORLD_COUNT * (1 + DENSE_2048_PHASES_PER_WORLD + 1 + 2);
	constexpr size_t CURRENT_MINIMUM_SPANS = 1 + BENCHMARK_WORLD_COUNT * (3 + 2);

	struct Metrics {
		std::vector<PanelMetric> Toolbar;
		std::vector<PanelMetric> PreferencesGeneral;
		std::vector<PanelMetric> GeneralSettings;
		std::vector<PanelMetric> DefaultWorlds;
		std::vector<PanelMetric> FrameGraph;
		std::vector<PanelMetric> PausedFrameGraphAverage;
		std::vector<PanelMetric> PausedFrameGraphEveryFrame;
		std::vector<PanelMetric> PausedDenseFrameGraphAverage;
		std::vector<PanelMetric> PausedDenseFrameGraphEveryFrame;
		std::vector<PanelMetric> PausedDense2048FrameGraphAverage;
		std::vector<PanelMetric> PausedDense2048FrameGraphEveryFrame;

		~Metrics() {
			Print("toolbar", Toolbar);
			Print("preferences_general", PreferencesGeneral);
			Print("general_settings_contents", GeneralSettings);
			Print("default_worlds", DefaultWorlds);
			Print("frame_graph_average", FrameGraph);
			Print("frame_graph_paused_average_current", PausedFrameGraphAverage);
			Print("frame_graph_paused_every_frame_current", PausedFrameGraphEveryFrame);
			Print("frame_graph_paused_average_dense", PausedDenseFrameGraphAverage);
			Print("frame_graph_paused_every_frame_dense", PausedDenseFrameGraphEveryFrame);
			Print("frame_graph_paused_average_dense_2048", PausedDense2048FrameGraphAverage);
			Print("frame_graph_paused_every_frame_dense_2048", PausedDense2048FrameGraphEveryFrame);
		}

		static void Print(const char *panel, const std::vector<PanelMetric> &metrics) {
			if (metrics.empty()) {
				return;
			}
			uint64_t totalScopeNanoseconds = 0;
			uint64_t minimumScopeNanoseconds = UINT64_MAX;
			uint64_t maximumScopeNanoseconds = 0;
			uint64_t minimumFrameAllocations = UINT64_MAX;
			uint64_t maximumFrameAllocations = 0;
			uint64_t minimumFrameAllocatedBytes = UINT64_MAX;
			uint64_t maximumFrameAllocatedBytes = 0;
			bool heapProfileEnabled = true;
			for (const PanelMetric &metric : metrics) {
				totalScopeNanoseconds += metric.MeanScopeNanoseconds;
				minimumScopeNanoseconds = std::min(minimumScopeNanoseconds, metric.MeanScopeNanoseconds);
				maximumScopeNanoseconds = std::max(maximumScopeNanoseconds, metric.MeanScopeNanoseconds);
				minimumFrameAllocations = std::min(minimumFrameAllocations, metric.FrameAllocations);
				maximumFrameAllocations = std::max(maximumFrameAllocations, metric.FrameAllocations);
				minimumFrameAllocatedBytes = std::min(minimumFrameAllocatedBytes, metric.FrameAllocatedBytes);
				maximumFrameAllocatedBytes = std::max(maximumFrameAllocatedBytes, metric.FrameAllocatedBytes);
				heapProfileEnabled = heapProfileEnabled && metric.HeapProfileEnabled;
			}
			std::cout << "detail\tstudio.bench.interface\t" << panel << "\tharness_runs_including_warmups\t"
					  << metrics.size() << "\tmean_scope_ns_per_frame_min\t" << minimumScopeNanoseconds
					  << "\tmean_scope_ns_per_frame_max\t" << maximumScopeNanoseconds
					  << "\tmean_scope_ns_per_frame_average\t" << totalScopeNanoseconds / metrics.size()
					  << "\tlast_frame_allocations_min\t" << minimumFrameAllocations
					  << "\tlast_frame_allocations_max\t" << maximumFrameAllocations
					  << "\tlast_frame_allocated_bytes_min\t" << minimumFrameAllocatedBytes
					  << "\tlast_frame_allocated_bytes_max\t" << maximumFrameAllocatedBytes
					  << "\theap_profile_enabled\t" << (heapProfileEnabled ? "true" : "false") << "\n";
		}
	};

	Metrics &Observed() {
		static Metrics metrics;
		return metrics;
	}

	void PrepareBenchmarkConfiguration() {
		// Configuration must exist before any static Editor, so its shutdown
		// cannot outlive the root it writes. Benchmark preferences stay local.
		static const bool prepared = [] {
			studio::SetConfigRoot(engine::core::Paths::Base() / "studio-benchmark-config");
			return true;
		}();
		(void)prepared;
	}

	struct BenchmarkConfiguration {
		BenchmarkConfiguration() {
			PrepareBenchmarkConfiguration();
		}
	};

	struct Context {
		ImGuiContext *Previous = ImGui::GetCurrentContext();
		ImGuiContext *Handle = nullptr;

		Context() {
			IMGUI_CHECKVERSION();
			Handle = ImGui::CreateContext();
			ImGui::SetCurrentContext(Handle);

			ImGuiIO &io = ImGui::GetIO();
			io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
			io.DisplaySize = ImVec2(1600.0f, 900.0f);
			io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
			io.DeltaTime = 1.0f / 60.0f;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.AddMousePosEvent(-100.0f, -100.0f);
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
		}

		~Context() {
			ImGui::SetCurrentContext(Handle);
			ImGui::DestroyContext(Handle);
			ImGui::SetCurrentContext(Previous);
		}

		Context(const Context &) = delete;
		Context &operator=(const Context &) = delete;
	};

	struct Fixture {
		BenchmarkConfiguration Configuration;
		Context ImGuiContext;
		studio::Editor Editor;
		studio::LoadedCppPlugin DefaultPlugin;

		Fixture() {
			studio::ToolsProbe::Prepare(Editor, DefaultPlugin);
			for (size_t frame = 0; frame < WARM_FRAMES; frame++) {
				ToolbarFrame();
				PreferencesFrame();
				GeneralSettingsFrame();
				DefaultWorldsFrame();
			}
		}

		~Fixture() {
			studio::ToolsProbe::Finish(Editor, DefaultPlugin);
		}

		void BeginFrame() {
			ImGui::NewFrame();
		}

		static void EndFrame() {
			ImGui::Render();
		}

		void ToolbarFrame(uint64_t *scopeNanoseconds = nullptr, PanelMetric *allocations = nullptr) {
			BeginFrame();
			const bool sampleAllocations = allocations != nullptr && HeapProfile::IsCompiledIn();
			if (allocations != nullptr) {
				allocations->HeapProfileEnabled = HeapProfile::IsCompiledIn();
			}
			const HeapTotals before = sampleAllocations ? HeapProfile::Totals() : HeapTotals{};
			const auto started = Clock::now();
			studio::ToolsProbe::DrawToolbar(Editor);
			const auto ended = Clock::now();
			if (scopeNanoseconds != nullptr) {
				*scopeNanoseconds += static_cast<uint64_t>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(ended - started).count()
				);
			}
			if (sampleAllocations) {
				const HeapTotals after = HeapProfile::Totals();
				allocations->FrameAllocations = after.TotalBlocks - before.TotalBlocks;
				allocations->FrameAllocatedBytes = after.TotalBytes - before.TotalBytes;
			}
			EndFrame();
		}

		void PreferencesFrame(uint64_t *scopeNanoseconds = nullptr, PanelMetric *allocations = nullptr) {
			BeginFrame();
			ImGui::SetNextWindowPos(ImVec2(40.0f, 100.0f), ImGuiCond_Always);
			ImGui::SetNextWindowSize(ImVec2(580.0f, 680.0f), ImGuiCond_Always);
			const bool sampleAllocations = allocations != nullptr && HeapProfile::IsCompiledIn();
			if (allocations != nullptr) {
				allocations->HeapProfileEnabled = HeapProfile::IsCompiledIn();
			}
			const HeapTotals before = sampleAllocations ? HeapProfile::Totals() : HeapTotals{};
			const auto started = Clock::now();
			studio::ToolsProbe::DrawSettings(Editor);
			const auto ended = Clock::now();
			if (scopeNanoseconds != nullptr) {
				*scopeNanoseconds += static_cast<uint64_t>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(ended - started).count()
				);
			}
			if (sampleAllocations) {
				const HeapTotals after = HeapProfile::Totals();
				allocations->FrameAllocations = after.TotalBlocks - before.TotalBlocks;
				allocations->FrameAllocatedBytes = after.TotalBytes - before.TotalBytes;
			}
			EndFrame();
		}

		void GeneralSettingsFrame(uint64_t *scopeNanoseconds = nullptr, PanelMetric *allocations = nullptr) {
			BeginFrame();
			ImGui::SetNextWindowPos(ImVec2(40.0f, 100.0f), ImGuiCond_Always);
			ImGui::SetNextWindowSize(ImVec2(580.0f, 680.0f), ImGuiCond_Always);
			ImGui::Begin("Preferences General contents");
			const bool sampleAllocations = allocations != nullptr && HeapProfile::IsCompiledIn();
			if (allocations != nullptr) {
				allocations->HeapProfileEnabled = HeapProfile::IsCompiledIn();
			}
			const HeapTotals before = sampleAllocations ? HeapProfile::Totals() : HeapTotals{};
			const auto started = Clock::now();
			studio::ToolsProbe::DrawGeneralSettings(Editor);
			const auto ended = Clock::now();
			if (scopeNanoseconds != nullptr) {
				*scopeNanoseconds += static_cast<uint64_t>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(ended - started).count()
				);
			}
			if (sampleAllocations) {
				const HeapTotals after = HeapProfile::Totals();
				allocations->FrameAllocations = after.TotalBlocks - before.TotalBlocks;
				allocations->FrameAllocatedBytes = after.TotalBytes - before.TotalBytes;
			}
			ImGui::End();
			EndFrame();
		}

		void DefaultWorldsFrame(uint64_t *scopeNanoseconds = nullptr, PanelMetric *allocations = nullptr) {
			BeginFrame();
			ImGui::SetNextWindowPos(ImVec2(40.0f, 100.0f), ImGuiCond_Always);
			ImGui::SetNextWindowSize(ImVec2(580.0f, 680.0f), ImGuiCond_Always);
			ImGui::Begin("Default Worlds page");
			const bool sampleAllocations = allocations != nullptr && HeapProfile::IsCompiledIn();
			if (allocations != nullptr) {
				allocations->HeapProfileEnabled = HeapProfile::IsCompiledIn();
			}
			const HeapTotals before = sampleAllocations ? HeapProfile::Totals() : HeapTotals{};
			const auto started = Clock::now();
			studio::ToolsProbe::DrawDefaultWorldSettings(Editor);
			const auto ended = Clock::now();
			if (scopeNanoseconds != nullptr) {
				*scopeNanoseconds += static_cast<uint64_t>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(ended - started).count()
				);
			}
			if (sampleAllocations) {
				const HeapTotals after = HeapProfile::Totals();
				allocations->FrameAllocations = after.TotalBlocks - before.TotalBlocks;
				allocations->FrameAllocatedBytes = after.TotalBytes - before.TotalBytes;
			}
			ImGui::End();
			EndFrame();
		}
	};

	struct FrameGraphFixture {
		BenchmarkConfiguration Configuration;
		Context ImGuiContext;
		studio::Editor Editor;
		const bool WasEnabled = engine::core::FrameGraph::IsEnabled();
		bool Dense = false;
		bool Paused = false;
		size_t PublishedSpanCount = 0;
		size_t DensePhasesPerWorld = DENSE_WORK_SPANS.size();
		std::vector<studio::DiagnosticSpan> PublishedSnapshot;

		FrameGraphFixture(
			studio::DiagnosticAggregation mode = studio::DiagnosticAggregation::Average,
			int interval = 1,
			bool dense = false,
			bool paused = false,
			size_t densePhasesPerWorld = DENSE_WORK_SPANS.size()
		)
			: Dense(dense), Paused(paused), DensePhasesPerWorld(densePhasesPerWorld) {
			studio::ToolsProbe::PrepareFrameGraph(Editor, mode, interval);
			engine::core::FrameGraph::SetEnabled(true);
			if (!Paused) {
				return;
			}

			if (Dense) {
				RecordDenseFrame(DensePhasesPerWorld);
			} else {
				RecordFrame();
			}
			const size_t minimumSpans =
				Dense ? (DensePhasesPerWorld == DENSE_2048_PHASES_PER_WORLD ? DENSE_2048_MINIMUM_SPANS
																			: DENSE_MINIMUM_SPANS)
					  : CURRENT_MINIMUM_SPANS;
			if (engine::core::FrameGraph::Spans().size() < minimumSpans) {
				throw std::runtime_error("frame graph benchmark fixture recorded too few spans");
			}
			DrawPanelFrame();
			PublishedSpanCount = studio::ToolsProbe::PublishedFrameGraphSpanCount(Editor);
			if (PublishedSpanCount < minimumSpans) {
				DrawPanelFrame();
				PublishedSpanCount = studio::ToolsProbe::PublishedFrameGraphSpanCount(Editor);
			}
			if (PublishedSpanCount < minimumSpans) {
				throw std::runtime_error("paused frame graph benchmark did not publish its warm frame");
			}
			PublishedSnapshot = studio::ToolsProbe::PublishedFrameGraphSnapshot(Editor);
			studio::ToolsProbe::PauseFrameGraph(Editor);
			for (size_t frame = 0; frame < WARM_FRAMES; frame++) {
				DrawPausedFrame(nullptr, nullptr, true);
			}
		}

		~FrameGraphFixture() {
			engine::core::FrameGraph::SetEnabled(WasEnabled);
		}

		void RecordFrame() {
			using engine::core::FrameGraph;
			using engine::core::ProfileCategory;
			FrameGraph::BeginFrame();
			{
				FrameGraph::Scope application("application", ProfileCategory::Engine);
				for (size_t world = 0; world < 8; world++) {
					FrameGraph::Scope worldScope("world", ProfileCategory::ECS);
					{ FrameGraph::Scope update("world.update", ProfileCategory::Simulation); }
					{
						FrameGraph::Scope submit("renderer.executegraph", ProfileCategory::Render);
						FrameGraph::Report("gpu skybox-compute", ProfileCategory::Gpu, 0.018f);
						FrameGraph::Report("jobs.join.assigned", ProfileCategory::ECS, 0.012f);
					}
				}
			}
			FrameGraph::EndFrame();
		}

		void RecordDenseFrame(size_t phasesPerWorld) {
			using engine::core::FrameGraph;
			using engine::core::ProfileCategory;
			FrameGraph::BeginFrame();
			{
				FrameGraph::Scope application("application", ProfileCategory::Engine);
				for (size_t world = 0; world < BENCHMARK_WORLD_COUNT; world++) {
					FrameGraph::Scope worldScope("world", ProfileCategory::ECS);
					for (size_t phaseIndex = 0; phaseIndex < phasesPerWorld; phaseIndex++) {
						const WorkSpan &work = DENSE_WORK_SPANS[phaseIndex % DENSE_WORK_SPANS.size()];
						FrameGraph::Scope phase(work.Name, work.Category);
					}
					{
						FrameGraph::Scope submit("renderer.executegraph", ProfileCategory::Render);
						FrameGraph::Report("gpu skybox-compute", ProfileCategory::Gpu, 0.018f);
						FrameGraph::Report("jobs.join.assigned", ProfileCategory::ECS, 0.012f);
					}
				}
			}
			FrameGraph::EndFrame();
		}

		void DrawPanelFrame() {
			ImGui::SetCurrentContext(ImGuiContext.Handle);
			ImGui::NewFrame();
			ImGui::SetNextWindowPos(ImVec2(40.0f, 100.0f));
			ImGui::SetNextWindowSize(ImVec2(900.0f, 700.0f));
			studio::ToolsProbe::DrawFrameGraph(Editor);
			ImGui::Render();
		}

		void DrawFrame(uint64_t *scopeNanoseconds = nullptr) {
			RecordFrame();
			ImGui::NewFrame();
			ImGui::SetNextWindowPos(ImVec2(40.0f, 100.0f), ImGuiCond_Always);
			ImGui::SetNextWindowSize(ImVec2(900.0f, 700.0f), ImGuiCond_Always);
			const auto started = Clock::now();
			studio::ToolsProbe::DrawFrameGraph(Editor);
			const auto ended = Clock::now();
			if (scopeNanoseconds != nullptr) {
				*scopeNanoseconds += static_cast<uint64_t>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(ended - started).count()
				);
			}
			ImGui::Render();
		}

		void DrawPausedFrame(
			uint64_t *scopeNanoseconds = nullptr,
			PanelMetric *allocations = nullptr,
			bool validateSnapshot = false
		) {
			using engine::core::FrameGraph;
			if (Dense) {
				RecordFrame();
			} else {
				RecordDenseFrame(DENSE_WORK_SPANS.size());
			}
			const size_t minimumLiveSpans = Dense ? CURRENT_MINIMUM_SPANS : DENSE_MINIMUM_SPANS;
			if (FrameGraph::Spans().size() < minimumLiveSpans ||
				!studio::ToolsProbe::FrameGraphPaused(Editor) ||
				FrameGraph::Spans().size() == PublishedSpanCount) {
				throw std::runtime_error("paused frame graph benchmark did not change the live fixture");
			}
			ImGui::SetCurrentContext(ImGuiContext.Handle);
			ImGui::NewFrame();
			ImGui::SetNextWindowPos(ImVec2(40.0f, 100.0f));
			ImGui::SetNextWindowSize(ImVec2(900.0f, 700.0f));
			const bool sampleAllocations = allocations != nullptr && HeapProfile::IsCompiledIn();
			if (allocations != nullptr) {
				allocations->HeapProfileEnabled = HeapProfile::IsCompiledIn();
			}
			const HeapTotals before = sampleAllocations ? HeapProfile::Totals() : HeapTotals{};
			const auto started = Clock::now();
			studio::ToolsProbe::DrawFrameGraph(Editor);
			const auto ended = Clock::now();
			if (scopeNanoseconds != nullptr) {
				*scopeNanoseconds += static_cast<uint64_t>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(ended - started).count()
				);
			}
			if (sampleAllocations) {
				const HeapTotals after = HeapProfile::Totals();
				allocations->FrameAllocations = after.TotalBlocks - before.TotalBlocks;
				allocations->FrameAllocatedBytes = after.TotalBytes - before.TotalBytes;
			}
			if (validateSnapshot &&
				!studio::ToolsProbe::PublishedFrameGraphSnapshotMatches(Editor, PublishedSnapshot)) {
				throw std::runtime_error("paused frame graph benchmark changed its published snapshot");
			}
			ImGui::Render();
		}
	};

	Fixture &Live() {
		static Fixture fixture;
		return fixture;
	}

	studio::Editor &ContentWorlds(size_t folders) {
		PrepareBenchmarkConfiguration();
		struct Held {
			size_t FoldersPerWorld = 0;
			std::unique_ptr<studio::Editor> Editor;
		};
		static std::vector<Held> built;
		for (const Held &held : built)
			if (held.FoldersPerWorld == folders) return *held.Editor;
		auto editor = std::make_unique<studio::Editor>();
		studio::ToolsProbe::PrepareContentWorlds(*editor, folders);
		built.push_back({folders, std::move(editor)});
		return *built.back().Editor;
	}

	void EmptyFrame() {
		ImGui::NewFrame();
		ImGui::Render();
	}

	void MeasurePausedFrameGraph(FrameGraphFixture &fixture, std::vector<PanelMetric> &metrics) {
		uint64_t scopeNanoseconds = 0;
		PanelMetric allocations;
		for (size_t frame = 0; frame < FRAMES_PER_SAMPLE; frame++) {
			const bool sample = frame + 1 == FRAMES_PER_SAMPLE;
			fixture.DrawPausedFrame(&scopeNanoseconds, sample ? &allocations : nullptr);
		}
		if (!studio::ToolsProbe::PublishedFrameGraphSnapshotMatches(
				fixture.Editor, fixture.PublishedSnapshot
			)) {
			throw std::runtime_error("paused frame graph benchmark changed its published snapshot");
		}
		allocations.MeanScopeNanoseconds = scopeNanoseconds / FRAMES_PER_SAMPLE;
		metrics.push_back(allocations);
		Consume(allocations.MeanScopeNanoseconds);
	}
}

BENCH("toolbar full ImGui frame", FRAMES_PER_SAMPLE) {
	Fixture &fixture = Live();
	uint64_t scopeNanoseconds = 0;
	PanelMetric allocations;
	for (size_t frame = 0; frame < FRAMES_PER_SAMPLE; frame++) {
		const bool sample = frame + 1 == FRAMES_PER_SAMPLE;
		fixture.ToolbarFrame(&scopeNanoseconds, sample ? &allocations : nullptr);
	}
	allocations.MeanScopeNanoseconds = scopeNanoseconds / FRAMES_PER_SAMPLE;
	Observed().Toolbar.push_back(allocations);
	Consume(allocations.MeanScopeNanoseconds);
}

BENCH("preferences General full ImGui frame", FRAMES_PER_SAMPLE) {
	Fixture &fixture = Live();
	uint64_t scopeNanoseconds = 0;
	PanelMetric allocations;
	for (size_t frame = 0; frame < FRAMES_PER_SAMPLE; frame++) {
		const bool sample = frame + 1 == FRAMES_PER_SAMPLE;
		fixture.PreferencesFrame(&scopeNanoseconds, sample ? &allocations : nullptr);
	}
	allocations.MeanScopeNanoseconds = scopeNanoseconds / FRAMES_PER_SAMPLE;
	Observed().PreferencesGeneral.push_back(allocations);
	Consume(allocations.MeanScopeNanoseconds);
}

BENCH("Preferences General contents in ImGui frame", FRAMES_PER_SAMPLE) {
	Fixture &fixture = Live();
	uint64_t scopeNanoseconds = 0;
	PanelMetric allocations;
	for (size_t frame = 0; frame < FRAMES_PER_SAMPLE; frame++) {
		const bool sample = frame + 1 == FRAMES_PER_SAMPLE;
		fixture.GeneralSettingsFrame(&scopeNanoseconds, sample ? &allocations : nullptr);
	}
	allocations.MeanScopeNanoseconds = scopeNanoseconds / FRAMES_PER_SAMPLE;
	Observed().GeneralSettings.push_back(allocations);
	Consume(allocations.MeanScopeNanoseconds);
}

BENCH("Preferences Default Worlds contents in ImGui frame", FRAMES_PER_SAMPLE) {
	Fixture &fixture = Live();
	uint64_t scopeNanoseconds = 0;
	PanelMetric allocations;
	for (size_t frame = 0; frame < FRAMES_PER_SAMPLE; frame++) {
		const bool sample = frame + 1 == FRAMES_PER_SAMPLE;
		fixture.DefaultWorldsFrame(&scopeNanoseconds, sample ? &allocations : nullptr);
	}
	allocations.MeanScopeNanoseconds = scopeNanoseconds / FRAMES_PER_SAMPLE;
	Observed().DefaultWorlds.push_back(allocations);
	Consume(allocations.MeanScopeNanoseconds);
}

BENCH("empty ImGui frame", FRAMES_PER_SAMPLE) {
	Live();
	for (size_t frame = 0; frame < FRAMES_PER_SAMPLE; frame++) {
		EmptyFrame();
	}
}

BENCH("frame graph Average 250 ms retained ImGui panel", FRAMES_PER_SAMPLE) {
	static FrameGraphFixture fixture;
	uint64_t scopeNanoseconds = 0;
	for (size_t frame = 0; frame < FRAMES_PER_SAMPLE; frame++) {
		fixture.DrawFrame(&scopeNanoseconds);
	}
	const uint64_t meanScopeNanoseconds = scopeNanoseconds / FRAMES_PER_SAMPLE;
	Observed().FrameGraph.push_back(PanelMetric{.MeanScopeNanoseconds = meanScopeNanoseconds});
	Consume(meanScopeNanoseconds);
}

BENCH("Frame Graph paused average · current fixture", FRAMES_PER_SAMPLE) {
	static FrameGraphFixture fixture(studio::DiagnosticAggregation::Average, 1, false, true);
	MeasurePausedFrameGraph(fixture, Observed().PausedFrameGraphAverage);
}

BENCH("Frame Graph paused every frame · current fixture", FRAMES_PER_SAMPLE) {
	static FrameGraphFixture fixture(studio::DiagnosticAggregation::Latest, 0, false, true);
	MeasurePausedFrameGraph(fixture, Observed().PausedFrameGraphEveryFrame);
}

BENCH("Frame Graph paused average · dense 240 spans", FRAMES_PER_SAMPLE) {
	static FrameGraphFixture fixture(studio::DiagnosticAggregation::Average, 1, true, true);
	MeasurePausedFrameGraph(fixture, Observed().PausedDenseFrameGraphAverage);
}

BENCH("Frame Graph paused every frame · dense 240 spans", FRAMES_PER_SAMPLE) {
	static FrameGraphFixture fixture(studio::DiagnosticAggregation::Latest, 0, true, true);
	MeasurePausedFrameGraph(fixture, Observed().PausedDenseFrameGraphEveryFrame);
}

BENCH("Frame Graph paused average · dense 2048 CPU spans", FRAMES_PER_SAMPLE) {
	static FrameGraphFixture fixture(
		studio::DiagnosticAggregation::Average, 1, true, true, DENSE_2048_PHASES_PER_WORLD
	);
	MeasurePausedFrameGraph(fixture, Observed().PausedDense2048FrameGraphAverage);
}

BENCH("Frame Graph paused every frame · dense 2048 CPU spans", FRAMES_PER_SAMPLE) {
	static FrameGraphFixture fixture(
		studio::DiagnosticAggregation::Latest, 0, true, true, DENSE_2048_PHASES_PER_WORLD
	);
	MeasurePausedFrameGraph(fixture, Observed().PausedDense2048FrameGraphEveryFrame);
}

BENCH("idle content.demand.scan · 10 furnished worlds", 1000) {
	studio::Editor &editor = ContentWorlds(0);
	for (size_t call = 0; call < 1000; ++call)
		Consume(studio::ToolsProbe::ScanContent(editor));
}

BENCH("idle content.demand.scan · 10 worlds with 1,000 folders each", 1000) {
	studio::Editor &editor = ContentWorlds(1000);
	for (size_t call = 0; call < 1000; ++call)
		Consume(studio::ToolsProbe::ScanContent(editor));
}
