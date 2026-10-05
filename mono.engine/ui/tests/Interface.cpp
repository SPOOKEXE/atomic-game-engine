#include <engine/render/Renderer.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/ui/Interface.hpp>

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>

#include <imgui.h>
#include <memory>
#include <string>

TEST_SUITE_ID("engine.ui.interface")

TEST_CASE("headless interface accepts SDL pointer and keyboard events", "[ui][headless]") {
	engine::render::Renderer renderer;
	engine::ui::Interface interface;
	engine::ui::InterfaceSettings settings;
	settings.DisplayWidth = 640;
	settings.DisplayHeight = 480;
	REQUIRE(interface.Initialise(renderer, nullptr, settings));

	SDL_Event motion{};
	motion.type = SDL_EVENT_MOUSE_MOTION;
	motion.motion.x = 120.0f;
	motion.motion.y = 80.0f;
	interface.ProcessEvent(motion);
	SDL_Event click{};
	click.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
	click.button.button = SDL_BUTTON_LEFT;
	interface.ProcessEvent(click);
	SDL_Event key{};
	key.type = SDL_EVENT_KEY_DOWN;
	key.key.key = SDLK_F5;
	key.key.scancode = SDL_SCANCODE_F5;
	interface.Begin(1.0f / 60.0f);
	CHECK(ImGui::GetMousePos().x == 120.0f);
	CHECK(ImGui::GetMousePos().y == 80.0f);
	CHECK(ImGui::IsMouseDown(ImGuiMouseButton_Left));
	interface.End();

	click.type = SDL_EVENT_MOUSE_BUTTON_UP;
	interface.ProcessEvent(click);
	interface.Begin(1.0f / 60.0f);
	CHECK_FALSE(ImGui::IsMouseDown(ImGuiMouseButton_Left));
	interface.End();

	interface.ProcessEvent(key);
	interface.Begin(1.0f / 60.0f);
	CHECK(ImGui::IsKeyDown(ImGuiKey_F5));
	interface.End();

	key.type = SDL_EVENT_KEY_UP;
	interface.ProcessEvent(key);
	interface.Begin(1.0f / 60.0f);
	CHECK_FALSE(ImGui::IsKeyDown(ImGuiKey_F5));
	interface.End();

	std::string text = "original";
	SDL_Event typed{};
	typed.type = SDL_EVENT_TEXT_INPUT;
	typed.text.text = text.c_str();
	interface.ProcessEvent(typed);
	text.assign("replaced");
	interface.Begin(1.0f / 60.0f);
	const ImVector<ImWchar> &characters = ImGui::GetIO().InputQueueCharacters;
	REQUIRE(characters.Size >= 8);
	CHECK(characters[0] == 'o');
	CHECK(characters[7] == 'l');
	interface.End();
}

TEST_CASE("automation events retain their pointer position through a frame", "[ui][headless]") {
	engine::render::Renderer renderer;
	engine::ui::Interface interface;
	engine::ui::InterfaceSettings settings;
	settings.DisplayWidth = 640;
	settings.DisplayHeight = 480;
	REQUIRE(interface.Initialise(renderer, nullptr, settings));

	SDL_Event physical{};
	physical.type = SDL_EVENT_MOUSE_MOTION;
	physical.motion.x = 10.0f;
	physical.motion.y = 20.0f;
	interface.ProcessEvent(physical);

	SDL_Event syntheticMotion{};
	syntheticMotion.type = SDL_EVENT_MOUSE_MOTION;
	syntheticMotion.motion.x = 320.0f;
	syntheticMotion.motion.y = 240.0f;
	interface.QueueAutomationEvent(syntheticMotion);
	SDL_Event syntheticDown{};
	syntheticDown.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
	syntheticDown.button.button = SDL_BUTTON_LEFT;
	interface.QueueAutomationEvent(syntheticDown);

	interface.Begin(1.0f / 60.0f);
	CHECK(ImGui::GetMousePos().x == 320.0f);
	CHECK(ImGui::GetMousePos().y == 240.0f);
	CHECK(ImGui::IsMouseDown(ImGuiMouseButton_Left));
	interface.End();
}

