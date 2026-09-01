#pragma once

// Analysis and persistent asset choices for importing Roblox containers.
//
// The decoder lives in `engine::bake`; this layer compares its neutral tree to
// the classes and properties the editor can actually create. Keeping that
// comparison out of the ImGui panel makes the report usable by tests and future
// command-line tooling without a window.

#include <engine/bake/RobloxModel.hpp>

#include <cstddef>
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

	// Compares every decoded instance and property with the current ECS class
	// table. Callers must have registered the normal engine class tree first.
	RobloxImportAnalysis AnalyzeRobloxImport(const engine::bake::RobloxModel &model);

	// Groups repeated references by their stable Roblox id or URI and applies
	// any choices loaded from configuration.
	std::vector<RobloxAssetChoice>
	RobloxAssetChoices(const engine::bake::RobloxModel &model, const RobloxAssetMappings &mappings);

	// Builds a decoded place into one edit-mode world. Matching service roots
	// are reused, missing classes become Folders, script source is staged in the
	// world's source cache, and selected asset URIs are rewritten before values
	// cross into ECS storage.
	bool ImportRobloxPlace(
		engine::ecs::Store &store,
		const engine::bake::RobloxModel &model,
		const RobloxAssetMappings &mappings,
		RobloxImportResult &out,
		std::string &error,
		const RobloxImportOptions &options = {}
	);

	// Asset choices are editor preferences, not part of the imported world.
	// Missing configuration is a successful empty mapping.
	bool LoadRobloxAssetMappings(RobloxAssetMappings &out, std::string &error);
	bool SaveRobloxAssetMappings(const RobloxAssetMappings &mappings, std::string &error);

	const char *Describe(engine::bake::RobloxAssetKind kind);
}
