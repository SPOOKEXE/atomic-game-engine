#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Paths.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/parallel/Jobs.hpp>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <client/Client.hpp>

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
			// TODO(v0.5): hand this to the Luau host once L13 exists. Refusing
			// loudly beats accepting a flag and doing nothing with it.
			ENGINE_WARN(
				"--script is accepted but has no effect until the scripting layer lands in "
				"v0.5. Ignoring '{}'.",
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

		engine::parallel::Jobs::Start();

		Timestep.SetRate(Settings.TickRate);
		ENGINE_INFO("simulation at {:.0f} Hz, rendering unlocked from it", Settings.TickRate);

		Store.BindToCallingThread();
		BuildDemoWorld(Store, Scheduler, Settings.Entities);

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
		const int ticks = Timestep.Advance(delta);
		Scheduler.ClearTimings();

		{
			ENGINE_PROFILE_CAT("simulation", engine::core::ProfileCategory::Simulation);
			for (int tick = 0; tick < ticks; tick++) {
				// The world's clock moves, then the world's systems run. No
				// system is handed a delta, so none of them can be handed the
				// wrong one.
				Store.AdvanceTick(Timestep.Delta());
				Scheduler.RunPhases(
					Store, engine::ecs::Phase::PreSimulation, engine::ecs::Phase::PostSimulation
				);
			}
		}

		{
			// Once per frame, and after the alpha is known — this is the phase
			// that turns simulation state into something to draw, so it is the
			// one that interpolates.
			ENGINE_PROFILE_CAT("pre-render", engine::core::ProfileCategory::Simulation);
			Store.SetFrame(delta, Timestep.Alpha());
			Scheduler.RunPhases(Store, engine::ecs::Phase::PreRender, engine::ecs::Phase::PreRender);
		}

		Statistics.Record(Clock.Now(), delta);

		// Ticks actually achieved, over a one-second window. It matches the
		// configured rate until the machine cannot keep up, and the gap is the
		// number worth seeing.
		if (Clock.Now() - TickWindowStarted >= 1.0) {
			const auto elapsed = static_cast<float>(Clock.Now() - TickWindowStarted);
			MeasuredTicksPerSecond = static_cast<float>(Timestep.TotalTicks() - TicksAtWindowStart) / elapsed;
			TickWindowStarted = Clock.Now();
			TicksAtWindowStart = Timestep.TotalTicks();
		}

		int pixelWidth = 0;
		int pixelHeight = 0;
		SDL_GetWindowSizeInPixels(Window, &pixelWidth, &pixelHeight);

		{
			ENGINE_PROFILE_CAT("debug panels", engine::core::ProfileCategory::Render);

			Overlay.Resize(pixelWidth, pixelHeight);

			if (Settings.ShowStatistics || Settings.ShowFrameGraph) {
				SystemTimings.clear();
				for (const auto &timing : Scheduler.Timings()) {
					SystemTimings.push_back(engine::render::SystemTiming{timing.Name, timing.Milliseconds});
				}

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
				panels.DroppedSpans = FrameGraph::Dropped();
				panels.Systems = SystemTimings;
				panels.Counters = counters;
				// Asked of the world, not read back off the command line. The
				// number that matters is what the world actually holds, and
				// the day something spawns or destroys an entity those two
				// stop being the same.
				panels.Entities = Store.CountMatching<Transform, Visual>();
				panels.TickRate = Settings.TickRate;
				panels.TicksPerSecond = MeasuredTicksPerSecond;
				panels.DroppedTicks = Timestep.Dropped();
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

		// Both of these are read out of the world rather than out of a scene
		// object. The camera was placed by a system in the simulation and the
		// draw list was filled by one in PreRender, so what is drawn is exactly
		// what the tick produced.
		LastFrame = Renderer.Render(
			Store.Resource<ActiveCamera>()->Value, Store.Resource<DrawList>()->Instances, Overlay
		);

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
				Timestep.TotalTicks(),
				Settings.TickRate,
				Clock.Now() > 0.0 ? static_cast<double>(Timestep.TotalTicks()) / Clock.Now() : 0.0,
				Timestep.Dropped()
			);
		}

		return 0;
	}
}
