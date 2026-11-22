#pragma once

// The F9 debug panel: a small Dear ImGui window over the running client with
// three tabs, Frame Graph, Network Diagnostics and Memory.
//
// It is the host overlay of `Renderer::Render`, so it is drawn by the pipeline it
// reports on. The F3, F4 and F5 overlays stay as they are because they still
// draw when that pipeline is the thing that broke; this panel is the
// interactive view (sortable, filterable, collapsible) for a client that works.
//
// `DrawTrafficBreakdown` is shared with Studio's Network panel so both programs
// show replication traffic the same way.

#include <engine/core/FrameGraph.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/diagnostics/DebugPanels.hpp>
#include <engine/replication/Protocol.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct SDL_Window;
union SDL_Event;

namespace engine::ui {
	class Interface;
}

namespace client {
	// Draws one traffic table: a row per message kind with messages and bytes
	// in each direction, plus an unnamed row and a total. Call between an
	// ImGui frame's `Begin` and `End`.
	//
	// @param id        Unique ImGui table id.
	// @param inbound   Messages received (or delivered downstream).
	// @param outbound  Messages sent (or acknowledged upstream).
	// @param inLabel   Column heading for the inbound pair.
	// @param outLabel  Column heading for the outbound pair.
	void DrawTrafficBreakdown(
		const char *id,
		const engine::replication::TrafficBreakdown &inbound,
		const engine::replication::TrafficBreakdown &outbound,
		const char *inLabel = "in",
		const char *outLabel = "out"
	);

	// Bytes as B, KiB, MiB or GiB with one decimal.
	std::string FormatBytes(uint64_t bytes);

	// One world's row in the Memory tab.
	struct DebugWorldMemory {
		std::string Name;
		size_t Entities = 0;
		size_t Tables = 0;
		size_t ResidentBytes = 0;

		// Per component: registered name, live rows and resident column bytes.
		struct Component {
			std::string Name;
			size_t Rows = 0;
			size_t Bytes = 0;
		};
		std::vector<Component> Components;
	};

	// One frame graph span, with its name copied so a paused panel holds it.
	struct DebugSpan {
		std::string Name;
		uint32_t Depth = 0;
		float Milliseconds = 0.0f;
		float SelfMilliseconds = 0.0f;
	};

	// Everything the panel shows, gathered by the client once per frame while
	// the panel is open.
	struct DebugPanelSample {
		// Frame Graph tab.
		float FrameMilliseconds = 0.0f;
		float UnmarkedMilliseconds = 0.0f;
		std::vector<DebugSpan> Spans;

		// Network Diagnostics tab. `Connected` false shows a single line.
		bool Connected = false;
		engine::render::NetworkStatistics Link;
		engine::replication::TrafficBreakdown Received;
		engine::replication::TrafficBreakdown Sent;

		// Memory tab.
		engine::core::HeapTotals Heap;
		bool HeapCompiledIn = false;
		engine::render::GpuMemoryStatistics Gpu;
		std::vector<DebugWorldMemory> Worlds;
	};

	// The panel and its ImGui context. The context is made on first open, so a
	// client that never presses F9 pays nothing.
	class DebugPanel {
	  public:
		DebugPanel();
		~DebugPanel();
		DebugPanel(const DebugPanel &) = delete;
		DebugPanel &operator=(const DebugPanel &) = delete;

		// Opens or closes. Opening builds the ImGui context the first time.
		//
		// @return `false` when opening failed (no window, so no context).
		bool Toggle(engine::render::Renderer &renderer, SDL_Window *window);

		bool IsOpen() const {
			return Open;
		}

		// Hands an SDL event to ImGui while open.
		//
		// @return `true` when ImGui wants it and the game should not see it.
		bool ProcessEvent(const SDL_Event &event);

		// Builds this frame's window. Call once per frame before `Render`.
		void Draw(const DebugPanelSample &sample);

		// The host overlay to pass to `Renderer::Render`, or null while closed.
		engine::render::FrameOverlayHook *Hook();

		// Releases the ImGui context. Call before the renderer shuts down.
		void Shutdown();

	  private:
		bool Open = false;
		std::unique_ptr<engine::ui::Interface> Ui;

		// Frame Graph tab controls.
		std::array<char, 64> SpanFilter{};
		float MinimumMilliseconds = 0.0f;
		int DepthLimit = 6;
		bool Paused = false;
		DebugPanelSample Held;
		uint64_t LastDrawNanoseconds = 0;

		// Memory tab control.
		std::array<char, 64> ComponentFilter{};
	};
}
