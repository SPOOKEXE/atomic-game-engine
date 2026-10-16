#pragma once

#include <engine/core/Bytes.hpp>
#include <engine/script/ProceduralJobs.hpp>

#include <atomic>

namespace engine::script::procedural_detail {

	struct PublicationCandidate {
		ProceduralPublished Published{};
		std::vector<ProceduralCompletion *> Accepted{};
		size_t Bytes = 0;
	};

	bool Refuse(procedural::Diagnostic &diagnostic, procedural::Status status, std::string_view field);
	bool SameScope(procedural::RegionScope left, procedural::RegionScope right);
	bool KeyScopeMatches(std::span<const uint8_t> key, procedural::RegionScope scope);
	bool ScopeLess(procedural::RegionScope left, procedural::RegionScope right);
	bool Contains(procedural::Bounds bounds, procedural::Point point);
	bool ValidSource(const ProceduralSource &source);
	bool SourceLess(const ProceduralSource &left, const ProceduralSource &right);
	bool CompileAuthority(
		const ProceduralAuthority &authority,
		procedural::CompiledRecipe &out,
		procedural::Diagnostic &diagnostic
	);
	bool ValidateAuthoredFacts(
		const ProceduralAuthority &authority,
		const procedural::CompiledRecipe &recipe,
		procedural::Diagnostic &diagnostic
	);
	bool InvalidateChangedFacts(
		const ProceduralAuthority &previous,
		ProceduralAuthority &candidate,
		const procedural::CompiledRecipe &recipe,
		procedural::Diagnostic &diagnostic
	);
	bool CaptureAuthoredFacts(
		const ProceduralAuthority &authority,
		const procedural::CompiledRecipe &recipe,
		procedural::Bounds owner,
		procedural::Bounds query,
		procedural::AuthoredInputQuery &out,
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

namespace engine::script::procedural_source_detail {
	const ProceduralAuthority *Authority(const ecs::Store &store);
	bool MayEdit(const ecs::Store &store, procedural::Diagnostic &diagnostic);
	bool CaptureAuthorityRegion(
		const ProceduralAuthority &,
		procedural::RegionScope,
		procedural::CompiledRecipe &,
		procedural::RegionRequest &,
		procedural::Diagnostic &
	);
	bool PreparePreviewAuthority(
		const ecs::Store &, std::span<const uint8_t>, ProceduralAuthority &, procedural::Diagnostic &
	);
	bool
	ReplaceAuthority(ecs::Store &store, ProceduralAuthority candidate, procedural::Diagnostic &diagnostic);
}

namespace engine::script {
	namespace procedural_job_detail {
		struct Job;
	}
	struct ProceduralJobs::State {
		uint64_t StoreIdentity = 0;
		std::vector<std::unique_ptr<procedural_job_detail::Job>> Jobs{};
		std::vector<ProceduralCompletion> Completed{};
		procedural::Diagnostic Diagnostic{};
	};
}
