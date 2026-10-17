#include "DrawSignature.hpp"

#include <engine/render/Renderer.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/ui/Interface.hpp>

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <imgui.h>
#include <memory>
#include <string>

TEST_SUITE_ID("engine.ui.interface")

TEST_CASE(
	"draw signatures cover visible geometry and command state without changing context",
	"[ui][headless][signature]"
) {
	engine::render::Renderer renderer;
	engine::ui::Interface interface;
	engine::ui::InterfaceSettings settings;
	settings.DisplayWidth = 640;
	settings.DisplayHeight = 480;
	REQUIRE(interface.Initialise(renderer, nullptr, settings));
	const auto frame = [&] {
		interface.Begin(1.0f / 60.0f);
		auto *list = ImGui::GetBackgroundDrawList();
		list->AddRectFilled({32, 32}, {96, 96}, IM_COL32(240, 200, 160, 255));
		list->AddImage(ImTextureRef{ImTextureID{19}}, {112, 32}, {176, 96});
		interface.End();
	};
	frame();
	frame();
	const auto baseline = interface.Signature();
	frame();
	CHECK(interface.Signature() == baseline);
	ImDrawData *draw = ImGui::GetDrawData();
	REQUIRE(draw != nullptr);
	REQUIRE(draw->CmdListsCount > 0);
	const auto signature = [&] { return engine::ui::draw_signature_detail::DrawGeometrySignature(draw); };
	CHECK(signature() == baseline);
	ImDrawList &list = *draw->CmdLists[0];
	REQUIRE_FALSE(list.VtxBuffer.empty());
	REQUIRE_FALSE(list.IdxBuffer.empty());
	REQUIRE_FALSE(list.CmdBuffer.empty());
	const auto changed = [&](auto &field, const auto &replacement) {
		const auto previous = field;
		field = replacement;
		CHECK(signature() != baseline);
		field = previous;
		CHECK(signature() == baseline);
	};
	changed(draw->DisplayPos.x, draw->DisplayPos.x + 1);
	changed(draw->DisplaySize.x, draw->DisplaySize.x + 1);
	changed(draw->FramebufferScale.x, draw->FramebufferScale.x + 1);
	changed(list.VtxBuffer[0].pos.x, list.VtxBuffer[0].pos.x + 1);
	changed(list.VtxBuffer[0].uv.x, list.VtxBuffer[0].uv.x + 0.25f);
	changed(list.VtxBuffer[0].col, list.VtxBuffer[0].col ^ ImU32{0x00010000});
	changed(list.IdxBuffer[0], static_cast<ImDrawIdx>(list.IdxBuffer[0] + 1));
	auto &command = list.CmdBuffer[0];
	changed(command.ClipRect.x, command.ClipRect.x + 1);
	changed(command.TexRef, ImTextureRef{ImTextureID{29}});
	changed(command.ElemCount, command.ElemCount + 1);
	changed(command.IdxOffset, command.IdxOffset + 1);
	changed(command.VtxOffset, command.VtxOffset + 1);
	changed(command.UserCallback, ImDrawCallback_ResetRenderState);
	ImGuiContext *owner = ImGui::GetCurrentContext();
	ImGuiContext *foreign = ImGui::CreateContext();
	ImGui::SetCurrentContext(foreign);
	CHECK(signature() == baseline);
	CHECK(ImGui::GetCurrentContext() == foreign);
	ImGui::DestroyContext(foreign);
	ImGui::SetCurrentContext(owner);
	CHECK(engine::ui::draw_signature_detail::DrawGeometrySignature(nullptr) == 0);
}

