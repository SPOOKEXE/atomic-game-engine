#include <engine/core/Clock.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Paths.hpp>
#include <engine/ui/Theme.hpp>

#include <SDL3/SDL.h>

#include <launcher/Launcher.hpp>
#include <launcher/Programs.hpp>

namespace launcher {

	Launcher::Launcher() = default;

	Launcher::~Launcher() {
		// **Interface before renderer**, because the imgui backend releases GPU
		// objects the device owns and the device goes away with the renderer.
		Interface.Shutdown();
		Renderer.Shutdown();

		if (Window != nullptr) {
			SDL_DestroyWindow(Window);
			Window = nullptr;
		}
		SDL_Quit();
	}

	bool Launcher::Initialise(const Options &options) {
		Settings = options;
		Catalogue = Modes();

		// Before anything reads a file, for `Editor::Initialise`'s reason:
		// changing it later leaves whatever had already loaded pointing at the
		// old tree.
		if (!Settings.Assets.empty()) {
			engine::core::Paths::SetAssetsOverride(Settings.Assets);
		}

		if (!SDL_Init(SDL_INIT_VIDEO)) {
			ENGINE_ERROR("SDL_Init: {}", SDL_GetError());
			return false;
		}

		// **`Paths::Base` rather than `argv[0]`.** A launcher started from a
		// desktop entry, a file manager or a shell alias gets an `argv[0]` that
		// is a name rather than a path on at least one of the three platforms,
		// and the whole of program discovery hangs off this being right. `Base`
		// asks the platform, caches the answer and falls back to the working
		// directory only where it cannot.
		Stage = StageRoot(engine::core::Paths::Base());
		ENGINE_INFO("launcher: looking for programs under {}", Stage.string());

		// **Before the window, and it is the slow part of startup.** Each of
		// these is a child process that prints its option table and exits, and
		// together they are about 1.2 s in a `dev` build - see
		// `Description.hpp` for the measurement and for why it is neither
		// threaded nor cached. Doing it after the window would mean a window
		// that is up and empty while they run, which reads as a hang; a window
		// that opens a second later reads as a program starting.
		Programs.Load(Stage, ProgramsOf(Catalogue));

		if (!Settings.Headless) {
			Window = SDL_CreateWindow(
				"atomic",
				Settings.Width,
				Settings.Height,
				SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY
			);
			if (Window == nullptr) {
				ENGINE_ERROR("SDL_CreateWindow: {}", SDL_GetError());
				return false;
			}
		}

		// One frame in flight. A launcher draws a form; queueing ahead of the
		// GPU buys nothing and costs a frame of latency on every click.
		if (!Renderer.Initialise(Window, 1)) {
			return false;
		}

		engine::ui::InterfaceSettings interfaceSettings;

		// **Docking off, and it is not an oversight.** `InterfaceSettings::Docking`
		// says why: a tool showing one panel does not want the dockspace's
		// invisible full-window host swallowing its clicks, and this program is
		// one panel from the first frame to the last.
		interfaceSettings.Docking = false;
		interfaceSettings.Scale = Settings.Scale;
		interfaceSettings.DisplayWidth = Settings.Width;
		interfaceSettings.DisplayHeight = Settings.Height;

		if (!Interface.Initialise(Renderer, Window, interfaceSettings)) {
			ENGINE_ERROR("the launcher's interface failed to start");
			return false;
		}

		// **After `Initialise`, which creates the context this reads**, and not
		// optional. Without it every collapsing header on the form comes out in
		// imgui's own bright blue rather than the engine's palette, so the
		// launcher and the editor it opens look like two different programs -
		// which, for the first window somebody sees, is the wrong first thing to
		// say.
		engine::ui::ApplyEditorTheme(Settings.Scale);

		if (!Settings.StartMode.empty()) {
			const Mode *mode = FindMode(Catalogue, Settings.StartMode);
			if (mode == nullptr) {
				ENGINE_WARN("launcher: no mode called '{}'", Settings.StartMode);
			} else {
				Open(*mode);
			}
		}

		return true;
	}

