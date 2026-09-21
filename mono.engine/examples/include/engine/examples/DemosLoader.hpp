#pragma once

// Discovers the demo assets shipped with the engine.
//
// Script demos and authored worlds have different load paths. Keeping their
// directories and classification here stops each program from rebuilding the
// same filesystem rules.
//
// @tier L12 · shared

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::examples {

	// Distinguishes source-script demos from authored-world demos.
	enum class DemoKind {
		Script,
		World,
	};

	// One discovered runnable demo and its load path.
	struct DemoEntry {
		// User-visible demo name derived from its staged file.
		std::string Name;
		// Load path selected for this demo.
		DemoKind Kind = DemoKind::Script;
		// Staged file to load when this demo is selected.
		std::filesystem::path Path;
	};

	// One view of the staged demo tree.
	class DemosLoader {
	  public:
		// Uses the staged demo root when root is empty.
		explicit DemosLoader(std::filesystem::path root = {});

		// Returns the staged root searched for script and world demos.
		const std::filesystem::path &Root() const;

		// The directory that owns one kind of demo.
		std::filesystem::path Directory(DemoKind kind) const;

		// Resolves a runnable file without allowing it to leave its directory.
		// Scripts may be `.luau` or `.js`; worlds must be `.aworld`. Returns an
		// empty path for an invalid, unsupported or missing entry.
		std::filesystem::path Resolve(DemoKind kind, std::string_view name) const;

		// Lists one kind, sorted by file name. Script listings contain canonical
		// `.luau` demos only, so generated JavaScript twins do not duplicate menu
		// entries. World listings contain `.aworld` demos.
		std::vector<DemoEntry> List(DemoKind kind) const;

		// Lists scripts followed by authored worlds, sorted within each kind.
		std::vector<DemoEntry> List() const;

		// Finds a canonical listed demo by its file name. This uses the same
		// `.luau` and `.aworld` rules as `List`, rather than every runnable format
		// accepted by `Resolve`.
		std::optional<DemoEntry> Find(DemoKind kind, std::string_view name) const;

		// Resolves the demo root from the current assets configuration.
		static std::filesystem::path DefaultRoot();

	  private:
		std::filesystem::path Root_;
	};
}
