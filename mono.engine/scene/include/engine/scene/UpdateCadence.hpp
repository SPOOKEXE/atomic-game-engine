#pragma once

// Fixed-tick update policy. Gameplay clocks and physics never use this gate.
// Presentation keeps its commit history local; authority-owned domains may save theirs.
// @tier L7 · shared

#include <engine/core/Name.hpp>
#include <engine/ecs/Entity.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {
	// Independent consumers, with names used by the serialized policy.
	enum class UpdateDomain : uint8_t {
		// World observations without advancing gameplay.
		Perception,
		// Decisions from committed observations.
		Behaviour,
		// Sampled bones, independent of semantic track time.
		VisualPose,
		// Presentation work without damage or gameplay events.
		CosmeticEffects,
		// Number of independent domains.
		Count,
	};

	// Number of separately scheduled domains.
	inline constexpr size_t UPDATE_DOMAIN_COUNT = static_cast<size_t>(UpdateDomain::Count);
	// Bounds malformed policy intervals before they become effectively disabled updates.
	inline constexpr uint32_t MAXIMUM_CADENCE_TICKS = 1048576;
	// Maximum persistent phase identity length, in UTF-8 bytes.
	inline constexpr size_t MAXIMUM_CADENCE_IDENTITY_BYTES = 256;

	// Reasons that require fresh full-fidelity work regardless of distance.
	enum class UpdateCriticality : uint32_t {
		// No gameplay requirement.
		None = 0,
		// An active combat participant.
		Combat = 1,
		// A projectile whose gameplay continues at full fidelity.
		Projectile = 2,
		// A player-owned subject.
		PlayerOwned = 4,
		// An explicit subsystem requirement.
		Required = 8,
	};

	// One domain's authored tick interval and freshness limit.
	struct CadenceInterval {
		// Fixed tick multiple, with one preserving full fidelity.
		uint32_t IntervalTicks = 1;
		// Age at which fresh work is required, even after a skipped scheduled tick.
		uint32_t MaximumStalenessTicks = 1;
	};

	// Per-subject update policy. Identity must be persistent text before throttling is allowed.
	struct UpdateCadence {
		// Stable phase seed, never an entity index or Name::Id.
		core::Name Identity;
		// Domain owner responsible for committing outputs; null means the policy's own entity.
		ecs::Entity Owner;
		// Increment when changing inputs that invalidate an outstanding result.
		uint64_t Revision = 1;
		// Independent domain intervals, indexed by UpdateDomain.
		std::array<CadenceInterval, UPDATE_DOMAIN_COUNT> Domains{};
		// Ticks to retain full fidelity after a critical update.
		uint32_t DemotionDelayTicks = 0;
		// UpdateCriticality bits supplied by gameplay, in addition to automatic ownership checks.
		uint32_t CriticalReasons = 0;
	};

	// Last successful domain update, suitable for authority-owned save state.
	struct DomainCadenceState {
		// Last committed fixed tick.
		uint64_t LastCommittedTick = 0;
		// Input revision consumed by that update.
		uint64_t SourceRevision = 0;
		// Policy revision consumed by that update.
		uint64_t PolicyRevision = 0;
		// Inclusive full-fidelity demotion deadline.
		uint64_t HoldUntilTick = 0;
		// Commit generation rejects duplicated or superseded results; zero means uninitialized.
		uint64_t Generation = 0;
		// Critical reasons present when the output was committed.
		uint32_t CriticalReasons = 0;
		// Authored interval consumed by the update.
		uint32_t IntervalTicks = 0;
		// Authored freshness bound consumed by the update.
		uint32_t MaximumStalenessTicks = 0;
		// Stable phase consumed by the update.
		uint32_t Phase = 0;
	};

	// Saved scheduling state for authority-owned consumer outputs.
	struct UpdateCadenceState {
		// Each domain advances only when its own result commits.
		std::array<DomainCadenceState, UPDATE_DOMAIN_COUNT> Domains{};
	};

	// Inspectable reason for accepting or deferring work.
	enum class CadenceReason : uint8_t {
		// This tick's output is already current or no update is due.
		Current,
		// No output has yet committed.
		FirstUpdate,
		// The stable periodic slot is due.
		Scheduled,
		// The maximum age has been reached.
		MaximumStaleness,
		// Inputs or authored cadence changed.
		RevisionChanged,
		// Gameplay requires full-fidelity work now.
		Critical,
		// Demotion hysteresis retains full fidelity.
		DemotionHold,
		// Policy cannot safely schedule this update.
		InvalidPolicy,
		// A restored or reset clock precedes the last committed tick.
		ClockReset,
	};

	// Immutable admission token. CommitUpdate rechecks it before moving the clock.
	struct CadenceDecision {
		// Whether the consumer should evaluate its current inputs once.
		bool Due = false;
		// Why work is due or refused.
		CadenceReason Reason = CadenceReason::Current;
		// Fixed tick at admission.
		uint64_t Tick = 0;
		// Age of the last committed output in fixed ticks.
		uint64_t AgeTicks = 0;
		// Caller-owned input revision at admission.
		uint64_t SourceRevision = 0;
		// Authored policy revision at admission.
		uint64_t PolicyRevision = 0;
		// Generation the token is allowed to replace.
		uint64_t ExpectedGeneration = 0;
		// Demotion deadline to retain on success.
		uint64_t HoldUntilTick = 0;
		// Critical reasons observed at admission.
		uint32_t CriticalReasons = 0;
		// Authored interval captured by the token.
		uint32_t IntervalTicks = 1;
		// Authored freshness bound captured by the token.
		uint32_t MaximumStalenessTicks = 1;
		// Persistent identity's stable periodic slot.
		uint32_t Phase = 0;
	};

	// Returns the stable serialized name, or an empty view for an invalid domain.
	std::string_view UpdateDomainName(UpdateDomain domain);
	// Validates all bounded intervals, revisions, and critical-reason bits.
	bool ValidUpdateCadence(const UpdateCadence &policy);
	// Derives a deterministic phase from UTF-8 identity bytes with fixed FNV-1a arithmetic.
	uint32_t CadencePhase(std::string_view identity, uint32_t intervalTicks);
	// Decides work from fixed ticks only. No time accumulation or catch-up loop is performed.
	CadenceDecision CadenceDue(
		const UpdateCadence &policy,
		const DomainCadenceState &state,
		UpdateDomain domain,
		uint64_t tick,
		uint64_t sourceRevision,
		uint32_t criticalReasons = 0
	);
	// Commits a completed, current token once. Refusal leaves scheduling state unchanged.
	bool CommitUpdate(
		DomainCadenceState &state,
		const CadenceDecision &decision,
		uint64_t currentTick,
		uint64_t currentSourceRevision,
		uint64_t currentPolicyRevision,
		bool completed
	);

	// Local derived pose history. It is neither serialized nor replicated as gameplay state.
	struct PoseCadenceHistory {
		// Admission limit; overflow preserves full-fidelity evaluation instead of dropping a pose.
		static constexpr size_t MAXIMUM_POSES = 65536;
		// History keyed by complete animator handles, pruned when those animators disappear.
		std::unordered_map<ecs::Entity, DomainCadenceState> Poses;
		// Evaluated animators during the last presentation pass.
		uint64_t Evaluated = 0;
		// Retained poses during the last presentation pass.
		uint64_t Skipped = 0;
		// Critical promotions during the last presentation pass.
		uint64_t Promotions = 0;
		// Oldest retained output observed during the last presentation pass.
		uint64_t MaximumAgeTicks = 0;
		// Animators evaluated without history because admission was full.
		uint64_t CapacityRefused = 0;
		// Due animators retaining their last good pose while clip content is unavailable.
		uint64_t Unavailable = 0;
	};

	// Registers authored, saved, and local derived cadence storage before Components::Seal.
	void RegisterUpdateCadenceComponents();
}
