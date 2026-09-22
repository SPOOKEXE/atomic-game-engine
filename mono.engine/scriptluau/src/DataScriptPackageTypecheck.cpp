// Static checking for source submitted through atomic.data-script.v1.
//
// A package is a single in-memory Luau module. It has no filesystem resolver
// and no `require`, matching the package-only runtime. The frontend is built
// only for this admission path, before a caller serializes or mutates a world.

#include "PackageLuauDeclarations.hpp"

#include <engine/script/DataScriptPackage.hpp>
#include <engine/scriptluau/Runtime.hpp>

#include <Luau/BuiltinDefinitions.h>
#include <Luau/Config.h>
#include <Luau/Frontend.h>
#include <Luau/ToString.h>
#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace engine::script {

	namespace {
		constexpr size_t MAX_DETAIL_BYTES = 512;

		struct PackageFiles final : Luau::FileResolver {
			std::string Entry;
			std::string Source;

			std::optional<Luau::SourceCode> readSource(const Luau::ModuleName &name) override {
				if (name != Entry) return std::nullopt;
				return Luau::SourceCode{Source, Luau::SourceCode::Module};
			}
		};

		struct StrictConfigs final : Luau::ConfigResolver {
			Luau::Config Strict;

			StrictConfigs() {
				Strict.mode = Luau::Mode::Strict;
			}

			const Luau::Config &
			getConfig(const Luau::ModuleName &, const Luau::TypeCheckLimits &) const override {
				return Strict;
			}
		};

		void AppendBounded(std::string &out, std::string_view text) {
			const size_t available = MAX_DETAIL_BYTES > out.size() ? MAX_DETAIL_BYTES - out.size() : 0;
			out.append(text.substr(0, available));
		}

		std::string At(const Luau::Location &location) {
			// The synthetic strict directive is line zero in the checked source.
			// Never expose that line to the package author.
			const unsigned line = location.begin.line == 0 ? 0 : location.begin.line - 1;
			return std::to_string(line + 1) + ":" + std::to_string(location.begin.column + 1) + ": ";
		}

		void RemoveGlobal(Luau::Frontend &frontend, std::string_view name) {
			const Luau::AstName global =
				frontend.globals.globalNames.names->getOrAdd(name.data(), name.size());
			frontend.globals.globalScope->bindings.erase(global);
		}

		bool LoadPackageVocabulary(Luau::Frontend &frontend, std::string &error) {
			// Types and constructors come from the generated binding contract. This
			// keeps class properties, datatype operators, and Instance.new overloads
			// synchronized with the VM binding metadata.
			const Luau::LoadDefinitionFileResult loaded = frontend.loadDefinitionFile(
				frontend.globals,
				frontend.globals.globalScope,
				std::string(GENERATED_LUAU_DECLARATIONS),
				"@engine",
				false
			);
			if (!loaded.success) {
				error = "data-script package checker vocabulary failed";
				if (!loaded.parseResult.errors.empty()) {
					error += ": ";
					error += loaded.parseResult.errors.front().what();
				}
				return false;
			}

			// The generated declaration describes every normal game-script global.
			// A package opens only the same safe libraries and scene mutation surface
			// as its runtime, so services, script identity, and module loading leave
			// the static scope before its package-specific values are installed.
			constexpr std::array<std::string_view, 24> UNAVAILABLE_GLOBALS{
				"MessagingService",
				"TeleportService",
				"CrossWorldService",
				"MemoryStoreService",
				"DataStoreService",
				"RunService",
				"ComputeService",
				"DataSceneService",
				"ContentService",
				"CollectionService",
				"HttpService",
				"SoundService",
				"TweenService",
				"Debris",
				"UserInputService",
				"ContextActionService",
				"SettingsService",
				"Scope",
				"script",
				"require",
				"task",
				"wait",
				"spawn",
				"delay",
			};
			for (const std::string_view name : UNAVAILABLE_GLOBALS) {
				RemoveGlobal(frontend, name);
			}
			// `os` and `debug` are absent from the runtime's safe-library set. The
			// frontend supplies them as built-ins, so remove those separately.
			RemoveGlobal(frontend, "os");
			RemoveGlobal(frontend, "debug");

			constexpr std::string_view PACKAGE_DECLARATIONS = R"(
declare Package: {
    seed: string,
    parameters: {[string]: boolean | number | string},
    seedStream: (string) -> string,
    asset: (string) -> string,
}

type PackageDataModel = {
    Workspace: Workspace,
    JobId: string,
    GetService: ((self: PackageDataModel, service: "Workspace") -> Workspace) &
                ((self: PackageDataModel, service: "Lighting") -> Lighting),
}
declare game: PackageDataModel
)";
			const Luau::LoadDefinitionFileResult package = frontend.loadDefinitionFile(
				frontend.globals,
				frontend.globals.globalScope,
				std::string(PACKAGE_DECLARATIONS),
				"@package",
				false
			);
			if (package.success) return true;

			error = "data-script package checker vocabulary failed";
			if (!package.parseResult.errors.empty()) {
				error += ": ";
				error += package.parseResult.errors.front().what();
			}
			return false;
		}
	}

	bool
	CheckLuauDataScriptPackageSource(std::string_view source, std::string_view entry, std::string &error) {
		error.clear();
		if (source.size() > DATA_SCRIPT_PACKAGE_MAX_SOURCE_BYTES) {
			error = "data-script package source exceeds the static-check byte limit";
			return false;
		}
		PackageFiles files;
		files.Entry = entry;
		// The frontend lets an in-source mode hotcomment win over ConfigResolver.
		// Prefixing is the local way to make strict checking an admission rule
		// without changing the exact bytes that runtime execution receives.
		files.Source = "--!strict\n";
		files.Source.append(source);
		StrictConfigs configs;
		Luau::FrontendOptions options;
		options.runLintChecks = false;
		Luau::Frontend frontend(Luau::SolverMode::New, &files, &configs, options);
		Luau::registerBuiltinGlobals(frontend, frontend.globals);
		if (!LoadPackageVocabulary(frontend, error)) return false;
		Luau::freeze(frontend.globals.globalTypes);

		const std::string module(entry);
		const Luau::CheckResult checked = frontend.check(module);
		const Luau::SourceModule *const parsed = frontend.getSourceModule(module);
		if (parsed != nullptr && !parsed->parseErrors.empty()) {
			error = "data-script package syntax error at " + At(parsed->parseErrors.front().getLocation()) +
					parsed->parseErrors.front().what();
			if (error.size() > MAX_DETAIL_BYTES) error.resize(MAX_DETAIL_BYTES);
			return false;
		}
		if (!checked.errors.empty()) {
			error = "data-script package type error at " + At(checked.errors.front().location);
			AppendBounded(
				error,
				Luau::toString(checked.errors.front(), Luau::TypeErrorToStringOptions{frontend.fileResolver})
			);
			return false;
		}
		if (!checked.timeoutHits.empty()) {
			error = "data-script package type checking exceeded its bounded analysis limit";
			return false;
		}
		return true;
	}
}
