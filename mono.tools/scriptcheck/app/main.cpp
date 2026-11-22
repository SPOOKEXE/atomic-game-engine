// Type-checks Luau against the engine declarations, from the command line or
// as a language server. The checking itself is `scriptcheck::Checker`.
//
// Upstream's `luau-analyze` cannot do this: it has no flag for loading a
// definition file, so every `Instance` and `workspace` would be an unknown
// global. This loads the declarations into the global scope first, which is
// what a language server does too.

#include <engine/core/Arguments.hpp>
#include <engine/core/Log.hpp>

#include <cstdio>
#include <format>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <scriptcheck/Check.hpp>
#include <scriptcheck/Server.hpp>
#include <sstream>

namespace {

	namespace fs = std::filesystem;

	std::optional<std::string> Read(const fs::path &path) {
		std::ifstream file(path, std::ios::binary);
		if (!file) {
			return std::nullopt;
		}
		std::ostringstream contents;
		contents << file.rdbuf();
		return contents.str();
	}

	// Status goes to stderr: stdout carries diagnostics, JSON or the protocol,
	// and the engine log writes to stdout.
	template <typename... Values> void Say(std::format_string<Values...> format, Values &&...values) {
		std::fprintf(
			stderr, "scriptcheck: %s\n", std::format(format, std::forward<Values>(values)...).c_str()
		);
	}

	// Writes `<dir>/.atomic/engine.d.luau` and a luau-lsp settings file
	// pointing at it, so an editor outside this repository sees the same
	// vocabulary. The folder's own `.vscode` and `.luaurc` are left alone.
	int EmitLspConfig(const fs::path &folder, const std::string &declarations) {
		std::error_code error;
		const fs::path atomic = folder / ".atomic";
		fs::create_directories(atomic, error);
		std::ofstream(atomic / "engine.d.luau", std::ios::binary | std::ios::trunc) << declarations;

		const nlohmann::json settings = {
			{"luau-lsp.types.definitionFiles", {".atomic/engine.d.luau"}},
			{"luau-lsp.fflags.enableNewSolver", true},
			{"luau-lsp.platform.type", "standard"},
			{"luau-lsp.sourcemap.enabled", false},
			{"luau-lsp.require.useOriginalRequireByStringSemantics", false},
		};
		std::ofstream(atomic / "luau-lsp.json", std::ios::trunc) << settings.dump(1, '\t') << "\n";
		if (!fs::is_regular_file(atomic / "luau-lsp.json", error)) {
			Say("could not write {}", (atomic / "luau-lsp.json").string());
			return 2;
		}

		std::printf(
			"wrote %s and %s\n"
			"luau-lsp: luau-lsp analyze --settings=.atomic/luau-lsp.json <files>\n"
			"VS Code: copy the keys of .atomic/luau-lsp.json into .vscode/settings.json\n",
			scriptcheck::Display(atomic / "engine.d.luau").c_str(),
			scriptcheck::Display(atomic / "luau-lsp.json").c_str()
		);
		return 0;
	}
}

int main(int argc, char **argv) {
	engine::core::Log::Initialise("scriptcheck");
	engine::core::Arguments arguments(
		"scriptcheck",
		"atomic - type-checks Luau against the engine declarations. Pass files, a plugin folder "
		"(one holding plugin.json) or a project folder."
	);
	arguments.Value(
		"definitions",
		"PATH",
		"engine.d.luau to check against (else $ATOMIC_LUAU_DEFINITIONS, "
		"beside this program, or the checkout)"
	);
	arguments.Value("format", "FORMAT", "text (default) or json");
	arguments.Flag("serve", "Run as a language server on stdin/stdout");
	arguments.Value("emit-lsp-config", "DIR", "Write .atomic/engine.d.luau and a luau-lsp config into DIR");
	arguments.Flag(
		"allow-missing-requires", "Type an unresolved relative require as any instead of an error"
	);

	const engine::core::Arguments::Result parsed = arguments.Parse(argc, argv);
	if (parsed.Ok && parsed.VersionRequested) {
		std::printf("%s", arguments.VersionLine().c_str());
		return 0;
	}
	if (!parsed.Ok || parsed.HelpRequested) {
		return parsed.Ok ? 0 : 2;
	}
	if (parsed.DescribeRequested) {
		std::fputs(arguments.Describe().c_str(), stdout);
		return 0;
	}

	const bool serve = arguments.Has("serve");
	const bool json = arguments.Get("format").value_or("text") == "json";

	const std::optional<fs::path> definitions = scriptcheck::FindDefinitions(arguments.Get("definitions"));
	const std::optional<std::string> declarations = definitions ? Read(*definitions) : std::nullopt;
	if (!declarations) {
		Say("no engine declarations{}{}. Pass --definitions, set ATOMIC_LUAU_DEFINITIONS, or run `just "
			"bindings` in the checkout.",
			definitions ? " at " : "",
			definitions ? definitions->string() : std::string());
		return 2;
	}

	if (const auto folder = arguments.Get("emit-lsp-config")) {
		return EmitLspConfig(fs::path(*folder), *declarations);
	}

	scriptcheck::Checker checker;
	std::vector<scriptcheck::Diagnostic> broken;
	if (!checker.Load(*declarations, broken)) {
		for (const scriptcheck::Diagnostic &diagnostic : broken) {
			std::fprintf(stderr, "%s: %s\n", definitions->string().c_str(), diagnostic.Message.c_str());
		}
		Say("{} did not load. It is generated: the fault is in mono.tools/bindings.", definitions->string());
		return 1;
	}
	checker.SetReportMissingRequires(!arguments.Has("allow-missing-requires"));

	if (serve) {
		std::ios::sync_with_stdio(false);
		scriptcheck::Server server(checker);
		return server.Run(std::cin, std::cout);
	}

	if (arguments.Positional().empty()) {
		Say("nothing to check: pass files or a folder");
		return 2;
	}

	std::vector<fs::path> roots;
	std::vector<fs::path> files;
	bool missing = false;
	for (const std::string_view argument : arguments.Positional()) {
		const fs::path path(argument);
		std::error_code error;
		if (!fs::exists(path, error)) {
			Say("no such file or folder: {}", path.string());
			missing = true;
			continue;
		}
		scriptcheck::Target target = scriptcheck::Discover(path);
		roots.insert(roots.end(), target.Roots.begin(), target.Roots.end());
		files.insert(files.end(), target.Files.begin(), target.Files.end());
	}
	checker.SetRoots(roots);

	std::vector<scriptcheck::Diagnostic> all;
	for (const fs::path &file : files) {
		std::vector<scriptcheck::Diagnostic> found = checker.Check(file);
		all.insert(all.end(), found.begin(), found.end());
	}
	scriptcheck::Sort(all);

	if (json) {
		std::printf("%s\n", scriptcheck::FormatJson(all).c_str());
	} else {
		for (const scriptcheck::Diagnostic &diagnostic : all) {
			std::printf("%s\n", scriptcheck::FormatText(diagnostic).c_str());
		}
	}

	if (!all.empty() || missing) {
		Say("{} error(s) in {} file(s) checked against {}", all.size(), files.size(), definitions->string());
		return 1;
	}
	Say("ok - {} file(s) agree with {}", files.size(), definitions->string());
	return 0;
}
