#pragma once

// Fixed-tick NPC work admission over scene cadence policy. Perception and
// behaviour results, including an outstanding admission, belong to ECS rows.
// Gameplay timers continue on every fixed tick, whether either domain is due.
// @tier L9 · shared

#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/scene/UpdateCadence.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace engine::ecs {
	class Store;
}

namespace engine::script {
	struct ServiceSurface;

	// Saved observation produced by the NPC perception domain.
	struct NpcPerception {
		core::Vector3 TargetPosition; // Last committed target position.
		bool Visible = false;		  // Whether that target was visible at the observation.
	};

	// Saved movement decision produced by the NPC behaviour domain.
	struct NpcBehaviour {
		core::Vector3 MoveDirection; // Last committed movement direction.
	};

	// Result of requesting the current actor/domain cadence decision.
	struct NpcCadenceAdmission {
		bool Due = false; // Whether work is admitted for this fixed tick.
		scene::CadenceReason Reason =
			scene::CadenceReason::InvalidPolicy; // Policy decision or refusal reason.
		uint64_t Token = 0;						 // Opaque token required to commit this admission.
		uint64_t Tick = 0;						 // Fixed tick for which the admission was issued.
	};

	// The two slots are runtime-only. A save retains the component's presence but
	// clears every outstanding admission when loaded into another Store/runtime.
	struct NpcCadencePending {
		// One domain's current admission, bound to its actor, runtime and store.
		struct Slot {
			scene::CadenceDecision Decision; // Captured policy decision and source revision.
			ecs::Entity Actor;				 // Actor that received the admission.
			uint64_t Token = 0;				 // Opaque admission token checked at commit.
			uint64_t RuntimeIdentity = 0;	 // Runtime incarnation that issued the token.
			uint64_t StoreIdentity = 0;		 // Store incarnation that owns the actor.
		};
		std::array<Slot, 2> Domains{}; // Perception and behaviour slots, indexed by UpdateDomain.
	};

	// Registers saved outputs and transient admissions before Components::Seal.
	void RegisterUpdateCadenceServiceComponents();

	// Configures perception and behaviour, plus supplied pose/effect intervals.
	// Omitted intervals retain existing values. A differing identity is refused.
	bool ConfigureNpcCadence(
		ecs::Store &store,
		ecs::Entity actor,
		std::string_view persistentIdentity,
		scene::CadenceInterval perception,
		scene::CadenceInterval behaviour,
		uint32_t demotionDelayTicks,
		std::optional<scene::CadenceInterval> visualPose = std::nullopt,
		std::optional<scene::CadenceInterval> cosmeticEffects = std::nullopt
	);

	// Changes combat fidelity without trusting a script flag for player ownership.
	bool SetNpcCombat(ecs::Store &store, ecs::Entity actor, bool active);

	// One outstanding due admission per actor and domain. A later Begin replaces it.
	NpcCadenceAdmission BeginNpcUpdate(
		ecs::Store &store,
		uint64_t runtimeIdentity,
		ecs::Entity actor,
		scene::UpdateDomain domain,
		uint64_t sourceRevision
	);

	// Publishes the completed output and cadence state together, or changes neither.
	bool CommitNpcPerception(
		ecs::Store &store,
		uint64_t runtimeIdentity,
		ecs::Entity actor,
		uint64_t token,
		uint64_t sourceRevision,
		bool visible,
		const core::Vector3 &targetPosition,
		bool completed
	);
	// Commits the movement result only if the runtime, token and source revision still match.
	bool CommitNpcBehaviour(
		ecs::Store &store,
		uint64_t runtimeIdentity,
		ecs::Entity actor,
		uint64_t token,
		uint64_t sourceRevision,
		const core::Vector3 &moveDirection,
		bool completed
	);

	// Combat may consume NPC perception and behaviour only after both refreshed
	// against the current policy and source revision in this fixed tick.
	bool NpcCombatReady(const ecs::Store &store, ecs::Entity actor, uint64_t sourceRevision);

	// One VM-neutral description installed by both language adapters.
	const ServiceSurface &UpdateCadenceServiceSurface();
}
