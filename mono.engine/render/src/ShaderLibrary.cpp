#include <engine/ecs/Store.hpp>
#include <engine/gui/Compile.hpp>
#include <engine/render/ShaderCompiler.hpp>
#include <engine/render/ShaderLibrary.hpp>
#include <engine/resources/Shaders.hpp>
#include <engine/scene/ShaderLens.hpp>
#include <engine/scene/Shaders.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

namespace engine::render {

	namespace {
		// The shaders this engine ships.
		//
		// **Two, and adding a third is a decision rather than a file drop.**
		// `docs/retired/DEFERRED.md` D00110 names the trap this list exists to close:
		// six fragments in `resources/shaders/` would compile, stage, pass every
		// test and be loaded by nothing. A name here is what loads one, so the
		// list and the directory are added to in one change or neither.
		constexpr std::array<std::string_view, 2> BUILT_IN{"unlit", "toon"};

		// Lens programs are only useful through ShaderLens. Keeping this list
		// separate from material programs makes an incompatible shader fail at
		// the library boundary instead of reaching a device pipeline by accident.
		constexpr std::array<std::string_view, 1> BUILT_IN_LENSES{"gravitational-lens"};

		// The staged SPIR-V for a built-in, as words.
		//
		// **Whole-file rather than streamed**, because a shader module is a few
		// kilobytes and is read once. The two failures are told apart: a file
		// that is not there is a build that did not stage, and a length that is
		// not a multiple of four is not a SPIR-V module whatever else it is.
		std::vector<uint32_t> ReadWords(const std::filesystem::path &path, std::string &error) {
			std::ifstream file(path, std::ios::binary | std::ios::ate);
			if (!file) {
				error = "built-in shader not staged: " + path.string();
				return {};
			}

			const std::streamoff size = file.tellg();
			if (size <= 0 || size % 4 != 0) {
				error = "built-in shader is not a SPIR-V module: " + path.string();
				return {};
			}

			std::vector<uint32_t> words(static_cast<size_t>(size) / 4);
			file.seekg(0);
			file.read(reinterpret_cast<char *>(words.data()), size);
			if (!file) {
				error = "built-in shader could not be read: " + path.string();
				return {};
			}
			return words;
		}
	}

	std::span<const std::string_view> BuiltInShaderNames() {
		return BUILT_IN;
	}

	bool IsBuiltInShader(std::string_view name) {
		return std::find(BUILT_IN.begin(), BUILT_IN.end(), name) != BUILT_IN.end();
	}

	std::span<const std::string_view> BuiltInLensShaderNames() {
		return BUILT_IN_LENSES;
	}

	bool IsBuiltInLensShader(std::string_view name) {
		return std::find(BUILT_IN_LENSES.begin(), BUILT_IN_LENSES.end(), name) != BUILT_IN_LENSES.end();
	}

	struct ShaderLibrary::Impl {
		// **One compiler for the library rather than one per compile.** Building
		// a `shaderc` instance acquires its options and its include resolver;
		// `ShaderCompiler`'s own header says it is reusable, and a world being
		// edited compiles the same script repeatedly.
		ShaderCompiler Compiler;

		// Keyed by `core::Name::Id`, matching `MeshTable::Entries` and
		// `MaterialCatalogue::ColourMaps`: a `Name` is already an integer in
		// this process, so hashing the integer skips the registry lock that
		// comparing text would take.
		std::unordered_map<uint32_t, ShaderModule> Modules;

		// What moved on the last `Refresh`, including what was dropped.
		std::vector<core::Name> Changed;

		// Scratch for the walk, kept so a steady frame allocates nothing.
		std::vector<core::Name> Demanded;

		// The other half of the walk - see `gui::DemandedShaders`. Kept
		// separate from `Demanded` and merged in, rather than widening that
		// scratch's own writer, because `scene::DemandedShaders` already owns
		// the contract "cleared first, sorted, deduplicated" and a second
		// caller into the same buffer would have to know not to violate it.
		std::vector<core::Name> GuiDemanded;

