#pragma once

// The client program's own code — an attachment on top of the engine, not a
// layer of it.
//
// It owns the window, the swapchain, the event loop and the tick. Everything it
// does is a call into a library, which is what makes `mono.client/app/main.cpp`
// three lines long and what will let single-player link the server library into
// this same process later.

#include <engine/core/Clock.hpp>
#include <engine/core/FixedTimestep.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/input/Actions.hpp>
#include <engine/render/DebugPanels.hpp>
#include <engine/render/Renderer.hpp>

#include <client/Demo.hpp>
#include <cstdint>
#include <filesystem>
#include <string>

struct SDL_Window;

namespace client {

	struct Options {
		int Width = 1280;
		int Height = 720;

		// How many cubes the demo scene builds.
		uint32_t Entities = 2048;

		// Simulation ticks per second, independent of the frame rate. The
		// frame runs as fast as it can; the simulation advances in fixed steps
		// and the render interpolates between them.
		double TickRate = 60.0;

		// -1 runs until the window is closed. A frame budget is what makes the
		// client usable from a test or a CI job.
		int64_t MaximumFrames = -1;

		bool ShowStatistics = false;
		bool ShowFrameGraph = false;
		engine::render::ProfilerTab Tab = engine::render::ProfilerTab::Frame;

		// Present without waiting for vblank, to measure the frame rather than
		// the display.
		bool Uncapped = false;

		// Seconds to wait for a Tracy profiler to attach before starting.
		// Zero means do not wait. Tracy is on-demand, so a short run with
		// nothing attached records nothing at all — which looks identical to a
		// broken profiler until you know.
		double ProfilerWaitSeconds = 0.0;

		// Run for this long, then exit. Zero means run until closed.
		double ProfileSeconds = 0.0;

		// Read staged data from here instead of from beside the binary.
		std::filesystem::path AssetsDirectory;

		// TODO(v0.5): run this Luau script at startup. Parsed and reported now
		// so that a command line written against the roadmap fails with a clear
		// message rather than being silently ignored — there is no VM until
		// L13 exists.
		std::string ScriptPath;
	};

	class Client {
	  public:
		Client() = default;
		~Client();

		Client(const Client &) = delete;
		Client &operator=(const Client &) = delete;

		bool Initialise(const Options &options);
		void Shutdown();

		// Runs until the window is closed, the frame budget is spent, or the
		// profile duration elapses. Returns the process exit code.
		int Run();

	  private:
		void PumpEvents();
		void Step();
		void WriteSnapshot();

		Options Settings;

		SDL_Window *Window = nullptr;
		engine::render::Renderer Renderer;
		engine::render::OverlayImage Overlay;
		engine::render::FrameStatistics Statistics;

		engine::input::Actions Actions;
		engine::core::FrameClock Clock;
		engine::core::FixedTimestep Timestep;

		// The world, and the only place simulation state lives. Everything
		// below this line is the *program* — window, frame budget, panel
		// scroll — which is not world state and does not belong in the store.
		engine::ecs::Store Store{"client"};
		engine::ecs::Scheduler Scheduler;

		std::vector<engine::render::SystemTiming> SystemTimings;

		bool Running = false;
		int64_t FramesDrawn = 0;

		// A one-second window over ticks, so the panel can show the rate the
		// simulation actually achieved rather than the one it was asked for.
		double TickWindowStarted = 0.0;
		uint64_t TicksAtWindowStart = 0;
		float MeasuredTicksPerSecond = 0.0f;
		int ProfilerScroll = 0;
		// How deep the flamegraph is drawn, on - and =. Starts at everything
		// that was recorded; a deep tree is unreadable and a shallow one hides
		// the answer, so which it is has to be adjustable while looking at it.
		uint32_t ProfilerDepth = engine::core::FrameGraph::MAXIMUM_DEPTH;
		engine::render::FrameResult LastFrame;
	};
}
