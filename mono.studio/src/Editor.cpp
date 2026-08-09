#include <engine/core/Bytes.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Paths.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/parallel/Process.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Interpolation.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/ui/Theme.hpp>

#include <SDL3/SDL.h>
#include <spdlog/sinks/base_sink.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <client/Replicated.hpp>
#include <client/Scene.hpp>
#include <imgui.h>
#include <mutex>
#include <studio/Editor.hpp>
#include <studio/Presentation.hpp>
#include <studio/RojoSync.hpp>

#include <fstream>
#include <sstream>
#include <studio/Keybinds.hpp>
#include <studio/Widgets.hpp>

namespace studio {

	using engine::core::CFrame;
	using engine::core::LogLevel;
	using engine::core::Name;
	using engine::core::Vector3;
	using engine::ecs::NULL_ENTITY;
	using engine::ecs::Phase;
	using engine::ecs::Scheduler;
	using engine::ecs::Store;
	using engine::world::WorldSettings;
	using engine::world::WorldStatus;

	namespace {
		// How many lines the output panel keeps.
		//
		// Bounded rather than unbounded, because a script erroring every tick
		// at sixty ticks a second fills memory in an afternoon — and the last
		// thousand lines are the ones anybody reads.
		constexpr size_t OUTPUT_LIMIT = 1024;

		// The name a brand-new game and its first world take.
		constexpr std::string_view DEFAULT_GAME = "Untitled";
		constexpr std::string_view DEFAULT_WORLD = "Start";

		// The two worlds a new game opens with. See `Editor::NewGame` for why
		// there are two of them and why they are these two.
		constexpr std::string_view SKYGRID_WORLD = "SkyGrid";
		constexpr std::string_view MIRROR_WORLD = "Mirrors";
		constexpr std::string_view MESHGRID_WORLD = "Meshes";
	}

	// The engine log, teed into the Output panel.
	//
	// **Without this the Output panel is a list of the editor's own
	// announcements**, and the one thing an author actually wants there — what
	// their script printed, and the error when it stopped — goes to a terminal
	// they did not open. `print`, `warn` and `error` are userland globals that
	// land in this logger, which `core/Log.hpp` says in its first sentence.
	//
	// **Thread-safe because it has to be.** A world ticks on a job worker, a
	// script's error is logged from there, and the panel is drawn on the driver
	// thread. `base_sink<std::mutex>` gives the lock; `Take` hands the lines
	// over and leaves none, so nothing is drawn twice.
	class PanelSink final : public spdlog::sinks::base_sink<std::mutex> {
	  public:
		std::vector<Message> Take() {
			std::lock_guard lock(Guard);
			std::vector<Message> taken;
			taken.swap(Pending);
			return taken;
		}

	  protected:
		void sink_it_(const spdlog::details::log_msg &message) override {
			// The payload rather than the formatted line. A panel line does not
			// want the timestamp and the logger name that a terminal does —
			// there is one process, the lines are in order, and the width is
			// worth more than the prefix.
			Message line;
			line.Text.assign(message.payload.data(), message.payload.size());

			switch (message.level) {
			case spdlog::level::err:
			case spdlog::level::critical:
				line.Level = LogLevel::Error;
				break;
			case spdlog::level::warn:
				line.Level = LogLevel::Warning;
				break;
			default:
				line.Level = LogLevel::Info;
				break;
			}

			std::lock_guard lock(Guard);

			// Bounded here as well as in the panel, because a script erroring
			// every tick at sixty ticks a second fills this between two frames
			// whatever the panel does with it afterwards.
			if (Pending.size() < 4096) {
				Pending.push_back(std::move(line));
			}
		}

		void flush_() override {}

	  private:
		std::mutex Guard;
		std::vector<Message> Pending;
	};

	const char *Describe(RunMode mode) {
		switch (mode) {
		case RunMode::Edit:
			return "Edit";
		case RunMode::Server:
			return "Run";
		case RunMode::Play:
			return "Play";
		}
		return "?";
	}

	Editor::Editor() = default;

	Editor::~Editor() {
		Shutdown();
	}

	bool Editor::Initialise(const Options &options) {
		Settings = options;

		// The panels the command line asked for. Held as editor state rather
		// than read from `Settings` each frame, because F7 and F8 toggle them
		// and a flag that could be turned on from two places and off from one
		// is a flag that gets stuck.
		ShowStatistics = Settings.ShowStatistics;
		ShowFrameGraph = Settings.ShowFrameGraph;
		ShowAssets = Settings.ShowAssetsPanel;
		IdleCloseSeconds = Settings.IdleCloseSeconds;
		Extras[0].Open = Settings.ShowSecondViewport;

		// Before anything reads a file. Changing it later would leave whatever
		// had already loaded pointing at the old tree.
		if (!Settings.Assets.empty()) {
			engine::core::Paths::SetAssetsOverride(Settings.Assets);
			ENGINE_INFO("assets from {}", Settings.Assets.string());
		}

		if (!SDL_Init(SDL_INIT_VIDEO)) {
			ENGINE_ERROR("SDL_Init: {}", SDL_GetError());
			return false;
		}

		if (!Settings.Headless) {
			Window = SDL_CreateWindow(
				"atomic studio",
				Settings.Width,
				Settings.Height,
				SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY
			);
			if (!Window) {
				ENGINE_ERROR("SDL_CreateWindow: {}", SDL_GetError());
				return false;
			}
		}

		// Null when headless, which is what puts the renderer in that mode.
		if (!Renderer.Initialise(Window)) {
			return false;
		}

		// **After the renderer and only with a window**, because the present
		// mode belongs to a swapchain and a headless run has none. The client
		// makes the same call for `--uncapped`; see `Options::Uncapped` for why
		// an editor wants it for a different reason.
		//
		// **Every run rather than only under a flag.** The editor's default is
		// unpaced-and-capped rather than paced by the display, so this is the
		// call that puts the swapchain in the state `Editor::VerticalSync`
		// already claims it is in. The flag only clears the ceiling, and is not
		// read again — the Preferences page is what moves either of them after
		// this point.
		if (Settings.Uncapped) {
			FrameCap = 0.0f;
		}

		if (!Settings.Headless && !Renderer.SetVerticalSync(false)) {
			// **The preference follows the device rather than the other way
			// round.** A driver with no immediate present mode is an ordinary
			// machine, not a fault, and the frame limiter below must not run
			// against a display that is already pacing the frame.
			ENGINE_WARN("this device will not present without vblank; frames stay paced by the display");
			VerticalSync = true;
		}

		engine::ui::InterfaceSettings interfaceSettings;
		interfaceSettings.Scale = Settings.Scale;
		interfaceSettings.Docking = true;
		interfaceSettings.DisplayWidth = Settings.Width;
		interfaceSettings.DisplayHeight = Settings.Height;

		// Beside the binary rather than in the working directory, which is
		// wherever the launcher happened to be. A layout that moved every time
		// somebody started the editor from a different shell would read as the
		// editor forgetting it.
		interfaceSettings.LayoutPath = (engine::core::Paths::Base() / "studio-layout.ini").string();

		// **Beside the layout, for the layout's reason.** Keys are a thing
		// somebody sets once and expects to find again; a table forgotten on
		// exit is a rebinding page that does not really rebind anything. Every
		// action ships unbound, so a fresh install reads nothing and the menus
		// are the whole interface until somebody says otherwise. See
		// `Keybinds::Load`.
		ContentSourcesPath = engine::core::Paths::Base() / "studio-content.ini";
		if (!Content.Load(ContentSourcesPath)) {
			// A fresh install gets one origin on this machine, which is what
			// works with nothing else running — `DeliverySettings::Default`.
			Content = ContentSources::Default();
		}

		// **Built here rather than lazily on the first fetch**, so a
		// misconfigured source says so at start-up in the log rather than as a
		// stream of individually plausible failures later — `ContentRoot::Mount`'s
		// rule, which every other resolver in this stack already follows.
		RebuildContentClients();

		KeybindPath = engine::core::Paths::Base() / "studio-keybinds.ini";
		if (Keybinds::Load(KeybindPath)) {
			ENGINE_INFO("keybinds from {}", KeybindPath.string());
		}

		// **The interface runs headless, and that is the point.** Its backends
		// need a window and are not started without one — but the context is, so
		// every panel's code executes, every layout is computed and every action
		// a script or an agent triggers goes through exactly the path a person's
		// click would. What is missing is the drawing.
		if (!Interface.Initialise(Renderer, Window, interfaceSettings)) {
			ENGINE_ERROR("the editor interface would not start");
			return false;
		}

		if (!Interface.IsDrawable()) {
			ENGINE_INFO("headless: the panels run and nothing draws them");
		}

		// Attached before anything else runs, so a failure during start-up is
		// in the panel rather than only in a terminal nobody opened.
		Sink = std::make_shared<PanelSink>();
		engine::core::Log::Logger().sinks().push_back(Sink);

		engine::parallel::Jobs::Start(engine::parallel::WorkersPerHost(1));

		// **Every class a game file can name, registered before one is read.**
		// A loader that depended on somebody else having registered `Part`
		// first would fail with "no class named Part" on a perfectly good file.
		engine::scene::RegisterSceneClasses();
		engine::gui::RegisterGuiClasses();
		engine::script::ScriptClass();

		Universe = std::make_unique<engine::world::Universe>();
		Commands = std::make_unique<CommandLog>(*Universe);

		// After both, because several polls read them.
		RegisterOperators();

		if (!Settings.Game.empty()) {
			if (!OpenGame(Settings.Game)) {
				// Not fatal. An editor that refused to start because of one bad
				// file is an editor you cannot use to fix that file.
				NewGame();
			}
		} else {
			NewGame();
		}

		// **The scene a capture was asked for, made the active one.** The
		// capture photographs whichever world the drawing viewport shows, so
		// naming one has to move the viewport rather than reach past it — there
		// is one scene target per panel and only the panel that drew this frame
		// has anything in it.
		//
		// Done here, before the first frame, so every frame of the run is of the
		// scene under test rather than only the captured one. A name nothing
		// answers to is ignored: a capture is a diagnostic, and one that aborted
		// a run because a scene had been renamed would be worse than one that
		// photographs the default.
		if (!Settings.CaptureWorld.empty()) {
			const WorldId wanted = Universe->Find(Name(Settings.CaptureWorld));
			if (wanted.IsValid()) {
				Active = wanted;
				SelectionWorld = Active;
			} else {
				ENGINE_WARN("capture: no world called '{}' — capturing the active one", Settings.CaptureWorld);
			}
		}

		// Back and up, looking at the origin — where a new scene's first part
		// is. A camera at the origin looking down the axis starts inside
		// whatever gets made first, which reads as a black viewport.
		CameraYaw = -0.6f;
		CameraPitch = -0.45f;
		CameraFrame = CFrame(Vector3{18.0f, 14.0f, 18.0f});

		// **The second viewport starts where the first does.** Left at the
		// identity it sits at the origin looking down an axis, which is inside
		// or past whatever the world holds — a panel that opens showing nothing
		// reads as a panel that does not work, and that is exactly how it read.
		for (ViewportState &view : Extras) {
			view.Yaw = CameraYaw;
			view.Pitch = CameraPitch;
			view.Frame = CameraFrame;
		}

		// After the game is loaded and the camera is placed, because starting a
		// run needs worlds to start it in.
		// **Every world, because a command line means the game and not a
		// scene.** `--run play` is how a build server and a capture drive the
		// editor; there is no focused viewport to scope it to, and starting one
		// scene would leave the others silent in a check that means to exercise
		// all of them. A person picks a scene; a flag takes the lot.
		if (Settings.StartIn != RunMode::Edit) {
			for (const WorldId id : Universe->Worlds()) {
				if (!Universe->IsRemote(id)) {
					SetRunMode(id, Settings.StartIn);
				}
			}
		}

		// **After the game is loaded**, so the first thing a client can ask
		// about is a universe that has its worlds rather than an empty one.
		StartControl();

		// TODO(render-pipeline): the Pipeline Profile panel opened here with
		// `colour` watched, and `Renderer::Inspect` told the renderer to keep a
		// readable copy of that resource so the panel could show its picture.

		Running = true;
		return true;

	}

