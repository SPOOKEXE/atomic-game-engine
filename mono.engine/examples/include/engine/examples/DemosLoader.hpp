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

	enum class DemoKind {
		Script,
		World,
	};

	struct DemoEntry {
		std::string Name;
		DemoKind Kind = DemoKind::Script;
		std::filesystem::path Path;
	};

	// One view of the staged demo tree.
	class DemosLoader {
	  public:
		// Uses the staged demo root when root is empty.
		explicit DemosLoader(std::filesystem::path root = {});

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
