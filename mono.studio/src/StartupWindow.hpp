#pragma once

#include <engine/render/Overlay.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

struct SDL_Window;
struct SDL_Surface;

namespace studio {
	// Fits the software font's ASCII row while keeping shader names on one line.
	std::string StartupStatus(size_t completed, size_t total, std::string_view shaderName, size_t columns);

	// A CPU-only startup surface, independent of the GPU renderer being prepared.
	class StartupWindow {
	  public:
		StartupWindow() = default;
		~StartupWindow();
		StartupWindow(const StartupWindow &) = delete;
		StartupWindow &operator=(const StartupWindow &) = delete;

		bool Open(const std::filesystem::path &iconPath);
		void Close();
		bool Update(size_t completed, size_t total, std::string_view shaderName);

	  private:
		bool Draw();
		SDL_Window *Window = nullptr;
		SDL_Surface *CanvasSurface = nullptr;
		SDL_Surface *Icon = nullptr;
		engine::render::OverlayImage Canvas;
		std::string Status;
		float Progress = 0.0f;
		bool Cancelled = false;
		bool Dirty = true;
	};
}
