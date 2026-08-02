#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Paths.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/parallel/Process.hpp>
#include <engine/world/Postbox.hpp>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <client/Client.hpp>
#include <client/Replicated.hpp>

namespace client {

	using engine::core::FrameGraph;
	using engine::core::Metrics;
	using engine::input::Action;
	using engine::render::ProfilerTab;

	Client::~Client() {
		Shutdown();
	}

	bool Client::Initialise(const Options &options) {
		Settings = options;

		// Before anything reads a file. Changing it later would leave whatever
		// had already loaded pointing at the old tree.
		if (!Settings.AssetsDirectory.empty()) {
			engine::core::Paths::SetAssetsOverride(Settings.AssetsDirectory);
			ENGINE_INFO("assets from {}", Settings.AssetsDirectory.string());
		}

		if (!Settings.ScriptPath.empty()) {
			// TODO(v0.6): hand this to the Luau host once L13 exists. Refusing
			// loudly beats accepting a flag and doing nothing with it.
			ENGINE_WARN(
				"--script is accepted but has no effect until the scripting layer lands in "
				"v0.6. Ignoring '{}'.",
				Settings.ScriptPath
			);
		}

		if (!SDL_Init(SDL_INIT_VIDEO)) {
			ENGINE_ERROR("SDL_Init: {}", SDL_GetError());
			return false;
		}

		Window = SDL_CreateWindow(
			"atomic", Settings.Width, Settings.Height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY
		);
		if (!Window) {
			ENGINE_ERROR("SDL_CreateWindow: {}", SDL_GetError());
			return false;
		}

		if (!Renderer.Initialise(Window)) {
			return false;
		}

		if (Settings.Uncapped && !Renderer.SetVerticalSync(false)) {
			ENGINE_WARN("--uncapped had no effect; frames stay paced by the display");
		}

		engine::parallel::Jobs::Start(engine::parallel::WorkersPerHost(1));

		ENGINE_INFO("simulation at {:.0f} Hz, rendering unlocked from it", Settings.TickRate);

		Universe_ = std::make_unique<engine::world::Universe>();

		const uint32_t worlds = std::max(1u, Settings.Worlds);
		for (uint32_t index = 0; index < worlds; index++) {
			engine::world::WorldSettings world;

			// Named rather than numbered, because a name is what a bus
			// envelope, a snapshot and a view header all carry. The first keeps
			// the name it has always had, so nothing that referred to it has to
			// learn a new one.
			world.Name = engine::core::Name(
				index == 0 ? std::string("client.world") : "client.world." + std::to_string(index)
			);
			world.TickRate = Settings.TickRate;

			const engine::world::WorldId id = Universe_->Create(world);
			if (!id.IsValid()) {
				ENGINE_ERROR("could not create the world to render");
				return false;
			}

			Universe_->Enter(id, [this](engine::ecs::Store &store, engine::ecs::Scheduler &systems) {
				BuildDemoWorld(store, systems, Settings.Entities);
			});

			// Sized at the world's entity count rather than at what it drew
			// this frame, so publishing never allocates. A demo world draws at
			// most one instance per entity.
			Views.Track(id, world.Name, Settings.Entities);
			Simulated.push_back(id);
		}

		// The first is the one the panels report on and the one the composed
		// camera comes from. A client draws one world's worth of camera however
		// many it composites.
		Rendered = Simulated.front();

		if (!BeginConnecting()) {
			return false;
		}

		if (worlds > 1) {
			ENGINE_INFO("compositing {} worlds, {:.0f} units apart", worlds, Settings.ViewSpacing);
		}

		FrameGraph::SetEnabled(Settings.ShowFrameGraph);

		// Tracy is on-demand: it collects nothing until a profiler attaches, so
		// a short run with nothing listening produces an empty capture. Waiting
		// makes that an explicit choice rather than a surprise.
		if (Settings.ProfilerWaitSeconds > 0.0 && !ENGINE_PROFILE_ATTACHED()) {
			ENGINE_INFO("waiting up to {:.1f}s for a Tracy profiler", Settings.ProfilerWaitSeconds);

			const uint64_t started = engine::core::Clock::Nanoseconds();
			while (!ENGINE_PROFILE_ATTACHED()) {
				const double waited = static_cast<double>(engine::core::Clock::Nanoseconds() - started) / 1e9;
				if (waited >= Settings.ProfilerWaitSeconds) {
					ENGINE_INFO("no profiler attached; continuing without one");
					break;
				}
				SDL_Delay(50);
			}
			if (ENGINE_PROFILE_ATTACHED()) {
				ENGINE_INFO("profiler attached");
			}
		}

		Running = true;
		return true;
	}

