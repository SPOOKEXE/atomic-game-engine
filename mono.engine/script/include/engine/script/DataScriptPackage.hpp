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

	// Largest accepted UTF-8 manifest, in bytes.
	inline constexpr size_t DATA_SCRIPT_PACKAGE_MAX_MANIFEST_BYTES = 64u * 1024u;
	// Largest accepted package source payload, in bytes.
	inline constexpr size_t DATA_SCRIPT_PACKAGE_MAX_SOURCE_BYTES = 256u * 1024u;
	// Largest accepted individual asset payload, in bytes.
	inline constexpr size_t DATA_SCRIPT_PACKAGE_MAX_ASSET_BYTES = 16u * 1024u * 1024u;
	// Maximum digest-addressed asset declarations in one package.
	inline constexpr size_t DATA_SCRIPT_PACKAGE_MAX_ASSETS = 128;
	// Maximum scalar parameter declarations in one package.
	inline constexpr size_t DATA_SCRIPT_PACKAGE_MAX_PARAMETERS = 64;

	// A parameter is deliberately scalar. Tables would need a second recursive
	// wire format beside the script codec, while a scalar stays unambiguous in
	// JSON, Python, and both VMs.
	struct DataScriptScalar {
		// Package integers, seeds, and seed streams cross both VMs as canonical
		// decimal strings. Luau and JavaScript cannot both represent int64 and
		// uint64 exactly as numeric values.
		enum class Kind : uint8_t { Boolean, Integer, Number, String };

		// Active payload alternative serialized in this scalar.
		Kind Type = Kind::String;
		// Boolean payload when Type is Boolean.
		bool Boolean = false;
		// Signed integer payload when Type is Integer.
		int64_t Integer = 0;
		// Floating-point payload when Type is Number.
		double Number = 0.0;
		// UTF-8 payload when Type is String.
		std::string String;
	};

	// A data-script execution parameter.
	struct DataScriptParameter {
		// Script-visible name.
		std::string Name;
		// VM-neutral serialized value.
		DataScriptScalar Value;
	};

	// A data-script package asset.
	struct DataScriptPackageAsset {
		// Manifest-relative path used to request this digest-addressed asset.
		std::string Path;
		// Content hash of the associated payload.
		assets::ContentHash Hash;
	};

	// The package's declared ceilings. Runtime memory, instruction, and job
	// budgets remain Runtime-owned, where both VM adapters already enforce them.
	struct DataScriptPackageBudget {
		// Maximum source payload bytes permitted by this package.
		uint64_t SourceBytes = DATA_SCRIPT_PACKAGE_MAX_SOURCE_BYTES;
		// Maximum combined asset payload bytes permitted by this package.
		uint64_t AssetBytes = DATA_SCRIPT_PACKAGE_MAX_ASSET_BYTES;
		// Digest-addressed package assets.
		uint64_t Assets = DATA_SCRIPT_PACKAGE_MAX_ASSETS;
		// Maximum scalar parameter declarations permitted by this package.
		uint64_t Parameters = DATA_SCRIPT_PACKAGE_MAX_PARAMETERS;
	};

	// Verified manifest data needed to execute one data-script package.
	struct DataScriptPackage {
		// Required manifest format identifier for this package version.
		static constexpr std::string_view FORMAT = "atomic.data-script.v1";

		// Script entry point passed to the VM runner.
		std::string Entry;
		// BLAKE3 digest required for the separately supplied source bytes.
		assets::ContentHash SourceHash;
		// Digest-addressed package assets.
		std::vector<DataScriptPackageAsset> Assets;
		// Script-visible scalar parameters admitted by the manifest.
		std::vector<DataScriptParameter> Parameters;
		// Granted script capabilities.
		ScriptCapabilities Capabilities = ScriptCapabilities::None;
		// Declared package resource limits.
		DataScriptPackageBudget Budget;
		// Deterministic package seed.
		uint64_t Seed = 0;
	};

	// A data-script package parse result.
	struct DataScriptPackageParseResult {
		// Verified package manifest.
		std::optional<DataScriptPackage> Package;
		// Failure diagnostic.
		std::string Error;

		// Reports whether parsing produced a verified package.
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
