#pragma once

// VM-free procedural ownership, copied source capture and bounded background work.
// Authored facts and accepted artifacts live in the ECS. Workers hold owned values
// only; the caller explicitly publishes a complete batch at its mutation barrier.
// @tier L9 · shared

#include <engine/parallel/Jobs.hpp>
#include <engine/procedural/Region.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::script {
	struct ProceduralSceneTarget;
	namespace procedural_detail {
		struct PublicationCandidate;
	}

	// Named authored facts, without caller-supplied digest or coverage claims.
	struct ProceduralSource {
		procedural::SourceKind Kind = procedural::SourceKind::Village; // Source schema.
		std::string Name{};											   // Stable authored source name.
		procedural::Point Anchor{};									   // Position in canonical subcell units.
		std::string TargetVillage{}; // Entrance destination, empty for villages.
	};

	// Registered scope and its owner-thread invalidation revision.
	struct ProceduralRegion {
		procedural::RegionScope Scope{}; // Registered regional output scope.
		// Owner lifecycle only. This number never enters canonical bytes or seeds.
		uint64_t SourceRevision = 1;
	};

	// Only Configure/Set/RemoveProcedural functions write this resource. Snapshot
	// codecs retain authored bytes and reset the process-local incarnation.
	struct ProceduralAuthority {
		std::string Owner{};									// Stable owner within this world.
		std::vector<uint8_t> RecipeBytes{};						// Canonical checked REC1 recipe.
		std::vector<ProceduralSource> Sources{};				// Sorted authoritative local facts.
		std::vector<procedural::AuthoredSpline> Splines{};		// Sorted authored road controls.
		std::vector<procedural::PlacementOverride> Overrides{}; // Sorted sparse placement edits.
		std::vector<ProceduralRegion> Regions{};				// Sorted registered scopes.
		uint64_t Incarnation = 0; // Process-local reconfiguration fence, never saved.
	};

	// Accepted artifact and the fixed-barrier receipt for one region.
	struct ProceduralPublication {
		procedural::RegionScope Scope{};	  // Regional owner.
		procedural::Digest GenerationKey{};	  // BLAKE3 digest of canonical input key.
		procedural::Digest ArtifactDigest{};  // BLAKE3 digest of canonical output.
		std::vector<uint8_t> ArtifactBytes{}; // Immutable canonical RGA1 output.
		uint64_t AcceptedRevision = 0;		  // Captured local source revision.
		uint64_t ActivationTick = 0;		  // Authority-selected publication tick.
	};

	// Canonical region output, not a second mutable placement world. Publication
	// replaces the entire table with one noexcept swap after every check succeeds.
	struct ProceduralPublished {
		std::vector<ProceduralPublication> Regions{}; // Sorted complete accepted outputs.
	};

	// Admission and storage ceilings, including work retained after cancellation.
	struct ProceduralHostLimits {
		static constexpr size_t MaximumSources = 512; // Authored source records.
		static constexpr size_t MaximumFacts = 512;	  // Sources, splines and overrides together.
		static constexpr size_t MaximumRegions = 64;  // Registered and published scopes.
		static constexpr size_t MaximumQueryVisits = MaximumSources * 2;  // Both local source queries.
		static constexpr size_t MaximumAuthorityBytes = 262144;			  // Encoded authored bytes.
		static constexpr size_t MaximumPublishedBytes = 16 * 1024 * 1024; // Total accepted artifact bytes.
		static constexpr size_t MaximumRetainedRequests = 8;			  // Active and unconsumed records.
		// Conservative reservation includes bounded kernel candidate workspace,
		// copied inputs, canonical key, artifact encoding and completion storage.
		static constexpr size_t BytesPerRequest = 32 * 1024 * 1024;
		static constexpr size_t MaximumRetainedBytes = 64 * 1024 * 1024; // Work reservation ceiling.
	};

	// Register once during startup, before the component registry is sealed.
	void RegisterProceduralComponents();

	// Configuration is transactional and refused in adopt-only replica stores.
	// Existing accepted output remains until a newer complete batch is published.
	bool ConfigureProceduralWorld(
		ecs::Store &store,
		std::string_view owner,
		const procedural::Recipe &recipe,
		std::span<const procedural::RegionScope> regions,
		std::span<const ProceduralSource> sources,
		procedural::Diagnostic &diagnostic
	);

	// Edits invalidate only registered regions whose complete query halo contains
	// the old or new source anchor. Recipe edits invalidate every registered region.
	bool SetProceduralRecipe(
		ecs::Store &store, const procedural::Recipe &recipe, procedural::Diagnostic &diagnostic
	);
	// Adds or replaces one named source and invalidates its old and new influence.
	bool SetProceduralSource(
		ecs::Store &store, const ProceduralSource &source, procedural::Diagnostic &diagnostic
	);
	// Removes one named source and invalidates regions covering its old position.
	bool RemoveProceduralSource(
		ecs::Store &store,
		procedural::SourceKind kind,
		std::string_view name,
		procedural::Diagnostic &diagnostic
	);

	// Captures fresh, complete local witnesses directly from bounded ECS facts.
	// Recomputes all included leaf digests, including an empty-query witness.
	bool CaptureProceduralRegion(
		const ecs::Store &store,
		procedural::RegionScope scope,
		procedural::CompiledRecipe &recipe,
		procedural::RegionRequest &request,
		procedural::Diagnostic &diagnostic
	);

	// Portable authored state only. Neither tickets, caches nor scope revisions
	// enter this format. Import refuses malformed input before replacing ECS state.
	bool EncodeProceduralAuthority(
		const ecs::Store &store, std::vector<uint8_t> &out, procedural::Diagnostic &diagnostic
	);
	// Restores checked authored facts transactionally, without queue state.
	bool DecodeProceduralAuthority(
		ecs::Store &store, std::span<const uint8_t> bytes, procedural::Diagnostic &diagnostic
	);

	// Observable request lifecycle, separate from canonical artifact identity.
	enum class ProceduralJobStatus : uint8_t {
		Pending,  // Work is still running or waiting behind an earlier ticket.
		Ready,	  // Complete artifact awaits explicit authority publication.
		Canceled, // Request cannot publish.
		Failed,	  // Generation refused or failed.
		Published // Canonical artifact entered ECS at the selected barrier.
	};

	// Nonblocking snapshot of monotonic kernel work counters.
	struct ProceduralProgress {
		uint64_t Ticket = 0;									   // Owner request identity.
		ProceduralJobStatus Status = ProceduralJobStatus::Pending; // Current lifecycle state.
		uint32_t CandidateCells = 0;							   // Candidate cells examined.
		uint32_t WorkUnits = 0;									   // Declared work units consumed.
		uint32_t SpatialTests = 0;								   // Local constraint comparisons performed.
		uint32_t Placements = 0;								   // Placement records prepared.
		uint32_t Roads = 0;										   // Road records prepared.
	};

	// Owned completion retained until the caller clears observations.
	struct ProceduralCompletion {
		uint64_t Ticket = 0;									   // Owner request identity.
		ProceduralJobStatus Status = ProceduralJobStatus::Pending; // Observed lifecycle state.
		procedural::RegionScope Scope{};						   // Requested regional output.
		procedural::Digest GenerationKey{};						   // Verified canonical input digest.
		procedural::Digest ArtifactDigest{};					   // Verified canonical output digest.
		std::vector<uint8_t> ArtifactBytes{}; // Complete immutable output, absent on refusal.
		procedural::Diagnostic Diagnostic{};  // Bounded refusal or failure detail.
	};

	// Bounded per-owner queue with explicit atomic ECS publication.
	class ProceduralJobs {
	  public:
		// One queue belongs to one exact store incarnation and one runtime owner.
		explicit ProceduralJobs(const ecs::Store &store);
		// Requests stop on every worker, then joins all of them.
		~ProceduralJobs();
		ProceduralJobs(const ProceduralJobs &) = delete;
		ProceduralJobs &operator=(const ProceduralJobs &) = delete;
		ProceduralJobs(ProceduralJobs &&) = delete;
		ProceduralJobs &operator=(ProceduralJobs &&) = delete;

		// Zero means synchronous refusal. Serial and Threaded are supported;
		// Processed is refused until a checked procedural worker protocol exists.
		uint64_t Submit(
			ecs::Store &store,
			procedural::RegionScope scope,
			parallel::JobContext context = parallel::JobContext::Threaded
		);
		// Observes the completed ticket prefix without waiting for worker work.
		void Poll();
		// Idempotently stops unpublished work; published artifacts remain accepted.
		bool Cancel(uint64_t ticket);
		// Reads bounded atomic progress without waiting for a worker.
		bool Progress(uint64_t ticket, ProceduralProgress &out) const;
		// View remains valid until Poll or ClearCompletions changes the collection.
		std::span<const ProceduralCompletion> Completions() const;
		// Releases observed requests, including canceled records and their budget.
		void ClearCompletions();
		// Drops one observed request without disturbing other owners sharing this queue.
		// Pending work retains its reservation until Poll observes completion.
		bool Release(uint64_t ticket);
		// Includes active, canceled and completed unconsumed requests.
		size_t RetainedCount() const;
		// Conservative work reservation held by every retained request.
		size_t ReservedBytes() const;
		// Most recent synchronous admission or publication refusal.
		const procedural::Diagnostic &LastDiagnostic() const;

		// Re-captures every source/key fence, verifies canonical output, then swaps
		// all requested regions together. Refusal preserves the previous table.
		// The caller chooses and records a fixed activation tick after readiness.
		// This publishes canonical artifacts only; scene materialization is separate.
		bool PublishBatch(ecs::Store &store, std::span<const uint64_t> tickets, uint64_t activationTick);

		// Publishes canonical artifacts and all bound visible regional outputs together.
		bool PublishSceneBatch(
			ecs::Store &store,
			std::span<const uint64_t> tickets,
			std::span<const ProceduralSceneTarget> targets,
			uint64_t activationTick
		);

	  private:
		bool PreparePublication(
			ecs::Store &store,
			std::span<const uint64_t> tickets,
			uint64_t activationTick,
			procedural_detail::PublicationCandidate &out
		);
		struct State;
		std::unique_ptr<State> Held;
	};
}
