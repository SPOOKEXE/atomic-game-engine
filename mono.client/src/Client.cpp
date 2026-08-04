#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Paths.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/game/Game.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/parallel/Process.hpp>
#include <engine/scene/ActiveCamera.hpp>
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
			ENGINE_INFO("scene from {}", Settings.ScriptPath);
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

		if (!Settings.GameFile.empty()) {
			if (!LoadGameFile()) {
				return false;
			}
		} else if (!BuildDemoWorlds()) {
			return false;
		}

		// The first is the one the panels report on and the one the composed
		// camera comes from. A client draws one world's worth of camera however
		// many it composites.
		Rendered = Simulated.front();

		if (!BeginConnecting()) {
			return false;
		}

		if (Simulated.size() > 1) {
			ENGINE_INFO("compositing {} worlds, {:.0f} units apart", Simulated.size(), Settings.ViewSpacing);
		}

		FrameGraph::SetEnabled(Settings.ShowFrameGraph);
		return FinishStartup();
	}

	bool Client::LoadGameFile() {
		engine::game::GameInfo info;
		std::string error;

		if (!engine::game::LoadGame(*Universe_, Settings.GameFile, info, error)) {
			ENGINE_ERROR("--game '{}' failed: {}", Settings.GameFile.string(), error);
			return false;
		}

		const auto worlds = Universe_->Worlds();
		if (worlds.empty()) {
			ENGINE_ERROR("--game '{}' holds no worlds", Settings.GameFile.string());
			return false;
		}

		// **Both halves, in one process.** A single-player run is a server and
		// a client at once — `HostRole::OfBoth` says so and `RunService.hpp`
		// argues that both being true is a legal answer rather than a bug — so
		// a game's `Script` and its `LocalScript` both run here.
		engine::script::RuntimeLimits limits;
		limits.Role = engine::script::HostRole::OfBoth();

		for (const engine::world::WorldId id : worlds) {
			std::string failure;

			Universe_->Enter(id, [&](engine::ecs::Store &store, engine::ecs::Scheduler &systems) {
				InstallPresentation(store, systems, Settings.Entities);

				// The scripts before the camera, so a scene that aimed one of
				// its own keeps it — see `InstallDefaultCamera`.
				Runtimes.push_back(engine::game::StartWorldScripts(store, systems, limits, failure));
				InstallDefaultCamera(store, systems);
			});

			if (!failure.empty()) {
				ENGINE_ERROR("world '{}': {}", Universe_->NameOf(id).Text(), failure);
			}

			Views.Track(id, Universe_->NameOf(id), Settings.Entities);
			Simulated.push_back(id);
		}

		ENGINE_INFO(
			"playing '{}' — {} world(s)", info.Name.IsValid() ? info.Name.Text() : "game", worlds.size()
		);
		return true;
	}

	bool Client::BuildDemoWorlds() {
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

			// **There is one path now, and it is the scripted one.**
			// `BuildDemoWorld` built the ring scene in C++ and died at v0.6:
			// `Rings.luau` builds the same scene through the same class table,
			// and keeping both would have been two ways to do one job — the
			// most expensive kind of debt in a monorepo, because both accumulate
			// callers.
			//
			// `--script` with no argument therefore falls back to the example
			// rather than to a second implementation.
			const std::string scenePath = Settings.ScriptPath.empty()
											  ? engine::examples::ExamplePath("Rings.luau")
											  : Settings.ScriptPath;

			bool scripted = true;
			Universe_->Enter(
				id,
				[this, &scripted, &scenePath](engine::ecs::Store &store, engine::ecs::Scheduler &systems) {
					// A script that fails leaves an empty world rather than a
					// half-built one, so the failure is reported here and the
					// client stops instead of presenting a black screen and
					// letting somebody wonder why.
					scripted = BuildScriptedWorld(store, systems, scenePath, Settings.Entities);
				}
			);

			if (!scripted) {
				ENGINE_ERROR("the scene script failed, so there is nothing to render");
				return false;
			}

			// Sized at the world's entity count rather than at what it drew
			// this frame, so publishing never allocates. A demo world draws at
			// most one instance per entity.
			Views.Track(id, world.Name, Settings.Entities);
			Simulated.push_back(id);
		}

		return true;
	}

	bool Client::FinishStartup() {
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

		Universe_->Enter(Replicated, [this](engine::ecs::Store &store, engine::ecs::Scheduler &systems) {
			// The v0.2 refusal, used for what it was reserved for. A replica
			// that published to a bus would be telling the universe
			// something the server never said; the inbox still delivers,
			// which is how it receives.
			store.SetResource(engine::world::Replica{true});

			// A replicated world runs no *simulation* system: everything in
			// it arrived, and simulating it here would be this process
			// disagreeing with the authority once per tick. What this
			// installs is the `PreRender` half — the draw list and the
			// system that fills it — which derives what to draw and writes
			// no component.
			//
			// **The rate the snapshot buffer measures its delay against is the
			// server's, and nothing on the wire carries it.** What is passed is
			// the rate this process was told to run at, which is the same
			// default both programs take. A disagreement is absorbed by the
			// buffer's own correction up to a few percent, and past that shows
			// as `replica.stalls` rather than as something mysterious.
			engine::replication::InterpolationSettings interpolation;
			interpolation.TickRate = Settings.TickRate;

			BuildReplicatedWorld(store, systems, interpolation);
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

			// **Here, not in the render pass.** This instant is the one where
			// the store holds the tick the server described; a pass that only
			// ran when a frame was drawn would miss a received tick whenever
			// the frame rate dipped below the tick rate, and the buffer would
			// then be interpolating across gaps the network never produced.
			RecordReplicatedTick(store, Connection->Applied());
		});

		// The exchange, before the world. A client that sat there with an empty
		// scene used to have one explanation; it now has two, and the log has to
		// say which — the handshake never finished, or it finished and the
		// snapshot has not arrived.
		if (!ReportedAdmission && (Connection->Admitted() || Connection->Rejected())) {
			ReportedAdmission = true;
			if (Connection->Admitted()) {
				ENGINE_INFO("admitted by {}, waiting for the world", Settings.ConnectAddress);
			} else {
				ENGINE_ERROR("the server at {} did not admit this client", Settings.ConnectAddress);
			}
		}

		// Once, on the tick it becomes true. A client that logged this every
		// frame would write six hundred lines a second saying the same thing.
		//
		// This is also where the replicated world starts being drawn, and the
		// reason it waits for the join is the channel's size: a view channel
		// allocates its slots once so that publishing never allocates, and the
		// only number this process has to size one with is what actually
		// arrived. Doubled, so a world that grows a little afterwards still
		// publishes; past that `Compositor::Publish` refuses and says so, which
		// beats a frame with holes in it.
		if (!ReportedJoin && Connection->Joined()) {
			ReportedJoin = true;

			size_t entities = 0;
			Universe_->Enter(Replicated, [&entities](engine::ecs::Store &store) {
				store.EachEntity([&entities](engine::ecs::Entity) { entities++; });
			});

			Views.Track(Replicated, engine::core::Name("client.replica"), entities * 2);

			ENGINE_INFO("joined: {} entities at tick {}", entities, Connection->Applied());
		}

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

		if (Actions.Fired(Action::ToggleNetwork)) {
			// **Refused rather than toggled when there is nothing to show.**
			// A client run without `--connect` has no link, and a network panel
			// full of zeroes reads as a link that is up and idle. Saying so once
			// beats a key that silently does nothing, which reads as a broken
			// binding.
			if (Connection == nullptr) {
				if (!ReportedNoNetwork) {
					ReportedNoNetwork = true;
					ENGINE_INFO("F4: no network panel — this client was not given --connect");
				}
			} else {
				Settings.ShowNetwork = !Settings.ShowNetwork;
			}
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
		// **The display is waited for before the input is read**, for the reason
		// `Editor::Run` gives at length: the swapchain wait is most of a frame
		// with vertical sync on, and doing it after the pump means every frame is
		// drawn from input that is already a frame old. The client has the same
		// shape as the studio and had the same frame of delay in it.
		//
		// It costs a frame of nothing when it fails — minimised, or mid-resize —
		// and `Render` reaches the same conclusion for itself below.
		Renderer.WaitForFrame();

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
			// **Cleared once, before any world is asked.** `CollectSurfaceViews`
			// clears as well, but only the drawn world reaches it — a world that
			// returns early for want of a camera would otherwise leave the
			// previous frame's mirrors in the list, and the surface pass would
			// go on rendering a camera that is no longer in the scene.
			Surfaces.clear();

			for (const engine::world::WorldId id : Simulated) {
				Universe_->Present(id, delta, Universe_->AlphaOf(id));

				// Published from inside the world, straight after its PreRender
				// phase filled the draw list. The camera and the list stay
				// where they were produced; what leaves is a copy in a buffer
				// the renderer owns the other end of.
				Universe_->Enter(id, [this, id](engine::ecs::Store &store) {
					const auto *active = store.Resource<engine::scene::ActiveCamera>();
					const auto *list = store.Resource<DrawList>();
					if (active == nullptr || list == nullptr) {
						return;
					}

					// The live camera is a row: `ActiveCamera` names which
					// entity it is and the placement and the lens are the
					// components on it.
					const auto *placement = store.Get<engine::scene::Transform>(active->Entity);
					const auto *lens = store.Get<engine::scene::Camera>(active->Entity);
					if (placement == nullptr || lens == nullptr) {
						return;
					}

					if (id == Rendered) {
						// Kept for the replicated view below, which has no
						// camera of its own.
						ComposedFrame = placement->Frame;
						ComposedCamera = *lens;

						// **The surface cameras, read from the world that owns
						// them.** All of them: the pipeline renders one offscreen
						// view per surface index since v0.8, so a room of
						// mirrored walls gets a working mirror per wall rather
						// than one wall's image projected across all four.
						(void)CollectSurfaceViews(store, Surfaces);
					}

					Views.Publish(
						id, placement->Frame, *lens, list->Instances, store.Time().Tick, store.Time().Alpha
					);
				});
			}

			// The replicated world, once it has joined and been given a
			// channel. Presented like any other world — `PreRender` is where
			// deriving what to draw belongs, whoever owns the simulation.
			//
			// **It is looked at through this client's own camera**, because a
			// replica has none: a camera is an entity, and an authoritative
			// entity minted in a replica collides exactly with one the server
			// minted, which is what `Store::SetAdoptOnly` refuses. A local row
			// in a replicated world is safe — `Store::CreatePredicted` mints
			// from a range the server never allocates from — and since v0.8
			// something does own it: `AimReplicaViewer` puts a predicted camera
			// in the replica and names it `ActiveCamera`.
			//
			// **Before `Present`, because `PreRender` is where the mirrors are
			// aimed.** `aim-surface-cameras` reads `ActiveCamera` and reflects
			// through it, so setting the eye afterwards would aim every mirror
			// at where the client stood last frame.
			if (ReportedJoin) {
				Universe_->Enter(Replicated, [this](engine::ecs::Store &store) {
					(void)AimReplicaViewer(store, ComposedFrame, ComposedCamera);
				});

				Universe_->Present(Replicated, delta, Universe_->AlphaOf(Replicated));

				Universe_->Enter(Replicated, [this](engine::ecs::Store &store) {
					const auto *list = store.Resource<DrawList>();
					if (list == nullptr) {
						return;
					}
					Views.Publish(
						Replicated,
						ComposedFrame,
						ComposedCamera,
						list->Instances,
						store.Time().Tick,
						store.Time().Alpha
					);
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
				PanelsShown != (Settings.ShowStatistics || Settings.ShowNetwork || Settings.ShowFrameGraph) ||
				PanelTab != Settings.Tab || PanelScroll != ProfilerScroll || PanelDepth != ProfilerDepth ||
				PanelWidth != pixelWidth || PanelHeight != pixelHeight;

			const bool redraw = settingsChanged || Clock.Now() - PanelsDrawn >= PANEL_UPDATE_SECONDS;

			if (redraw) {
				PanelsDrawn = Clock.Now();
				PanelsShown = Settings.ShowStatistics || Settings.ShowNetwork || Settings.ShowFrameGraph;
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
			} else if (Settings.ShowStatistics || Settings.ShowNetwork || Settings.ShowFrameGraph) {
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
				panels.ShowNetwork = Settings.ShowNetwork;
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
						panels.Entities +=
							store.CountMatching<engine::scene::Transform, engine::scene::Visual>();
					});
				}
				if (ReportedJoin) {
					// The replica counts too. A number that ignored it would
					// say the client is drawing fewer things than it is.
					Universe_->Enter(Replicated, [&panels](engine::ecs::Store &store) {
						panels.Entities +=
							store.CountMatching<engine::scene::Transform, engine::scene::Visual>();
					});
				}
				panels.TickRate = Settings.TickRate;
				panels.TicksPerSecond = MeasuredTicksPerSecond;
				panels.DroppedTicks = Universe_->StatisticsOf(Rendered).DroppedTicks;
				panels.DrawCalls = LastFrame.DrawCalls;
				panels.Triangles = LastFrame.Triangles;
				panels.Backend = Renderer.BackendName();
				panels.Network = SampleNetwork();
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
		// Counted rather than read off the option, because `--connect` adds a
		// view the option does not know about. Two views drawn on top of each
		// other is two scenes inside one, which reads as a rendering fault.
		Views.Compose(Views.Count() > 1 ? Settings.ViewSpacing : 0.0f);
		LastFrame =
			Renderer.Render(Views.CameraFrame(), Views.Camera(), Views.Instances(), Overlay, Surfaces);

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
				// **The pass counts are here because nothing else can report
				// them.** A shadow pass or a surface pass that silently did not
				// run looks exactly like one that ran and changed nothing, and
				// neither has a unit test — `AGENTS.md` names the GPU exception
				// and refuses a mock renderer to close it. The last frame's
				// draw calls are the cheapest honest evidence that the passes
				// are being submitted at all.
				ENGINE_INFO(
					"profiled for {:.1f}s over {} frames · {} draw call(s), {} culled, {} surfaced, "
					"{} surface pass(es)",
					Clock.Now(),
					FramesDrawn,
					LastFrame.DrawCalls,
					LastFrame.Culled,
					LastFrame.SurfaceInstances,
					LastFrame.SurfacePasses
				);
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

		ReportReplica();
		return 0;
	}

	engine::render::NetworkStatistics Client::SampleNetwork() {
		engine::render::NetworkStatistics network;
		if (Connection == nullptr) {
			// Not connected stays not connected, and every field below stays
			// zero. `DrawDebugPanels` draws nothing at all for this.
			return network;
		}

		network.Connected = true;
		network.Joined = Connection->Joined();
		network.AppliedTick = Connection->Applied();

		const engine::net::ConnectionStats &link = Connection->Link().Stats();
		network.ReceivedBytes = link.BytesReceived;
		network.SentBytes = link.BytesSent;
		network.RoundTripMilliseconds = link.RoundTripMilliseconds;
		network.PacketsLost = link.PacketsLost;
		network.PacketsStale = link.PacketsStale;
		network.SendsOverBudget = link.SendsOverBudget;

		// **Rates are a derivative and `net` only keeps the integral.** Every
		// counter above is cumulative over the connection's life, so a rate has
		// to come from two readings and the time between them. Taken here
		// rather than in `net` because a counter that had to be sampled on a
		// clock would be a counter that read one, and this module's whole
		// discipline is that it does not.
		//
		// The window is however long it has been since the last panel redraw —
		// a twentieth of a second at the panel's own rate. Short enough to
		// follow a stream that starts and stops, long enough that it is not one
		// packet's worth of noise.
		const double now = Clock.Now();
		const double elapsed = now - NetworkSampledAt;
		if (NetworkSampled && elapsed > 0.0) {
			network.ReceivedBytesPerSecond =
				static_cast<double>(link.BytesReceived - NetworkLastReceivedBytes) / elapsed;
			network.SentBytesPerSecond = static_cast<double>(link.BytesSent - NetworkLastSentBytes) / elapsed;
			network.ReceivedPacketsPerSecond =
				static_cast<double>(link.PacketsReceived - NetworkLastReceivedPackets) / elapsed;
			network.SentPacketsPerSecond =
				static_cast<double>(link.PacketsSent - NetworkLastSentPackets) / elapsed;
		}

		NetworkSampled = true;
		NetworkSampledAt = now;
		NetworkLastReceivedBytes = link.BytesReceived;
		NetworkLastSentBytes = link.BytesSent;
		NetworkLastReceivedPackets = link.PacketsReceived;
		NetworkLastSentPackets = link.PacketsSent;

		const engine::replication::Replica::Statistics &replica = Connection->ReplicaStats();
		network.Snapshots = replica.Snapshots;
		network.Deltas = replica.Deltas;
		network.Structures = replica.Structures;
		network.Malformed = replica.Malformed;
		network.Stale = replica.Stale;

		// The interpolation half, and the row count that says whether any of it
		// reached a draw list. Only once the world exists — before the join
		// there is no replicated store to enter.
		if (ReportedJoin) {
			Universe_->Enter(Replicated, [&network](engine::ecs::Store &store) {
				network.Entities = store.CountMatching<engine::scene::Transform>();

				if (const auto *drawList = store.Resource<DrawList>()) {
					network.Drawn = drawList->Instances.size();
				}
				if (const auto *buffer = store.Resource<engine::replication::SnapshotBuffer>()) {
					network.TickRate = buffer->MeasuredTickRate();
					network.BehindTicks = buffer->Behind();
					network.Stalls = buffer->Stats().Stalls;
					network.Interpolated = buffer->Stats().Interpolated;
					network.Held = buffer->Stats().Held;
				}
			});
		}

		return network;
	}

	void Client::ReportReplica() {
		if (Connection == nullptr) {
			return;
		}

		// **A replica that joined and drew nothing reads from outside exactly
		// like a replica that never joined**, and the difference is four
		// numbers this process already has. Printed at exit rather than left in
		// the F3 panel, because the reported symptom — "nothing appears in the
		// scene" — is one somebody hits on a machine where they are looking at
		// the window rather than at a counter, and a run with `--frames`
		// produces no window to look at at all.
		//
		// Read in this order, and the first one that is wrong is the answer:
		// rows arrived, rows were drawn, the world moved between ticks.
		size_t entities = 0;
		size_t drawn = 0;
		double behind = 0.0;
		uint64_t stalls = 0;
		uint64_t interpolated = 0;
		uint64_t held = 0;
		double rate = 0.0;

		Universe_->Enter(Replicated, [&](engine::ecs::Store &store) {
			store.EachEntity([&entities](engine::ecs::Entity) { entities++; });

			if (const auto *drawList = store.Resource<DrawList>()) {
				drawn = drawList->Instances.size();
			}
			if (const auto *buffer = store.Resource<engine::replication::SnapshotBuffer>()) {
				behind = buffer->Behind();
				stalls = buffer->Stats().Stalls;
				interpolated = buffer->Stats().Interpolated;
				held = buffer->Stats().Held;
				rate = buffer->MeasuredTickRate();
			}
		});

		ENGINE_INFO(
			"replica: {} entities · {} drawn · {:.2f} ticks behind · {} stall(s) · {} interpolated / {} held "
			"· {:.1f} Hz measured",
			entities,
			drawn,
			behind,
			stalls,
			interpolated,
			held,
			rate
		);

		// **Drawn and never seen is the third case, and it is a framing
		// problem rather than a replication one.** The composited camera is the
		// demo world's: it is placed from *that* world's bounds and its far
		// plane follows the same distance, so a replicated world larger than
		// the demo is drawn outside a frustum sized for something else.
		// `mono.client/AGENTS.md` records the camera of its own that fixes it.
		if (drawn > 0) {
			ENGINE_INFO(
				"replica: drawn through the demo world's camera — `--view-spacing 0` overlays the two if the "
				"replicated world is not on screen"
			);
		}
	}
}
