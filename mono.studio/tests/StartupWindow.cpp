#include "StartupWindow.hpp"

#include <engine/testing/Suite.hpp>

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <string>

TEST_SUITE_ID("studio.startup_window")

TEST_CASE("startup status keeps counts and bounds shader names", "[studio][startup]") {
	CHECK(studio::StartupStatus(2, 10, "mesh.vert", 44) == "2 / 10 - mesh.vert");
	CHECK(studio::StartupStatus(20, 10, "mesh.vert", 44) == "10 / 10 - mesh.vert");
	CHECK(studio::StartupStatus(0, 0, "Preparing renderer", 44) == "Preparing renderer");
	CHECK(studio::StartupStatus(1, 2, "bad\nname", 44) == "1 / 2 - bad?name");
	CHECK(
		studio::StartupStatus(1, 2, std::string(4000, 'x'), 44) == "1 / 2 - " + std::string(33, 'x') + "..."
	);
	CHECK(studio::StartupStatus(1, 2, "mesh", 0).empty());
	CHECK(studio::StartupStatus(1, 2, "mesh", 2) == "1 ");
	CHECK(
		studio::StartupStatus(
			std::numeric_limits<size_t>::max(), std::numeric_limits<size_t>::max(), "mesh", 44
		)
			.size() == 44
	);
}

namespace {
	struct DummyVideo {
		std::string PreviousDriver;
		bool HadDriver = false;
		bool Ready = false;

		DummyVideo() {
			const char *previous = SDL_GetHint(SDL_HINT_VIDEO_DRIVER);
			HadDriver = previous != nullptr;
			if (previous != nullptr) PreviousDriver = previous;
			SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
			Ready = SDL_InitSubSystem(SDL_INIT_VIDEO);
		}

		~DummyVideo() {
			if (Ready) SDL_QuitSubSystem(SDL_INIT_VIDEO);
			if (HadDriver)
				SDL_SetHint(SDL_HINT_VIDEO_DRIVER, PreviousDriver.c_str());
			else
				SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
		}
	};
}

TEST_CASE("startup surface draws without GPU and preserves editor events", "[studio][startup]") {
	DummyVideo video;
	REQUIRE(video.Ready);
	studio::StartupWindow popup;
	REQUIRE(popup.Open("missing-startup-test-icon.png"));
	int windowCount = 0;
	SDL_Window **windows = SDL_GetWindows(&windowCount);
	REQUIRE(windows != nullptr);
	REQUIRE(windowCount == 1);
	SDL_Window *window = windows[0];
	SDL_free(windows);
	CHECK(SDL_GetRenderer(window) == nullptr);
	CHECK(popup.Update(1, 2, "test shader"));
	SDL_Surface *pixels = SDL_GetWindowSurface(window);
	REQUIRE(pixels != nullptr);
	Uint8 red = 0, green = 0, blue = 0, alpha = 0;
	REQUIRE(SDL_ReadSurfacePixel(pixels, 1, 1, &red, &green, &blue, &alpha));
	CHECK(red == 0);
	CHECK(green == 0);
	CHECK(blue == 0);
	REQUIRE(SDL_ReadSurfacePixel(pixels, 50, 303, &red, &green, &blue, &alpha));
	CHECK(red == 255);

	SDL_Event unrelated{};
	unrelated.type = SDL_EVENT_USER;
	REQUIRE(SDL_PushEvent(&unrelated));
	SDL_Event close{};
	close.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
	close.window.windowID = SDL_GetWindowID(window);
	REQUIRE(SDL_PushEvent(&close));
	CHECK_FALSE(popup.Update(2, 2, "test shader"));
	CHECK(SDL_HasEvent(SDL_EVENT_USER));
	SDL_FlushEvent(SDL_EVENT_USER);
	popup.Close();
	CHECK(SDL_GetWindowFromID(close.window.windowID) == nullptr);
}

TEST_CASE("startup quit is sticky and an unopened popup is harmless", "[studio][startup]") {
	studio::StartupWindow unopened;
	CHECK(unopened.Update(0, 0, ""));
	DummyVideo video;
	REQUIRE(video.Ready);
	studio::StartupWindow popup;
	REQUIRE(popup.Open("missing-startup-test-icon.png"));
	SDL_Event quit{};
	quit.type = SDL_EVENT_QUIT;
	REQUIRE(SDL_PushEvent(&quit));
	CHECK_FALSE(popup.Update(0, 0, "Preparing renderer"));
	CHECK_FALSE(popup.Update(1, 1, "shader"));
	popup.Close();
	REQUIRE(popup.Open("missing-startup-test-icon.png"));
	CHECK(popup.Update(0, 0, "Preparing renderer"));
}
