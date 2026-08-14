#include <engine/core/Arguments.hpp>

#include <spirv_msl.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

// One `.spv` in, one `.msl` out. Every decision here is a SPIRV-Cross option and
// every one of them is a decision about what SDL's Metal backend binds, so the
// comments are about SDL rather than about the translator.

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
			error = path.string() + " is not a SPIR-V module — its length is not a multiple of four";
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
	if (parsed.HelpRequested) {
		std::cout << arguments.Help();
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

	std::string source;
	try {
		spirv_cross::CompilerMSL compiler(std::move(words));

		spirv_cross::CompilerMSL::Options options = compiler.get_msl_options();

		// **Discrete bindings rather than an argument buffer, which is the
		// default and is deliberate anyway.** SDL's Metal backend binds each
		// texture, sampler and buffer to its own index — `SDL_CreateGPUShader`
		// documents the order — so a shader that expected its resources packed
		// into one argument buffer would find nothing bound at all.
		options.argument_buffers = false;

		// MSL 2.0, which is macOS 10.13 and iOS 11. Chosen rather than derived:
		// it is the floor SDL's Metal backend itself requires, and asking for
		// less would refuse constructs SPIRV-Cross emits for perfectly ordinary
		// SPIR-V while asking for more would refuse the hardware SDL supports.
		options.msl_version = spirv_cross::CompilerMSL::Options::make_msl_version(2, 0);

		// One emitter for both Apple platforms. `platform` decides a handful of
		// availability spellings and iOS is the narrower of the two, so an iOS
		// build would need this switched rather than the file re-translated —
		// recorded here because nothing in this repository builds for iOS yet.
		options.platform = spirv_cross::CompilerMSL::Options::macOS;

		compiler.set_msl_options(options);
		source = compiler.compile();
	} catch (const spirv_cross::CompilerError &failure) {
		// **The one place a translation failure is allowed to be a sentence.**
		// A built-in shader that cannot be translated is a build failure, the
		// same way a built-in that cannot be compiled is — the runtime half in
		// `render::ShaderCompiler` is where a failure becomes a diagnostic
		// somebody reads instead.
		std::cerr << "shadercross: " << inputs[0] << ": " << failure.what() << "\n";
		return 1;
	}

	if (!WriteText(fs::path(*output), source, error)) {
		std::cerr << "shadercross: " << error << "\n";
		return 1;
	}
	return 0;
}
