#include <engine/core/Arguments.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <shadercheck/Contract.hpp>
#include <shadercheck/Spirv.hpp>
#include <string>
#include <vector>

// Walk, read, reflect, check, print. Every decision that is a decision lives in
// the library; what is left here is which files to hand it and how the table
// looks.

namespace {

	namespace fs = std::filesystem;

	// The stage a compiled shader's name claims. The build compiles
	// `opaque.vert` to `opaque.vert.spv`, so the stage is the second extension
	// from the right.
	//
	// Reading it from the name rather than from the module is the whole point:
	// `render::Renderer` asks for a file by name and tells SDL which stage it
	// is, so a `.frag` holding a vertex entry point is a pipeline that fails to
	// create and a name that explains nothing.
	bool StageFromName(const fs::path &path, shadercheck::Stage &stage) {
		const std::string name = path.filename().string();
		const size_t spv = name.rfind(".spv");
		if (spv == std::string::npos || spv + 4 != name.size()) {
			return false;
		}
		const size_t dot = name.rfind('.', spv - 1);
		if (dot == std::string::npos) {
			return false;
		}

		const std::string suffix = name.substr(dot + 1, spv - dot - 1);
		if (suffix == "vert") {
			stage = shadercheck::Stage::Vertex;
			return true;
		}
		if (suffix == "frag") {
			stage = shadercheck::Stage::Fragment;
			return true;
		}
		if (suffix == "comp") {
			stage = shadercheck::Stage::Compute;
			return true;
		}
		return false;
	}

	bool ReadFile(const fs::path &path, std::vector<std::byte> &out) {
		std::ifstream file(path, std::ios::binary | std::ios::ate);
		if (!file) {
			return false;
		}
		const std::streamsize size = file.tellg();
		if (size < 0) {
			return false;
		}
		file.seekg(0);
		out.resize(static_cast<size_t>(size));
		return static_cast<bool>(file.read(reinterpret_cast<char *>(out.data()), size));
	}

	std::string Display(const fs::path &path) {
		std::error_code error;
		const fs::path relative = fs::relative(path, fs::current_path(), error);
		if (!error && !relative.empty() && relative.begin()->string() != "..") {
			return relative.generic_string();
		}
		return path.lexically_normal().generic_string();
	}
}

int main(int argc, char **argv) {
	engine::core::Arguments arguments(
		"shadercheck",
		"Reads compiled SPIR-V and checks it against the resource contract\n"
		"SDL_CreateGPUShader holds every backend to. Give it directories or .spv files.\n"
		"\n"
		"  shadercheck .cache/build/dev/shaderstage\n"
		"\n"
		"It translates nothing and proves nothing about a Metal device; it proves that\n"
		"the modules carry what a translation to MSL, DXIL or anything else reads.\n"
		"docs/DEFERRED.md D00001 carries the rest."
	);
	arguments.Flag("quiet", "Print only the shaders with findings");

	const auto parsed = arguments.Parse(argc, argv);
	if (parsed.HelpRequested) {
		std::cout << arguments.Help();
		return 0;
	}
	if (!parsed.Ok) {
		std::cerr << "shadercheck: " << parsed.Error << "\n";
		return 2;
	}

	std::vector<std::string_view> roots = arguments.Positional();
	if (roots.empty()) {
		std::cerr << "shadercheck: name a directory of compiled shaders, or some .spv files.\n";
		return 2;
	}

	std::vector<fs::path> shaders;
	for (const std::string_view root : roots) {
		const fs::path path(root);
		std::error_code error;
		if (!fs::exists(path, error)) {
			std::cerr << "shadercheck: no such path — " << root << "\n";
			return 2;
		}
		if (!fs::is_directory(path, error)) {
			shaders.push_back(path);
			continue;
		}
		for (const fs::directory_entry &entry : fs::recursive_directory_iterator(path)) {
			if (entry.is_regular_file() && entry.path().extension() == ".spv") {
				shaders.push_back(entry.path());
			}
		}
	}

	// **An empty run is a failure, not a pass.** A check that silently succeeds
	// when it was pointed at the wrong directory is the shape of every rule this
	// repository has watched stop being true — `just docs-check` at v0.2 and
	// `just preset=ci check` at v0.4 both reported green while checking nothing.
	if (shaders.empty()) {
		std::cerr << "shadercheck: no .spv files under the paths given. Nothing was checked.\n";
		return 2;
	}

	std::sort(shaders.begin(), shaders.end());

	size_t failed = 0;
	for (const fs::path &path : shaders) {
		const std::string display = Display(path);

		shadercheck::Stage stage = shadercheck::Stage::Unsupported;
		if (!StageFromName(path, stage)) {
			std::cout << display << ": FAIL\n";
			std::cout << "    the name says no stage; the build writes <name>.<vert|frag|comp>.spv\n";
			++failed;
			continue;
		}

		std::vector<std::byte> bytes;
		if (!ReadFile(path, bytes)) {
			std::cout << display << ": FAIL\n";
			std::cout << "    cannot read the file\n";
			++failed;
			continue;
		}

		const shadercheck::Module module = shadercheck::ReflectBytes(bytes);
		const std::vector<shadercheck::Finding> findings = shadercheck::Check(module, stage);
		const std::vector<uint32_t> metal = shadercheck::MetalIndices(module.Resources);

		if (!findings.empty()) {
			++failed;
			std::cout << display << ": FAIL\n";
			for (const shadercheck::Finding &finding : findings) {
				std::cout << "    " << finding.Message << "\n";
			}
			continue;
		}

		if (arguments.Has("quiet")) {
			continue;
		}

		std::cout << display << ": ok — " << shadercheck::StageName(module.EntryStage) << " '"
				  << module.EntryPointName << "', " << module.Resources.size() << " resource(s)\n";
		for (size_t index = 0; index < module.Resources.size(); ++index) {
			const shadercheck::Resource &resource = module.Resources[index];
			const bool isTexture = resource.Kind == shadercheck::ResourceKind::SampledTexture ||
								   resource.Kind == shadercheck::ResourceKind::StorageTexture;
			std::cout << "    set " << resource.Set << " binding " << resource.Binding << "  "
					  << shadercheck::KindName(resource.Kind) << " " << resource.Name << "  ->  "
					  << (isTexture ? "[[texture(" : "[[buffer(") << metal[index] << ")]]\n";
		}
	}

	if (failed > 0) {
		std::cout << "\nshadercheck FAILED — " << failed << " of " << shaders.size()
				  << " shader(s) break the contract SDL_CreateGPUShader documents.\n";
		return 1;
	}

	std::cout << "\nshaders ok — " << shaders.size()
			  << " module(s) single-entry, decorated and in the sets SDL's contract names.\n";
	return 0;
}
