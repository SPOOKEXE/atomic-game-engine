// Type-checks Luau scripts against the generated declarations.
//
// **The other half of `just bindings-check`.** That one asks whether the
// declaration files still match the class table. This asks whether the scripts
// people actually write still match the declaration files — and the two
// questions are different: a property can be removed from the class table,
// regenerate cleanly, and leave every script that named it broken with nothing
// reporting it until the scene fails to build.
//
// **Upstream's `luau-analyze` cannot do this**, which is why there is a tool
// here rather than a line in the Justfile. It has no flag for loading a
// definition file, so it checks against the built-in globals alone — and every
// `Instance`, `Vector3` and `workspace` in this engine would come back as an
// unknown global. What a language server does instead is `loadDefinitionFile`
// into the global scope before checking, which is what this does, so the answer
// here and the squiggles in an editor come from one code path.
//
// Strict mode regardless of what a file asks for. `--!nonstrict` at the top of a
// script would otherwise turn this check off from inside the thing being
// checked.

#include <engine/core/Arguments.hpp>
#include <engine/core/Log.hpp>

#include <Luau/BuiltinDefinitions.h>
#include <Luau/Config.h>
#include <Luau/Frontend.h>
#include <Luau/ToString.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

	constexpr const char *DEFAULT_DEFINITIONS = "mono.engine/script/bindings/engine.d.luau";

	std::optional<std::string> Read(const std::filesystem::path &path) {
		std::ifstream file(path, std::ios::binary);
		if (!file) {
			return std::nullopt;
		}
		std::ostringstream contents;
		contents << file.rdbuf();
		return contents.str();
	}

	// Reads a module's source off disk. A module name is a path here, because
	// nothing being checked has a `require`.
	struct Files : Luau::FileResolver {
		std::optional<Luau::SourceCode> readSource(const Luau::ModuleName &name) override {
			std::optional<std::string> source = Read(name);
			if (!source) {
				return std::nullopt;
			}
			return Luau::SourceCode{*source, Luau::SourceCode::Module};
		}
	};

	// **Strict for every file, and it ignores `.luaurc`.** The checked-in
	// `.luaurc` already says strict, but a config resolver that read it would
	// make this check answer differently depending on which directory a script
	// was moved to — and a per-directory answer is not what a repository-wide
	// gate should be.
	struct Configs : Luau::ConfigResolver {
		Luau::Config Strict;

		Configs() {
			Strict.mode = Luau::Mode::Strict;
		}

		const Luau::Config &
		getConfig(const Luau::ModuleName &, const Luau::TypeCheckLimits &) const override {
			return Strict;
		}
	};

	void Report(const std::string &path, const Luau::Location &at, const std::string &message) {
		// `path(line,column): message`, which is what every compiler in this
		// build already emits and what an editor's error list can parse. Luau
		// counts from zero and humans do not.
		std::printf("%s(%d,%d): %s\n", path.c_str(), at.begin.line + 1, at.begin.column + 1, message.c_str());
	}
}

int main(int argc, char **argv) {
	engine::core::Log::Initialise("scriptcheck");

	engine::core::Arguments arguments(
		"scriptcheck", "atomic — type-checks Luau scripts against the generated declarations."
	);
	arguments.Value("definitions", "PATH", "The declaration file to check against");

	const engine::core::Arguments::Result parsed = arguments.Parse(argc, argv);
	if (!parsed.Ok || parsed.HelpRequested) {
		return parsed.Ok ? 0 : 2;
	}

	const std::vector<std::string_view> &scripts = arguments.Positional();
	if (scripts.empty()) {
		ENGINE_ERROR("nothing to check: pass one or more .luau files");
		return 2;
	}

	const std::string definitions = std::string(arguments.Get("definitions").value_or(DEFAULT_DEFINITIONS));

	const std::optional<std::string> source = Read(definitions);
	if (!source) {
		ENGINE_ERROR("could not read {} — run `just bindings`", definitions);
		return 2;
	}

	Files files;
	Configs configs;

	// The lints are luau-analyze's business rather than this tool's. What is
	// being gated here is whether a script still agrees with the bindings, and
	// a shadowed local failing that gate would make the two arguments about a
	// script's *style* and its *correctness* into one argument.
	Luau::FrontendOptions options;
	options.runLintChecks = false;

	Luau::Frontend frontend(Luau::SolverMode::New, &files, &configs, options);
	Luau::registerBuiltinGlobals(frontend, frontend.globals);

	// **Into the global scope, which is what makes this different from running
	// the file as a module.** `declare` is only meaningful here; a script that
	// `require`d these declarations would be reading a table, not gaining a
	// vocabulary.
	const Luau::LoadDefinitionFileResult loaded = frontend.loadDefinitionFile(
		frontend.globals, frontend.globals.globalScope, *source, "@engine", false
	);

	int failures = 0;

	if (!loaded.success) {
		// The declaration file itself is wrong, which is a generator bug rather
		// than a script one — so it is named as such and nothing else is
		// checked. Every script would fail against a vocabulary that did not
		// load, and a thousand consequential errors bury the one cause.
		for (const Luau::ParseError &error : loaded.parseResult.errors) {
			Report(definitions, error.getLocation(), std::string("SyntaxError: ") + error.what());
			failures++;
		}
		if (loaded.module != nullptr) {
			for (const Luau::TypeError &error : loaded.module->errors) {
				Report(definitions, error.location, Luau::toString(error));
				failures++;
			}
		}

		ENGINE_ERROR(
			"{} did not load. It is generated — the fault is in mono.tools/bindings rather than in "
			"the file.",
			definitions
		);
		return 1;
	}

	// Frozen so a script cannot add to the vocabulary it is being checked
	// against, which is also what the sandbox does at run time.
	Luau::freeze(frontend.globals.globalTypes);

	for (const std::string_view script : scripts) {
		const std::string path(script);

		if (!std::filesystem::exists(path)) {
			ENGINE_ERROR("no such script: {}", path);
			failures++;
			continue;
		}

		const Luau::CheckResult result = frontend.check(path);
		for (const Luau::TypeError &error : result.errors) {
			Report(
				path,
				error.location,
				Luau::toString(error, Luau::TypeErrorToStringOptions{frontend.fileResolver})
			);
			failures++;
		}
	}

	if (failures > 0) {
		ENGINE_ERROR(
			"{} error(s). A script and the bindings disagree — either the script names something "
			"the engine no longer has, or the class table changed and `just bindings` has not run.",
			failures
		);
		return 1;
	}

	ENGINE_INFO("scriptcheck ok — {} script(s) agree with {}", scripts.size(), definitions);
	return 0;
}
