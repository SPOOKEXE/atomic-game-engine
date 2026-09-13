#pragma once

// The value form of a repository data-script package.
//
// A package is an untrusted manifest plus separately supplied source and asset
// bytes. The manifest names every byte by its BLAKE3 address and carries only
// scalar parameters, so a transport can decode it without acquiring a VM or a
// filesystem handle.
//
// @tier L9 · shared

#include <engine/assets/ContentHash.hpp>
#include <engine/script/Runtime.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::script {

	inline constexpr size_t DATA_SCRIPT_PACKAGE_MAX_MANIFEST_BYTES = 64u * 1024u;
	inline constexpr size_t DATA_SCRIPT_PACKAGE_MAX_SOURCE_BYTES = 256u * 1024u;
	inline constexpr size_t DATA_SCRIPT_PACKAGE_MAX_ASSET_BYTES = 16u * 1024u * 1024u;
	inline constexpr size_t DATA_SCRIPT_PACKAGE_MAX_ASSETS = 128;
	inline constexpr size_t DATA_SCRIPT_PACKAGE_MAX_PARAMETERS = 64;

	// A parameter is deliberately scalar. Tables would need a second recursive
	// wire format beside the script codec, while a scalar stays unambiguous in
	// JSON, Python, and both VMs.
	struct DataScriptScalar {
		// Package integers, seeds, and seed streams cross both VMs as canonical
		// decimal strings. Luau and JavaScript cannot both represent int64 and
		// uint64 exactly as numeric values.
		enum class Kind : uint8_t { Boolean, Integer, Number, String };

		Kind Type = Kind::String;
		bool Boolean = false;
		int64_t Integer = 0;
		double Number = 0.0;
		std::string String;
	};

	struct DataScriptParameter {
		std::string Name;
		DataScriptScalar Value;
	};

	struct DataScriptPackageAsset {
		std::string Path;
		assets::ContentHash Hash;
	};

	// The package's declared ceilings. Runtime memory, instruction, and job
	// budgets remain Runtime-owned, where both VM adapters already enforce them.
	struct DataScriptPackageBudget {
		uint64_t SourceBytes = DATA_SCRIPT_PACKAGE_MAX_SOURCE_BYTES;
		uint64_t AssetBytes = DATA_SCRIPT_PACKAGE_MAX_ASSET_BYTES;
		uint64_t Assets = DATA_SCRIPT_PACKAGE_MAX_ASSETS;
		uint64_t Parameters = DATA_SCRIPT_PACKAGE_MAX_PARAMETERS;
	};

	struct DataScriptPackage {
		static constexpr std::string_view FORMAT = "atomic.data-script.v1";

		std::string Entry;
		assets::ContentHash SourceHash;
		std::vector<DataScriptPackageAsset> Assets;
		std::vector<DataScriptParameter> Parameters;
		ScriptCapabilities Capabilities = ScriptCapabilities::None;
		DataScriptPackageBudget Budget;
		uint64_t Seed = 0;
	};

	struct DataScriptPackageParseResult {
		std::optional<DataScriptPackage> Package;
		std::string Error;

		explicit operator bool() const {
			return Package.has_value();
		}
	};

	// Parses exactly version one of the data-script package manifest.
	//
	// Unknown and duplicate fields are refused so a newer producer cannot claim
	// semantics this reader quietly ignores.
	DataScriptPackageParseResult ParseDataScriptPackage(std::string_view manifest);

	// Returns one deterministic uint64 stream from a package seed and stable
	// stream name. It hashes an explicit byte order, so the result is independent
	// of the host's endianness and of process-local random state.
	uint64_t DataScriptSeedStream(uint64_t seed, std::string_view stream);
}
