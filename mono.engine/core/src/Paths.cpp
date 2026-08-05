#include "platform/Executable.hpp"

#include <engine/core/Paths.hpp>

namespace engine::core {

	const std::filesystem::path &Paths::Base() {
		static const std::filesystem::path base = [] {
			auto executable = platform::ExecutablePath();
			if (executable.empty()) {
				// Only reachable if the OS refused to say where we are. The
				// working directory is wrong more often than not, but a wrong
				// answer that resolves is better than no answer at all.
				return std::filesystem::current_path();
			}
			return executable.parent_path();
		}();
		return base;
	}

	namespace {
		std::filesystem::path &AssetsOverride() {
			static std::filesystem::path override;
			return override;
		}
	}

	const std::filesystem::path &Paths::Assets() {
		const auto &override = AssetsOverride();
		return override.empty() ? Base() : override;
	}

	void Paths::SetAssetsOverride(const std::filesystem::path &directory) {
		AssetsOverride() = directory;
	}

	std::filesystem::path Paths::Shaders(std::string_view module) {
		return Assets() / "shaders" / std::filesystem::path(module);
	}

	std::filesystem::path Paths::Fonts() {
		return Assets() / "fonts";
	}
}
