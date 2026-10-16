#pragma once

#include <engine/core/Bytes.hpp>
#include <engine/script/ProceduralJobs.hpp>

#include <atomic>

namespace engine::script::procedural_detail {

	bool Refuse(procedural::Diagnostic &diagnostic, procedural::Status status, std::string_view field);
	bool SameScope(procedural::RegionScope left, procedural::RegionScope right);
	bool ScopeLess(procedural::RegionScope left, procedural::RegionScope right);
	bool Contains(procedural::Bounds bounds, procedural::Point point);
	bool ValidSource(const ProceduralSource &source);
	bool SourceLess(const ProceduralSource &left, const ProceduralSource &right);
	bool CompileAuthority(
		const ProceduralAuthority &authority,
		procedural::CompiledRecipe &out,
		procedural::Diagnostic &diagnostic
	);
	procedural::Digest Hash(std::span<const uint8_t> bytes);
	uint64_t TakeSequence(std::atomic<uint64_t> &next);
	uint64_t NextIncarnation();
	bool WriteAuthority(
		const ProceduralAuthority &authority, std::vector<uint8_t> &out, procedural::Diagnostic &diagnostic
	);
	bool ReadAuthority(
		std::span<const uint8_t> bytes, ProceduralAuthority &out, procedural::Diagnostic &diagnostic
	);
	bool WritePublished(
		const ProceduralPublished &published, std::vector<uint8_t> &out, procedural::Diagnostic &diagnostic
	);
	bool ReadPublished(
		std::span<const uint8_t> bytes, ProceduralPublished &out, procedural::Diagnostic &diagnostic
	);
}
