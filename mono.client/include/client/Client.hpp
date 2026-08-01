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

	// Everything the command line decides, in one place.
	//
	// Parsed once and copied into the Client by Initialise, so nothing reads the
	// argument list again after start-up and there is one answer to "what is
	// this process configured to do".
	struct Options {
		// Window width in logical pixels, before any display scaling.
		int Width = 1280;

		// Window height in logical pixels. The window is resizable, so this is
		// where it starts and not where it stays.
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

		// Open the F3 statistics panel at startup, rather than waiting for
		// somebody to press F3.
		bool ShowStatistics = false;

		// Open the F5 frame graph at startup.
		//
		// Worth setting deliberately: collection only runs while the panel is
		// open, so a run that meant to record a graph and never opened one has
		// nothing to show for it afterwards.
		bool ShowFrameGraph = false;

		// Which frame-graph view the panel opens on. Naming one opens the panel,
		// because asking to see something is not a separate request from showing
		// it.
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

	// The window, the renderer and the frame loop over one world.
	//
	// Initialise, then Run until it returns, then Shutdown. Non-copyable because
	// it owns an SDL window and a graphics device, and there is no sensible
	// meaning for a second object holding the same two.
	class Client {
	  public:
		Client() = default;
		~Client();

		Client(const Client &) = delete;
		Client &operator=(const Client &) = delete;

		// Applies `options`, opens the window, starts the renderer and builds
		// the demo world.
		//
		// @param options Parsed command line. Copied, not referenced.
		// @return False if SDL, the window or the renderer would not start. The
		//         reason is logged before returning, because the caller has no
		//         way to say anything more useful about it than that.
		bool Initialise(const Options &options);

		// Tears the window and renderer down, in that order for a reason the
		// implementation explains, and stops the job system. Safe to call
		// whether or not Initialise got as far as opening anything.
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