		std::unordered_map<uint32_t, ShaderModule> LensModules;
		std::vector<core::Name> LensChanged;
		std::vector<core::Name> LensDemanded;

		Impl() {
			Compiler.SetOptimise(true);
		}
	};

	ShaderLibrary::ShaderLibrary() : State(std::make_unique<Impl>()) {}

	ShaderLibrary::~ShaderLibrary() = default;

	size_t ShaderLibrary::Refresh(ecs::Store &store) {
		State->Changed.clear();
		scene::DemandedShaders(store, State->Demanded);

		// **Merged in rather than resolved by a second pass**, because every
		// name below this line is treated identically regardless of which
		// module asked - a `ShaderScript` an `ImageLabel` selects compiles
		// through the exact door a `Material` selecting the same name does.
		gui::DemandedShaders(store, State->GuiDemanded);
		State->Demanded.insert(State->Demanded.end(), State->GuiDemanded.begin(), State->GuiDemanded.end());
		std::sort(
			State->Demanded.begin(),
			State->Demanded.end(),
			[](const core::Name &left, const core::Name &right) { return left.Id() < right.Id(); }
		);
		State->Demanded.erase(
			std::unique(State->Demanded.begin(), State->Demanded.end()), State->Demanded.end()
		);

		// **Dropped first, so a name that stops being asked for and starts again
		// in the same call is resolved rather than skipped.** That is not a
		// contrived order: an author retyping a shader's name goes through the
		// invalid name for one keystroke, and the walk below sees the new one.
		for (auto entry = State->Modules.begin(); entry != State->Modules.end();) {
			const core::Name name = core::Name::FromId(entry->first);
			const bool wanted =
				std::find(State->Demanded.begin(), State->Demanded.end(), name) != State->Demanded.end();
			if (wanted) {
				++entry;
				continue;
			}
			State->Changed.push_back(name);
			entry = State->Modules.erase(entry);
		}

		for (const core::Name &name : State->Demanded) {
			const scene::ShaderText text = scene::ShaderTextOf(store, name);
			const auto found = State->Modules.find(name.Id());
			const bool held = found != State->Modules.end();

			if (text.Found) {
				// **The integer compare that keeps a GLSL front end out of the
				// frame loop.** A script whose revision has not moved since it
				// was compiled is the steady state of every world that is not
				// being edited. A module that came from a built-in is
				// recompiled whatever the revision says, because a script
				// appearing under a built-in's name is an override arriving.
				if (held && !found->second.BuiltIn && found->second.Revision == text.Revision) {
					continue;
				}

				ShaderCompilation result =
					State->Compiler.Compile(text.Code, ShaderStage::Fragment, name.Text());

				ShaderModule module;
				module.Revision = text.Revision;
				if (result.Failed) {
					// **A diagnostic and not a fatal**, which is
					// `render/AGENTS.md`'s split between the two compilers: a
					// built-in that fails to compile fails the build, and a
					// shader somebody is writing fails with a line number and
					// the engine keeps running.
					module.Error = std::move(result.Error);
				} else {
					module.SpirV = std::move(result.SpirV);
					module.Capabilities = std::move(result.Capabilities);
					module.Optimizations = std::move(result.Optimizations);
				}

				State->Modules[name.Id()] = std::move(module);
				State->Changed.push_back(name);
				continue;
			}

			// **Neither a built-in nor a missing name can change while the
			// engine runs**, so a held module of either kind is left alone. That
			// is what stops a typo being re-reported once a frame for the life
			// of a session.
			if (held) {
				continue;
			}

			ShaderModule module;
			if (IsBuiltInShader(name.Text())) {
				module.BuiltIn = true;
				// SPIR-V, whatever the device takes. This library's output is
				// the intermediate the renderer then translates if it has to, so
				// a built-in and a `ShaderScript` reach `AddShaderVariant` as the
				// same kind of thing.
				module.SpirV = ReadWords(
					resources::Shader(std::string(name.Text()) + ".frag", resources::ShaderForm::SpirV),
					module.Error
				);
				if (module.Error.empty()) {
					module.Capabilities = InspectShaderCapabilities(module.SpirV);
				}
			} else {
				// **Said out loud rather than passed over.** A misspelled shader
				// and a part deliberately left on the engine's default look
				// identical from the frame - `MissingTexture` makes the same
				// argument for a texture and exists for the same reason.
				module.Error = "no ShaderScript named '" + std::string(name.Text()) +
							   "' and the engine ships no shader of that name";
			}

			State->Modules[name.Id()] = std::move(module);
			State->Changed.push_back(name);
		}

		return State->Changed.size();
	}