	void Client::Shutdown() {
		// The connection before the socket it borrows, and both before the
		// universe holding the world it writes into. A connector outliving its
		// transport is a dangling reference in a destructor.
		Connection.reset();
		if (Socket != nullptr) {
			Socket->Close();
			Socket.reset();
		}

		if (Window) {
			// Before SDL_Quit: the renderer holds a device that holds the
			// window, and tearing SDL down underneath it is a crash on exit.
			Renderer.Shutdown();
			SDL_DestroyWindow(Window);
			Window = nullptr;
			SDL_Quit();
		}
		engine::parallel::Jobs::Stop();
	}

	bool Client::BeginConnecting() {
		if (Settings.ConnectAddress.empty()) {
			return true;
		}

		const std::optional<engine::net::Endpoint> server =
			engine::net::Endpoint::Parse(Settings.ConnectAddress);
		if (!server.has_value()) {
			ENGINE_ERROR("--connect '{}' is not a host:port", Settings.ConnectAddress);
			return false;
		}

		// Before the socket, and before any datagram can arrive: a snapshot
		// names its components and a process that has not registered them
		// resolves the names to nothing and applies an empty world.
		RegisterReplicatedComponents();

		// Port zero: the client does not need a known address, only the server
		// does. Binding a fixed one would stop two clients sharing a machine.
		Socket = engine::net::MakeUdpTransport(0);
		if (Socket == nullptr) {
			ENGINE_ERROR("could not open a socket to connect from");
			return false;
		}

		engine::world::WorldSettings world;
		world.Name = engine::core::Name("client.replica");
		world.TickRate = Settings.TickRate;

		Replicated = Universe_->Create(world);
		if (!Replicated.IsValid()) {
			ENGINE_ERROR("could not create the replicated world");
			return false;
		}

		Universe_->Enter(Replicated, [](engine::ecs::Store &store) {
			// The v0.2 refusal, used for what it was reserved for. A replica
			// that published to a bus would be telling the universe something
			// the server never said; the inbox still delivers, which is how it
			// receives.
			store.SetResource(engine::world::Replica{true});

			// A replicated world runs no systems of its own. Everything in it
			// arrived, and simulating it here would be this process disagreeing
			// with the authority once per tick.
		});

		Connection = std::make_unique<engine::replication::Connector>(
			*Socket, *server, engine::core::Clock::Seconds()
		);

		ENGINE_INFO("connecting to {} from {}", server->Text(), Socket->Local().Text());
		return true;
	}

	void Client::PollServer(double nowSeconds) {
		if (Connection == nullptr) {
			return;
		}

		Universe_->Enter(Replicated, [this, nowSeconds](engine::ecs::Store &store) {
			Connection->Poll(store, nowSeconds);
		});

		// Once, on the tick it becomes true. A client that logged this every
		// frame would write six hundred lines a second saying the same thing.
		if (!ReportedJoin && Connection->Joined()) {
			ReportedJoin = true;

			size_t entities = 0;
			Universe_->Enter(Replicated, [&entities](engine::ecs::Store &store) {
				store.EachEntity([&entities](engine::ecs::Entity) { entities++; });
			});

			ENGINE_INFO("joined: {} entities at tick {}", entities, Connection->Applied());
		}

		// **The replicated world is not drawn yet, and that is a stated gap
		// rather than an oversight.** What arrives is `server.Position` and
		// `server.Velocity`; what the renderer needs is `Transform`, `Visual`
		// and a `DrawList`, and the two programs declare none of those in
		// common. `mono.engine/scene` at v0.4 is the item that gives both halves
		// one set of components, and drawing this is the first thing that falls
		// out of it. Bridging it here would mean a translation system this
		// program would then have to delete.

		Connection->Advance(nowSeconds);
	}

	void Client::PumpEvents() {
		ENGINE_PROFILE("pump events");

		// Cleared before the pump, not after: an action fired during this
		// frame's events has to survive until something reads it.
		Actions.BeginFrame();

		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			Actions.HandleEvent(event);
		}

