#pragma once

// From what an author wrote to GLSL the compiler takes.
//
// A `ShaderScript` is either a whole fragment program (it holds `main`) or a
// surface: `void Surface(SurfaceInput i, inout SurfaceOutput o)`, and optionally
// `vec3 Light(SurfaceOutput s, LightSample l)`. A surface is wrapped with the
// engine's declarations, lighting loop and outputs, once per pass it is drawn
// in. Both kinds may `#include` a `ShaderModule` of the world by name, or an
// engine module:
//
//   engine/surface   SurfaceInput, SurfaceOutput, LightSample, DefaultSurface
//                    (always present in a surface)
//   engine/pass      PassInput and the images a `vec4 Pass(PassInput i)` or a
//                    `void Compute(PassInput i, ivec2 pixel)` reads, and
//                    `Store` for the latter (always present in a pass)
//   engine/brdf      lobes for a Light function (always present in a surface)
//
// Text in, text out: nothing here touches a device or a store.
//
// @tier L12 · client

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::render {
	// Largest assembled author text, include depth and module count.
	inline constexpr size_t MAX_SHADER_AUTHOR_BYTES = 1024 * 1024;
	inline constexpr size_t MAX_SHADER_INCLUDE_DEPTH = 16;
	inline constexpr size_t MAX_SHADER_INCLUDES = 64;

	// The text of a world's module, or nothing when the world holds none.
	using ShaderModuleResolver = std::function<std::optional<std::string>(std::string_view name)>;

	// The pass an assembled surface is drawn in.
	enum class SurfacePass : uint8_t {
		// One lit colour: transparent, mirrored and portal draws.
		Forward,
		// The eight G-buffer targets of the main opaque pass.
		GBuffer,
	};

	// What assembly produced.
	struct ShaderAssembly {
		// GLSL for the forward pass, or the whole program.
		std::string Forward;
		// GLSL for the G-buffer pass; empty for a whole program.
		std::string GBuffer;
		// Whether the source declared a Surface.
		bool Surface = false;
		// Whether it declared a Pass, which is drawn over the lit image.
		bool Pass = false;
		// Whether the pass is `void Compute(PassInput i, ivec2 pixel)`, compiled
		// for the compute stage.
		bool Compute = false;
		// Whether it declared a Light as well.
		bool Lit = false;
		// The named images a pass reads, in binding order.
		std::vector<std::string> Images;
		// Why there is no text; empty on success.
		std::string Error;
	};

	// Expands and wraps one script. Compile errors name the author's own file and
	// line, including those inside modules.
	ShaderAssembly
	AssembleShader(std::string_view name, std::string_view source, const ShaderModuleResolver &modules);

	// The text of an engine module, or nothing.
	std::optional<std::string_view> EngineShaderModule(std::string_view name);
}