TEST_CASE("draw byte signatures preserve alignment and exact tails", "[ui][headless][signature]") {
	constexpr uint64_t SEED = 1469598103934665603ull;
	alignas(uint64_t) std::array<std::byte, 544> aligned{};
	alignas(uint64_t) std::array<std::byte, 552> shifted{};
	for (size_t index = 0; index < aligned.size(); ++index)
		aligned[index] = static_cast<std::byte>((index * 29 + 17) & 255);
	for (const size_t size : {0u,  1u,	2u,	 7u,   8u,	 9u,   15u,	 16u,  17u,	 31u,  32u,	 33u,
							  63u, 64u, 65u, 127u, 128u, 129u, 255u, 256u, 257u, 511u, 512u, 513u}) {
		const auto expected = engine::ui::draw_signature_detail::FoldBytes(SEED, aligned.data(), size);
		for (size_t offset = 0; offset < sizeof(uint64_t); ++offset) {
			INFO("bytes=" << size << " offset=" << offset);
			std::copy_n(aligned.data(), size, shifted.data() + offset);
			CHECK(
				engine::ui::draw_signature_detail::FoldBytes(SEED, shifted.data() + offset, size) == expected
			);
			shifted[offset + size] ^= std::byte{1};
			CHECK(
				engine::ui::draw_signature_detail::FoldBytes(SEED, shifted.data() + offset, size) == expected
			);
			if (size > 0) {
				shifted[offset + size - 1] ^= std::byte{1};
				CHECK(
					engine::ui::draw_signature_detail::FoldBytes(SEED, shifted.data() + offset, size) !=
					expected
				);
			}
		}
	}
	std::array<std::byte, 16> zeros{};
	CHECK(
		engine::ui::draw_signature_detail::FoldBytes(SEED, nullptr, 0) ==
		engine::ui::draw_signature_detail::FoldBytes(SEED, zeros.data(), 0)
	);
	for (size_t size = 0; size < zeros.size(); ++size)
		CHECK(
			engine::ui::draw_signature_detail::FoldBytes(SEED, zeros.data(), size) !=
			engine::ui::draw_signature_detail::FoldBytes(SEED, zeros.data(), size + 1)
		);
}

TEST_CASE("large draw signatures retain every lane and chunk order", "[ui][headless][signature]") {
	constexpr uint64_t SEED = 1469598103934665603ull;
	constexpr size_t CHUNK_BYTES = 4 * sizeof(uint64_t);
	std::array<std::byte, 1027> bytes{};
	for (size_t index = 0; index < bytes.size(); ++index)
		bytes[index] = static_cast<std::byte>((index * 29 + index / 17 + 17) & 255);
	const auto signature = [&] {
		return engine::ui::draw_signature_detail::FoldBytes(SEED, bytes.data(), bytes.size());
	};
	const auto baseline = signature();
	for (size_t lane = 0; lane < 4; ++lane) {
		for (size_t chunk : {0u, 16u, 31u}) {
			for (size_t byte : {0u, 3u, 7u}) {
				INFO("lane=" << lane << " chunk=" << chunk << " byte=" << byte);
				const size_t index = chunk * CHUNK_BYTES + lane * sizeof(uint64_t) + byte;
				bytes[index] ^= std::byte{1};
				CHECK(signature() != baseline);
				bytes[index] ^= std::byte{1};
				CHECK(signature() == baseline);
			}
		}
	}
	const auto exchanged = [&](size_t first, size_t second, size_t count) {
		INFO("swap=" << first << "," << second << " bytes=" << count);
		std::swap_ranges(bytes.begin() + first, bytes.begin() + first + count, bytes.begin() + second);
		CHECK(signature() != baseline);
		std::swap_ranges(bytes.begin() + first, bytes.begin() + first + count, bytes.begin() + second);
		CHECK(signature() == baseline);
	};
	exchanged(0, sizeof(uint64_t), sizeof(uint64_t));
	exchanged(0, CHUNK_BYTES, CHUNK_BYTES);
	exchanged(0, 16 * CHUNK_BYTES, CHUNK_BYTES);
	exchanged(16 * CHUNK_BYTES, 31 * CHUNK_BYTES, CHUNK_BYTES);
	for (size_t index = 1024; index < bytes.size(); ++index) {
		bytes[index] ^= std::byte{1};
		CHECK(signature() != baseline);
		bytes[index] ^= std::byte{1};
		CHECK(signature() == baseline);
	}
	CHECK(engine::ui::draw_signature_detail::FoldBytes(SEED + 1, bytes.data(), bytes.size()) != baseline);
}

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

