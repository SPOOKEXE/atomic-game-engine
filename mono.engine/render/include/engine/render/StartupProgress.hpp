#pragma once

// Startup progress reported while the renderer compiles shaders and builds pipelines.

#include <cstdint>
#include <functional>
#include <string_view>

namespace engine::render {
	// Completed shader compilation groups and the current native shader or pipeline.
	// Groups include their graphics variants. Total zero denotes device setup.
	struct StartupProgress {
		// Groups whose native compilation work has finished.
		size_t Completed = 0;
		// Planned groups, fixed once device capabilities are known.
		size_t Total = 0;
		// Current native operation. Valid only for the callback invocation.
		std::string_view ShaderName;
	};

	// Called on the Initialise owner thread. False cancels between native operations.
	using StartupProgressCallback = std::function<bool(const StartupProgress &)>;
}
