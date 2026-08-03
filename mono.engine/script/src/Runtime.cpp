#include "JavaScriptRuntime.hpp"
#include "LuauRuntime.hpp"

#include <engine/script/Runtime.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace engine::script {

	Language LanguageOf(std::string_view path) {
		const std::filesystem::path file(path);
		const std::string extension = file.extension().string();

		if (extension == ".js" || extension == ".mjs" || extension == ".ts") {
			return Language::JavaScript;
		}

		// Luau by default rather than by extension match, so a file with no
		// suffix runs somewhere rather than being refused for a reason that has
		// nothing to do with what is in it.
		return Language::Luau;
	}

	bool Runtime::RunFile(const std::string &path) {
		std::ifstream file(path, std::ios::binary);
		if (!file) {
			Error = "could not open " + path;
			return false;
		}

		std::ostringstream contents;
		contents << file.rdbuf();
		return Run(contents.str(), path);
	}

	std::unique_ptr<Runtime> MakeRuntime(ecs::Store &store, Language language, const RuntimeLimits &limits) {
		if (language == Language::JavaScript) {
			return std::make_unique<JavaScriptRuntime>(store, limits);
		}
		return std::make_unique<LuauRuntime>(store, limits);
	}

	bool RunScriptFile(
		ecs::Store &store, const std::string &path, std::string &error, const RuntimeLimits &limits
	) {
		// The extension picks the VM, which is what makes "two languages, one
		// binding surface" a fact a caller never has to think about: a loader
		// hands over a path and the same world comes back either way.
		const std::unique_ptr<Runtime> runtime = MakeRuntime(store, LanguageOf(path), limits);
		if (runtime->RunFile(path)) {
			return true;
		}

		error = runtime->LastError();
		return false;
	}
}