TEST_CASE("interface owns its context beside a foreign host", "[ui][headless][context]") {
	struct ForeignHost {
		ImGuiContext *Previous = ImGui::GetCurrentContext();
		ImGuiContext *Context = ImGui::CreateContext();
		std::filesystem::path Directory =
			std::filesystem::temp_directory_path() /
			("mono-interface-owner-" +
			 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		ForeignHost() {
			REQUIRE(std::filesystem::create_directory(Directory));
			ImGui::SetCurrentContext(Context);
			auto &io = ImGui::GetIO();
			io.IniFilename = io.LogFilename = nullptr;
			io.DisplaySize = {73, 41};
			io.ConfigFlags = ImGuiConfigFlags_None;
		}
		~ForeignHost() {
			ImGui::SetCurrentContext(Context);
			ImGui::GetIO().IniFilename = ImGui::GetIO().LogFilename = nullptr;
			ImGui::DestroyContext(Context);
			ImGui::SetCurrentContext(Previous);
			std::error_code ignored;
			std::filesystem::remove_all(Directory, ignored);
		}
	} foreign;
	engine::render::Renderer renderer;
	engine::ui::Interface interface;
	engine::ui::InterfaceSettings settings;
	settings.DisplayWidth = 640;
	settings.DisplayHeight = 480;
	settings.LayoutPath = (foreign.Directory / "layout.ini").string();
	const auto layout = settings.LayoutPath;
	REQUIRE(interface.Initialise(renderer, nullptr, settings));
	auto *owned = ImGui::GetCurrentContext();
	CHECK(owned != foreign.Context);
	ImGui::SetCurrentContext(foreign.Context);
	CHECK(ImGui::GetIO().IniFilename == nullptr);
	CHECK(ImGui::GetIO().DisplaySize.x == 73);
	CHECK(ImGui::GetIO().ConfigFlags == ImGuiConfigFlags_None);
	REQUIRE(owned != foreign.Context);
	ImGui::SetCurrentContext(owned);
	REQUIRE(ImGui::GetIO().IniFilename != nullptr);
	CHECK(std::string(ImGui::GetIO().IniFilename) == layout);
	settings.LayoutPath = "replaced";
	CHECK(std::string(ImGui::GetIO().IniFilename) == layout);
	ImGui::GetIO().WantCaptureMouse = true;
	ImGui::GetIO().WantCaptureKeyboard = false;
	ImGui::SetCurrentContext(foreign.Context);
	ImGui::GetIO().WantCaptureMouse = false;
	ImGui::GetIO().WantCaptureKeyboard = true;
	CHECK(interface.WantsMouse());
	CHECK_FALSE(interface.WantsKeyboard());
	CHECK(ImGui::GetCurrentContext() == foreign.Context);
	SDL_Event motion{};
	motion.type = SDL_EVENT_MOUSE_MOTION;
	motion.motion.x = 240;
	motion.motion.y = 36;
	interface.ProcessEvent(motion);
	CHECK(ImGui::GetCurrentContext() == foreign.Context);
	interface.Begin(1.f / 60);
	CHECK(ImGui::GetCurrentContext() == owned);
	CHECK(ImGui::GetMousePos().x == 240);
	CHECK(ImGui::GetMousePos().y == 36);
	ImGui::Begin("Owned host");
	ImGui::TextUnformatted("Owned layout");
	ImGui::End();
	ImGui::SetCurrentContext(foreign.Context);
	interface.End();
	CHECK(ImGui::GetCurrentContext() == owned);
	ImGui::SetCurrentContext(foreign.Context);
	interface.PresentPlatformWindows();
	CHECK_FALSE(interface.HasPlatformWindows());
	CHECK(interface.HasFocus());
	CHECK(ImGui::GetCurrentContext() == foreign.Context);
	interface.Shutdown();
	CHECK(ImGui::GetCurrentContext() == foreign.Context);
	CHECK(ImGui::GetIO().IniFilename == nullptr);
	CHECK(ImGui::GetIO().DisplaySize.x == 73);
	CHECK(std::filesystem::is_regular_file(foreign.Directory / "layout.ini"));
	CHECK_FALSE(interface.WantsMouse());
	CHECK_FALSE(interface.WantsKeyboard());
}
