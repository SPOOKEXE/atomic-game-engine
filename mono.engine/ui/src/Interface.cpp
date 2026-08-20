#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/render/InterfacePass.hpp>
#include <engine/ui/Fonts.hpp>
#include <engine/ui/Interface.hpp>
#include <engine/ui/Theme.hpp>

#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_sdlgpu3.h>
#include <imgui.h>
#include <string>

namespace engine::ui {

	struct Interface::Impl {
		render::InterfacePass Spatial;
		double SpatialSeconds = 0.0;
		ImGuiContext *Context = nullptr;

		// Whether the backends are running. False headless, where the context
		// exists and nothing draws it.
		bool Drawable = false;

		// What the display size is, when there is no window to ask.
		ImVec2 Display{0.0f, 0.0f};

		// Held for the lifetime of the context. imgui stores the pointer rather
		// than the string and writes the file from its own timer, so a
		// temporary here is a use-after-free that only fires once the layout
		// has been idle for the auto-save interval.
		std::string LayoutPath;

		// This frame's draw data, produced by `End` and consumed by the two
		// hook calls. Null between frames, which is what makes a renderer that
		// calls the hook without a frame having been built a no-op rather than
		// a crash.
		ImDrawData *Draw = nullptr;

		bool Ready = false;
	};

	Interface::Interface() : State(std::make_unique<Impl>()) {}

	Interface::~Interface() {
		Shutdown();
	}

	bool
	Interface::Initialise(render::Renderer &renderer, SDL_Window *window, const InterfaceSettings &settings) {
		if (State->Ready) {
			return true;
		}

		// A window decides whether anything is *drawn*. Everything else - the
		// context, the fonts, the theme, every panel's code - happens either way.
		const bool drawable = window != nullptr;

		const render::BackendHandles backend = renderer.Backend();
		if (backend.Device == nullptr) {
			// Not an assertion. A headless run legitimately has no device, and
			// the caller decides whether that is fatal - same contract as
			// `Renderer::Initialise`.
			ENGINE_ERROR("ui::Interface needs an initialised renderer");
			return false;
		}

		IMGUI_CHECKVERSION();
		State->Context = ImGui::CreateContext();
		if (State->Context == nullptr) {
			return false;
		}

		ImGuiIO &io = ImGui::GetIO();

		// **Keyboard navigation on, gamepad navigation off.** An editor is
		// driven by a keyboard and a mouse, and the gamepad mapping steals the
		// arrow keys from a text field the moment a controller is plugged in -
		// which reads as the script editor being broken by unrelated hardware.
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

		if (settings.Docking) {
			io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
		}

		if (settings.LayoutPath.empty()) {
			// Null rather than the default "imgui.ini", so a program that said
			// nothing writes nothing rather than dropping a file wherever it
			// was started from.
			io.IniFilename = nullptr;
		} else {
			State->LayoutPath = settings.LayoutPath;
			io.IniFilename = State->LayoutPath.c_str();
		}

		// **Before the first frame, which is the whole requirement.** imgui
		// loads the ini lazily on the first `NewFrame`, so a settings handler
		// registered any later has already had its lines skipped as belonging
		// to nobody - and the chosen palette would then silently reset on
		// every start.
		InstallThemeSettings();

		// **Real faces, rasterised at the scale rather than stretched.** imgui's
		// built-in font is a 13px bitmap: proportional, unhinted, and with no
		// monospace companion - fine for a debug overlay and wrong for something
		// somebody reads all day. `ui::LoadFonts` is the table of what is
		// vendored; a missing file leaves the built-in in place and says so
		// once, because an editor that refused to open over a font would be
		// worse than one that opens ugly.
		const float scale = settings.Scale > 0.0f ? settings.Scale : 1.0f;
		io.FontGlobalScale = 1.0f;

		if (!LoadFonts(scale)) {
			ImFontConfig font;
			font.SizePixels = 13.0f * scale;
			io.Fonts->AddFontDefault(&font);
		}

		ApplyEditorTheme(scale);

		if (drawable && !State->Spatial.Initialise(backend.Device, backend.ColourFormat, 16.0f * scale)) {
			ENGINE_ERROR("ui::Interface could not initialise the spatial GUI pass");
			ImGui::DestroyContext(State->Context);
			State->Context = nullptr;
			return false;
		}
		State->Spatial.SetImageSource([this, &renderer](const core::Name &name) {
			render::InterfaceImage image;
			image.Texture = renderer.TextureHandle(name);
			if (image.Texture == nullptr) {
				return image;
			}
			image.Cell = renderer.TextureCell(name, State->SpatialSeconds);
			(void)renderer.TextureSize(name, image.Width, image.Height);
			return image;
		});

		State->Display = ImVec2(
			static_cast<float>(std::max(settings.DisplayWidth, 1)),
			static_cast<float>(std::max(settings.DisplayHeight, 1))
		);

		if (!drawable) {
			// The context is live and nothing will draw it. The display size the
			// platform backend would have reported is supplied here and in
			// `Begin`, because a zero-sized display clips every panel to nothing
			// - which looks exactly like the panels not running.
			io.DisplaySize = State->Display;
			io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

			// The atlas is normally built by the renderer backend. Nothing will,
			// and `NewFrame` asserts on an atlas that has not been.
			io.Fonts->Build();

			State->Ready = true;
			State->Drawable = false;
			return true;
		}

		if (!ImGui_ImplSDL3_InitForSDLGPU(window)) {
			ENGINE_ERROR("ImGui_ImplSDL3_InitForSDLGPU failed");
			State->Spatial.Shutdown();
			ImGui::DestroyContext(State->Context);
			State->Context = nullptr;
			return false;
		}

		ImGui_ImplSDLGPU3_InitInfo info{};
		info.Device = static_cast<SDL_GPUDevice *>(backend.Device);
		info.ColorTargetFormat = static_cast<SDL_GPUTextureFormat>(backend.ColourFormat);
		info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;

		if (!ImGui_ImplSDLGPU3_Init(&info)) {
			ENGINE_ERROR("ImGui_ImplSDLGPU3_Init failed");
			ImGui_ImplSDL3_Shutdown();
			State->Spatial.Shutdown();
			ImGui::DestroyContext(State->Context);
			State->Context = nullptr;
			return false;
		}

		State->Ready = true;
		State->Drawable = true;
		return true;
	}

