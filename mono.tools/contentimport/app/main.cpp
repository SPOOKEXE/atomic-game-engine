// Brings files and folders into the local content store.
//
// **The repeatable form of "put this in the cdn".** `ROADMAP.md` v0.10 asks for
// a named list of files to be added to the store, and a shell loop that did it
// once would leave nothing anybody could run again — on another machine, or after
// the store was cleared. This is that loop with a name.
//
//     contentimport ~/Music/album ~/art/fox.png
//     contentimport --root /tmp/store --publish --key HEX ~/art
//
// **Directories are walked and files are taken as they are.** A person dragging
// content in thinks in folders, and requiring them to expand one is asking them
// to do what a program is for.
//
// @tier shared

#include <engine/assets/Signature.hpp>
#include <engine/core/Arguments.hpp>
#include <engine/core/Log.hpp>

#include <array>
#include <assetc/Bake.hpp>
#include <cdn/LocalStore.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <shaderc/shaderc.hpp>
#include <string>
#include <vector>

namespace {
	// Seconds since the Unix epoch.
	//
	// **Read here and passed down**, because `cdn::LocalStore` holds no notion of
	// "now" — `assets::Grant`'s standing rule, and what lets a suite pin a
	// timestamp.
	uint64_t NowSeconds() {
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
										 std::chrono::system_clock::now().time_since_epoch()
		)
										 .count());
	}

	// A signing seed from sixty-four hex characters.
	//
	// **Spelled here and again in `mono.cdn/app/main.cpp`**, which is two copies
	// of one parser and is worth naming rather than leaving to be noticed. The
	// alternative is exporting it from `assets`, and the reason not to is that
	// `SigningKey::FromSeed` takes bytes on purpose: a *hex* constructor invites
	// a key in a config file, and this engine wants one on a command line or in
	// an environment variable. Two eighteen-line copies in two tools is the
	// cheaper of the two mistakes.
	std::optional<engine::assets::SigningKey> SeedFromHex(std::string_view text) {
		const std::string hex(text);
		if (hex.size() != 64) {
			return std::nullopt;
		}

		std::array<std::byte, 32> seed{};
		for (size_t index = 0; index < seed.size(); index++) {
			const std::string byte = hex.substr(index * 2, 2);
			char *end = nullptr;
			const long value = std::strtol(byte.c_str(), &end, 16);
			if (end != byte.c_str() + 2) {
				return std::nullopt;
			}
			seed[index] = static_cast<std::byte>(value);
		}
		return engine::assets::SigningKey::FromSeed(seed);
	}

	// Every file under a path, or the path itself when it is a file.
	//
	// **Skips anything unreadable rather than stopping.** A folder somebody points
	// at may hold a socket, a broken symlink or a file they cannot read, and
	// refusing the whole import over one of those would make the tool useless on
	// exactly the messy directories it exists for.
	std::vector<std::filesystem::path> Expand(const std::filesystem::path &root) {
		std::vector<std::filesystem::path> files;
		std::error_code failure;

		if (std::filesystem::is_regular_file(root, failure)) {
			files.push_back(root);
			return files;
		}

		if (!std::filesystem::is_directory(root, failure)) {
			ENGINE_ERROR("not a file or a directory: {}", root.string());
			return files;
		}

		for (std::filesystem::recursive_directory_iterator walk(
				 root, std::filesystem::directory_options::skip_permission_denied, failure
			 );
			 walk != std::filesystem::recursive_directory_iterator();
			 walk.increment(failure)) {
			if (failure) {
				break;
			}
			if (walk->is_regular_file(failure)) {
				files.push_back(walk->path());
			}
		}

		return files;
	}
}

namespace {
	// Which shaderc stage a shader's extension means.
	//
	// **The extension, because that is what an author already writes.** SDL and
	// every compiler in this vendor tree agree on `.vert`, `.frag` and `.comp`,
	// and inventing a manifest field to say the same thing again would be a
	// second description of a fact the filename already carries.
	bool StageOf(const std::string &extension, shaderc_shader_kind &kind) {
		if (extension == "vert") {
			kind = shaderc_glsl_vertex_shader;
			return true;
		}
		if (extension == "frag") {
			kind = shaderc_glsl_fragment_shader;
			return true;
		}
		if (extension == "comp") {
			kind = shaderc_glsl_compute_shader;
			return true;
		}

		// **`.glsl` is deliberately not a stage.** It is what a shared include
		// is called, and compiling one on its own would fail on every file
		// somebody wrote to be included rather than compiled.
		return false;
	}

