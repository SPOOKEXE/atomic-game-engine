#include <engine/core/Bytes.hpp>
#include <engine/core/Log.hpp>

#include <spdlog/sinks/base_sink.h>
#include <engine/core/Paths.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Classes.hpp>
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

#include <algorithm>
#include <mutex>
#include <client/Scene.hpp>
#include <imgui.h>
#include <studio/Editor.hpp>
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
		if (Settings.Uncapped && !Settings.Headless && !Renderer.SetVerticalSync(false)) {
			ENGINE_WARN("--uncapped had no effect; frames stay paced by the display");
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
		engine::script::ScriptClass();

		Universe = std::make_unique<engine::world::Universe>();

		if (!Settings.Game.empty()) {
			if (!OpenGame(Settings.Game)) {
				// Not fatal. An editor that refused to start because of one bad
				// file is an editor you cannot use to fix that file.
				NewGame();
			}
		} else {
			NewGame();
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
		if (Settings.StartIn != RunMode::Edit) {
			SetRunMode(Settings.StartIn);
		}

		Running = true;
		return true;
	}

	void Editor::Shutdown() {
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

		if (Mode != RunMode::Edit) {
			// Runtimes hold a `Store &`, and the stores are the universe's. Let
			// go of them before it goes away.
			EndRun();
		}

		Runtimes.clear();
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
			const float delta = Clock.Tick();

			engine::core::FrameGraph::BeginFrame();

			PumpEvents();
			Simulate(delta);
			Present(delta);

			engine::core::FrameGraph::EndFrame();

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
			if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
				event.window.windowID == SDL_GetWindowID(Window)) {
				Running = false;
			}
		}
	}

	void Editor::Simulate(float frameSeconds) {
		if (Mode == RunMode::Edit) {
			// **A world being edited does not tick, and that is deliberate.**
			// A universe that simulated while somebody was authoring would
			// settle physics under their hands — a part placed in the air would
			// be on the floor by the time they looked away, and nothing would
			// tell them why.
			return;
		}

		if (Paused) {
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
		Interface.Begin(frameSeconds);
		DrawInterface();
		Interface.End();

		PresentWorld(frameSeconds);
	}

	void Editor::InstallExampleScript(
		Store &store, std::string_view file, std::string_view instanceName
	) {
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

		const Entity script =
			store.CreateInstance(engine::script::ScriptClass(), std::string(instanceName));
		if (script == NULL_ENTITY) {
			return;
		}

		store.SetParent(script, service);
		store.SetProperty(script, Name("Source"), &PATH, sizeof(PATH));
	}

	void Editor::ExpandWorldTree(WorldId world) {
		Universe->Enter(world, [this](Store &store) {
			// Every entity, not only the ones with children. A leaf in the set
			// costs one comparison the first time the tree draws and is then
			// dropped; filtering them out here would mean walking the children
			// of everything, which is the work the explorer is about to do
			// anyway.
			store.EachEntity([this](Entity instance) { Expanded.push_back(instance.Id); });
		});

		ExpandedWorlds.push_back(world.Index);
	}

	void Editor::EnsureViewerCamera(
		WorldId world, const engine::core::CFrame &eye, const engine::scene::Camera &lens
	) {
		if (!world.IsValid() || Universe->IsRemote(world)) {
			return;
		}

		Universe->Enter(world, [&](Store &store) {
			const Entity workspace = engine::scene::WorkspaceOf(store);
			if (workspace == NULL_ENTITY) {
				return;
			}

			Entity camera = store.FindFirstChild(workspace, "Camera");

			if (camera == NULL_ENTITY) {
				camera = store.CreateInstance(engine::scene::CameraClass(), "Camera");
				if (camera == NULL_ENTITY) {
					return;
				}

				store.SetParent(camera, workspace);

				// **Marked before anything can save it.** A save between
				// creating it and marking it would put this editor's viewpoint
				// into the game file, which is the whole thing this component
				// exists to stop.
				store.Set(camera, engine::scene::TransientComponent{});

				// The world's live camera, so a script asking what it is
				// looking through gets this rather than nothing.
				engine::scene::ActiveCamera active;
				active.Entity = camera;
				store.SetResource(active);
			}

			// **Followed, not driven, when somebody is looking through it.**
			// Writing the eye into the camera every frame would fight an author
			// dragging its CFrame in the properties panel — the view would
			// snap back on the next frame and the field would look broken.
			if (FollowCamera == camera) {
				return;
			}

			if (auto *transform = store.GetMutable<engine::scene::Transform>(camera)) {
				transform->Frame = eye;
			}
			if (auto *component = store.GetMutable<engine::scene::Camera>(camera)) {
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

		// **The frame graph is only collected while it is being read.**
		// Recording every span of every frame costs real time, and the whole
		// reason to look at that panel is that time is scarce. The client's
		// overlay makes the same trade.
		//
		// A snapshot at the end of the run counts as reading it, and is the
		// only way to profile something — a window drag — that occupies the
		// hands that would otherwise be opening the panel.
		engine::core::FrameGraph::SetEnabled(ShowFrameGraph || !Settings.ProfileSnapshot.empty());
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
		size_t candidates[1 + EXTRA_VIEWPORTS];
		size_t candidateCount = 0;

		if (ShowViewport) {
			candidates[candidateCount++] = 0;
		}
		for (size_t index = 0; index < EXTRA_VIEWPORTS; index++) {
			if (Extras[index].Open) {
				candidates[candidateCount++] = index + 1;
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

		// PreRender runs whether or not the simulation did: it is the phase
		// that turns state into something to draw, and an edited world's state
		// changes without a tick.
		if (shown.IsValid()) {
			Universe->Present(shown, frameSeconds, Universe->AlphaOf(shown));
		}

		const std::vector<engine::scene::DrawInstance> *instances = nullptr;
		std::vector<engine::scene::DrawInstance> drawn;

		if (shown.IsValid()) {
			Universe->Enter(shown, [&](Store &store) {
				if (const auto *list = store.Resource<client::DrawList>()) {
					// Copied out rather than borrowed. The renderer's call
					// happens outside `Enter`, and a span into a store nobody
					// is inside is a pointer across a boundary that rule 3
					// exists to keep closed.
					drawn = list->Instances;
				}
			});
			instances = &drawn;
		}

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

		// **The viewer's camera, kept in step with the eye.** It is what a
		// script sees as the current camera and what the explorer shows; the
		// editor's free camera is still what decides the view unless somebody
		// is looking through this one.
		EnsureViewerCamera(shown, eye, lens);

		LastFrame = Renderer.Render(
			eye,
			lens,
			instances != nullptr ? std::span<const engine::scene::DrawInstance>(*instances)
								 : std::span<const engine::scene::DrawInstance>{},
			Overlay,
			nullptr,
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
	}

	bool Editor::IsSelected(Entity instance) const {
		return std::find(Selection.begin(), Selection.end(), instance) != Selection.end();
	}

	// --- the game ----------------------------------------------------------

	void Editor::NewGame() {
		EndRun();
		Scripts.clear();
		ActiveScript = -1;
		ClearSelection();
		Expanded.clear();

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
		});

		// A floor, because an empty world is a black frame and a black frame
		// looks like a broken renderer. This is the one piece of content the
		// editor authors on a user's behalf, and it is a `Part` like any other
		// — deletable, renameable, and written into the save file.
		Universe->Enter(mirrors, [this](Store &store) {
			const Entity baseplate = store.CreateInstance(engine::scene::PartClass(), "Baseplate");

			// **Under the workspace, because that is what a workspace is for.**
			// `AddWorld` installed the fixtures before this ran, so the parent
			// exists; the guard is for the case where it did not, and a floor
			// at the root is a better answer there than no floor at all.
			if (const Entity workspace = engine::scene::WorkspaceOf(store);
				workspace != engine::ecs::NULL_ENTITY) {
				store.SetParent(baseplate, workspace);
			}

			const Vector3 size{128.0f, 1.0f, 128.0f};
			store.SetProperty(baseplate, Name("Size"), &size, sizeof(size));

			const Vector3 position{0.0f, -0.5f, 0.0f};
			store.SetProperty(baseplate, Name("Position"), &position, sizeof(position));

			const bool anchored = true;
			store.SetProperty(baseplate, Name("Anchored"), &anchored, sizeof(anchored));

			const engine::core::Color3 grey{0.32f, 0.34f, 0.36f};
			store.SetProperty(baseplate, Name("Color"), &grey, sizeof(grey));

			InstallExampleScript(store, "Mirrors-1-world.luau", "MirrorScene");
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

		// Enough frames to outlast a first-run layout rebuild. See
		// `FocusWorlds`.
		FocusWorlds = 4;

		Say("new game: two worlds, skygrid and mirrors, ticking in parallel");
	}

	bool Editor::OpenGame(const std::filesystem::path &path) {
		EndRun();

		engine::game::GameInfo info;
		std::string error;

		if (!engine::game::LoadGame(*Universe, path, info, error)) {
			Say("open failed: " + error, LogLevel::Error);
			return false;
		}

		Scripts.clear();
		ActiveScript = -1;
		ClearSelection();
		Expanded.clear();

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
		Universe->Enter(world, [&](Store &store) {
			const engine::ecs::ClassInfo &info = engine::ecs::Classes::Describe(klass);
			created = store.CreateInstance(klass, Label(info.Name));

			if (created != NULL_ENTITY && parent != NULL_ENTITY && store.Alive(parent)) {
				store.SetParent(created, parent);
			}
		});

		if (created != NULL_ENTITY) {
			Select(world, created, false);
			if (parent != NULL_ENTITY) {
				Expanded.push_back(parent.Id);
			}
			MarkModified();
		}
		return created;
	}

	void Editor::DeleteSelection() {
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
			}
		});

		if (copies.empty()) {
			return;
		}

		Selection = copies;
		MarkModified();
	}

	// --- running --------------------------------------------------------------

	void Editor::SetRunMode(RunMode mode) {
		if (mode == Mode) {
			return;
		}

		if (Mode != RunMode::Edit) {
			EndRun();
		}

		if (mode == RunMode::Edit) {
			Say("stopped — the scene is back as it was");
			return;
		}

		if (!BeginRun(mode)) {
			Say("could not start: the universe would not snapshot", LogLevel::Error);
			return;
		}

		Say(std::string(Describe(mode)) + " started");
	}

	bool Editor::BeginRun(RunMode mode) {
		// **Script buffers first, and the order is the whole point.** What is
		// on screen has to be in the world before the snapshot is taken —
		// otherwise Stop restores a universe from *before* the author's code
		// was filed, and pressing Play deletes whatever they had just typed.
		// That is not a subtle failure and it is not recoverable; it was the
		// first thing this function got wrong.
		for (OpenScript &tab : Scripts) {
			if (tab.Modified) {
				SaveScriptTab(tab);
			}
		}

		// **The snapshot Stop restores.** Roblox's model, and the part people
		// forget: pressing Play must not leave an author's scene as whatever
		// their scripts made of it. `Universe::Save` and `Load` are exactly
		// that operation and have existed since v0.2 with no caller that needed
		// them.
		EditSnapshot.clear();

		engine::core::ByteWriter writer;
		if (!Universe->Save(writer)) {
			// A universe holding a component with no serialisation cannot be
			// snapshotted, and starting anyway would mean a Stop that cannot
			// put the scene back. Refusing to start is the honest failure.
			return false;
		}

		const auto bytes = writer.Bytes();
		EditSnapshot.assign(bytes.begin(), bytes.end());

		engine::script::RuntimeLimits limits;
		limits.Role.Server = true;
		limits.Role.Client = mode == RunMode::Play;

		// **The one place in the repository that sets this.**
		// `script::HostRole::Studio` has defaulted to false since v0.6 with a
		// comment naming this version, on the grounds that editor-only
		// behaviour must never appear in a shipped game because a default was
		// optimistic.
		limits.Role.Studio = true;

		Runtimes.clear();
		Mode = mode;

		// A run always starts running. Carrying a pause across Stop and Play
		// would be a game that came up frozen for a reason nobody could see.
		Paused = false;

		// **The player starts in the world being looked at.** A run whose
		// player was nowhere would have every world idle from the first frame,
		// and the lifecycle would close all but one of them before anybody had
		// pressed anything.
		PlayerWorld = Active;
		Lives.clear();

		for (const WorldId id : Universe->Worlds()) {
			if (Universe->IsRemote(id)) {
				continue;
			}

			std::shared_ptr<engine::script::Runtime> runtime;
			std::string failure;

			// **Through `game::StartWorldScripts`, which is the same call a
			// dedicated server makes.** What "running a game" means has to be
			// one function or the studio's Play and the server's hosting drift
			// — and the first thing to drift would be the heartbeat's delta.
			Universe->Enter(id, [&](Store &store, Scheduler &systems) {
				runtime = engine::game::StartWorldScripts(store, systems, limits, failure);
			});

			if (!failure.empty()) {
				Say("script error: " + failure, LogLevel::Error);
			}

			Runtimes.push_back(std::move(runtime));
		}

		return true;
	}

	void Editor::EndRun() {
		if (Mode == RunMode::Edit) {
			return;
		}

		// **Runtimes before the restore.** Each holds a `Store &` and the
		// stores are the universe's; `Universe::Load` destroys every world, so
		// a runtime still alive across it holds a reference to freed storage.
		Runtimes.clear();
		Mode = RunMode::Edit;
		Paused = false;

		if (EditSnapshot.empty()) {
			return;
		}

		engine::core::ByteReader reader(EditSnapshot);
		if (!Universe->Load(reader)) {
			// `Universe::Load` leaves the universe empty rather than half
			// restored, which is the right failure and a terrible surprise. Say
			// so loudly — the alternative is an author looking at an empty
			// explorer and concluding the editor deleted their game.
			Say("the scene could not be restored — the universe is empty", LogLevel::Error);
			EditSnapshot.clear();
			Active = WorldId{};
			ClearSelection();
			return;
		}

		EditSnapshot.clear();
		InstanceCounts.clear();

		// The schedulers went with the worlds, so the presentation systems have
		// to be installed again. A restored world with no draw list renders as
		// an empty frame, which reads as Stop having broken the renderer.
		for (const WorldId id : Universe->Worlds()) {
			Universe->Enter(id, [](Store &store, Scheduler &systems) {
				client::InstallPresentation(store, systems, 256);
			});
		}

		const auto worlds = Universe->Worlds();
		const Name previous = GameName;
		(void)previous;

		if (std::find(worlds.begin(), worlds.end(), Active) == worlds.end()) {
			Active = worlds.empty() ? WorldId{} : worlds.front();
		}

		SelectionWorld = Active;

		// Entity handles do not survive a snapshot restore as the same rows in
		// the same generations, so anything holding one is stale.
		ClearSelection();
		Expanded.clear();

		for (size_t index = Scripts.size(); index > 0; index--) {
			CloseScriptTab(index - 1);
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
		if (Mode != RunMode::Edit) {
			title += "  [";
			title += Describe(Mode);
			title += "]";
		}
		return title;
	}
}