	void Interface::Shutdown() {
		if (!State->Ready) {
			return;
		}

		if (State->Drawable) {
			// Only what was started. Shutting down a backend that never
			// initialised is an assertion inside imgui rather than a no-op.
			ShutdownBackends();
		}
		State->Spatial.Shutdown();

		ImGui::DestroyContext(State->Context);

		State->Context = nullptr;
		State->Draw = nullptr;
		State->Ready = false;
		State->Drawable = false;
	}

	void Interface::ShutdownBackends() {
		// Renderer backend first, platform backend second, context last. The
		// reverse of construction, and it matters: the renderer backend
		// releases GPU objects it registered with the context, so destroying
		// the context first leaves them to be freed by a device that has been
		// told nothing about them.
		ImGui_ImplSDLGPU3_Shutdown();
		ImGui_ImplSDL3_Shutdown();
	}

	bool Interface::IsDrawable() const {
		return State->Drawable;
	}

	void Interface::ProcessEvent(const SDL_Event &event) {
		if (!State->Ready || !State->Drawable) {
			return;
		}
		ImGui_ImplSDL3_ProcessEvent(&event);
	}

	void Interface::Begin(float frameSeconds) {
		// Four spans over one frame of imgui rather than one, because the
		// four cost different things: `Begin` and `End` are layout and
		// vertex generation on the CPU, `Prepare` uploads them, and
		// `Record` binds. The editor's `build interface` span covered all
		// of it and could not tell a heavy panel from a heavy upload.
		ENGINE_PROFILE_CAT("ui.begin", core::ProfileCategory::Render);

		if (!State->Ready) {
			return;
		}

		if (State->Drawable) {
			ImGui_ImplSDLGPU3_NewFrame();
			ImGui_ImplSDL3_NewFrame();
		} else {
			// What the platform backend would have supplied. Held rather than
			// recomputed, so a headless run's layout is the same every frame and
			// therefore the same on two runs.
			ImGuiIO &headless = ImGui::GetIO();
			headless.DisplaySize = State->Display;
			headless.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
		}

		// **The delta is overwritten after `NewFrame`, not before it.** The
		// SDL3 backend reads a wall clock and assigns `io.DeltaTime` itself, so
		// setting it first is a value that is thrown away. Passing it in is
		// what makes a fixed-step or headless driver produce the same frames
		// every run - the same rule `world::Driver` and `net` already hold.
		ImGui::NewFrame();
		ImGui::GetIO().DeltaTime = frameSeconds > 0.0f ? frameSeconds : 1.0f / 60.0f;
	}

