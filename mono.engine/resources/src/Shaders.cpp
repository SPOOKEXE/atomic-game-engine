#include <engine/core/Paths.hpp>
#include <engine/resources/Shaders.hpp>

#include <string>

namespace engine::resources {

	std::filesystem::path Shader(std::string_view name) {
		return core::Paths::Shaders(SHADER_MODULE) / std::filesystem::path(std::string(name) + ".spv");
	}
}