TEST_CASE("headless interfaces keep detached window requests on their virtual display", "[ui][headless]") {
	engine::render::Renderer renderer;
	engine::ui::Interface interface;
	interface.PresentPlatformWindows();
	CHECK_FALSE(interface.HasPlatformWindows());
	engine::ui::InterfaceSettings settings;
	settings.PlatformWindows = true;
	REQUIRE(interface.Initialise(renderer, nullptr, settings));
	CHECK((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) == 0);
	interface.Begin(1.0f / 60.0f);
	ImGui::Begin("Headless panel");
	ImGui::TextUnformatted("No native display required");
	ImGui::End();
	interface.End();
	interface.PresentPlatformWindows();
	CHECK_FALSE(interface.HasPlatformWindows());
	CHECK(interface.HasFocus());
	interface.Shutdown();
	interface.PresentPlatformWindows();
	CHECK_FALSE(interface.HasPlatformWindows());
}

// Opt in on a native desktop. SDL's dummy video driver cannot create detached
// desktop windows, and a headless GPU test cannot exercise their input lifecycle.
TEST_CASE("detached interface windows render and close without closing the host", "[ui][desktop][.]") {
	REQUIRE(SDL_Init(SDL_INIT_VIDEO));
	struct VideoLifetime {
		~VideoLifetime() {
			SDL_QuitSubSystem(SDL_INIT_VIDEO);
		}
	} videoLifetime;
	std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window(
		SDL_CreateWindow("Interface window test", 320, 240, SDL_WINDOW_RESIZABLE), SDL_DestroyWindow
	);
	REQUIRE(window != nullptr);
	engine::render::Renderer renderer;
	REQUIRE(renderer.Initialise(window.get()));
	engine::ui::Interface interface;
	engine::ui::InterfaceSettings settings;
	settings.PlatformWindows = true;
	REQUIRE(interface.Initialise(renderer, window.get(), settings));
	if ((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) == 0) {
		SKIP("SDL display backend does not support desktop platform windows");
	}
	bool open = true;
	const auto frame = [&] {
		SDL_Event event;
		while (SDL_PollEvent(&event))
			interface.ProcessEvent(event);
		interface.Begin(1.0f / 60.0f);
		if (open) {
			const ImGuiViewport *main = ImGui::GetMainViewport();
			ImGui::SetNextWindowPos(ImVec2(main->Pos.x + main->Size.x + 32.0f, main->Pos.y));
			ImGui::SetNextWindowSize(ImVec2(240.0f, 160.0f));
			ImGui::Begin("Detached panel", &open, ImGuiWindowFlags_NoSavedSettings);
			ImGui::TextUnformatted("Detached contents");
			ImGui::End();
		}
		interface.End();
		interface.PresentPlatformWindows();
	};
	frame();
	frame();
	frame();
	REQUIRE(interface.HasPlatformWindows());
	REQUIRE(ImGui::GetPlatformIO().Viewports.Size == 2);
	ImGuiViewport *detached = ImGui::GetPlatformIO().Viewports[1];
	const auto detachedId = static_cast<SDL_WindowID>(reinterpret_cast<intptr_t>(detached->PlatformHandle));
	REQUIRE(SDL_GetWindowFromID(detachedId) != nullptr);
	CHECK(detached->RendererUserData != nullptr);
	SDL_Event close{};
	close.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
	close.window.windowID = detachedId;
	interface.ProcessEvent(close);
	frame();
	CHECK_FALSE(open);
	frame();
	frame();
	CHECK_FALSE(interface.HasPlatformWindows());
	CHECK(SDL_GetWindowFromID(detachedId) == nullptr);
	CHECK(SDL_GetWindowFromID(SDL_GetWindowID(window.get())) == window.get());
}
