#pragma once
#include <engine/assets/ContentHash.hpp>
#include <engine/assets/Shader.hpp>
#include <engine/core/Name.hpp>
#include <engine/render/ComposerHlsl.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace engine::render::hlsl {
	inline constexpr std::string_view COOKED_ABI = "atomic.composer-hlsl.v1";
	inline constexpr std::string_view COOKED_SELECTOR = "composer_cooked_shader";
	inline constexpr uint64_t MAXIMUM_OWNER_COOKED_BYTES = 64ull * 1024 * 1024;
	inline constexpr size_t MAXIMUM_OWNER_PROGRAMS = 256;
	inline constexpr size_t MAXIMUM_LIBRARY_OWNERS = 64, MAXIMUM_LIBRARY_PROGRAMS = 4096;
	inline constexpr uint64_t MAXIMUM_COOKED_OPERATION_BYTES = 128ull * 1024 * 1024;
	// Both stages belong to one admitted artifact. No compiler or device object is
	// retained.
	struct CookedPair {
		Program SpirV;
		std::vector<std::byte> VertexMsl, FragmentMsl;
		assets::ContentHash DefinitionFingerprint;
		uint64_t PayloadBytes = 0;
		bool VertexRowMajor = false;
		bool operator==(const CookedPair &) const = default;
	};
	// Fixed carrier storage and every retained vector/string capacity, excluding
	// allocator bookkeeping.
	std::optional<uint64_t> CookedPairRetainedBytes(const CookedPair &, uint64_t maximumBytes = UINT64_MAX);
	// Covers source sockets and declarations only. Native cook selectors and frame
	// values are excluded.
	std::optional<std::string> DefinitionFingerprint(const Definition &, assets::ContentHash &);
	// Pinned build source provenance; empty when the build did not record it.
	std::string_view CookerIdentity();
	// Authoring-only cook, with explicit library source and optional offline MSL
	// generation.
	std::optional<std::string> CookArtifact(
		const Definition &,
		std::span<const Library>,
		std::string_view compilerIdentity,
		bool includeMsl,
		assets::ShaderData &,
		std::string_view translatorIdentity = {},
		uint64_t maximumBytes = MAXIMUM_COOKED_OPERATION_BYTES
	);
	// Reads one exact ASH1-content-addressed local artifact under an explicitly
	// supplied host root.
	std::optional<std::string> ReadArtifact(
		const std::filesystem::path &,
		core::Name,
		assets::ShaderData &,
		uint64_t maximumBytes = 128ull * 1024 * 1024
	);
	// Runtime admission reflects retained executable bytes; never recompiles or
	// translates them.
	std::optional<std::string> AdmitArtifact(
		const assets::ShaderData &, CookedPair &, uint64_t maximumBytes = MAXIMUM_COOKED_OPERATION_BYTES
	);
	// Owner-scoped successful replacements advance revision; refused edits retain
	// the previous pair.
	class CookedComposerLibrary {
	  public:
		std::optional<std::string> Install(
			core::Name owner,
			core::Name name,
			const assets::ShaderData &,
			uint64_t maximumBytes = MAXIMUM_COOKED_OPERATION_BYTES
		);
		const CookedPair *Find(core::Name owner, core::Name name, uint64_t &revision) const;
		bool Remove(core::Name owner, core::Name name, uint64_t installedRevision);
		void RemoveOwner(core::Name owner);
		uint64_t PayloadBytes(core::Name owner) const;
		// Fixed library storage, entry backing and nested pair capacities across all owners.
		std::optional<uint64_t> RetainedBytes(uint64_t maximumBytes = UINT64_MAX) const;

	  private:
		struct Entry {
			core::Name Owner, Name;
			uint64_t Revision = 0;
			CookedPair Pair;
		};
		std::vector<Entry> Entries;
	};
} // namespace engine::render::hlsl
