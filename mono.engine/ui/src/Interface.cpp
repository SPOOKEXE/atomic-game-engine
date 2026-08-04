#include <engine/core/Log.hpp>
#include <engine/ui/Interface.hpp>
#include <engine/ui/Theme.hpp>

#include <SDL3/SDL_gpu.h>

#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_sdlgpu3.h>
#include <imgui.h>
#include <string>

namespace engine::ui {

	struct Interface::Impl {
		ImGuiContext *Context = nullptr;

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

		if (window == nullptr) {
			ENGINE_ERROR("ui::Interface needs a window");
			return false;
		}

		const render::BackendHandles backend = renderer.Backend();
		if (backend.Device == nullptr) {
			// Not an assertion. A headless run legitimately has no device, and
			// the caller decides whether that is fatal — same contract as
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
		// arrow keys from a text field the moment a controller is plugged in —
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

		// The font atlas is rebuilt from this rather than scaled as a texture,
		// so text at 1.5x is sharp instead of blurry. Set before the backend
		// initialises, because that is when the atlas is first built.
		const float scale = settings.Scale > 0.0f ? settings.Scale : 1.0f;
		io.FontGlobalScale = 1.0f;
		ImFontConfig font;
		font.SizePixels = 13.0f * scale;
		io.Fonts->AddFontDefault(&font);

		ApplyEditorTheme(scale);

		if (!ImGui_ImplSDL3_InitForSDLGPU(window)) {
			ENGINE_ERROR("ImGui_ImplSDL3_InitForSDLGPU failed");
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
			ImGui::DestroyContext(State->Context);
			State->Context = nullptr;
			return false;
		}

		State->Ready = true;
		return true;
	}

	void Interface::Shutdown() {
		if (!State->Ready) {
			return;
		}

		// Renderer backend first, platform backend second, context last. The
		// reverse of construction, and it matters: the renderer backend
		// releases GPU objects it registered with the context, so destroying
		// the context first leaves them to be freed by a device that has been
		// told nothing about them.
		ImGui_ImplSDLGPU3_Shutdown();
		ImGui_ImplSDL3_Shutdown();
		ImGui::DestroyContext(State->Context);

		State->Context = nullptr;
		State->Draw = nullptr;
		State->Ready = false;
	}

	bool Interface::IsInitialised() const {
		return State->Ready;
	}

	void Interface::ProcessEvent(const SDL_Event &event) {
		if (!State->Ready) {
			return;
		}
		ImGui_ImplSDL3_ProcessEvent(&event);
	}

	void Interface::Begin(float frameSeconds) {
		if (!State->Ready) {
			return;
		}

		ImGui_ImplSDLGPU3_NewFrame();
		ImGui_ImplSDL3_NewFrame();

		// **The delta is overwritten after `NewFrame`, not before it.** The
		// SDL3 backend reads a wall clock and assigns `io.DeltaTime` itself, so
		// setting it first is a value that is thrown away. Passing it in is
		// what makes a fixed-step or headless driver produce the same frames
		// every run — the same rule `world::Driver` and `net` already hold.
		ImGui::NewFrame();
		ImGui::GetIO().DeltaTime = frameSeconds > 0.0f ? frameSeconds : 1.0f / 60.0f;
	}

	void Interface::End() {
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

	bool Interface::Prepare(void *commandBuffer) {
		if (!State->Ready || State->Draw == nullptr || commandBuffer == nullptr) {
			return false;
		}

		// A minimised window produces a frame with no area, and the backend
		// asserts on one rather than skipping it.
		if (State->Draw->DisplaySize.x <= 0.0f || State->Draw->DisplaySize.y <= 0.0f) {
			return false;
		}

		ImGui_ImplSDLGPU3_PrepareDrawData(State->Draw, static_cast<SDL_GPUCommandBuffer *>(commandBuffer));
		return true;
	}

	void Interface::Record(void *commandBuffer, void *renderPass) {
		if (!State->Ready || State->Draw == nullptr) {
			return;
		}

		ImGui_ImplSDLGPU3_RenderDrawData(
			State->Draw,
			static_cast<SDL_GPUCommandBuffer *>(commandBuffer),
			static_cast<SDL_GPURenderPass *>(renderPass)
		);
	}
}