	int Launcher::Run() {
		engine::core::FrameClock clock;

		while (!Quit) {
			// A frame that could not be acquired is a minimised or resizing
			// window. The form still runs - a supervised child is still polled
			// and still reported - and only the drawing is skipped.
			const bool drawing = Renderer.WaitForFrame();

			SDL_Event event;
			while (SDL_PollEvent(&event)) {
				Interface.ProcessEvent(event);

				if (event.type == SDL_EVENT_QUIT) {
					Quit = true;
				}
				if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && Window != nullptr &&
					event.window.windowID == SDL_GetWindowID(Window)) {
					Quit = true;
				}
			}

			const float delta = clock.Tick();
			Child.Poll(engine::core::Clock::Seconds());

			// **`HandOver` is a hide rather than an exit.** The launcher has to
			// outlive the child to know it ended, and coming back to the form
			// afterwards is the whole reason somebody would use a launcher
			// twice. A minimised window costs nothing: `WaitForFrame` above
			// reports no frame and this loop stops drawing.
			if (Window != nullptr) {
				const Mode *mode = FindMode(Catalogue, Open_);
				const bool handedOver =
					mode != nullptr && mode->After == Lifetime::HandOver && Child.Running();
				const bool minimised = (SDL_GetWindowFlags(Window) & SDL_WINDOW_MINIMIZED) != 0;

				if (handedOver && !minimised) {
					SDL_MinimizeWindow(Window);
				} else if (!handedOver && minimised && Child.State() != ChildState::Idle) {
					SDL_RestoreWindow(Window);
				}
			}

			if (drawing || Settings.Headless) {
				Frame(delta);
				FramesDrawn++;
			}

			if (Settings.MaximumFrames >= 0 && FramesDrawn >= Settings.MaximumFrames) {
				Quit = true;
			}
		}

		// **The child is stopped rather than orphaned.** `Process`'s destructor
		// already refuses to leave one behind, but it kills - and a supervised
		// server deserves the chance to close its sockets, which is what makes
		// the port free for the next run rather than held in TIME_WAIT.
		if (Child.Running()) {
			ENGINE_INFO("launcher: closing, stopping the child first");
			Child.RequestStop();
		}

		return 0;
	}

	void Launcher::Frame(float frameSeconds) {
		Interface.Begin(frameSeconds);

		if (Open_.empty()) {
			DrawModes();
		} else {
			DrawForm();
		}

		Interface.End();

		// **One view, empty.** `Renderer::Render` returns without presenting
		// when handed no views at all, so this is what carries the interface to
		// the swapchain. It draws no world because it has no instances; the
		// default pipeline clears and presents.
		engine::render::View view;
		view.Slot = 0;

		Renderer.Render(std::span<const engine::render::View>(&view, 1), Overlay, nullptr, true, &Interface);
	}

	void Launcher::Open(const Mode &mode) {
		Open_ = mode.Id;
		Query.clear();
		Failure.clear();

		const Description *description = Programs.Find(mode.Program);
		if (description == nullptr) {
			Failure = mode.Program + ": " + Programs.Failure(mode.Program);
			return;
		}

		// **Only when there is not one already.** Stepping out to the front
		// screen and back is something somebody does to read a blurb, and
		// rebuilding the form would throw away what they had typed.
		if (Forms.find(mode.Id) == Forms.end()) {
			Forms.emplace(mode.Id, NewForm(mode, *description));
		}
	}

	void Launcher::Launch() {
		const Mode *mode = FindMode(Catalogue, Open_);
		if (mode == nullptr) {
			return;
		}

		const Description *description = Programs.Find(mode->Program);
		const auto form = Forms.find(mode->Id);
		if (description == nullptr || form == Forms.end()) {
			return;
		}

		const std::filesystem::path program = ProgramPath(Stage, mode->Program);

		Failure.clear();
		if (!Child.Start(program, CommandLine(form->second, *description), Failure)) {
			return;
		}
	}
}
