#include <engine/core/Assert.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Paths.hpp>
#include <engine/resources/Shaders.hpp>

#include <string>

namespace engine::resources {

	std::filesystem::path Shader(std::string_view name, ShaderForm form) {
		// The suffixes the build writes. `_mono_add_shaders` names them and this
		// is the only other place they are spelled, which is what keeps a rename
		// of either from becoming a "shader not found" with no cause in it.
		const std::string suffix = form == ShaderForm::Msl ? ".msl" : ".spv";

		// The suffix is added here and by nobody else. A caller that spelled it
		// too asks for `opaque.vert.spv.spv`, and the symptom is a missing file
		// reported by whoever opens it rather than by whoever named it wrong.
		ENGINE_ASSERT_MSG(
			!name.ends_with(".spv") && !name.ends_with(".msl"),
			"built-in shader '{}' already carries a compiled-form suffix; pass the GLSL name",
			name
		);

		std::filesystem::path staged =
			core::Paths::Shaders(SHADER_MODULE) / std::filesystem::path(std::string(name) + suffix);

		// Nothing here opens the file, so a built-in the build did not stage
		// becomes an error in the caller with no path in it. This is the only
		// place the path is known.
		ENGINE_TRACE("built-in shader '{}' resolves to {}", name, staged.string());
		return staged;
	}
}