	void Editor::Shutdown() {
		// **First, and before the universe goes.** A socket thread parked on a
		// request the frame loop will never pump again would keep the process
		// alive; Stop wakes it and joins.
		ControlServer.Stop();

		// **Before anything is torn down**, because the graph's history is what
		// is being written and a snapshot taken after the universe has gone is
		// a snapshot of the shutdown.
		if (!Settings.ProfileSnapshot.empty()) {
			if (engine::core::FrameGraph::WriteSnapshot(Settings.ProfileSnapshot)) {
				ENGINE_INFO("frame graph written to {}", Settings.ProfileSnapshot.string());
			} else {
				ENGINE_ERROR("could not write {}", Settings.ProfileSnapshot.string());
			}
		}

		// **Written on the way out rather than on every edit.** The page changes
		// a binding as somebody types it, and a file rewritten per keystroke is
		// a file that records half a chord. See `Keybinds::Save`.
		if (!KeybindPath.empty() && !Keybinds::Save(KeybindPath)) {
			ENGINE_WARN("could not write {}", KeybindPath.string());
		}

		// Runtimes hold a `Store &`, and the stores are the universe's. Let go
		// of every one of them before it goes away.
		EndAllRuns();
		Runs.clear();

		// Before the universe, because it holds a reference to it.
		Commands.reset();
		Universe.reset();

		// Detached before the sink is dropped. The logger is process-wide and
		// outlives this object; a sink left on it after the editor is gone is a
		// dangling entry the next log line walks into.
		if (Sink != nullptr) {
			auto &sinks = engine::core::Log::Logger().sinks();
			sinks.erase(std::remove(sinks.begin(), sinks.end(), Sink), sinks.end());
			Sink.reset();
		}

		engine::parallel::Jobs::Stop();

		// Interface before renderer: the imgui backend releases GPU objects the
		// device made, so a device destroyed first leaves them to be freed by
		// nothing.
		Interface.Shutdown();
		Renderer.Shutdown();

		if (Window != nullptr) {
			SDL_DestroyWindow(Window);
			Window = nullptr;
		}

		SDL_Quit();
	}

	int Editor::Run() {
		while (Running) {
			// **The wait comes first, and that is the whole of the input-latency
			// fix.** `Renderer::Render` blocks the better part of a frame waiting
			// for the display, and it used to do so *after* the events had been
			// read — so every frame was built from input that was already a frame
			// old, and no amount of speed between the two would have closed it.
			// The editor was measured at 0.8 ms of CPU work in a 16.67 ms frame:
			// the delay was never the work, it was where the sleeping happened.
			//
			// Nothing is done with the result. A frame that could not be acquired
			// is minimised or mid-resize, and `Render` reaches the same
			// conclusion for itself a few lines later — checking it twice would
			// mean deciding here what to skip, which is exactly the knowledge
			// this loop does not have.
			Renderer.WaitForFrame();

			const float delta = Clock.Tick();
			const double frameBegan = Clock.Now();

			engine::core::FrameGraph::BeginFrame();

			PumpEvents();

			// **Between input and simulation**, which is where a person's click
			// would have landed. A tool that starts a world or writes a property
			// is doing what a hand on the mouse does, so it happens at the same
			// point in the frame and needs no separate ordering story.
			PumpControl();
			ControlWantsProfile = ControlSurface.WantsProfiling();

			Simulate(delta);
			Present(delta);

			engine::core::FrameGraph::EndFrame();

			// **After `EndFrame`, so the sleep is not measured as part of the
			// frame.** Inside it, the frame graph would report the editor
			// spending most of its time in "waiting", which is true and is the
			// opposite of the question the graph is opened to answer.
			//
			// Only when vertical sync is off: with it on the display already
			// paces the frame, and a second limiter would beat against it and
			// produce a stutter neither one causes alone.
			if (!VerticalSync && FrameCap > 0.0f && !Settings.Headless) {
				const double budget = 1.0 / static_cast<double>(FrameCap);
				const double spent = Clock.Now() - frameBegan;

				if (spent < budget) {
					std::this_thread::sleep_for(std::chrono::duration<double>(budget - spent));
				}
			}

			if (Settings.MaximumFrames >= 0 && FramesDrawn >= Settings.MaximumFrames) {
				Running = false;
			}
		}

		return 0;
	}