		if (Actions.Fired(Action::Quit)) {
			Running = false;
		}

		if (Actions.Fired(Action::ToggleStatistics)) {
			Settings.ShowStatistics = !Settings.ShowStatistics;
		}

		if (Actions.Fired(Action::ToggleFrameGraph)) {
			Settings.ShowFrameGraph = !Settings.ShowFrameGraph;
			// Collection is off until something asks for it, so opening the
			// panel is what turns it on.
			FrameGraph::SetEnabled(Settings.ShowFrameGraph);
			ProfilerScroll = 0;
		}

		const auto tabCount = static_cast<int>(ProfilerTab::Count);
		if (Actions.Fired(Action::NextProfilerTab)) {
			Settings.Tab = static_cast<ProfilerTab>((static_cast<int>(Settings.Tab) + 1) % tabCount);
			ProfilerScroll = 0;
		}
		if (Actions.Fired(Action::PreviousProfilerTab)) {
			Settings.Tab =
				static_cast<ProfilerTab>((static_cast<int>(Settings.Tab) + tabCount - 1) % tabCount);
			ProfilerScroll = 0;
		}

		if (Actions.Fired(Action::ScrollProfilerUp)) {
			ProfilerScroll = std::max(0, ProfilerScroll - 4);
		}
		if (Actions.Fired(Action::ScrollProfilerDown)) {
			ProfilerScroll += 4;
		}

		// Clamped to what was recorded. Offering a depth past MAXIMUM_DEPTH
		// would suggest there is something deeper to reveal, and there is not:
		// nothing below it was ever stored.
		if (Actions.Fired(Action::DecreaseProfilerDepth) && ProfilerDepth > 0) {
			ProfilerDepth--;
		}
		if (Actions.Fired(Action::IncreaseProfilerDepth) && ProfilerDepth < FrameGraph::MAXIMUM_DEPTH) {
			ProfilerDepth++;
		}

