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
	// Missing Roblox class and the number of instances using it.
	struct RobloxClassGap {
		// Roblox class absent from the engine class table.
		std::string ClassName;
		// Number of decoded instances with this class.
		size_t Instances = 0;
	};

	// Missing or conflicting Roblox property grouped by source type.
	struct RobloxPropertyGap {
		// Roblox class that declares the property.
		std::string ClassName;
		// Source property name.
		std::string PropertyName;
		// Decoded Roblox property type.
		std::string SourceType;
		// Engine property type that conflicts with the source type.
		std::string ExpectedType;
		// Number of instances containing this property.
		size_t Occurrences = 0;
	};

	// A source property that reached the importer but could not be represented
	// by the selected engine class. Entries are sorted by their full source key.
	struct RobloxPropertySkip {
		// Roblox class containing the skipped property.
		std::string ClassName;
		// Skipped Roblox property name.
		std::string PropertyName;
		// Rule that prevented representing the source property.
		std::string Reason;
		// Number of skipped occurrences with this key.
		size_t Occurrences = 0;
	};

	// A source class that the importer represented with a different engine
	// class. The note names behaviour or geometry that the replacement cannot
	// reproduce.
	struct RobloxClassSubstitution {
		// Original Roblox class name.
		std::string SourceClass;
		// Engine class used as its replacement.
		std::string TargetClass;
		// Number of instances using the replacement.
		size_t Instances = 0;
		// Behaviour or geometry the replacement cannot reproduce.
		std::string Note;
	};

	// Summary of class and property compatibility before import.
	struct RobloxImportAnalysis {
		// Total decoded instance count.
		size_t Instances = 0;
		// Number of distinct decoded Roblox classes.
		size_t Classes = 0;
		// Unsupported classes grouped by source class name.
		std::vector<RobloxClassGap> MissingClasses;
		// Source properties absent from selected engine classes.
		std::vector<RobloxPropertyGap> MissingProperties;
		// Source properties whose types conflict with engine properties.
		std::vector<RobloxPropertyGap> ConflictingProperties;
		// Source classes represented by fallback engine classes.
		std::vector<RobloxClassSubstitution> Substitutions;
	};

	// One external Roblox asset and its author-selected local replacement.
	struct RobloxAssetChoice {
		// Stable Roblox asset id or URI used as the mapping key.
		std::string Identifier;
		// Original asset URI from the decoded place.
		std::string SourceUri;
		// Asset kind inferred from the decoded reference.
		engine::bake::RobloxAssetKind Kind = engine::bake::RobloxAssetKind::Unknown;
		// Number of references to this asset in the source place.
		size_t Uses = 0;
		// Local asset path selected by the author, if any.
		std::string LocalAsset;
	};

	// Maps stable Roblox asset identifiers to local asset paths.
	using RobloxAssetMappings = std::map<std::string, std::string, std::less<>>;
	// Maps Roblox class names to selected engine class names.
	using RobloxClassMappings = std::map<std::string, std::string, std::less<>>;

	// Options that control how decoded Roblox content enters an edit world.
	struct RobloxImportOptions {
		// Roblox scripts are useful source material, but running them before their
		// classes and services are ported makes an imported scene fail on boot.
		bool DisableScripts = true;
	};

	// Counts and diagnostics produced while building an edit world.
	struct RobloxImportResult {
		// Number of source instances created in the destination world.
		size_t Instances = 0;
		// Number of existing service roots reused by the import.
		size_t ReusedRoots = 0;
		// Number of recovered source scripts.
		size_t Scripts = 0;
		// Number of recovered scripts disabled by import options.
		size_t DisabledScripts = 0;
		// Number of source properties written to destination instances.
		size_t Properties = 0;
		// Unsupported classes represented by Folder instances.
		std::vector<RobloxClassGap> FolderFallbackClasses;
		// Source properties refused by the selected engine classes.
		std::vector<RobloxPropertySkip> SkippedProperties;
		// Source classes imported through alternative engine classes.
		std::vector<RobloxClassSubstitution> Substitutions;
		// Human-readable import notes.
		std::vector<std::string> Notes;
	};

	// Analysis and import output from porting a place into a world document.
	struct RobloxWorldPortResult {
		// Compatibility analysis completed before writing the destination.
		RobloxImportAnalysis Analysis;
		// Result of creating the destination edit world.
		RobloxImportResult Import;
	};

	// One recovered script's eligibility for a generated Rojo project.
	//
	// A ready subject has a source path relative to the project root. An invalid
	// subject stays visible with the exact hierarchy rule that refused it.
	struct RobloxRojoSubject {
		// Source instance path used as the Rojo subject identifier.
		std::string InstancePath;
		// Roblox class name used to select the generated Rojo rule.
		std::string ClassName;
		// Path relative to the generated Rojo project.
		std::filesystem::path SourcePath;
		// Whether this record passed its validation rules.
		bool Valid = false;
		// Reason this operation produced its reported state.
		std::string Reason;
	};

	// What creating a Rojo project wrote and what it deliberately left embedded.
	struct RobloxRojoSetupResult {
		// Written Rojo project manifest path.
		std::filesystem::path ProjectFile;
		// Number of eligible scripts written beside the project file.
		size_t ScriptsWritten = 0;
		// Every recovered script and its Rojo eligibility result.
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
	// asset URIs are rewritten before values cross into ECS storage. The result
	// groups every fallback class and skipped property by its source spelling.
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
	// Persists author-selected Roblox asset replacements to the studio mapping file.
	bool SaveRobloxAssetMappings(const RobloxAssetMappings &mappings, std::string &error);
	// Loads persisted Roblox-to-engine class replacement choices.
	bool LoadRobloxClassMappings(RobloxClassMappings &out, std::string &error);
	// Persists Roblox-to-engine class replacement choices for later imports.
	bool SaveRobloxClassMappings(const RobloxClassMappings &mappings, std::string &error);

	// Returns the stable display name for a decoded Roblox asset kind.
	const char *Describe(engine::bake::RobloxAssetKind kind);
}
