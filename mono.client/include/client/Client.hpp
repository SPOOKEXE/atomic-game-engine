#pragma once

// The client program's own code — an attachment on top of the engine, not a
// layer of it.
//
// It owns the window, the swapchain, the event loop and the tick. Everything it
// does is a call into a library, which is what makes `mono.client/app/main.cpp`
// three lines long and what will let single-player link the server library into
// this same process later.

#include <engine/core/Clock.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/input/Actions.hpp>
#include <engine/net/Transport.hpp>
#include <engine/render/DebugPanels.hpp>
#include <engine/render/FrameStatistics.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/replication/Connector.hpp>
#include <engine/scene/Components.hpp>
#include <engine/world/Universe.hpp>

#include <client/Compositor.hpp>
#include <client/Scene.hpp>
#include <cstdint>
#include <filesystem>
#include <memory>
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

		// How many cubes the demo scene builds, per world.
		uint32_t Entities = 2048;

		// How many worlds to simulate and composite.
		//
		// Each is a whole world with its own clock, its own store and its own
		// view channel, and the compositor places them side by side because two
		// worlds' coordinates do not mean the same thing. One is the ordinary
		// case; more than one is what makes the compositing path a path rather
		// than a plan.
		uint32_t Worlds = 1;

		// World units between adjacent views.
		//
		// A compositor's decision, not a simulation's — which is why it is a
		// client option and not a world setting.
		float ViewSpacing = 40.0f;

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

		// Open the F4 network panel at startup.
		//
		// **Ignored without `--connect`.** There is no link to report on, and
		// the panel refuses to draw zeroes rather than claiming an idle one.
		bool ShowNetwork = false;

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

		// The scene script to build the world from. Empty means `Rings.luau`.
		//
		// The extension picks the VM, so this is the only place either one is
		// chosen — and since v0.6 it is the *only* way this program gets a
		// world. The C++ scene it used to fall back to is deleted, because two
		// ways to build one and only one of them exercising the bindings meant
		// the bindings were the untested half.
		std::string ScriptPath;

		// `host:port` of a server to replicate from. Empty means run the local
		// demo alone.
		//
		// **This adds a world rather than replacing one.** The replicated world
		// is a second world in the same universe, marked `world::Replica` so its
		// bus handle refuses writes, and the demo keeps running beside it. That
		// is not a placeholder arrangement — a client compositing several worlds
		// is what v0.2 built the compositor for, and one of them being somebody
		// else's authority is the case this version adds.
		std::string ConnectAddress;
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

		// The connection to a server, when `--connect` was given.
		//
		// For a test, which is the only way to prove a join happened rather than
		// a socket being opened.
		//
		// @return The connector, or null when running the demo alone.
		engine::replication::Connector *Server() {
			return Connection.get();
		}

		// The replicated world's handle.
		//
		// @return The handle, or an invalid one when not connected.
		engine::world::WorldId ReplicatedWorld() const {
			return Replicated;
		}

	  private:
		void PumpEvents();
		void Step();
		void WriteSnapshot();

		// Opens the socket, creates the replica world and joins.
		//
		// @return `false` when the address will not parse or the socket will not
		//         open. A client that meant to connect and silently did not is
		//         a client that looks like a server bug.
		bool BeginConnecting();

		// Takes one tick's worth of what the server sent.
		//
		// @param nowSeconds The current time.
		void PollServer(double nowSeconds);

		// Gathers what the F4 panel shows, and moves the rate window on.
		//
		// **Not `const`, and not idempotent.** A byte *rate* is a derivative and
		// `net` only keeps the integral, so this reads the cumulative counters,
		// subtracts what it read last time, and remembers the new reading —
		// calling it twice in one frame would report the second call's window as
		// zero seconds long. Called once per panel redraw and nowhere else.
		//
		// @return The panel's view of the link, all zero and `Connected` false
		//         when this client was never given `--connect`.
		engine::render::NetworkStatistics SampleNetwork();

		// Logs what the replicated world received, drew and interpolated.
		//
		// Nothing when this client never connected. See the body for why it is
		// four numbers rather than one — "joined and drew nothing" and "never
		// joined" look identical from outside, and telling them apart is the
		// whole reason this exists.
		void ReportReplica();

		Options Settings;

		SDL_Window *Window = nullptr;
		engine::render::Renderer Renderer;
		engine::render::OverlayImage Overlay;
		engine::render::FrameStatistics Statistics;

		engine::input::Actions Actions;
		engine::core::FrameClock Clock;

		// The world, and the only place simulation state lives. Everything
		// below this line is the *program* — window, frame budget, panel
		// scroll — which is not world state and does not belong in the store.
		// The universe this client drives, and the world it draws.
		//
		// A client renders one world while the rest keep simulating, which is
		// why presentation is a separate call from the tick. Today there is one
		// world; nothing above this line assumes so.
		//
		// Held by pointer because a universe binds its driver thread on
		// construction, and that thread is decided in Initialise rather than
		// wherever this object was declared.
		std::unique_ptr<engine::world::Universe> Universe_;

		// The world the panels report on, and the first view composited.
		engine::world::WorldId Rendered;

		// Every world this client simulates, in creation order.
		std::vector<engine::world::WorldId> Simulated;

		// The socket and the connection to a server. Both null unless
		// `--connect` was given, which is what keeps a single-player run from
		// opening a port it has no use for.
		std::unique_ptr<engine::net::Transport> Socket;
		std::unique_ptr<engine::replication::Connector> Connection;

		// The world the server owns. Invalid when not connected.
		engine::world::WorldId Replicated;

		// Whether the join has been reported, and therefore whether the
		// replicated world has a view channel yet. A client logs "joined" once,
		// not every frame at six hundred a second.
		bool ReportedJoin = false;

		// Whether the key exchange's outcome has been reported.
		//
		// Separate from the join because the two now fail differently: an
		// exchange that never completes and a world that never finishes
		// arriving both look like "nothing happened" from outside, and they want
		// completely different investigations.
		bool ReportedAdmission = false;

		// Whether F4 has already said there is no network to show. Once, not
		// once per press.
		bool ReportedNoNetwork = false;

		// The previous reading of the link's cumulative counters, and when it
		// was taken. `SampleNetwork` differences against these to turn totals
		// into rates — see its declaration for why the differencing lives here
		// rather than in `net`.
		bool NetworkSampled = false;
		double NetworkSampledAt = 0.0;
		uint64_t NetworkLastReceivedBytes = 0;
		uint64_t NetworkLastSentBytes = 0;
		uint64_t NetworkLastReceivedPackets = 0;
		uint64_t NetworkLastSentPackets = 0;

		// The camera the rendered world placed this frame.
		//
		// Held on the client rather than read twice because the replicated
		// world is drawn through it and has no camera of its own: a camera is
		// an entity, and a replica may not mint one until the predicted-entity
		// index range exists.
		// The surface camera this frame renders an offscreen view from, and
		// whether the world had one. See `FindSurfaceCamera`.
		engine::render::SurfaceView Surface;
		bool HaveSurface = false;

		engine::core::CFrame ComposedFrame;
		engine::scene::Camera ComposedCamera;

		// N views in, one frame out. The renderer draws what this composed
		// rather than reaching into a store that somebody else is writing.
		Compositor Views;

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

		// When the panels were last drawn, and what they were showing at the
		// time.
		//
		// The overlay texture holds the last image drawn into it and is
		// presented from there every frame, so the panels are regenerated on a
		// clock rather than per frame — a person cannot read a number that
		// changes a thousand times a second, and rasterising one costs the same
		// whether they can or not.
		//
		// The rest is what forces an early redraw. A tab change or a scroll that
		// waited for the next tick would read as a dropped key press.
		double PanelsDrawn = 0.0;
		bool PanelsShown = false;
		engine::render::ProfilerTab PanelTab = engine::render::ProfilerTab::Frame;
		int PanelScroll = 0;
		uint32_t PanelDepth = engine::core::FrameGraph::MAXIMUM_DEPTH;
		int PanelWidth = 0;
		int PanelHeight = 0;

		engine::render::FrameResult LastFrame;
	};
}
