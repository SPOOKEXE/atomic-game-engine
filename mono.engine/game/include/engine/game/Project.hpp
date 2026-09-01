#pragma once

// Portable project classification and package files.
//
// A package is a deployment envelope around the authored `.auniverse` format,
// not another game format. The package reader validates and extracts one
// universe plus its processed asset store; `LoadGame` remains the one reader of
// the universe and world documents inside it.
//
// @tier L12 · shared

#include <engine/game/Game.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::game {

	// The product an author chose to export.
	//
	// @since v0.21
	enum class ExportProduct : uint8_t {
		// One active authored world.
		WorldFile,

		// A manifest with editable sidecar worlds and assets.
		UniverseFolder,

		// One server-loadable archive with a complete processed store.
		ProjectZip,
	};

	// Returns the enforced extension for an export product.
	const char *ExtensionOf(ExportProduct product);

	// The kind of project path handed to a runtime.
	//
	// @since v0.21
	enum class ProjectKind : uint8_t {
		// No supported extension.
		Unknown,

		// A scene script handled by the existing server path.
		SceneScript,

		// A monolithic `.agame` document.
		GameFile,

		// An editable `.auniverse` manifest.
		UniverseFolder,

		// A portable `.zip` package.
		ProjectZip,
	};

	// Classifies one path without opening it.
	ProjectKind ClassifyProject(const std::filesystem::path &path);

	// Returns a stable name for a project kind.
	const char *Describe(ProjectKind kind);

	// The public delivery preference recorded by a package.
	//
	// A server policy may override or refuse this hint.
	//
	// @since v0.21
	enum class ProjectDeliveryPreference : uint8_t {
		// The server proxies requested content.
		Relay,

		// The server gives clients approved public endpoints.
		Redirect,
	};

	// Returns a stable package spelling for a delivery preference.
	const char *Describe(ProjectDeliveryPreference preference);

	// Parses a package delivery preference.
	std::optional<ProjectDeliveryPreference> ProjectDeliveryPreferenceOf(std::string_view text);

	// How a project validation finding affects the operation.
	//
	// @since v0.21
	enum class ProjectFindingSeverity : uint8_t {
		// The project cannot be safely used.
		Error,

		// The project is usable, but the operator should review the finding.
		Warning,

		// A requested online check was deliberately not run.
		Skipped,
	};

	// One stable, structured project validation finding.
	//
	// @since v0.21
	struct ProjectValidationFinding {
		// Stable machine-readable code.
		std::string Code;

		// Whether this blocks use.
		ProjectFindingSeverity Severity = ProjectFindingSeverity::Error;

		// Subsystem or source row that produced the finding.
		std::string Source;

		// Portable package path or asset name, when one applies.
		std::string Path;

		// Human-readable explanation.
		std::string Explanation;
	};

	// A complete validation result shared by Studio and server loading.
	//
	// @since v0.21
	struct ProjectValidationReport {
		// Findings in discovery order.
		std::vector<ProjectValidationFinding> Findings;

		// Whether no error-severity finding exists.
		bool Passed() const;

		// Adds one finding.
		void
		Add(std::string code,
			ProjectFindingSeverity severity,
			std::string source,
			std::string path,
			std::string explanation);

		// Appends another report without changing its order.
		void Append(ProjectValidationReport other);
	};

	// Hostile-input ceilings for opening one project package.
	//
	// @since v0.21
	struct ProjectPackageLimits {
		// Maximum central-directory entries, including directories.
		uint32_t MaximumEntries = 100000;

		// Maximum portable entry-name length in bytes.
		size_t MaximumPathBytes = 240;

		// Maximum slash-separated nesting depth.
		uint32_t MaximumNesting = 32;

		// Maximum uncompressed bytes in one entry. Eight GiB.
		uint64_t MaximumFileBytes = 8ull * 1024ull * 1024ull * 1024ull;

		// Maximum uncompressed bytes across the archive. Sixty-four GiB.
		uint64_t MaximumTotalBytes = 64ull * 1024ull * 1024ull * 1024ull;

		// Largest accepted uncompressed-to-compressed ratio.
		uint32_t MaximumCompressionRatio = 200;

		// Maximum `project.xml` bytes.
		size_t MaximumProjectDocumentBytes = 64u * 1024u;
	};

	// Public settings written into `project.xml`.
	//
	// No private or operator-only setting has a field here, which keeps secrets
	// out by construction rather than by a blacklist at serialization time.
	//
	// @since v0.21
	struct ProjectPackageOptions {
		// Public publisher key that verifies the embedded catalogue.
		std::string PublisherKey;

		// Suggested delivery mode. A server remains authoritative.
		ProjectDeliveryPreference Delivery = ProjectDeliveryPreference::Relay;

		// Public HTTP origins suggested to a server.
		std::vector<UniverseCdn> Cdns;

		// Build profile that created the archive, such as `studio`.
		std::string CreationProfile = "studio";

		// Oldest engine version allowed to open it. Empty uses this build.
		std::string MinimumEngine;

		// Newest engine version allowed to open it. Empty uses this build.
		std::string MaximumEngine;

		// Whether replacing an existing destination is explicit.
		bool ReplaceExisting = false;
	};

	// Public metadata inspected from `project.xml`.
	//
	// Payload counts and the digest cover every regular entry except
	// `project.xml`, avoiding a document that would have to hash itself.
	//
	// @since v0.21
	struct ProjectPackageInfo {
		// Package format version.
		uint32_t FormatVersion = 0;

		// Relative `.auniverse` entrypoint.
		std::filesystem::path UniverseEntrypoint;

		// Oldest compatible engine version.
		std::string MinimumEngine;

		// Newest compatible engine version.
		std::string MaximumEngine;

		// Profile that created the package.
		std::string CreationProfile;

		// Public publisher key for the embedded content store.
		std::string PublisherKey;

		// Public delivery preference.
		ProjectDeliveryPreference Delivery = ProjectDeliveryPreference::Relay;

		// Public HTTP origin hints in declared order.
		std::vector<UniverseCdn> Cdns;

		// Number of regular payload files.
		uint64_t FileCount = 0;

		// Total uncompressed payload bytes.
		uint64_t UncompressedBytes = 0;

		// BLAKE3 digest over ordered payload paths, sizes, and bytes.
		std::string ContentDigest;
	};

	// An opened project and the lifetime of any temporary extraction.
	//
	// Move-only so a server cannot accidentally destroy storage still named by
	// a loaded universe.
	//
	// @since v0.21
	class OpenedProject {
	  public:
		// Removes a temporary extraction.
		~OpenedProject();

		OpenedProject(const OpenedProject &) = delete;
		OpenedProject &operator=(const OpenedProject &) = delete;

		// Moves ownership of the extraction lifetime.
		OpenedProject(OpenedProject &&other) noexcept;

		// Replaces this project and cleans up its former extraction.
		OpenedProject &operator=(OpenedProject &&other) noexcept;

		// The `.agame` or `.auniverse` to load through `LoadGame`.
		const std::filesystem::path &Entrypoint() const;

		// The local processed store, or empty when none is declared.
		const std::filesystem::path &Assets() const;

		// Package metadata. FormatVersion is zero for a non-package project.
		const ProjectPackageInfo &Package() const;

		// Whether this object owns a temporary extraction.
		bool Temporary() const;

	  private:
		friend std::optional<OpenedProject>
		OpenProject(const std::filesystem::path &, const ProjectPackageLimits &, ProjectValidationReport &);

		OpenedProject() = default;
		void Cleanup();

		std::filesystem::path RootPath;
		std::filesystem::path EntrypointPath;
		std::filesystem::path AssetsPath;
		ProjectPackageInfo PackageMetadata;
		bool OwnsRoot = false;
	};

	// Verifies a processed store's signed catalogue and every catalogue asset.
	ProjectValidationReport
	ValidateProcessedAssetStore(const std::filesystem::path &directory, std::string_view publisherKey);

	// Writes, reopens, and atomically publishes a deterministic Project ZIP.
	//
	// The staging root may contain only `game.auniverse`, `game.worlds/`,
	// `assets/`, and optional `authoring/` payloads.
	bool WriteProjectPackage(
		const std::filesystem::path &stagingRoot,
		const std::filesystem::path &destination,
		const ProjectPackageOptions &options,
		ProjectPackageInfo &written,
		ProjectValidationReport &report
	);

	// Opens `.agame`, `.auniverse`, or a validated and extracted Project ZIP.
	std::optional<OpenedProject> OpenProject(
		const std::filesystem::path &path, const ProjectPackageLimits &limits, ProjectValidationReport &report
	);
}