	void Editor::PumpEvents() {
		ENGINE_PROFILE("pump events");

		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			// **Every event, before anything else looks at it.** imgui decides
			// whether it wanted an event after being told about it, so a
			// program that filtered first would have a script editor that never
			// received the letter W because the camera was listening for it.
			Interface.ProcessEvent(event);

			if (event.type == SDL_EVENT_QUIT) {
				Running = false;
			}

			// **One event per dropped path, which is what makes multi-select
			// drag and drop free.** SDL brackets a multi-file drop with
			// `DROP_BEGIN` and `DROP_COMPLETE` and sends a `DROP_FILE` for each
			// path between them, so importing each as it arrives handles one
			// file, forty files and a folder without a special case for any of
			// them.
			//
			// **Not filtered by whether imgui wanted the event.** A drop has no
			// keyboard or mouse capture to respect — imgui does not consume
			// them — and a drop that only worked when the pointer was over the
			// right panel would be a rule nobody could guess.
			if (event.type == SDL_EVENT_DROP_FILE && event.drop.data != nullptr) {
				DropAssetPath(event.drop.data);
			}
			if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
				event.window.windowID == SDL_GetWindowID(Window)) {
				Running = false;
			}
		}
	}

	void Editor::Simulate(float frameSeconds) {
		// **Cleared first and set once, at the tick.** Every early return below
		// is a frame the universe does not advance, and `Present` has to know
		// which — see `studio::PresentationAlpha` for what reading it wrong
		// did.
		Advancing = false;

		if (!AnyRunning()) {
			// **A world being edited does not tick, and that is deliberate.**
			// A universe that simulated while somebody was authoring would
			// settle physics under their hands — a part placed in the air would
			// be on the floor by the time they looked away, and nothing would
			// tell them why.
			//
			// Nothing running means nothing ticks at all; the worlds that *are*
			// running are kept apart from the ones that are not by
			// `SyncWorldStates`, which suspends the rest.
			return;
		}

		// **Paused when every running scene is paused.** `Universe::Tick`
		// advances the universe rather than a world, so a half-paused universe
		// is expressed by suspending the paused worlds — which is what the loop
		// below does — rather than by skipping the tick.
		bool anyLive = false;
		for (const WorldRun &run : Runs) {
			if (!run.Paused) {
				anyLive = true;
			}
			Universe->SetState(
				run.World,
				run.Paused ? engine::world::WorldState::Suspended : engine::world::WorldState::Active
			);
		}

		if (!anyLive) {
			// **The clock stops and nothing else does.** The runtimes are
			// still alive, their connections still exist and the snapshot Stop
			// restores is untouched — a paused run resumes rather than
			// restarts. Skipping the tick is the whole of it, which is why
			// this is a flag and not a fourth `RunMode`.
			return;
		}

		// **Before the tick, so a world woken this frame ticks this frame.**
		// Opening a world and then not running it until the next frame is a
		// teleport that arrives one frame late for no reason anybody could
		// find.
		UpdateWorldLifecycle();

		ENGINE_PROFILE_CAT("simulation", engine::core::ProfileCategory::Simulation);

		// **Every world, together.** `Universe::Tick` runs them under the
		// universe's `ExecutionMode`, which is `WorldParallel` by default — so
		// subworlds are already simulated alongside each other rather than one
		// after another, and suspending the empty ones is what keeps that
		// affordable.
		// **Before the tick, not after it — and a server publishes after.** The
		// difference is the editor, and it cost a failing test to find.
		//
		// A world clears its change bits at the *start* of a tick, so bits set
		// during the tick's own phases survive until the next one begins.
		// `mono.server` therefore publishes straight after ticking and loses
		// nothing, because on a server every write happens inside a system.
		//
		// **In a studio they do not.** Dragging a part in the viewport, typing a
		// number into the properties panel, deleting an instance — every one of
		// those is a write between two ticks, and a publish that ran before them
		// and a `ClearChanges` that ran after would drop the bit without anyone
		// being told. The author would watch the server view move and the client
		// view sit still, which reads as replication being broken rather than as
		// an ordering mistake.
		//
		// Publishing here catches both: the previous tick's system writes are
		// still marked, and so is everything done to the world since. The tick
		// below then clears exactly what was just sent.
		for (WorldRun &run : Runs) {
			if (run.Paused) {
				continue;
			}
			for (const std::unique_ptr<PlayLink> &link : run.Links) {
				if (link != nullptr) {
					link->Step(*Universe);
				}
			}
		}

		Advancing = true;
		Universe->Tick(frameSeconds);
	}

	void Editor::Present(float frameSeconds) {
		// **Before the panels draw, so the numbers they show are this frame's.**
		// Sampling afterwards would put the frame-rate history one frame behind
		// the graph it is drawn beside.
		SampleFrame(frameSeconds);

		// Drained once per frame, before anything draws it. The sink collects
		// from whatever thread logged; this is the only place the panel's own
		// list is written, which is what keeps the panel free of a lock.
		if (Sink != nullptr) {
			for (Message &line : Sink->Take()) {
				Output.push_back(std::move(line));
			}
			while (Output.size() > OUTPUT_LIMIT) {
				Output.pop_front();
			}
		}

		// The interface first, because the viewport rectangle it produces is
		// what the world is projected for — and a frame that drew the world
		// against last frame's rectangle would stretch for one frame after
		// every splitter drag.
		//
		// **Profiled, and it was not.** Building the panels is most of an
		// editor's frame — every window, every table, every widget's layout —
		// and it had no span at all, so it appeared in the frame graph as a wide
		// blank between the simulation and `Renderer::Render`. The panel was
		// telling the truth twice over and neither reading was legible: the gap
		// *was* the interface, and `unmarked` was already counting it. A hole in
		// a flame graph reads as a broken widget, which is exactly how it was
		// reported.
		// **Before the interface, so a panel drawn this frame reads this
		// frame's numbers.** Pumping after would show the counters as they were
		// a frame ago, which for a rate row is the difference between a
		// transfer that looks stalled and one that is.
		//
		// Cheap when nothing is configured: both are absent and this is two
		// null checks.
		{
			ENGINE_PROFILE_CAT("content", engine::core::ProfileCategory::Assets);
			PumpContent(frameSeconds);

			// **After the panels drew and before they draw again**, which is
			// what makes "only rows imgui actually drew" the bound: a row asks
			// for a picture while drawing, and this builds a couple of them in
			// the gap. See Thumbnails.cpp.
			PumpThumbnails();
		}

		{
			ENGINE_PROFILE_CAT("build interface", engine::core::ProfileCategory::Render);

			Interface.Begin(frameSeconds);
			DrawInterface();
			Interface.End();
		}

		PresentWorld(frameSeconds);
	}

	void Editor::InstallExampleScript(Store &store, std::string_view file, std::string_view instanceName) {
		// **A `Script` in the tree, not a scene built behind somebody's back.**
		// `examples::LoadScene` would run the file right now and leave the
		// mirror's parts in the world as though an author had placed them —
		// which is the wrong shape for an editor twice over: the geometry would
		// be saved into every game file made from a new place, and the thing
		// that produced it would be invisible. A script instance is content: it
		// is in the explorer, it opens in the script editor, Play runs it and
		// Stop takes its work away again.
		const Entity service = store.FindFirstRoot("ServerScriptService");
		if (service == NULL_ENTITY) {
			return;
		}

		// **Located with `ExamplePath` and filed under a relative name**, and
		// the two are different on purpose. The scenes stage into
		// `<stage>/assets/examples` while `Paths::Assets()` is each program's
		// own directory — a layout mismatch `ExamplePath` already knows how to
		// bridge — so finding the file needs its fallback. What goes *into* the
		// world is the short name, because an absolute path from this machine
		// would be written into the save file.
		const Name PATH(std::string("examples/") + std::string(file));
		const Name located(engine::examples::ExamplePath(std::string(file)));

		// **Read now and filed into the world**, rather than left as a path for
		// the runtime to resolve later. `ReadSource` looks in the cache before
		// the filesystem, so filing it here is what makes the game *contain*
		// the program — a `.agame` saved from a new place carries the text and
		// opens on a machine that has no engine checkout beside it.
		std::string text;
		std::string error;
		if (!engine::script::ReadSource(store, located, text, error)) {
			// Not fatal, and not silent. A build with no staged assets is a
			// real situation — the editor still works, it just has nothing to
			// put in the new place.
			ENGINE_WARN("no example script to install: {}", error);
			return;
		}

		engine::script::SourceCache cache;
		if (const auto *existing = store.Resource<engine::script::SourceCache>(); existing != nullptr) {
			cache = *existing;
		}
		cache.Set(PATH, text);
		store.SetResource(cache);

		const Entity script = store.CreateInstance(engine::script::ScriptClass(), std::string(instanceName));
		if (script == NULL_ENTITY) {
			return;
		}

		store.SetParent(script, service);
		store.SetProperty(script, Name("Source"), &PATH, sizeof(PATH));
	}

	void Editor::ExpandWorldTree(WorldId world) {
		WorldTree &tree = TreeFor(world);
		tree.Open.clear();

		Universe->Enter(world, [&tree](Store &store) {
			// Every entity, not only the ones with children. A leaf in the open
			// set is one entry the compile never finds a child for and never
			// asks about again; filtering them out here would mean walking the
			// children of everything, which is the work the explorer is about
			// to do anyway.
			store.EachEntity([&tree](Entity instance) { tree.Open.push_back(instance); });
		});

		ExpandedWorlds.push_back(world.Index);
	}

	void Editor::ReleaseViewerCamera(size_t viewport) {
		if (viewport >= Viewers.size()) {
			return;
		}

		ViewerCamera &viewer = Viewers[viewport];
		if (viewer.Instance == NULL_ENTITY) {
			viewer.World = WorldId{};
			return;
		}

		// **Cleared before the destroy rather than after**, so a world that has
		// already gone — closed, or never valid — still leaves the record empty.
		// Leaving the handle behind would make the next `Ensure` believe it
		// already has a camera in a world that cannot produce one.
		const WorldId world = viewer.World;
		const Entity instance = viewer.Instance;
		viewer = ViewerCamera{};

		if (!world.IsValid() || Universe->IsRemote(world)) {
			return;
		}

		Universe->Enter(world, [&](Store &store) {
			if (!store.Alive(instance)) {
				return;
			}

			// **The world's live camera is dropped with it when it was this
			// one.** A resource naming a destroyed entity is a dangling handle
			// that `AimSurfaceCameras` checks for and every other reader does
			// not, and the symptom would be a world that renders from nothing
			// after a panel is closed.
			if (const auto *active = store.Resource<engine::scene::ActiveCamera>();
				active != nullptr && active->Entity == instance) {
				store.SetResource(engine::scene::ActiveCamera{});
			}

			// **`DestroyInstance`, because this camera has a parent.** It was
			// parented into the workspace when it was minted, and `Destroy`
			// frees the row while leaving the workspace's `LastChild` naming
			// it — so the next camera parented into that workspace writes
			// through a handle to a row that is gone. A viewport switched to
			// another world and back is all it took.
			store.DestroyInstance(instance);
		});
	}

	void Editor::EnsureViewerCamera(
		size_t viewport,
		WorldId world,
		const engine::core::CFrame &eye,
		const engine::scene::Camera &lens,
		Entity follow
	) {
		if (viewport >= Viewers.size()) {
			return;
		}

		if (!world.IsValid() || Universe->IsRemote(world)) {
			ReleaseViewerCamera(viewport);
			return;
		}

		// **Repointing a panel destroys the camera it left behind.** A viewport
		// switched between worlds all session would otherwise seed one in each,
		// and every one of them would still be named that world's active camera.
		if (Viewers[viewport].World != world) {
			ReleaseViewerCamera(viewport);
		}

		Universe->Enter(world, [&](Store &store) {
			const Entity workspace = engine::scene::WorkspaceOf(store);
			if (workspace == NULL_ENTITY) {
				return;
			}

			ViewerCamera &viewer = Viewers[viewport];

			// A world can drop it without this editor knowing — a snapshot
			// restore replaces every entity, and the handle this held names a
			// row that is no longer there.
			if (viewer.Instance != NULL_ENTITY && !store.Alive(viewer.Instance)) {
				viewer = ViewerCamera{};
			}

			if (viewer.Instance == NULL_ENTITY) {
				// **Named per panel, because they share a workspace.** Four
				// instances called `Camera` in one explorer is four rows nobody
				// can tell apart, and `FindFirstChild` would hand every panel
				// the first of them — which is the shared camera this replaces,
				// reached by a different route.
				const std::string name =
					viewport == 0 ? std::string("Camera") : "Camera" + std::to_string(viewport + 1);

				const Entity camera = store.CreateInstance(engine::scene::CameraClass(), name);
				if (camera == NULL_ENTITY) {
					// A replica refuses to mint an authoritative entity. That is
					// not a failure here — `client::AimReplicaViewer` is the path
					// for those, and it puts a predicted camera in instead.
					return;
				}

				store.SetParent(camera, workspace);

				// **Marked before anything can save it.** A save between
				// creating it and marking it would put this editor's viewpoint
				// into the game file, which is the whole thing this component
				// exists to stop.
				store.Set(camera, engine::scene::TransientComponent{});

				viewer.World = world;
				viewer.Instance = camera;
			}

			// **Named every frame, not once at creation.** It used to be set
			// only when the instance was minted, so a script assigning
			// `workspace.CurrentCamera` took the world's eye away for good — and
			// `scene::AimSurfaceCameras` reflects through whatever this names,
			// so every mirror in the scene started reflecting from a camera the
			// viewport was not looking through, with nothing on screen to say
			// why.
			engine::scene::ActiveCamera active;
			active.Entity = viewer.Instance;
			store.SetResource(active);

			// **Followed, not driven, when somebody is looking through it.**
			// Writing the eye into the camera every frame would fight an author
			// dragging its CFrame in the properties panel — the view would
			// snap back on the next frame and the field would look broken.
			if (follow == viewer.Instance) {
				return;
			}

			if (auto *transform = store.GetMutable<engine::scene::Transform>(viewer.Instance)) {
				transform->Frame = eye;
			}
			if (auto *component = store.GetMutable<engine::scene::Camera>(viewer.Instance)) {
				*component = lens;
			}
		});
	}

	void Editor::SampleFrame(float frameSeconds) {
		// **Sampled every frame, whether or not a panel is open.** A frame-rate
		// panel that started collecting when it was opened would show an empty
		// graph for its first second — which is exactly the second somebody
		// opened it to look at, because they opened it when the editor
		// stuttered.
		Statistics.Record(Clock.Now(), frameSeconds);

		// **Accumulated from the frame delta, and handed to the renderer that
		// draws against it.** `Renderer::SetAnimationTime` carries why the clock
		// is the caller's: a module holding one has a notion of "now" to drift.
		AnimationSeconds += frameSeconds;
		Renderer.SetAnimationTime(AnimationSeconds);

		// **The frame graph is only collected while it is being read.**
		// Recording every span of every frame costs real time, and the whole
		// reason to look at that panel is that time is scarce. The client's
		// overlay makes the same trade.
		//
		// A snapshot at the end of the run counts as reading it, and is the
		// only way to profile something — a window drag — that occupies the
		// hands that would otherwise be opening the panel.
		// **Three reasons to record, and the third is not a panel.** This line
		// runs every frame and is the authority, so a caller that switched the
		// graph on from outside had it switched off again before the next frame —
		// which is exactly what `profile_frame` did until it had a flag of its
		// own to set.
		engine::core::FrameGraph::SetEnabled(
			ShowFrameGraph || ControlWantsProfile || !Settings.ProfileSnapshot.empty()
		);
	}

	void Editor::PresentWorld(float frameSeconds) {
		// **Which panel this frame draws.** `Renderer::Render` owns the whole
		// frame — swapchain, interface, present — so it draws one world per
		// call. With both viewports open they take turns: each holds its own
		// target and shows the last texture drawn into it, so each refreshes at
		// half the frame rate. Drawing both in one frame means `Render` taking
		// a list of views, which is a change to the shared renderer and is
		// tracked separately.
		// **Round-robin over whatever is open.** `Renderer::Render` owns the
		// whole frame — swapchain, interface, present — so it draws one world
		// per call, and N open panels therefore take turns. Each keeps its own
		// target and shows the last texture drawn into it.
		//
		// Skipping the closed ones matters: rotating through four slots with
		// one panel open would redraw it every fourth frame for no reason.
		size_t candidates[2 + EXTRA_VIEWPORTS];
		size_t candidateCount = 0;

		if (ShowViewport) {
			candidates[candidateCount++] = 0;
		}
		for (size_t index = 0; index < EXTRA_VIEWPORTS; index++) {
			if (Extras[index].Open) {
				candidates[candidateCount++] = index + 1;
			}
		}

		// **The asset preview is one more slot in the rotation, and it took
		// every frame instead.** It used to be tested before the loop and
		// `return` on success — and because a hovered row re-asks for its
		// preview on every frame it is hovered, that early return fired on
		// *every* frame too. `Renderer::Render` owns the swapchain and the
		// present, so the editor's own chrome was never drawn for as long as the
		// cursor rested on a mesh: the whole window went black and came back the
		// moment the pointer moved away.
		//
		// The comment that used to sit here said the cost was "a hovered row's
		// worth of frames rather than a permanent share of the rotation". That
		// was the intent and the code did the opposite — it took the whole
		// rotation and left nothing for the panels.
		//
		// As a candidate it gets one turn in N like everything else, so a
		// hovered preview refreshes at a share of the frame rate and the editor
		// keeps drawing. A preview refreshing at a third of 120 fps is forty
		// updates a second on a thing being looked at, which is not something an
		// eye can see; a window that stops being drawn is.
		if (!PreviewWanted.empty()) {
			candidates[candidateCount++] = PREVIEW_SLOT;
		}

		// **A closed panel gives its camera back, every frame rather than on an
		// event.** There is no close callback to hang this on — a panel is open
		// because imgui says its window is, and it can be shut by the title bar,
		// by a menu item or by a saved layout arriving from disk. Reconciling
		// against the list that was just built covers all three, and costs a
		// walk of four entries on a frame where nothing changed.
		//
		// Without it a session that has opened and closed panels leaves a camera
		// in the world for each one: undriven, listed in the explorer, saved
		// nowhere but visible everywhere, and one of them still named the
		// world's active camera.
		for (size_t index = 0; index < Viewers.size(); index++) {
			const bool live = index == 0 ? ShowViewport : Extras[index - 1].Open;
			if (!live) {
				ReleaseViewerCamera(index);
			}
		}

		if (candidateCount == 0) {
			// Nothing to draw into. The frame still runs — the chrome is drawn
			// and presented — so the editor does not freeze when every viewport
			// is closed.
			DrawingViewport = 0;
		} else {
			RoundRobin = (RoundRobin + 1) % candidateCount;
			DrawingViewport = candidates[RoundRobin];
		}

		// **This frame belongs to the preview**, and it is spent the same way a
		// viewport spends one: `Render` owns the swapchain, so whichever slot the
		// rotation picked gets the whole call. The difference from what this used
		// to do is only that it had to be *picked*.
		if (DrawingViewport == PREVIEW_SLOT) {
			if (RenderPreviewSlot()) {
				PreviewWanted.clear();
				return;
			}

			// It asked and could not be drawn — an unloaded mesh, or a bounds
			// entry that has gone. Fall through to the first viewport rather than
			// spending the frame on nothing.
			PreviewWanted.clear();
			DrawingViewport = candidateCount > 1 ? candidates[0] : 0;
		}

		ViewportState *extra = ExtraAt(DrawingViewport);
		const bool drawingSecond = extra != nullptr;

		// **The second panel defaults to a *different* world, not to the active
		// one.** Two viewports showing the same world is one view drawn twice
		// at half the rate — strictly worse than one viewport, and the first
		// thing somebody opening the second one would see. Where there is only
		// one world it follows the active one, because a blank panel is worse
		// than a duplicate.
		if (drawingSecond && !extra->World.IsValid()) {
			for (const WorldId id : Universe->Worlds()) {
				if (id != Active) {
					extra->World = id;
					break;
				}
			}
		}

		const WorldId shown = drawingSecond ? (extra->World.IsValid() ? extra->World : Active) : Active;

		// **Resolved before anything presents, because `PreRender` reads it.**
		// It used to be worked out after the present call, which was harmless
		// while nothing in that phase cared where the viewport was looking from.
		// `aim-surface-cameras` does: a mirror reflects the eye, so a phase that
		// ran before the eye was known would reflect through last frame's.
		// **The scene's camera when one is being looked through.** The free
		// camera is what an editor flies; a `Camera` instance is content, and
		// moving content should move what a viewport showing it draws — which
		// it did not, and which is why the instance looked broken.
		engine::core::CFrame eye = drawingSecond ? extra->Frame : CameraFrame;
		float reach = drawingSecond ? extra->Speed : CameraSpeed;

		const Entity follow = drawingSecond ? extra->Follow : FollowCamera;
		if (follow != NULL_ENTITY && shown.IsValid()) {
			bool followed = false;
			Universe->Enter(shown, [&](Store &store) {
				if (!store.Alive(follow)) {
					return;
				}
				if (const auto *transform = store.Get<engine::scene::Transform>(follow)) {
					eye = transform->Frame;
					followed = true;
				}
			});

			if (!followed) {
				// The camera was deleted, or the viewport was pointed at another
				// world. Dropped rather than left pointing at a dead handle,
				// which would freeze the view where the camera used to be.
				if (drawingSecond) {
					extra->Follow = NULL_ENTITY;
				} else {
					FollowCamera = NULL_ENTITY;
				}
			}
		}
		engine::render::SceneTarget &target = drawingSecond ? extra->Target : WorldTarget;

		engine::scene::Camera lens;
		lens.FarPlane = std::max(lens.FarPlane, reach * 40.0f);

		// A followed camera brings its own field of view and clip planes: those
		// are its properties, and looking through it while ignoring them would
		// be looking through something else.
		if (follow != NULL_ENTITY && shown.IsValid()) {
			Universe->Enter(shown, [&](Store &store) {
				if (store.Alive(follow)) {
					if (const auto *component = store.Get<engine::scene::Camera>(follow)) {
						lens = *component;
					}
				}
			});
		}


		// PreRender runs whether or not the simulation did: it is the phase
		// that turns state into something to draw, and an edited world's state
		// changes without a tick.
		// **A replica is given this viewport's eye before it presents.** It has
		// no camera of its own — an authoritative entity minted in a replica
		// would collide with one the authority minted — so `AimReplicaViewer`
		// puts a predicted one there and names it `ActiveCamera`.
		//
		// Before `Present`, because `aim-surface-cameras` runs in `PreRender`
		// and reflects through whatever `ActiveCamera` names. Setting the eye
		// afterwards aims every mirror at where the viewport was last frame,
		// which is a reflection that lags the camera by one frame and reads as
		// a mirror that is not tracking.
		if (shown.IsValid() && IsReplicaWorld(shown)) {
			Universe->Enter(shown, [&](Store &store) {
				(void)client::AimReplicaViewer(store, eye, lens);
			});
		}

		// **And the same for a world that is not a replica, which is the half
		// the rule above was written for and did not cover.** `AimReplicaViewer`
		// and `EnsureViewerCamera` are the two ways this editor names an eye —
		// one per world kind — and only the first was on this side of `Present`.
		// The second ran after it, so `aim-surface-cameras` reflected through
		// whatever `ActiveCamera` had been left pointing at.
		//
		// With one viewport that is last frame's eye, which reads as a mirror
		// lagging by a frame. **With two it is the other viewport's camera**,
		// because the studio round-robins one panel per frame and the last to
		// run wins: a mirror in one panel then tracks the camera somebody is
		// flying in the other, and stops moving when they stop.
		if (shown.IsValid() && !IsReplicaWorld(shown)) {
			EnsureViewerCamera(DrawingViewport, shown, eye, lens, follow);
		}

		if (shown.IsValid()) {
			// **The render gate rides along with it**, because
			// `client::InstallPresentation` registers `sync-rendered` in this
			// same phase. That is what makes an edited world work at all: it
			// never ticks, so a gate maintained by the simulation would leave a
			// part dragged into `Workspace` invisible until somebody pressed
			// play. See `scene/Visibility.hpp`.
			// **A world that is not being ticked is presented at one, not at
			// its accumulator.** Alpha is where *between* two ticks to draw,
			// and a world nothing advances has no next tick to draw towards —
			// its accumulator stops wherever it stopped, which is usually zero,
			// and zero means "draw the previous frame". `capture-previous` is a
			// `PreSimulation` system and `Present` runs `PreRender` alone, so
			// that previous frame is wherever each part was created: an edited
			// world drew every part at its birthplace while the selection
			// outline followed the real transform.
			//
			// **This asked `StateOf` alone and that was wrong for Edit mode**,
			// which is where an author spends most of their time.
			// `SyncWorldStates` leaves every world `Active` when nothing is
			// running, so the state said "ticking" while `Simulate` was
			// returning before the tick. `studio::PresentationAlpha` carries
			// the whole argument and is where it is now decided, because
			// nothing in this class is reachable from a test.
			Universe->Present(
				shown,
				frameSeconds,
				PresentationAlpha(Advancing, Universe->StateOf(shown), Universe->AlphaOf(shown))
			);
		}

		const std::vector<engine::scene::DrawInstance> *instances = nullptr;
		std::vector<engine::scene::DrawInstance> drawn;

		// **Cleared before the world is asked, not inside the ask.** A viewport
		// with no world would otherwise keep whatever the last world it drew
		// held — a mirror in a scene that is no longer on screen, rendering into
		// a texture nothing samples, and a surface pass paid for every frame the
		// panel is empty.
		Surfaces.clear();

		if (shown.IsValid()) {
			Universe->Enter(shown, [&](Store &store) {
				if (const auto *list = store.Resource<client::DrawList>()) {
					// Copied out rather than borrowed. The renderer's call
					// happens outside `Enter`, and a span into a store nobody
					// is inside is a pointer across a boundary that rule 3
					// exists to keep closed.
					drawn = list->Instances;
				}

				// **The surface cameras, which the studio was never asking
				// for.** `Renderer::Render` takes them and the editor passed
				// nothing — so the surface pass never ran, no texture was ever
				// written, and a `Part` naming one sampled nothing. The mirror
				// example looked like a bug in the mirror: the frame was there,
				// the pane was empty, and the world behind it was rendering
				// perfectly.
				//
				// `client::CollectSurfaceViews` is the same call the client
				// makes; a second way of finding a scene's surface cameras would
				// be a second thing to keep in step with what `SurfaceSize` and
				// `Surface` mean.
				(void)client::CollectSurfaceViews(store, Surfaces);

				// TODO(render-pipeline): the world's pipelines were installed here
				// on first sight of the world, and the chosen key went into the
				// view below. See `client::InstallWorldPipelines`.
			});
			instances = &drawn;
		}

		// **Nothing here.** The viewer camera is placed before `Present`, above,
		// because `aim-surface-cameras` runs in that phase and reflects through
		// what it names. A replica gets `client::AimReplicaViewer` instead, for
		// the same reason and in the same place — a replica may not mint an
		// authoritative entity, so its viewpoint comes out of the predicted
		// range.
		//
		// **`DrawingViewport` below chooses the surface textures as well as the
		// scene target, and the mirrors need it to.** The views collected above
		// were aimed from *this* panel's eye a few lines ago — the aim is world
		// state and one panel draws per frame, so it is correct at the moment it
		// is read and wrong by the time the next panel draws. What outlives the
		// frame is the texture, so that is what is kept per viewport: every panel
		// composites its panes from reflections taken for its own camera, and a
		// panel that is not this frame's shows its own last image rather than
		// another panel's current one. They shared one set until v0.75, and
		// flying either camera moved the mirrors in both windows.

		// TODO(render-pipeline): this took a `render::View` per camera, and the
		// viewport set `view.World` and `view.Pipeline` together — the pipeline
		// key a world installs is qualified by the world id, so naming one
		// without the other asks for a pipeline nothing installed.
		LastFrame = Renderer.Render(
			eye,
			lens,
			instances != nullptr ? std::span<const engine::scene::DrawInstance>(*instances)
								 : std::span<const engine::scene::DrawInstance>{},
			Overlay,
			Surfaces,
			&Interface,
			target.IsValid() ? &target : nullptr,
			DrawingViewport
		);

		// **Presented, or simply drawn when there is nowhere to present.**
		// A headless renderer never presents by design, so counting presents
		// would leave `--frames` unreachable and the run would never end — which
		// is the one failure mode a build server cannot recover from.
		if (LastFrame.Presented || Settings.Headless) {
			FramesDrawn++;
		}

		// **After the frame rather than before it**, so the capture is of a
		// frame that has a scene texture — the first frame has none, because the
		// viewport panel only learns its size once it has been laid out.
		if (!Settings.Capture.empty() && FramesDrawn == CaptureAtFrame()) {
			Renderer.RequestSceneCapture(Settings.Capture);
		}
	}

	int64_t Editor::CaptureAtFrame() const {
		// The frame before the last, so the capture is requested on one frame
		// and written by the next — and the run still ends when it was told to.
		// With no budget, a handful of frames in: enough for the layout to
		// settle and the first scene to be presented.
		constexpr int64_t SETTLED = 4;
		return Settings.MaximumFrames > SETTLED ? Settings.MaximumFrames - 2 : SETTLED;
	}

	// --- selection ---------------------------------------------------------

	void Editor::Select(WorldId world, Entity instance, bool add) {
		if (world != SelectionWorld) {
			Selection.clear();
			SelectionWorld = world;
		}

		if (!add) {
			Selection.clear();
		}

		const auto found = std::find(Selection.begin(), Selection.end(), instance);
		if (found != Selection.end()) {
			if (add) {
				Selection.erase(found);
			}
			return;
		}

		Selection.push_back(instance);
	}

	void Editor::ClearSelection() {
		Selection.clear();

		// The anchor goes with it. `SelectRange` already falls back to a plain
		// click for an anchor it cannot find a row for, so a stale one is not a
		// fault — but an author whose next shift-click measures from a row they
		// deselected two minutes ago has no way to know why.
		SelectionAnchor = NULL_ENTITY;
	}

	bool Editor::IsSelected(Entity instance) const {
		return std::find(Selection.begin(), Selection.end(), instance) != Selection.end();
	}

	void Editor::UndoEdit() {
		if (Commands == nullptr || !Commands->CanUndo()) {
			return;
		}

		// Read before the call, because a successful undo pops it.
		const std::string what(Commands->NextUndo());

		if (!Commands->Undo()) {
			Say("nothing to undo — '" + what + "' is gone", engine::core::LogLevel::Warning);
			return;
		}

		// See the declaration: a handle the undo replaced is either dead or
		// about to name something else.
		ClearSelection();
		MarkModified();
		Say("undid " + what);
	}

	void Editor::RedoEdit() {
		if (Commands == nullptr || !Commands->CanRedo()) {
			return;
		}

		const std::string what(Commands->NextRedo());

		if (!Commands->Redo()) {
			Say("nothing to redo — '" + what + "' is gone", engine::core::LogLevel::Warning);
			return;
		}

		ClearSelection();
		MarkModified();
		Say("redid " + what);
	}

	// --- the game ----------------------------------------------------------

	void Editor::NewGame() {
		EndAllRuns();
		Scripts.clear();
		ActiveScript = -1;
		ClearSelection();
		Trees.clear();

		for (const WorldId existing : Universe->Worlds()) {
			Universe->Destroy(existing);
		}

		GameName = Name(DEFAULT_GAME);
		GamePath.clear();
		Modified = false;
		InstanceCounts.clear();
		ExpandedWorlds.clear();

		// **Two worlds, and the pair is the point rather than a bigger sample.**
		// A one-world template is a template for the thing this engine is not:
		// the universe holds subworlds, they tick *alongside* each other under
		// `ExecutionMode::WorldParallel`, and a viewport per world is what makes
		// that visible instead of asserted. A new game that opened one world
		// taught everybody the single-scene habit, and the second world was a
		// menu item nobody had a reason to click.
		//
		// They are also two different kinds of scene on purpose. The skygrid is
		// sparse geometry over empty sky — mostly background, nothing to hide a
		// culling or projection mistake behind. The mirror is the opposite: a
		// surface camera, a texture sampled back, and a floor under it. One
		// template exercises both halves of the renderer.
		const WorldId grid = AddWorld(Name(SKYGRID_WORLD));
		const WorldId mirrors = AddWorld(Name(MIRROR_WORLD));

		// **A third, and it is the one that shows what a `MeshPart` is.** The
		// other two are made of `Part`s, so a new game contained no example of
		// the class the mesh picker, the content pipeline and half of v0.9 exist
		// to serve — somebody looking for "how do I use a mesh" found a menu
		// item and no scene.
		//
		// **It costs nothing at start-up, which is the only reason it can be
		// here.** `MeshGrid.luau` seeds from the six built-in ids, and a built-in
		// is generated in-process and never fetched — `Editor::
		// RequestContentAsset` refuses to ask a CDN for one. So this world names
		// six meshes that are already registered and issues no request at all: a
		// template that pulled content on open would put back the twenty-nine
		// second start-up v0.10 spent a version removing.
		const WorldId meshes = AddWorld(Name(MESHGRID_WORLD));

		Active = grid;
		SelectionWorld = Active;

		// **Explicit, though it is also the default.** A universe that had
		// loaded a game which set something else keeps that setting, and the
		// whole reason this template has two worlds is to show them running
		// together. See `world::ExecutionMode`.
		Universe->SetMode(engine::world::ExecutionMode::WorldParallel);

		// **No floor here, and the example says why in its header.** A skygrid
		// with a baseplate under it is a baseplate with decoration above it: the
		// frustum fills with floor, nothing is culled, and the scene stops being
		// the thing it was written to be.
		Universe->Enter(grid, [this](Store &store) {
			InstallExampleScript(store, "SkyGrid.luau", "SkyGridScene");

			// **The 2D tree beside the 3D one, in the same world.** v0.8's
			// widget set is the version's headline and a template that did not
			// show it would leave `Instance.new("Frame")` as a thing you have
			// to know about — which is the "an API with no caller" the roadmap
			// refuses. The example builds its own `ScreenGui` from a script, so
			// it exercises the bindings as well as the layout.
			//
			// In the skygrid world rather than the mirror one: the skygrid is
			// mostly empty sky, so a panel over it is legible, and the mirror
			// scene is already the busy half of the template.
			InstallExampleScript(store, "Interface.luau", "InterfaceScene");
		});

		// **No baseplate here either, and the reason is specific rather than
		// symmetric.** `Mirrors-1-world.luau` builds its own `Floor` — 60x60,
		// top face at y = 0 — and the editor's baseplate is 128x128 with its top
		// face at *the same* y = 0. Two coplanar surfaces is z-fighting, and
		// during Play the mirror world showed a floor tearing between two greys.
		//
		// The general rule the baseplate came from — an empty world is a black
		// frame and a black frame looks like a broken renderer — still holds for
		// a world somebody made themselves. It does not hold for one whose
		// script lays a floor the moment it runs.
		Universe->Enter(mirrors, [this](Store &store) {
			InstallExampleScript(store, "Mirrors-1-world.luau", "MirrorScene");
		});

		// The mesh grid lays its own plate and its own camera, so it needs the
		// same nothing the other two do.
		Universe->Enter(meshes, [this](Store &store) {
			InstallExampleScript(store, "MeshGrid.luau", "MeshGridScene");
		});

		// **A viewport each, pinned rather than left following the active
		// world.** An extra viewport with no world of its own draws whatever is
		// being edited, so two panels would show one scene twice and the
		// template would demonstrate nothing.
		if (ViewportState *second = ExtraAt(1); second != nullptr) {
			second->World = mirrors;
			second->Open = true;
		}
		ShowViewport = true;

		// Open in the tree, both of them, and the Worlds panel in front. A
		// template whose second world is behind a collapsed arrow and an
		// unselected tab is a template nobody finds.
		ExpandWorldTree(grid);
		ExpandWorldTree(mirrors);
		ExpandWorldTree(meshes);

		// Enough frames to outlast a first-run layout rebuild. See
		// `FocusWorlds`.
		FocusWorlds = 4;

		Say("new game: three worlds — skygrid, mirrors and meshes — ticking in parallel");
	}

	bool Editor::OpenGame(const std::filesystem::path &path) {
		EndAllRuns();

		engine::game::GameInfo info;
		std::string error;

		if (!engine::game::LoadGame(*Universe, path, info, error)) {
			Say("open failed: " + error, LogLevel::Error);
			return false;
		}

		Scripts.clear();
		ActiveScript = -1;
		ClearSelection();
		Trees.clear();

		GameName = info.Name;
		GamePath = path;
		Modified = false;
		InstanceCounts.clear();

		// The client's half, on every world the file brought. A world with no
		// draw list renders as an empty frame, which reads as a broken renderer
		// rather than as a missing system.
		for (const WorldId id : Universe->Worlds()) {
			Universe->Enter(id, [](Store &store, Scheduler &systems) {
				client::InstallPresentation(store, systems, 256);

				// **Idempotent, which is what lets it run on every file
				// whatever its age.** A game saved before services existed has
				// none and gets them here; one saved after has them all and
				// gets nothing back. Branching on the file's format version
				// instead would be a version test that has to stay right
				// forever.
				engine::scene::InstallServices(store);
			});
		}

		Active = Universe->Worlds().empty() ? WorldId{} : Universe->Worlds().front();
		SelectionWorld = Active;

		Say("opened " + path.string() + " — " + std::to_string(info.Worlds.size()) + " world(s)");
		return true;
	}

	void Editor::SyncRojo(const std::filesystem::path &project) {
		if (Universe == nullptr || !Active.IsValid()) {
			Say("no scene to sync into", engine::core::LogLevel::Warning);
			return;
		}

		std::ifstream in(project, std::ios::binary);
		if (!in) {
			Say("could not read " + project.string(), engine::core::LogLevel::Error);
			return;
		}

		std::ostringstream buffer;
		buffer << in.rdbuf();

		RojoProject parsed;
		std::string error;
		if (!ParseRojoProject(buffer.str(), parsed, error)) {
			Say("not a Rojo project: " + error, engine::core::LogLevel::Error);
			return;
		}

		// **Relative to the project file, not to the working directory.** A
		// launcher's cwd must not decide which tree gets read — the same rule
		// `Keybinds::Load` gives for the configuration beside the binary.
		const std::filesystem::path root = project.parent_path();

		RojoSyncReport report;
		bool built = false;
		Universe->Enter(Active, [&](Store &store) {
			built = SyncRojoProject(parsed, root, store, report, error);
		});

		if (!built) {
			Say("sync failed: " + error, engine::core::LogLevel::Error);
			return;
		}

		Say("synced " + std::to_string(report.Instances) + " instances, " +
			std::to_string(report.Scripts) + " scripts from " + parsed.Name);

		for (const std::string &missing : report.Missing) {
			Say("  no such path: " + missing, engine::core::LogLevel::Warning);
		}

		// **Capped, because a project with a thousand unrecognised files would
		// otherwise be a thousand log lines nobody reads.** The count is still
		// reported, so the information is there without the flood.
		size_t shown = 0;
		for (const std::string &note : report.Notes) {
			if (shown++ >= 8) {
				Say("  ... and " + std::to_string(report.Notes.size() - 8) + " more notes");
				break;
			}
			Say("  " + note);
		}

		Modified = true;
		Touch(Active);
	}

	bool Editor::SaveGame(const std::filesystem::path &path) {
		// **Unsaved script buffers are flushed into their worlds first.**
		// Otherwise the file on disk is the game minus whatever is currently
		// being typed, which is the version of "saved" nobody means.
		for (OpenScript &tab : Scripts) {
			if (tab.Modified) {
				SaveScriptTab(tab);
			}
		}

		std::string error;
		if (!engine::game::SaveGame(*Universe, GameName, path, error)) {
			Say("save failed: " + error, LogLevel::Error);
			return false;
		}

		GamePath = path;
		Modified = false;
		Say("saved " + path.string());
		return true;
	}

	bool Editor::ExportActiveWorld(const std::filesystem::path &path) {
		if (!Active.IsValid()) {
			Say("no world to export", LogLevel::Warning);
			return false;
		}

		std::string error;
		if (!engine::game::ExportWorld(*Universe, Active, path, error)) {
			Say("export failed: " + error, LogLevel::Error);
			return false;
		}

		Say("exported '" + std::string(Label(Universe->NameOf(Active))) + "' to " + path.string());
		return true;
	}

	bool Editor::ExportUniverse(const std::filesystem::path &path) {
		// **The universe and every world under it, which is what an `.agame`
		// already is** — so this shares `SaveGame`'s writer and differs from
		// Save As in what it does to the editor afterwards, which is nothing.
		//
		// That difference is the whole reason it is a separate action rather
		// than a second name for Save As. Save As *adopts* the path: the title
		// bar changes, the modified marker clears, and Ctrl+S from then on
		// writes there. Exporting is handing a copy to somebody — a build
		// server, a teammate, a backup — and an author who exported a copy and
		// then pressed Ctrl+S expecting to save their own file would have
		// written over the copy instead. `ExportActiveWorld` has always drawn
		// the same line one level down.
		for (OpenScript &tab : Scripts) {
			// Same order as `SaveGame`, and for the same reason: an export
			// taken before the buffers were flushed is the game minus whatever
			// is currently being typed.
			if (tab.Modified) {
				SaveScriptTab(tab);
			}
		}

		std::string error;
		if (!engine::game::SaveGame(*Universe, GameName, path, error)) {
			Say("export failed: " + error, LogLevel::Error);
			return false;
		}

		Say("exported the universe — " + std::to_string(Universe->Count()) + " world(s) — to " +
			path.string());
		return true;
	}

	bool Editor::ImportWorldFile(const std::filesystem::path &path) {
		std::string error;

		// No rename first: a world whose name is free keeps the one the file
		// gave it, which is what somebody importing into an empty universe
		// expects. A clash gets a suffix rather than a refusal, because being
		// told "that name is taken" and having to guess a free one is a worse
		// answer than being given one.
		WorldId imported = engine::game::ImportWorld(*Universe, path, Name{}, error);

		if (!imported.IsValid()) {
			for (int attempt = 2; attempt < 100 && !imported.IsValid(); attempt++) {
				const std::string candidate = path.stem().string() + " " + std::to_string(attempt);
				imported = engine::game::ImportWorld(*Universe, path, Name(candidate), error);
			}
		}

		if (!imported.IsValid()) {
			Say("import failed: " + error, LogLevel::Error);
			return false;
		}

		Universe->Enter(imported, [](Store &store, Scheduler &systems) {
			client::InstallPresentation(store, systems, 256);
			engine::scene::InstallServices(store);
		});

		Active = imported;
		SelectionWorld = imported;
		ClearSelection();
		MarkModified();

		Say("imported '" + std::string(Label(Universe->NameOf(imported))) + "'");
		return true;
	}

	bool Editor::ImportUniverseFile(const std::filesystem::path &path) {
		engine::game::GameInfo info;
		std::string error;

		const size_t imported = engine::game::ImportUniverse(*Universe, path, info, error);

		if (imported == 0) {
			Say("import failed: " + error, LogLevel::Error);
			return false;
		}

		// The client's half and the fixtures, on every world that arrived —
		// the same two things `OpenGame` does, for the same reasons. A world
		// with no draw list renders as an empty frame.
		for (const WorldId id : Universe->Worlds()) {
			Universe->Enter(id, [](Store &store, Scheduler &systems) {
				client::InstallPresentation(store, systems, 256);
				engine::scene::InstallServices(store);
			});
		}

		InstanceCounts.clear();

		// Land on the first world that arrived, because an import somebody
		// cannot see is one they will do twice.
		if (!info.Worlds.empty()) {
			if (const WorldId first = Universe->Find(info.Worlds.front()); first.IsValid()) {
				Active = first;
				SelectionWorld = first;
			}
		}

		ClearSelection();
		MarkModified();

		// **Partial success is still success, and it says so.** `ImportUniverse`
		// keeps the worlds that read before the one that failed, so reporting
		// only the error would describe a universe that is not the one on
		// screen.
		if (!error.empty()) {
			Say("imported " + std::to_string(imported) + " world(s), then stopped: " + error,
				LogLevel::Warning);
			return true;
		}

		Say("imported " + std::to_string(imported) + " world(s) from " + path.string());
		return true;
	}

	WorldId Editor::AddWorld(Name name) {
		WorldSettings settings;
		settings.Name = name;
		settings.TickRate = Settings.TickRate;

		WorldStatus status = WorldStatus::Ok;
		const WorldId id = Universe->Create(settings, &status);
		if (!id.IsValid()) {
			Say("could not create world '" + std::string(Label(name)) + "'", LogLevel::Error);
			return id;
		}

		Universe->Enter(id, [](Store &store, Scheduler &systems) {
			client::InstallPresentation(store, systems, 256);

			// **The fixtures, on every world this program makes.** A world
			// with no `Workspace` is one where `game:GetService` fails and
			// where an author has nowhere obvious to put a part — and the one
			// place that would be discovered is a script that already ran.
			engine::scene::InstallServices(store);
		});

		// **`WorldId` is a reused slot**, so a cached count keyed by one can
		// outlive the world it counted and be shown for the next world to take
		// that slot. Cheaper to drop the lot on a change than to key the cache
		// on something that survives.
		InstanceCounts.clear();

		MarkModified();
		return id;
	}

	void Editor::RemoveWorld(WorldId world) {
		if (!world.IsValid()) {
			return;
		}

		// Tabs first. A script tab holding an entity in a world that no longer
		// exists is a save that writes into freed storage.
		for (size_t index = Scripts.size(); index > 0; index--) {
			if (Scripts[index - 1].World == world) {
				CloseScriptTab(index - 1);
			}
		}

		const std::string name(Label(Universe->NameOf(world)));
		Universe->Destroy(world);

		if (Active == world) {
			const auto remaining = Universe->Worlds();
			Active = remaining.empty() ? WorldId{} : remaining.front();
			SelectionWorld = Active;
			ClearSelection();
		}

		InstanceCounts.clear();
		MarkModified();
		Say("removed world '" + name + "'");
	}

	// --- instances ----------------------------------------------------------

	Entity Editor::InsertInstance(WorldId world, engine::ecs::ClassId klass, Entity parent) {
		if (!world.IsValid() || !klass.IsValid()) {
			return NULL_ENTITY;
		}

		Entity created = NULL_ENTITY;
		Entity landed = NULL_ENTITY;

		Universe->Enter(world, [&](Store &store) {
			const engine::ecs::ClassInfo &info = engine::ecs::Classes::Describe(klass);
			created = store.CreateInstance(klass, Label(info.Name));
			if (created == NULL_ENTITY) {
				return;
			}

			// **Nothing selected means `Workspace`, and since v0.7 it has to.**
			// An instance with no parent is an orphan: it is not in the scene,
			// nothing draws it, and `Insert Object` with an empty selection
			// would have quietly produced something invisible. It used to be
			// drawn, because an unparented instance was a root of the world and
			// roots were what the renderer collected — see
			// `scene/Visibility.hpp` for why that is no longer the rule.
			//
			// Studio does the same thing, and for an author it is the only
			// sensible reading of "insert a Part" with nothing highlighted.
			landed =
				parent != NULL_ENTITY && store.Alive(parent) ? parent : engine::scene::WorkspaceOf(store);

			if (landed != NULL_ENTITY) {
				store.SetParent(created, landed);
			}

			// **After the parent, not before it.** The document a redo rebuilds
			// from is taken here, and one taken before `SetParent` would rebuild
			// the instance as a root — an undo followed by a redo would quietly
			// move it out of the tree.
			if (Commands != nullptr) {
				Commands->RecordCreate(
					store, world, created, "Insert " + std::string(Label(info.Name))
				);
			}
		});

		if (created != NULL_ENTITY) {
			Select(world, created, false);
			SelectionAnchor = created;
			OpenPathTo(world, created);
			RevealSelection = true;
			MarkModified();

			// **Invalidated on a structural change and not on every edit.** A
			// property write is what actually *names* an asset, and invalidating
			// there would rescan every world on every frame of a dragged slider
			// — which is the shape of the bug this flag was added to fix. An
			// insert or a delete is rare and bounded; anything finer is what
			// `Rescan` is for.
			GalleryScanned = false;
		}
		return created;
	}

	void Editor::DeleteSelection() {
		// The other structural change. See `InsertInstance` for why only these
		// two.
		GalleryScanned = false;

		if (Selection.empty() || !SelectionWorld.IsValid()) {
			return;
		}

		// Copied, because closing a script tab walks `Selection` too and
		// erasing from underneath the loop is the classic version of this bug.
		const std::vector<Entity> doomed = Selection;

		for (const Entity instance : doomed) {
			for (size_t index = Scripts.size(); index > 0; index--) {
				if (Scripts[index - 1].Instance == instance) {
					CloseScriptTab(index - 1);
				}
			}
		}

		Universe->Enter(SelectionWorld, [&](Store &store) {
			for (const Entity instance : doomed) {
				if (store.Alive(instance)) {
					// **Recorded before the destroy**, because after it there is
					// nothing left to photograph and the undo would restore an
					// empty document — which reads as "undo did nothing" rather
					// than as a fault, and is therefore the version of this
					// mistake nobody reports.
					if (Commands != nullptr) {
						Commands->RecordDestroy(
							store,
							SelectionWorld,
							instance,
							"Delete " + std::string(Label(store.InstanceNameOf(instance)))
						);
					}
					store.DestroyInstance(instance);
				}
			}
		});

		ClearSelection();
		MarkModified();
	}

	void Editor::DuplicateSelection() {
		if (Selection.empty() || !SelectionWorld.IsValid()) {
			return;
		}

		const std::vector<Entity> sources = Selection;
		std::vector<Entity> copies;

		Universe->Enter(SelectionWorld, [&](Store &store) {
			for (const Entity source : sources) {
				if (!store.Alive(source)) {
					continue;
				}

				const Entity copy = store.CloneInstance(source);
				if (copy == NULL_ENTITY) {
					continue;
				}

				// **`CloneInstance` leaves the copy parented nowhere**, which
				// is `:Clone()`'s contract and the right one for a script. An
				// editor means "beside the original", so the parent is applied
				// here rather than changed there.
				store.SetParent(copy, store.ParentOf(source));
				copies.push_back(copy);

				if (Commands != nullptr) {
					Commands->RecordCreate(
						store,
						SelectionWorld,
						copy,
						"Duplicate " + std::string(Label(store.InstanceNameOf(source)))
					);
				}
			}
		});

		if (copies.empty()) {
			return;
		}

		Selection = copies;
		MarkModified();
	}

	// --- running --------------------------------------------------------------

	Editor::WorldRun *Editor::RunOf(WorldId world) {
		for (WorldRun &run : Runs) {
			if (run.World == world) {
				return &run;
			}
		}
		return nullptr;
	}

	const Editor::WorldRun *Editor::RunOf(WorldId world) const {
		for (const WorldRun &run : Runs) {
			if (run.World == world) {
				return &run;
			}
		}
		return nullptr;
	}

	RunMode Editor::ModeOf(WorldId world) const {
		const WorldRun *run = RunOf(world);
		return run != nullptr ? run->Mode : RunMode::Edit;
	}

	bool Editor::IsRunning(WorldId world) const {
		return RunOf(world) != nullptr;
	}

	bool Editor::IsPaused(WorldId world) const {
		const WorldRun *run = RunOf(world);
		return run != nullptr && run->Paused;
	}

	const Editor::WorldRun *Editor::RunForReplica(WorldId world) const {
		if (!world.IsValid()) {
			return nullptr;
		}

		for (const WorldRun &run : Runs) {
			for (const std::unique_ptr<PlayLink> &link : run.Links) {
				if (link != nullptr && link->ReplicaWorld() == world) {
					return &run;
				}
			}
		}
		return nullptr;
	}

	bool Editor::IsReplicaWorld(WorldId world) const {
		return RunForReplica(world) != nullptr;
	}

	bool Editor::AnyRunning() const {
		return !Runs.empty();
	}

	void Editor::SyncWorldStates() {
		// **`Universe::Tick` advances every world it holds**, so "not running"
		// has to be spelled out to the universe rather than merely meant. A
		// scene left alone while another runs would settle its physics and fire
		// its heartbeats, which is running by any name an author would use.
		//
		// With nothing running there is no tick at all, so everything goes back
		// to active — otherwise an author would return to Edit and find half
		// their scenes marked suspended for no reason they could see.
		const bool anything = AnyRunning();

		for (const WorldId id : Universe->Worlds()) {
			if (Universe->IsRemote(id)) {
				continue;
			}

			// **A client view is part of its run, not a scene being left
			// alone.** It carries no simulation system, so keeping it active
			// costs a barrier and buys the thing that matters: it is presented
			// and drawn every frame, exactly like the world it is a view of. A
			// suspended one would still draw — `Present` does not consult the
			// state — which is worse than either alternative, because it would
			// be marked stopped in the Worlds panel while visibly running.
			const bool wanted = !anything || IsRunning(id) || IsReplicaWorld(id);
			Universe->SetState(
				id, wanted ? engine::world::WorldState::Active : engine::world::WorldState::Suspended
			);
		}
	}

	void Editor::SetRunMode(WorldId world, RunMode mode) {
		if (!world.IsValid() || ModeOf(world) == mode) {
			return;
		}

		const Name name = Universe->NameOf(world);
		const std::string label = name.IsValid() ? std::string(Label(name)) : std::string("that scene");

		// **Stopped first, whatever the destination.** Switching a running world
		// from Run to Play is a restore followed by a start, not a mode swapped
		// underneath a live VM — the scripts have already changed the scene and
		// the author's content is in the snapshot.
		if (IsRunning(world)) {
			EndRun(world);
		}

		if (mode == RunMode::Edit) {
			Say("stopped '" + label + "' — the scene is back as it was");
			SyncWorldStates();
			return;
		}

		if (!BeginRun(world, mode)) {
			Say("could not start '" + label + "': the scene would not snapshot", LogLevel::Error);
			return;
		}

		Say(std::string(Describe(mode)) + " started in '" + label + "'");
	}

	bool Editor::BeginRun(WorldId world, RunMode mode) {
		if (!world.IsValid() || Universe->IsRemote(world)) {
			return false;
		}

		// **Script buffers first, and the order is the whole point.** What is
		// on screen has to be in the world before the snapshot is taken —
		// otherwise Stop restores a scene from *before* the author's code was
		// filed, and pressing Play deletes whatever they had just typed. That is
		// not a subtle failure and it is not recoverable; it was the first thing
		// this function got wrong.
		//
		// Only this world's tabs: another scene's unsaved edits are not part of
		// this run and flushing them would file code the author had not finished.
		for (OpenScript &tab : Scripts) {
			if (tab.World == world && tab.Modified) {
				SaveScriptTab(tab);
			}
		}

		// **The snapshot Stop restores, as a world document.** Roblox's model,
		// and the part people forget: pressing Play must not leave an author's
		// scene as whatever their scripts made of it.
		//
		// `WriteWorldDocument` rather than `Universe::Save`, because the
		// universe snapshot is *every* world at once — which is exactly what
		// made Stop restore scenes nobody had run. This is the same call
		// Duplicate and Rename already make.
		std::string error;
		std::string document = engine::game::WriteWorldDocument(*Universe, world, error);
		if (document.empty()) {
			// A world holding a component with no serialisation cannot be
			// written, and starting anyway would mean a Stop that cannot put the
			// scene back. Refusing to start is the honest failure.
			ENGINE_ERROR("snapshot of world for run: {}", error);
			return false;
		}

		engine::script::RuntimeLimits limits;
		limits.Role.Server = true;
		limits.Role.Client = mode == RunMode::Play;

		// **The one place in the repository that sets this.**
		// `script::HostRole::Studio` has defaulted to false since v0.6 with a
		// comment naming this version, on the grounds that editor-only
		// behaviour must never appear in a shipped game because a default was
		// optimistic.
		limits.Role.Studio = true;

		WorldRun run;
		run.World = world;
		run.Mode = mode;

		// A run always starts running. Carrying a pause across Stop and Play
		// would be a game that came up frozen for a reason nobody could see.
		run.Paused = false;
		run.Snapshot = std::move(document);

		// **The undo stack does not survive the run, and Stop is why.** Stop
		// restores the snapshot taken on the line above, which throws away
		// everything the run did — so a command recorded before it describes a
		// world that the restore has already put back, and one recorded during
		// it describes a world that no longer exists. Either way the entry is a
		// lie the moment Stop is pressed.
		//
		// Cleared here rather than at Stop so that the guarantee holds even for
		// a run that is never stopped cleanly.
		if (Commands != nullptr) {
			Commands->Clear();
		}

		std::string failure;

		// **Through `game::StartWorldScripts`, which is the same call a
		// dedicated server makes.** What "running a game" means has to be one
		// function or the studio's Play and the server's hosting drift — and the
		// first thing to drift would be the heartbeat's delta.
		Universe->Enter(world, [&](Store &store, Scheduler &systems) {
			run.Runtime = engine::game::StartWorldScripts(store, systems, limits, failure, &Breakpoints);
		});

		if (!failure.empty()) {
			Say("script error: " + failure, LogLevel::Error);
		}

		// **The client half, and only for Play.** Run is a dedicated server:
		// there is no client in the process, so there is nothing to replicate to
		// and a replica world would be a view of nobody. Play is both halves,
		// which this is what makes true — until now the difference between the
		// two modes was which scripts ran and what `IsServer()` answered, and a
		// property that never crossed a wire looked exactly like one that did.
		//
		// **Started after the scripts**, so the join snapshot describes a world
		// that has been built rather than an empty one. It would converge either
		// way — the snapshot is re-sent until it is acknowledged — but a client
		// view that opens blank and fills in reads as a bug in the link.
		if (mode == RunMode::Play) {
			// **One loop, N clients.** Two clients is what turns "the replica
			// disagrees with the server" into "these two clients disagree",
			// which is the bug class a play test exists for and the one a single
			// replica cannot show. The first is pinned to a viewport; the rest
			// are ordinary worlds reachable from the scene selector, for the
			// same reason only one was pinned before — a panel per client is
			// three pictures nobody asked for and a slice of the centre pane
			// each.
			for (int client = 0; client < PlayClients; client++) {
			auto link = std::make_unique<PlayLink>();

			const std::string label =
				PlayClients == 1 ? std::string("client") : "client " + std::to_string(client + 1);

			std::string linkError;
			if (link->Start(*Universe, world, Settings.TickRate, linkError, label)) {
				// **A viewport pinned to it, or the whole thing is invisible.**
				// The point of a client view is the *difference* between it and
				// the server's, and a difference nobody is shown is a feature
				// that exists in a log line. An extra viewport with no world of
				// its own draws whatever is being edited, so pinning is what
				// stops the second panel showing the server's scene twice.
				//
				// The first free panel, and one that is already pinned somewhere
				// is left alone: somebody who put a viewport on another scene
				// meant it, and taking that panel would be the editor
				// rearranging their layout when they pressed Play.
				//
				// **At most one, and this was over-reach when it was written.**
				// `--run play` starts every world in the game, so a rule of "one
				// panel per run" opened a viewport per *world* — three or four
				// pictures nobody asked for, each one a slice of the centre pane
				// and a turn in `PresentWorld`'s round robin, so every view also
				// refreshed a third as often. One client view is the feature;
				// the rest are reachable from the scene selector like any other
				// world.
				const bool anotherIsShown =
					std::any_of(Runs.begin(), Runs.end(), [](const WorldRun &other) {
						return std::any_of(
							other.Links.begin(),
							other.Links.end(),
							[](const std::unique_ptr<PlayLink> &link) {
								return link != nullptr && link->IsRunning();
							}
						);
					});

				// **Hoisted out of the loop it disables.** It cannot change
				// across iterations, so testing it inside the body made a reader
				// check every iteration for a mutation that is not there.
				if (!anotherIsShown) {
					const WorldId replica = link->ReplicaWorld();

					for (ViewportState &view : Extras) {
						if (view.World.IsValid() && view.World != replica) {
							continue;
						}

						view.World = replica;
						view.Open = true;

						// Where the main camera is, so the two panels start
						// looking at the same thing from the same place. Two
						// views of one game from different angles is a
						// comparison somebody has to do in their head.
						view.Frame = CameraFrame;
						view.Yaw = CameraYaw;
						view.Pitch = CameraPitch;
						break;
					}
				}

				run.Links.push_back(std::move(link));
			} else {
				// **Not fatal, and the run goes on without it.** A Play that
				// refused to start because its client view could not be made
				// would be a studio you cannot use to fix whatever broke it —
				// and the server half is exactly what Run already gives, so
				// there is a working thing to fall back to.
				Say("no client view: " + linkError, LogLevel::Warning);

				// And no point asking for the rest: whatever refused the first
				// one refuses them all, and four copies of one warning is a log
				// nobody reads.
				break;
			}
			}
		}

		Runs.push_back(std::move(run));

		// **The player goes to the first world started.** A run whose player was
		// nowhere would have the lifecycle close scenes before anybody had
		// pressed anything; one that moved the player on every start would
		// teleport them out of the scene they were watching.
		if (Runs.size() == 1 || !PlayerWorld.IsValid()) {
			PlayerWorld = world;
			Lives.clear();
		}

		SyncWorldStates();
		return true;
	}

	void Editor::EndRun(WorldId world) {
		WorldRun *record = RunOf(world);
		if (record == nullptr) {
			return;
		}

		// **The client view goes first of all.** It owns a world in this same
		// universe, and every viewport, the worlds panel and the lifecycle ask
		// `IsReplicaWorld` about it — so a link left alive across the restore
		// below would have each of them answering about a world that is being
		// rebuilt underneath them. It also holds no reference to the authority's
		// store, which is why it can go before the runtime rather than after.
		for (const std::unique_ptr<PlayLink> &link : record->Links) {
			if (link == nullptr) {
				continue;
			}
			// Any viewport pinned to the client view is unpinned before the
			// world under it disappears. A pin naming a destroyed world would
			// leave the panel following the active scene with no way to tell
			// that it had stopped showing what it was opened for.
			const WorldId replica = link->ReplicaWorld();
			for (ViewportState &view : Extras) {
				if (view.World == replica) {
					view.World = WorldId{};
					view.Follow = engine::ecs::NULL_ENTITY;
				}
			}

			link->Stop(*Universe);
		}
		record->Links.clear();

		// **The runtime goes before the restore.** It holds a `Store &` and the
		// store is the universe's; the world is about to be destroyed, so a
		// runtime still alive across it holds a reference to freed storage.
		const std::string document = std::move(record->Snapshot);
		record->Runtime.reset();

		Runs.erase(
			std::remove_if(
				Runs.begin(), Runs.end(), [world](const WorldRun &run) { return run.World == world; }
			),
			Runs.end()
		);

		if (document.empty()) {
			SyncWorldStates();
			return;
		}

		// Script tabs first: their entity handles do not survive the world being
		// rebuilt, and a tab that saved afterwards would write into storage that
		// had been freed. Only this world's — another scene's tabs are untouched
		// by this restore, which is the whole point of running one scene.
		for (size_t index = Scripts.size(); index > 0; index--) {
			if (Scripts[index - 1].World == world) {
				CloseScriptTab(index - 1);
			}
		}

		const Name name = Universe->NameOf(world);
		const bool wasActive = world == Active;

		// **Destroyed and rebuilt, because `ReadWorldDocument` creates a scene
		// rather than restoring into one.** `Universe::Adopt` reuses the hole a
		// destroy leaves, so the handle survives and a viewport pinned to this
		// world still points at it — the same trick `RenameWorld` depends on.
		Universe->Destroy(world);

		// **This scene's undo history goes with it, and only this scene's.** A
		// running world can still be manipulated — the gizmo and the explorer
		// both work during a play test on purpose — so edits are recorded during
		// the run, and the destroy above has just invalidated every handle they
		// name. Here rather than after the restore because the failure path
		// below returns without one, and those commands are just as dead.
		// See `CommandLog::Forget`.
		if (Commands != nullptr) {
			Commands->Forget(world);
		}

		std::string error;
		const WorldId restored = engine::game::ReadWorldDocument(*Universe, document, name, error);
		if (!restored.IsValid()) {
			// The world is gone and the replacement was refused. Loud, because
			// an author whose scene disappeared needs to know it was this and
			// not something they did.
			Say("the scene was lost while stopping it: " + error, LogLevel::Error);
			InstanceCounts.clear();
			ClearSelection();
			Active = Universe->Worlds().empty() ? WorldId{} : Universe->Worlds().front();
			SelectionWorld = Active;
			SyncWorldStates();
			return;
		}

		// The scheduler went with the world, so the presentation systems have to
		// be installed again. A restored world with no draw list renders as an
		// empty frame, which reads as Stop having broken the renderer.
		Universe->Enter(restored, [](Store &store, Scheduler &systems) {
			client::InstallPresentation(store, systems, 256);
		});

		// **Everything that held the old handle, repointed.** `Adopt` normally
		// hands back the same slot, but nothing promises it — and a viewport
		// pinned to a stale id draws nothing with no way to say why.
		if (wasActive) {
			Active = restored;
		}
		if (PlayerWorld == world) {
			PlayerWorld = restored;
		}
		for (ViewportState &viewport : Extras) {
			if (viewport.World == world) {
				viewport.World = restored;
			}
		}

		InstanceCounts.clear();

		// Entity handles do not survive the rebuild as the same rows in the same
		// generations, so anything holding one is stale.
		if (SelectionWorld == world || !SelectionWorld.IsValid()) {
			SelectionWorld = Active;
			ClearSelection();
		}
		Trees.clear();

		SyncWorldStates();
	}

	void Editor::EndAllRuns() {
		// By value and from the front, because `EndRun` erases from `Runs`.
		while (!Runs.empty()) {
			EndRun(Runs.front().World);
		}
	}

	// --- scripts ---------------------------------------------------------------

	void Editor::OpenScriptTab(WorldId world, Entity instance) {
		for (size_t index = 0; index < Scripts.size(); index++) {
			if (Scripts[index].World == world && Scripts[index].Instance == instance) {
				ActiveScript = static_cast<int>(index);
				return;
			}
		}

		OpenScript tab;
		tab.World = world;
		tab.Instance = instance;

		Universe->Enter(world, [&](Store &store) {
			const auto *source = store.Get<engine::script::Source>(instance);
			if (source == nullptr) {
				return;
			}
			tab.Path = source->Path;

			if (!tab.Path.IsValid()) {
				return;
			}

			// Through `ReadSource`, so an unsaved edit already in the cache is
			// what opens and a script whose text is only on disk still opens.
			// A second resolver here would be a second place to forget the
			// cache.
			std::string error;
			if (!engine::script::ReadSource(store, tab.Path, tab.Text, error)) {
				// Empty rather than refused. A script instance whose file does
				// not exist yet is a legal state — an author makes the instance
				// before choosing the file — so the editor opens an empty
				// buffer and saving creates it.
				tab.Text.clear();
			}
		});

		Scripts.push_back(std::move(tab));
		ActiveScript = static_cast<int>(Scripts.size() - 1);
	}

	void Editor::SaveScriptTab(OpenScript &tab) {
		if (!tab.World.IsValid()) {
			return;
		}

		Universe->Enter(tab.World, [&](Store &store) {
			if (!store.Alive(tab.Instance)) {
				return;
			}

			if (!tab.Path.IsValid()) {
				// A script with no path cannot be filed, so the editor gives it
				// one derived from its name. Under `Scripts/` so a game file's
				// paths look like paths rather than like bare identifiers.
				const Name name = store.InstanceNameOf(tab.Instance);
				const std::string leaf = name.IsValid() ? std::string(Label(name)) : "Script";
				tab.Path = Name("Scripts/" + leaf + ".luau");

				const engine::script::Source source{tab.Path};
				store.Set(tab.Instance, source);
			}

			auto *cache = store.ResourceMutable<engine::script::SourceCache>();
			if (cache == nullptr) {
				store.SetResource(engine::script::SourceCache{});
				cache = store.ResourceMutable<engine::script::SourceCache>();
			}

			cache->Set(tab.Path, tab.Text);
		});

		tab.Modified = false;
		MarkModified();
	}

	void Editor::CloseScriptTab(size_t index) {
		if (index >= Scripts.size()) {
			return;
		}

		Scripts.erase(Scripts.begin() + static_cast<long>(index));

		if (Scripts.empty()) {
			ActiveScript = -1;
		} else if (ActiveScript >= static_cast<int>(Scripts.size())) {
			ActiveScript = static_cast<int>(Scripts.size()) - 1;
		}
	}

	// --- odds and ends -----------------------------------------------------------

	void Editor::Say(std::string text, LogLevel level) {
		// **Logged and nothing else.** The panel gets it back through the sink,
		// which is what keeps the editor's own lines and a script's `print` in
		// one list in the order they actually happened. Pushing here as well
		// would show every editor message twice.
		switch (level) {
		case LogLevel::Error:
			ENGINE_ERROR("{}", text);
			break;
		case LogLevel::Warning:
			ENGINE_WARN("{}", text);
			break;
		default:
			ENGINE_INFO("{}", text);
			break;
		}
	}

	void Editor::MarkModified() {
		Modified = true;
	}

	std::string Editor::TitleText() const {
		std::string title = GameName.IsValid() ? std::string(Label(GameName)) : std::string(DEFAULT_GAME);
		if (Modified) {
			title += " *";
		}
		if (AnyRunning()) {
			title += "  [";
			title += Runs.size() == 1 ? Describe(Runs.front().Mode) : "Running";
			title += "]";
		}
		return title;
	}
}
