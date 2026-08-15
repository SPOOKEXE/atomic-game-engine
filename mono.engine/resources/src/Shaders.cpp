#include <engine/core/Paths.hpp>
#include <engine/resources/Shaders.hpp>

#include <string>

namespace engine::resources {

	std::filesystem::path Shader(std::string_view name, ShaderForm form) {
		// The suffixes the build writes. `_mono_add_shaders` names them and this
		// is the only other place they are spelled, which is what keeps a rename
		// of either from becoming a "shader not found" with no cause in it.
		const std::string suffix = form == ShaderForm::Msl ? ".msl" : ".spv";
		return core::Paths::Shaders(SHADER_MODULE) / std::filesystem::path(std::string(name) + suffix);
	}
}
