#pragma once

// The Luau type checker over the engine declarations, as a library.
//
// One implementation for every caller: the `scriptcheck` command line, its
// `--serve` language server, and Studio's Check button (through the control
// surface's `script_check`, which runs the staged program because Studio does
// not link `Luau.Analysis`).
//
// Requires resolve the way the engine resolves them at run time:
//   - a string (`./x`, `../x`, `@self/x`) uses Luau's require-by-string rule;
//   - `script.Parent.X` and `:FindFirstChild("X")` / `:WaitForChild("X")`
//     chains rooted at `script` follow the folder-to-instance rule
//     `MountModuleTree` and the plugin host use (Rojo's: `init.luau` is its
//     folder, `X.server.luau` is a Script named `X`).
// Anything else (a local, `game.ReplicatedStorage.X`) types as `any`.

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scriptcheck {

	// One finding. Lines and columns count from 1.
	struct Diagnostic {
		std::filesystem::path File;
		int Line = 1;
		int Column = 1;
		int EndLine = 1;
		int EndColumn = 1;

		// `TypeError`, `SyntaxError`, `UnknownRequire` or `RequireEscape`.
		std::string Code;
		std::string Message;
	};

	// One completion candidate. `Kind` is `property`, `keyword`, `string` or
	// `type`, close enough to LSP's kinds for a client to map.
	struct Completion {
		std::string Label;
		std::string Kind;
		std::string Detail;
	};

	// What `Discover` found under a path: the folders requires may not leave,
	// and the files to check.
	struct Target {
		// Each plugin folder (one holding `plugin.json`), or the project folder.
		// Empty when the caller named loose files.
		std::vector<std::filesystem::path> Roots;
		std::vector<std::filesystem::path> Files;
	};

	// A path to the files to check.
	//
	// A file is checked alone. A folder holding `plugin.json` anywhere below it
	// is one or more plugins: each plugin folder is a root and only files inside
	// a plugin are checked, so a repository's Lune tests next to the plugin are
	// left out. A folder without one is a project and is its own root. Hidden
	// folders and `node_modules` are skipped.
	Target Discover(const std::filesystem::path &path);

	// Where the engine declarations are, in order: `flag`, then
	// `$ATOMIC_LUAU_DEFINITIONS`, then `engine.d.luau` beside the running
	// program (the build stages a copy there), then
	// `mono.engine/script/bindings/engine.d.luau` in the working directory or
	// any parent of it or of the program.
	std::optional<std::filesystem::path> FindDefinitions(std::optional<std::string_view> flag);

	class Checker {
	  public:
		Checker();
		~Checker();
		Checker(const Checker &) = delete;
		Checker &operator=(const Checker &) = delete;

		// Loads declarations into the global scope and freezes it. Returns
		// false with the declaration file's own errors in `failures` when they
		// do not load, which is a generator bug rather than a script one.
		bool Load(std::string_view declarations, std::vector<Diagnostic> &failures);

		// Requires may not resolve outside these folders. A require that would
		// is reported as `RequireEscape` rather than read.
		void SetRoots(std::vector<std::filesystem::path> roots);

		// When false, a relative require naming no file types as `any` instead
		// of reporting `UnknownRequire`. For drafts checked from a scratch file,
		// where no neighbour exists to be found.
		void SetReportMissingRequires(bool report);

		// Replaces what is on disk for `file` until `Forget`, for an editor's
		// unsaved buffer. Marks it and its dependents for re-checking.
		void SetText(const std::filesystem::path &file, std::string text);
		void Forget(const std::filesystem::path &file);

		// Type-checks `file` and every module it requires, and returns the
		// diagnostics for `file` alone.
		std::vector<Diagnostic> Check(const std::filesystem::path &file);

		// The type at a position (1-based), for hover. Checks first if needed.
		std::optional<std::string> TypeAt(const std::filesystem::path &file, int line, int column);

		// Completion candidates at a position (1-based).
		std::vector<Completion> Complete(const std::filesystem::path &file, int line, int column);

	  private:
		struct State;
		std::unique_ptr<State> Inner;
	};

	// `path(line,column): Code: message`, the shape every compiler in the build
	// emits and an editor's error list parses.
	std::string FormatText(const Diagnostic &diagnostic);

	// `{"file","line","column","endLine","endColumn","code","message"}`.
	std::string FormatJson(const std::vector<Diagnostic> &diagnostics);

	// A path as it should be shown: relative to the working directory when
	// inside it, absolute otherwise.
	std::string Display(const std::filesystem::path &file);
}
