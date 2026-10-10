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
#include <chrono>
#include <cstdint>
#include <imgui.h>
#include <imgui_internal.h>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
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

		static void PrepareFrameGraph(Editor &editor) {
			editor.ShowFrameGraph = true;
			editor.FrameGraphState.Interval = 1;
			editor.FrameGraphState.Mode = DiagnosticAggregation::Average;
			editor.FrameGraphState.NextPublish = 0.0;
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

	struct PanelMetric {
		uint64_t MeanScopeNanoseconds = 0;
		uint64_t FrameAllocations = 0;
		uint64_t FrameAllocatedBytes = 0;
		bool HeapProfileEnabled = false;
	};

	struct Metrics {
		std::vector<PanelMetric> Toolbar;
		std::vector<PanelMetric> PreferencesGeneral;
		std::vector<PanelMetric> GeneralSettings;
		std::vector<PanelMetric> DefaultWorlds;
		std::vector<PanelMetric> FrameGraph;

		~Metrics() {
			Print("toolbar", Toolbar);
			Print("preferences_general", PreferencesGeneral);
			Print("general_settings_contents", GeneralSettings);
			Print("default_worlds", DefaultWorlds);
			Print("frame_graph_average", FrameGraph);
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

		FrameGraphFixture() {
			studio::ToolsProbe::PrepareFrameGraph(Editor);
			engine::core::FrameGraph::SetEnabled(true);
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