		if (Actions.Fired(Action::WriteProfilerSnapshot)) {
			WriteSnapshot();
		}
	}

	void Client::WriteSnapshot() {
		// Beside the binary rather than in the working directory, which is
		// wherever the launcher happened to be.
		const auto path = engine::core::Paths::Base() / "frame-graph-snapshot.txt";

		if (!FrameGraph::WriteSnapshot(path)) {
			// The overwhelmingly likely reason, and the one worth naming:
			// collection only retains while the panel is open, so F8 with F5
			// closed has nothing to write.
			ENGINE_WARN(
				"no snapshot written to {} — nothing retained. The frame graph only records "
				"while it is open, so press F5 first.",
				path.string()
			);
			return;
		}

		ENGINE_INFO(
			"snapshot: {} frame(s) over {:.2f}s written to {}",
			FrameGraph::HistoryFrames(),
			FrameGraph::HistorySeconds(),
			path.string()
		);
	}

	void Client::Step() {
		const float delta = Clock.Tick();

		// Everything from here to EndFrame is one frame's worth of spans. The
		// panels below draw the *previous* frame's, because this one has not
		// finished being measured.
		FrameGraph::BeginFrame();

		PumpEvents();

		// Simulation and rendering advance at different rates, and this is
		// where they separate. The frame runs as fast as the display and the
		// GPU allow; the simulation runs a whole number of fixed steps, which
		// is often zero on a fast machine and several after a stall.
		//
		// A system therefore never sees a variable delta, so the same scene
		// behaves identically at 30 fps and 600 — and a recorded run replays.
		// RENDER_PIPELINE.md §14.
		{
			// The universe owns the accumulator now, so the world runs however
			// many fixed ticks it owes and the client no longer keeps a second
			// copy of the rate it is running at.
			ENGINE_PROFILE_CAT("simulation", engine::core::ProfileCategory::Simulation);
			Universe_->Tick(delta);
		}

		{
			// After the tick and before presentation, the same place the server
			// publishes from — so what the replica applied this frame is what
			// the frame draws, rather than being one frame stale for no reason.
			ENGINE_PROFILE_CAT("replication", engine::core::ProfileCategory::ECS);
			PollServer(engine::core::Clock::Seconds());
		}

		{
			// Once per frame, and separate from the tick because a client draws
			// one world while the rest keep simulating. This is the phase that
			// turns simulation state into something to draw, so it is the one
			// that interpolates.
			ENGINE_PROFILE_CAT("pre-render", engine::core::ProfileCategory::Simulation);

			// Every world, not only the one whose camera is used. A world that
			// is composited but not presented would publish the frame it built
			// last time it was, which is a world that appears frozen for a
			// reason nothing reports.
			for (const engine::world::WorldId id : Simulated) {
				Universe_->Present(id, delta, Universe_->AlphaOf(id));

				// Published from inside the world, straight after its PreRender
				// phase filled the draw list. The camera and the list stay
				// where they were produced; what leaves is a copy in a buffer
				// the renderer owns the other end of.
				Universe_->Enter(id, [this, id](engine::ecs::Store &store) {
					const auto *camera = store.Resource<ActiveCamera>();
					const auto *list = store.Resource<DrawList>();
					if (camera == nullptr || list == nullptr) {
						return;
					}
					Views.Publish(id, camera->Value, list->Instances, store.Time().Tick, store.Time().Alpha);
				});
			}
		}

		Statistics.Record(Clock.Now(), delta);

		// Ticks actually achieved, over a one-second window. It matches the
		// configured rate until the machine cannot keep up, and the gap is the
		// number worth seeing.
		if (Clock.Now() - TickWindowStarted >= 1.0) {
			const auto elapsed = static_cast<float>(Clock.Now() - TickWindowStarted);
			const uint64_t ticksNow = Universe_->StatisticsOf(Rendered).Ticks;
			MeasuredTicksPerSecond = static_cast<float>(ticksNow - TicksAtWindowStart) / elapsed;
			TickWindowStarted = Clock.Now();
			TicksAtWindowStart = ticksNow;
		}

		int pixelWidth = 0;
		int pixelHeight = 0;
		SDL_GetWindowSizeInPixels(Window, &pixelWidth, &pixelHeight);

		{
			ENGINE_PROFILE_CAT("debug panels", engine::core::ProfileCategory::Render);

			Overlay.Resize(pixelWidth, pixelHeight);

			// The panels are redrawn on a clock of their own, and presented on
			// every frame from the texture they were last drawn into.
			//
			// They are read by a person, and a person cannot read a number that
			// changes a thousand times a second — past about twenty updates a
			// second the extra work buys a blur. Rasterising the glyphs and
			// pushing the image across were together the largest thing in the
			// frame, and at 1000 fps this is fifty times less of both.
			//
			// The *collection* is untouched: FrameGraph and Metrics still record
			// every frame, so nothing is missed. Only the drawing is throttled,
			// and RMAX still reports the worst frame in the window rather than
			// the worst frame that happened to be drawn.
			constexpr double PANEL_UPDATE_SECONDS = 1.0 / 20.0;

			// Anything a key press changed has to appear at once, or the panel
			// feels broken: pressing F6 and waiting fifty milliseconds for the
			// tab to change reads as a dropped input.
			const bool settingsChanged =
				PanelsShown != (Settings.ShowStatistics || Settings.ShowFrameGraph) ||
				PanelTab != Settings.Tab || PanelScroll != ProfilerScroll || PanelDepth != ProfilerDepth ||
				PanelWidth != pixelWidth || PanelHeight != pixelHeight;

			const bool redraw = settingsChanged || Clock.Now() - PanelsDrawn >= PANEL_UPDATE_SECONDS;

			if (redraw) {
				PanelsDrawn = Clock.Now();
				PanelsShown = Settings.ShowStatistics || Settings.ShowFrameGraph;
				PanelTab = Settings.Tab;
				PanelScroll = ProfilerScroll;
				PanelDepth = ProfilerDepth;
				PanelWidth = pixelWidth;
				PanelHeight = pixelHeight;
			}

			if (!redraw) {
				// Counters accumulate whether or not anyone is looking, and a
				// frame that does not draw them still has to drain them or the
				// next panel shows several frames added together.
				Metrics::Clear();
			} else if (Settings.ShowStatistics || Settings.ShowFrameGraph) {
				SystemTimings.clear();
				Universe_->Enter(Rendered, [this](engine::ecs::Store &, engine::ecs::Scheduler &systems) {
					for (const auto &timing : systems.Timings()) {
						SystemTimings.push_back(
							engine::render::SystemTiming{timing.Name, timing.Milliseconds}
						);
					}
				});

				const auto counters = Metrics::Drain();

				engine::render::DebugPanelData panels;
				panels.ShowStatistics = Settings.ShowStatistics;
				panels.ShowFrameGraph = Settings.ShowFrameGraph;
				panels.Tab = Settings.Tab;
				panels.Scroll = ProfilerScroll;
				panels.DepthLimit = ProfilerDepth;
				panels.HistorySeconds = FrameGraph::HistorySeconds();
				panels.TracyAttached = ENGINE_PROFILE_ATTACHED();
				panels.Statistics = &Statistics;
				panels.Spans = FrameGraph::Spans();
				panels.FrameMilliseconds = FrameGraph::FrameMilliseconds();
				panels.UnmarkedMilliseconds = FrameGraph::UnmarkedMilliseconds();
				panels.IdleMilliseconds =
					FrameGraph::CategoryMilliseconds(engine::core::ProfileCategory::Idle);
				panels.DroppedSpans = FrameGraph::Dropped();
				panels.Systems = SystemTimings;
				panels.Counters = counters;
				// Asked of the world, not read back off the command line. The
				// number that matters is what the world actually holds, and
				// the day something spawns or destroys an entity those two
				// stop being the same.
				panels.Entities = 0;
				for (const engine::world::WorldId id : Simulated) {
					Universe_->Enter(id, [&panels](engine::ecs::Store &store) {
						panels.Entities += store.CountMatching<Transform, Visual>();
					});
				}
				panels.TickRate = Settings.TickRate;
				panels.TicksPerSecond = MeasuredTicksPerSecond;
				panels.DroppedTicks = Universe_->StatisticsOf(Rendered).DroppedTicks;
				panels.DrawCalls = LastFrame.DrawCalls;
				panels.Triangles = LastFrame.Triangles;
				panels.Backend = Renderer.BackendName();
				// One logical pixel of the font per two physical, so the panels
				// stay the same apparent size on a high-DPI display.
				panels.Scale = pixelWidth >= 2400 ? 3 : 2;

				engine::render::DrawDebugPanels(Overlay, panels);
			} else {
				// Nothing drawn means nothing uploaded and no overlay pass.
				Overlay.Clear();
				// Counters accumulate whether or not anyone is looking, so
				// they still have to be drained.
				Metrics::Clear();
			}
		}

		// Drawn from what the compositor took off the view channels, not from a
		// store. The camera was placed by a system and the draw list was filled
		// by one in PreRender — but the renderer runs at the display's rate and
		// the worlds run at their own, so reaching into a store here would be
		// reading something somebody else is writing. Between them sits three
		// slots and an atomic index, which is what lets a slow frame drop
		// rather than throttle a simulation.
		Views.Compose(Settings.Worlds > 1 ? Settings.ViewSpacing : 0.0f);
		LastFrame = Renderer.Render(Views.Camera(), Views.Instances(), Overlay);

		FrameGraph::EndFrame();
		ENGINE_PROFILE_FRAME();

		if (LastFrame.Presented) {
			FramesDrawn++;
		}
	}

	int Client::Run() {
		if (!Running) {
			return 1;
		}

		while (Running) {
			Step();

			if (Settings.MaximumFrames >= 0 && FramesDrawn >= Settings.MaximumFrames) {
				ENGINE_INFO("frame budget of {} reached", Settings.MaximumFrames);
				break;
			}

			if (Settings.ProfileSeconds > 0.0 && Clock.Now() >= Settings.ProfileSeconds) {
				ENGINE_INFO("profiled for {:.1f}s over {} frames", Clock.Now(), FramesDrawn);
				break;
			}
		}

		if (Statistics.HasSamples()) {
			ENGINE_INFO(
				"{} frames · {:.1f} avg · {:.1f} min · {:.1f} max FPS",
				FramesDrawn,
				Statistics.Average(),
				Statistics.Minimum(),
				Statistics.Maximum()
			);
			ENGINE_INFO(
				"{} tick(s) at {:.0f} Hz · {:.1f} achieved · {} dropped",
				Universe_->StatisticsOf(Rendered).Ticks,
				Settings.TickRate,
				Clock.Now() > 0.0 ? static_cast<double>(Universe_->StatisticsOf(Rendered).Ticks) / Clock.Now()
								  : 0.0,
				Universe_->StatisticsOf(Rendered).DroppedTicks
			);
		}

		return 0;
	}
}