	// Compiles every shader under `raw` into `baked`, as `<name>.spv`.
	//
	// **Includes are resolved and the module is published flat.** The manifest
	// has no word for a dependency between two assets, so a shader that
	// `#include`s another cannot be delivered as two. The cost is that editing a
	// shared header rebuilds every shader that includes it, which is what a
	// build system does anyway and what a store cannot yet express.
	//
	// @param raw   Where the sources are.
	// @param baked Where the modules go.
	// @return How many compiled. Failures are logged and skipped rather than
	//         stopping the import — one bad shader should not hold back a
	//         publish of forty assets.
	size_t BakeShaders(const std::filesystem::path &raw, const std::filesystem::path &baked) {
		namespace fs = std::filesystem;

		std::error_code error;
		if (!fs::is_directory(raw, error)) {
			return 0;
		}

		shaderc::Compiler compiler;
		shaderc::CompileOptions options;
		options.SetOptimizationLevel(shaderc_optimization_level_performance);

		size_t compiled = 0;
		for (const fs::directory_entry &entry : fs::recursive_directory_iterator(raw, error)) {
			if (!entry.is_regular_file()) {
				continue;
			}

			std::string extension = entry.path().extension().string();
			if (!extension.empty() && extension.front() == '.') {
				extension.erase(extension.begin());
			}

			shaderc_shader_kind kind{};
			if (!StageOf(extension, kind)) {
				continue;
			}

			std::ifstream file(entry.path(), std::ios::binary);
			const std::string source(
				(std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()
			);
			if (source.empty()) {
				ENGINE_ERROR("shader: {} is empty", entry.path().string());
				continue;
			}

			const std::string name = entry.path().filename().string();
			const shaderc::SpvCompilationResult result =
				compiler.CompileGlslToSpv(source, kind, name.c_str(), options);
			if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
				ENGINE_ERROR("shader: {}: {}", name, result.GetErrorMessage());
				continue;
			}

			const fs::path relative = fs::relative(entry.path(), raw, error);
			const fs::path out = baked / (relative.string() + ".spv");
			fs::create_directories(out.parent_path(), error);

			const std::vector<uint32_t> words(result.cbegin(), result.cend());
			std::ofstream sink(out, std::ios::binary | std::ios::trunc);
			sink.write(
				reinterpret_cast<const char *>(words.data()),
				static_cast<std::streamsize>(words.size() * sizeof(uint32_t))
			);
			compiled++;
		}
		return compiled;
	}
}

