#include "StartupWindow.hpp"

#include <engine/core/Log.hpp>
#include <engine/render/DebugText.hpp>

#include <SDL3/SDL.h>

#include <algorithm>

namespace studio {
	std::string StartupStatus(size_t completed, size_t total, std::string_view shaderName, size_t columns) {
		std::string status;
		if (total != 0) {
			status = std::to_string(std::min(completed, total)) + " / " + std::to_string(total) + " - ";
		}
		const bool prefixTruncated = status.size() > columns;
		if (prefixTruncated) status.resize(columns);
		const size_t available = columns - status.size();
		const size_t shaderCharacters = std::min(shaderName.size(), available);
		for (size_t index = 0; index < shaderCharacters; index++) {
			const unsigned char character = static_cast<unsigned char>(shaderName[index]);
			status += character >= 32 && character <= 126 ? static_cast<char>(character) : '?';
		}
		const bool truncated = prefixTruncated || shaderName.size() > shaderCharacters;
		if (truncated && columns >= 3) status.replace(columns - 3, 3, "...");
		return status;
	}

	StartupWindow::~StartupWindow() {
		Close();
	}

	bool StartupWindow::Open(const std::filesystem::path &iconPath) {
		Close();
		Cancelled = false;
		Window =
			SDL_CreateWindow("Atomic Engine", 420, 420, SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
		if (Window == nullptr) {
			ENGINE_WARN("startup window: {}", SDL_GetError());
			return false;
		}
		Canvas.Resize(420, 420);
		CanvasSurface = SDL_CreateSurfaceFrom(
			420, 420, SDL_PIXELFORMAT_RGBA32, const_cast<uint8_t *>(Canvas.GetPixels()), 420 * 4
		);
		if (CanvasSurface == nullptr) {
			ENGINE_WARN("startup canvas: {}", SDL_GetError());
			Close();
			return false;
		}
		Icon = SDL_LoadPNG(iconPath.string().c_str());
		if (Icon != nullptr)
			SDL_SetWindowIcon(Window, Icon);
		else
			ENGINE_WARN("startup icon: {}", SDL_GetError());
		Status = "Preparing renderer";
		Progress = 0.0f;
		Dirty = true;
		if (!Draw()) {
			Close();
			return false;
		}
		if (!SDL_ShowWindow(Window)) {
			ENGINE_WARN("startup window show: {}", SDL_GetError());
			Close();
			return false;
		}
		return true;
	}

	void StartupWindow::Close() {
		if (Icon != nullptr) SDL_DestroySurface(Icon);
		if (CanvasSurface != nullptr) SDL_DestroySurface(CanvasSurface);
		if (Window != nullptr) SDL_DestroyWindow(Window);
		Icon = nullptr;
		CanvasSurface = nullptr;
		Window = nullptr;
		Canvas.Resize(0, 0);
	}

	bool StartupWindow::Update(size_t completed, size_t total, std::string_view shaderName) {
		if (Window == nullptr) return true;
		const std::string status = StartupStatus(completed, total, shaderName, 44);
		const float progress =
			total == 0 ? 0.0f
					   : static_cast<float>(
							 static_cast<double>(std::min(completed, total)) / static_cast<double>(total)
						 );
		Dirty |= status != Status || progress != Progress;
		Status = status;
		Progress = progress;
		SDL_PumpEvents();
		// Leave input for the hidden editor queued. Only this window's events
		// and application quit belong to the temporary startup surface.
		SDL_FilterEvents(
			[](void *userdata, SDL_Event *event) {
				auto &popup = *static_cast<StartupWindow *>(userdata);
				if (event->type == SDL_EVENT_QUIT) {
					popup.Cancelled = true;
					return false;
				}
				if (event->type >= SDL_EVENT_WINDOW_FIRST && event->type <= SDL_EVENT_WINDOW_LAST &&
					event->window.windowID == SDL_GetWindowID(popup.Window)) {
					popup.Cancelled |= event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED;
					popup.Dirty = true;
					return false;
				}
				return true;
			},
			this
		);
		if (Dirty && !Cancelled && !Draw()) Close();
		return !Cancelled;
	}

	bool StartupWindow::Draw() {
		Canvas.Clear();
		Canvas.Fill(0, 0, 420, 420, 0, 0, 0, 255);
		if (Icon != nullptr && Icon->w > 0 && Icon->h > 0) {
			const double scale = 112.0 / std::max(Icon->w, Icon->h);
			const int width = static_cast<int>(Icon->w * scale);
			const int height = static_cast<int>(Icon->h * scale);
			const SDL_Rect target{(420 - width) / 2, 64, width, height};
			SDL_BlitSurfaceScaled(Icon, nullptr, CanvasSurface, &target, SDL_SCALEMODE_LINEAR);
		}
		auto text = [&](int y, std::string_view value) {
			const int width = engine::render::DebugText::Measure(value, 2);
			engine::render::DebugText::Draw(Canvas, (420 - width) / 2, y, value, 255, 255, 255, 2);
		};
		text(204, "Atomic Engine");
		text(240, "Compiling shaders");
		text(260, "First launch may take a little longer.");
		Canvas.Blend(32, 300, 356, 8, 255, 255, 255, 255);
		Canvas.Blend(33, 301, 354, 6, 0, 0, 0, 255);
		Canvas.Blend(34, 302, static_cast<int>(352 * Progress), 4, 255, 255, 255, 255);
		text(328, Status);

		// A window surface can otherwise create an accelerated renderer inside
		// SDL. Keep this temporary surface independent of GPU startup.
		const char *hint = SDL_GetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION);
		const bool hadHint = hint != nullptr;
		const std::string previousHint = hadHint ? hint : "";
		SDL_SetHintWithPriority(SDL_HINT_FRAMEBUFFER_ACCELERATION, "0", SDL_HINT_OVERRIDE);
		SDL_Surface *surface = SDL_GetWindowSurface(Window);
		SDL_ResetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION);
		if (hadHint) SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, previousHint.c_str());
		if (surface == nullptr ||
			!SDL_BlitSurfaceScaled(CanvasSurface, nullptr, surface, nullptr, SDL_SCALEMODE_NEAREST) ||
			!SDL_UpdateWindowSurface(Window)) {
			ENGINE_WARN("startup surface: {}", SDL_GetError());
			return false;
		}
		Dirty = false;
		return true;
	}
}
