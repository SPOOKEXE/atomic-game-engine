#pragma once

// Analysis and persistent asset choices for importing Roblox containers.
//
// The decoder lives in `engine::bake`; this layer compares its neutral tree to
// the classes and properties the editor can actually create. Keeping that
// comparison out of the ImGui panel makes the report usable by tests and future
// command-line tooling without a window.

#include <engine/bake/RobloxModel.hpp>

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace studio {
	struct RobloxClassGap {
		std::string ClassName;
		size_t Instances = 0;
	};

	struct RobloxPropertyGap {
		std::string ClassName;
		std::string PropertyName;
		std::string SourceType;
		std::string ExpectedType;
		size_t Occurrences = 0;
	};

	struct RobloxImportAnalysis {
		size_t Instances = 0;
		size_t Classes = 0;
		std::vector<RobloxClassGap> MissingClasses;
		std::vector<RobloxPropertyGap> MissingProperties;
		std::vector<RobloxPropertyGap> ConflictingProperties;
	};

	struct RobloxAssetChoice {
		std::string Identifier;
		std::string SourceUri;
		engine::bake::RobloxAssetKind Kind = engine::bake::RobloxAssetKind::Unknown;
		size_t Uses = 0;
		std::string LocalAsset;
	};

	using RobloxAssetMappings = std::map<std::string, std::string, std::less<>>;
	using RobloxClassMappings = std::map<std::string, std::string, std::less<>>;

	struct RobloxImportOptions {
		// Roblox scripts are useful source material, but running them before their
		// classes and services are ported makes an imported scene fail on boot.
		bool DisableScripts = true;
	};

	struct RobloxImportResult {
		size_t Instances = 0;
		size_t ReusedRoots = 0;
		size_t Scripts = 0;
		size_t DisabledScripts = 0;
		size_t Properties = 0;
		size_t SkippedProperties = 0;
		std::vector<std::string> Notes;
	};

	struct RobloxWorldPortResult {
		RobloxImportAnalysis Analysis;
		RobloxImportResult Import;
	};

	// One recovered script's eligibility for a generated Rojo project.
	//
	// A ready subject has a source path relative to the project root. An invalid
	// subject stays visible with the exact hierarchy rule that refused it.
	struct RobloxRojoSubject {
		std::string InstancePath;
		std::string ClassName;
		std::filesystem::path SourcePath;
		bool Valid = false;
		std::string Reason;
	};

	// What creating a Rojo project wrote and what it deliberately left embedded.
	struct RobloxRojoSetupResult {
		std::filesystem::path ProjectFile;
		size_t ScriptsWritten = 0;
		std::vector<RobloxRojoSubject> Subjects;
	};

	// Compares every decoded instance and property with the current ECS class
	// table. Callers must have registered the normal engine class tree first.
	RobloxImportAnalysis AnalyzeRobloxImport(
		const engine::bake::RobloxModel &model, const RobloxClassMappings &classMappings = {}
	);

	// Groups repeated references by their stable Roblox id or URI and applies
	// any choices loaded from configuration.
	std::vector<RobloxAssetChoice>
	RobloxAssetChoices(const engine::bake::RobloxModel &model, const RobloxAssetMappings &mappings);

	// Classifies recovered scripts before any files are written. A simple subject
	// is under a service root through folders or standard script containers.
	// Complex instance trees, ambiguous names and paths Rojo would reinterpret
	// are reported as invalid instead of being flattened into a different game.
	std::vector<RobloxRojoSubject> RobloxRojoSubjects(const engine::bake::RobloxModel &model);

	// Creates a new Rojo project containing every valid recovered script. The
	// destination must not exist, so setup can never overwrite an author's work.
	// Files are staged beside it and renamed into place only after every write
	// succeeds.
	bool SetupRobloxRojoProject(
		const engine::bake::RobloxModel &model,
		const std::filesystem::path &destination,
		std::string_view projectName,
		RobloxRojoSetupResult &out,
		std::string &error
	);

	// Builds a decoded place into one edit-mode world. Matching service roots
	// are reused, missing classes use the selected engine class or a Folder
	// fallback, script source is staged in the world's source cache, and selected
	// asset URIs are rewritten before values cross into ECS storage.
	bool ImportRobloxPlace(
		engine::ecs::Store &store,
		const engine::bake::RobloxModel &model,
		const RobloxAssetMappings &assetMappings,
		const RobloxClassMappings &classMappings,
		RobloxImportResult &out,
		std::string &error,
		const RobloxImportOptions &options = {}
	);

	// Converts one Roblox place through the same analysis and import path as
	// the widget, then writes a standalone world document.
	bool PortRobloxPlace(
		const std::filesystem::path &source,
		const std::filesystem::path &destination,
		const RobloxAssetMappings &assetMappings,
		const RobloxClassMappings &classMappings,
		RobloxWorldPortResult &out,
		std::string &error,
		const RobloxImportOptions &options = {}
	);

	// Asset choices are editor preferences, not part of the imported world.
	// Missing configuration is a successful empty mapping.
	bool LoadRobloxAssetMappings(RobloxAssetMappings &out, std::string &error);
	bool SaveRobloxAssetMappings(const RobloxAssetMappings &mappings, std::string &error);
	bool LoadRobloxClassMappings(RobloxClassMappings &out, std::string &error);
	bool SaveRobloxClassMappings(const RobloxClassMappings &mappings, std::string &error);

	const char *Describe(engine::bake::RobloxAssetKind kind);
}