int main(int argc, char **argv) {
	engine::core::Arguments arguments(
		"contentimport", "Brings files and folders into the local content store."
	);

	arguments.Value("root", "PATH", "The store. Defaults to ~/Documents/atomic-game-engine/cdn");
	arguments.Flag("publish", "Publish raw/ into processed/ once the imports are done");
	arguments.Value("key", "HEX", "64 hex characters of Ed25519 seed. Defaults to the development key");

	const engine::core::Arguments::Result parsed = arguments.Parse(argc, argv);
	if (!parsed.Ok) {
		std::fprintf(stderr, "%s\n", parsed.Error.c_str());
		return 2;
	}
	if (parsed.HelpRequested) {
		std::printf("%s", arguments.Help().c_str());
		return 0;
	}

	const auto root = arguments.Get("root");
	const cdn::LocalPaths paths =
		root.has_value() ? cdn::LocalPathsUnder(std::filesystem::path(*root)) : cdn::DefaultLocalPaths();

	if (!cdn::EnsureLocalStore(paths)) {
		return 1;
	}
	ENGINE_INFO("content store at {}", paths.Root.string());

	const uint64_t now = NowSeconds();
	size_t imported = 0;
	size_t duplicates = 0;
	size_t failures = 0;
	uint64_t bytes = 0;

	for (const std::string_view given : arguments.Positional()) {
		for (const std::filesystem::path &file : Expand(std::filesystem::path(given))) {
			const auto report = cdn::ImportFile(paths, file, now);
			if (!report.has_value()) {
				failures++;
				continue;
			}
			if (report->Duplicate) {
				duplicates++;
			} else {
				imported++;
				bytes += report->Bytes;
			}
		}
	}

	ENGINE_INFO(
		"imported {} file(s), {} already present, {} failed, {} byte(s) added",
		imported,
		duplicates,
		failures,
		bytes
	);

	if (!arguments.Has("publish")) {
		return failures == 0 ? 0 : 1;
	}

	// **Baked before published, which is the step that was missing.** `raw/`
	// holds what somebody dragged in and a runtime decodes none of it —
	// `cdn/LocalStore.hpp` carries how long that went unnoticed and what it
	// looked like. `assetc::Bake` is the one baker; this supplies it the two
	// paths and nothing else, so a tree baked here and a tree baked by
	// `assetc --input` are the same tree.
	assetc::Settings baking;
	baking.Input = paths.Raw;
	baking.Output = paths.Baked;

	// **Every imported model is fitted into a unit box, and leaving it at the
	// authored scale was wrong in a way that took two versions to see.**
	//
	// A `MeshPart`'s `Size` *multiplies* the mesh's own coordinates rather than
	// fitting the mesh into a box — `examples/MeshGrid.luau` opens by calling
	// that load-bearing — so a mesh only behaves like the built-ins if its own
	// coordinates fit inside ±0.5. Then `Size` means metres for every part in
	// the scene rather than metres for some and a multiplier for others.
	//
	// This baked at authored scale, and a PMX character is authored around
	// twenty units tall. Two things followed, and only the first is obvious:
	//
	//   * the model drew about twenty times too big and overlapped every
	//     neighbour on the plate;
	//   * **the culling deleted it.** `scene::Bounds::HalfExtent` is `Size / 2`
	//     — two metres for a part sized four — and that is the box
	//     `graph::CullAndBound` tests against the frustum. The geometry extended
	//     ten times further than the box describing it, so the instance was
	//     culled while most of it was still on screen. Nothing is wrong with the
	//     cull; it was told a bounding box that was not one.
	//
	// The comment that used to be here argued that "silently rescaling
	// everything somebody already placed would move their world". That is a real
	// cost and it is the smaller one: nothing placed against these meshes could
	// have looked right, because the engine's whole size convention assumed the
	// unit box they did not have.
	//
	// **Not `assetc`'s default of four**, which fits a model to four metres and
	// is aimed at somebody pointing the baker at an art folder and wanting a
	// scene to be usable immediately. A store is a library, and a library entry
	// should carry no opinion about how big it is — one is the only scale that
	// means "whatever `Size` says".
	baking.ModelSize = 1.0f;

	// **The log, because `raw/` is flat and a model's sheets are not beside it.**
	// A `.pmx` names `tex/体.png`; import renamed both the model and the sheet to
	// hashes in one directory, so the reference cannot be followed through the
	// folder any more. `cdn::StoreTextureResolver` follows it through the import
	// log instead. Without this every PMX character baked with dangling sheet
	// names and published a model that arrives, draws, and is untextured.
	baking.ResolveTexture = cdn::StoreTextureResolver(paths);

	// **Shaders are baked here rather than in `assetc`, and the tier is why.**
	// `Tool::assetc` is `TIER shared` and so is `Engine::bake`; `libshaderc` is
	// gated on `MONO_BUILD_CLIENT`, so neither can link it. This program is a
	// bare `add_executable` with no tier, which is the same reason the mesh
	// baking lives here rather than in `Mono::cdn` — see this directory's
	// CMakeLists.
	//
	// **The server keeps carrying shaders regardless.** `AssetKind::Shader` is
	// in `assets`, which is shared, and an origin serves those bytes exactly as
	// it serves a mesh it never triangulates. What a server cannot do is compile
	// one, and this is the publish-time step that means it never has to.
	const size_t shaders = BakeShaders(paths.Raw, paths.Baked);

	std::string bakeFailure;
	const assetc::Report baked = assetc::Bake(baking, bakeFailure);
	if (!bakeFailure.empty()) {
		ENGINE_ERROR("bake: {}", bakeFailure);
		return 1;
	}
	ENGINE_INFO(
		"baked {} asset(s), {} failed — {} bytes out", baked.Assets.size(), baked.Failures, baked.OutputBytes
	);
	if (shaders > 0) {
		ENGINE_INFO("compiled {} shader(s) to SPIR-V", shaders);
	}

	// **Said separately, because it is a different kind of wrong.** A failed
	// asset did not bake and its row says so; a dangling texture reference bakes
	// perfectly and produces a model that draws untextured with nothing to
	// explain it. A store where this is non-zero has models whose sheets nothing
	// will ever fetch.
	if (baked.DanglingTextures > 0) {
		ENGINE_WARN(
			"{} model texture reference(s) named a file not in the store — those submeshes will draw "
			"untextured",
			baked.DanglingTextures
		);
	}

	// **The development identity when nobody says otherwise.** A store on this
	// machine, serving this machine's editor, had a key only so that the same
	// sixty-four characters could be mistyped in three places —
	// `cdn::DevelopmentSigningKey` carries what that costs and where it stops.
	// `--key` still overrides it, and `cdn --publish` still requires one.
	std::optional<engine::assets::SigningKey> signing;
	if (const auto hex = arguments.Get("key")) {
		signing = SeedFromHex(*hex);
		if (!signing.has_value()) {
			ENGINE_ERROR("--key must be 64 lowercase hex characters");
			return 2;
		}
	} else {
		signing = cdn::DevelopmentSigningKey();
		ENGINE_INFO("publishing with the development key — pass --key for an identity of your own");
	}

	const auto published = cdn::PublishLocal(paths, *signing, now);
	if (!published.has_value()) {
		return 1;
	}

	ENGINE_INFO(
		"published {} asset(s) in {} bundle(s), root {}",
		published->Assets,
		published->Bundles,
		published->Root.ToHex()
	);
	return 0;
}