	void Interface::End() {
		// Where imgui turns a frame of widgets into vertices, so this is
		// the span that scales with how much is on screen.
		ENGINE_PROFILE_CAT("ui.end", core::ProfileCategory::Render);

		if (!State->Ready) {
			return;
		}

		ImGui::Render();
		State->Draw = ImGui::GetDrawData();
	}

	bool Interface::WantsMouse() const {
		return State->Ready && ImGui::GetIO().WantCaptureMouse;
	}

	bool Interface::WantsKeyboard() const {
		return State->Ready && ImGui::GetIO().WantCaptureKeyboard;
	}

	void Interface::SubmitSpatial(
		const gui::DrawList &list,
		const core::Vector2 &canvas,
		const core::Vector2 &targetPixels,
		ecs::Store &store,
		double seconds
	) {
		State->SpatialSeconds = seconds;
		State->Spatial.Submit(list, canvas, targetPixels, store);
	}

	void Interface::SetSpatialViewportSource(std::function<render::InterfaceImage(ecs::Entity)> resolve) {
		State->Spatial.SetViewportSource(std::move(resolve));
	}

	bool Interface::Prepare(void *commandBuffer) {
		ENGINE_PROFILE_CAT("ui.prepare", core::ProfileCategory::Render);

		if (!State->Ready || commandBuffer == nullptr) {
			return false;
		}
		const bool spatial = State->Spatial.Prepare(commandBuffer);
		if (!State->Drawable) {
			return spatial;
		}
		if (State->Draw == nullptr) {
			return spatial;
		}

		// A minimised window produces a frame with no area, and the backend
		// asserts on one rather than skipping it.
		if (State->Draw->DisplaySize.x <= 0.0f || State->Draw->DisplaySize.y <= 0.0f) {
			return spatial;
		}

		ImGui_ImplSDLGPU3_PrepareDrawData(State->Draw, static_cast<SDL_GPUCommandBuffer *>(commandBuffer));
		return true;
	}

	uint32_t Interface::RecordWorld(
		void *commandBuffer,
		void *renderPass,
		const glm::mat4 &viewProjection,
		const core::CFrame &camera,
		const core::Color3 &ambient,
		const core::Vector3 &sun,
		uint32_t width,
		uint32_t height,
		bool alwaysOnTop
	) {
		return State->Spatial.RecordWorld(
			commandBuffer, renderPass, viewProjection, camera, ambient, sun, width, height, alwaysOnTop
		);
	}

	void Interface::Record(void *commandBuffer, void *renderPass) {
		ENGINE_PROFILE_CAT("ui.record", core::ProfileCategory::Render);

		if (!State->Ready || !State->Drawable || State->Draw == nullptr) {
			return;
		}

		ImGui_ImplSDLGPU3_RenderDrawData(
			State->Draw,
			static_cast<SDL_GPUCommandBuffer *>(commandBuffer),
			static_cast<SDL_GPURenderPass *>(renderPass)
		);
	}
}
