#include <engine/core/Arguments.hpp>
#include <engine/msl/Translate.hpp>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

// One `.spv` in, one `.msl` out. The translation itself is `Engine::msl`,
// because `render` needs the same one while the engine runs; what is here is
// which file to read and where to put the result.

namespace {

	namespace fs = std::filesystem;

	// A SPIR-V module as words.
	//
	// A module is a few kilobytes and is read once, so whole-file rather than
	// streamed. The two failures are told apart: a file that is not there is a
	// build that did not compile, and a length that is not a multiple of four is
	// not SPIR-V whatever else it is.
	bool ReadWords(const fs::path &path, std::vector<uint32_t> &words, std::string &error) {
		std::ifstream file(path, std::ios::binary | std::ios::ate);
		if (!file) {
			error = "cannot open " + path.string();
			return false;
		}

		const std::streamoff size = file.tellg();
		if (size <= 0 || size % 4 != 0) {
			error = path.string() + " is not a SPIR-V module - its length is not a multiple of four";
			return false;
		}

		words.resize(static_cast<size_t>(size) / 4);
		file.seekg(0);
		if (!file.read(reinterpret_cast<char *>(words.data()), size)) {
			error = "cannot read " + path.string();
			return false;
		}
		return true;
	}

	bool WriteText(const fs::path &path, const std::string &text, std::string &error) {
		std::error_code code;
		fs::create_directories(path.parent_path(), code);

		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		if (!file || !file.write(text.data(), static_cast<std::streamsize>(text.size()))) {
			error = "cannot write " + path.string();
			return false;
		}
		return true;
	}
}

int main(int argc, char **argv) {
	engine::core::Arguments arguments(
		"shadercross",
		"Translates one compiled SPIR-V module to Metal Shading Language.\n"
		"\n"
		"  shadercross opaque.vert.spv -o opaque.vert.msl\n"
		"\n"
		"MSL reserves `main`, so the entry point comes out as `main0` and every caller\n"
		"of SDL_CreateGPUShader has to ask for that name on this format. The emitted\n"
		"file is checked by `just shader-check`, which reads it back against the\n"
		"SPIR-V it came from. docs/DEFERRED.md D00001 carries the rest."
	);
	arguments.Value("o", "PATH", "Where to write the MSL. Required.");

	const auto parsed = arguments.Parse(argc, argv);
	if (parsed.VersionRequested) {
		std::cout << arguments.VersionLine();
		return 0;
	}
	if (parsed.HelpRequested) {
		std::cout << arguments.Help();
		return 0;
	}
	if (parsed.DescribeRequested) {
		std::fputs(arguments.Describe().c_str(), stdout);
		return 0;
	}
	if (!parsed.Ok) {
		std::cerr << "shadercross: " << parsed.Error << "\n";
		return 2;
	}

	const std::vector<std::string_view> inputs = arguments.Positional();
	const auto output = arguments.Get("o");
	if (inputs.size() != 1 || !output.has_value()) {
		std::cerr << "shadercross: name one .spv file and one -o PATH.\n";
		return 2;
	}

	std::vector<uint32_t> words;
	std::string error;
	if (!ReadWords(fs::path(inputs[0]), words, error)) {
		std::cerr << "shadercross: " << error << "\n";
		return 1;
	}

	const engine::msl::Translation translation = engine::msl::Translate(words);
	if (translation.Failed) {
		// **A built-in that cannot be translated fails the build**, the same way
		// one that cannot be compiled does. The runtime half in
		// `render::ShaderCompiler` is where a failure becomes a diagnostic
		// somebody reads instead of a build that stops.
		std::cerr << "shadercross: " << inputs[0] << ": " << translation.Error << "\n";
		return 1;
	}

	if (!WriteText(fs::path(*output), translation.Source, error)) {
		std::cerr << "shadercross: " << error << "\n";
		return 1;
	}
	return 0;
}