	size_t ShaderLibrary::RefreshLenses(ecs::Store &store) {
		State->LensChanged.clear();
		scene::DemandedLensShaders(store, State->LensDemanded);

		for (auto entry = State->LensModules.begin(); entry != State->LensModules.end();) {
			const core::Name name = core::Name::FromId(entry->first);
			const bool wanted = std::find(State->LensDemanded.begin(), State->LensDemanded.end(), name) !=
								State->LensDemanded.end();
			if (wanted) {
				++entry;
				continue;
			}
			State->LensChanged.push_back(name);
			entry = State->LensModules.erase(entry);
		}

		for (const core::Name &name : State->LensDemanded) {
			const scene::ShaderText text = scene::LensShaderTextOf(store, name);
			const auto found = State->LensModules.find(name.Id());
			const bool held = found != State->LensModules.end();

			if (text.Found) {
				if (held && !found->second.BuiltIn && found->second.Revision == text.Revision) {
					continue;
				}

				ShaderCompilation result =
					State->Compiler.Compile(text.Code, ShaderStage::Fragment, name.Text());
				ShaderModule module;
				module.Authored = true;
				module.Revision = text.Revision;
				if (result.Failed) {
					module.Error = std::move(result.Error);
				} else {
					module.SpirV = std::move(result.SpirV);
					module.Capabilities = std::move(result.Capabilities);
					module.Optimizations = std::move(result.Optimizations);
				}

				State->LensModules[name.Id()] = std::move(module);
				State->LensChanged.push_back(name);
				continue;
			}

			// A built-in or a missing name is stable until the world changes. An
			// authored module is not: its LensShader can be deleted while a live
			// ShaderLens still names it, so resolve the fallback rather than
			// retaining the removed source's old GPU pipeline.
			if (held && !found->second.Authored) {
				continue;
			}

			ShaderModule module;
			if (IsBuiltInLensShader(name.Text())) {
				module.BuiltIn = true;
				module.SpirV = ReadWords(
					resources::Shader(std::string(name.Text()) + ".frag", resources::ShaderForm::SpirV),
					module.Error
				);
				if (module.Error.empty()) {
					module.Capabilities = InspectShaderCapabilities(module.SpirV);
				}
			} else {
				module.Error = "no LensShader named '" + std::string(name.Text()) +
							   "' and the engine ships no lens shader of that name";
			}

			State->LensModules[name.Id()] = std::move(module);
			State->LensChanged.push_back(name);
		}

		return State->LensChanged.size();
	}

	const ShaderModule *ShaderLibrary::Find(const core::Name &name) const {
		if (!name.IsValid()) {
			return nullptr;
		}
		const auto found = State->Modules.find(name.Id());
		return found == State->Modules.end() ? nullptr : &found->second;
	}

	const ShaderModule *ShaderLibrary::FindLens(const core::Name &name) const {
		if (!name.IsValid()) {
			return nullptr;
		}
		const auto found = State->LensModules.find(name.Id());
		return found == State->LensModules.end() ? nullptr : &found->second;
	}

	std::span<const core::Name> ShaderLibrary::Changed() const {
		return State->Changed;
	}

	std::span<const core::Name> ShaderLibrary::ChangedLenses() const {
		return State->LensChanged;
	}

	size_t ShaderLibrary::Size() const {
		return State->Modules.size();
	}

	size_t ShaderLibrary::LensSize() const {
		return State->LensModules.size();
	}
}
