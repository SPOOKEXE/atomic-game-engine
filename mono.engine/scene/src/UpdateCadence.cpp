#include <engine/core/Bytes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/scene/UpdateCadence.hpp>

#include <limits>

namespace engine::scene {
	namespace {
		constexpr uint32_t ALL_CRITICAL_REASONS = 15;
		constexpr uint32_t CADENCE_ENCODING_VERSION = 1;
		constexpr std::array<std::string_view, UPDATE_DOMAIN_COUNT> DOMAIN_NAMES{
			"perception", "behaviour", "visual-pose", "cosmetic-effects"
		};

		bool ValidInterval(const CadenceInterval &interval) {
			return interval.IntervalTicks > 0 && interval.IntervalTicks <= MAXIMUM_CADENCE_TICKS &&
				   interval.MaximumStalenessTicks >= interval.IntervalTicks &&
				   interval.MaximumStalenessTicks <= MAXIMUM_CADENCE_TICKS;
		}

		void WritePolicies(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *policies = static_cast<const UpdateCadence *>(source);
			for (size_t index = 0; index < count; ++index) {
				const auto &policy = policies[index];
				writer.WriteUInt32(CADENCE_ENCODING_VERSION);
				writer.WriteName(policy.Identity);
				writer.WriteUInt64(policy.Owner.Id);
				writer.WriteUInt64(policy.Revision);
				writer.WriteUInt32(policy.DemotionDelayTicks);
				writer.WriteUInt32(policy.CriticalReasons);
				for (size_t domain = 0; domain < UPDATE_DOMAIN_COUNT; ++domain) {
					writer.WriteString(DOMAIN_NAMES[domain]);
					writer.WriteUInt32(policy.Domains[domain].IntervalTicks);
					writer.WriteUInt32(policy.Domains[domain].MaximumStalenessTicks);
				}
			}
		}

		void ReadPolicies(core::ByteReader &reader, void *destination, size_t count) {
			auto *policies = static_cast<UpdateCadence *>(destination);
			for (size_t index = 0; index < count; ++index) {
				UpdateCadence candidate;
				if (reader.ReadUInt32() != CADENCE_ENCODING_VERSION) {
					reader.Fail();
					return;
				}
				candidate.Identity = reader.ReadName();
				candidate.Owner = ecs::Entity(reader.ReadUInt64());
				candidate.Revision = reader.ReadUInt64();
				candidate.DemotionDelayTicks = reader.ReadUInt32();
				candidate.CriticalReasons = reader.ReadUInt32();
				for (size_t domain = 0; domain < UPDATE_DOMAIN_COUNT; ++domain) {
					if (reader.ReadString() != DOMAIN_NAMES[domain]) {
						reader.Fail();
						return;
					}
					candidate.Domains[domain].IntervalTicks = reader.ReadUInt32();
					candidate.Domains[domain].MaximumStalenessTicks = reader.ReadUInt32();
				}
				if (reader.Failed() || !ValidUpdateCadence(candidate)) {
					reader.Fail();
					return;
				}
				policies[index] = candidate;
			}
		}

		void WriteStates(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *states = static_cast<const UpdateCadenceState *>(source);
			for (size_t index = 0; index < count; ++index) {
				writer.WriteUInt32(CADENCE_ENCODING_VERSION);
				for (size_t domain = 0; domain < UPDATE_DOMAIN_COUNT; ++domain) {
					writer.WriteString(DOMAIN_NAMES[domain]);
					const auto &state = states[index].Domains[domain];
					writer.WriteUInt64(state.LastCommittedTick);
					writer.WriteUInt64(state.SourceRevision);
					writer.WriteUInt64(state.PolicyRevision);
					writer.WriteUInt64(state.HoldUntilTick);
					writer.WriteUInt64(state.Generation);
					writer.WriteUInt32(state.CriticalReasons);
					writer.WriteUInt32(state.IntervalTicks);
					writer.WriteUInt32(state.MaximumStalenessTicks);
					writer.WriteUInt32(state.Phase);
				}
			}
		}

		void ReadStates(core::ByteReader &reader, void *destination, size_t count) {
			auto *states = static_cast<UpdateCadenceState *>(destination);
			for (size_t index = 0; index < count; ++index) {
				UpdateCadenceState candidate;
				if (reader.ReadUInt32() != CADENCE_ENCODING_VERSION) {
					reader.Fail();
					return;
				}
				for (size_t domain = 0; domain < UPDATE_DOMAIN_COUNT; ++domain) {
					if (reader.ReadString() != DOMAIN_NAMES[domain]) {
						reader.Fail();
						return;
					}
					auto &state = candidate.Domains[domain];
					state.LastCommittedTick = reader.ReadUInt64();
					state.SourceRevision = reader.ReadUInt64();
					state.PolicyRevision = reader.ReadUInt64();
					state.HoldUntilTick = reader.ReadUInt64();
					state.Generation = reader.ReadUInt64();
					state.CriticalReasons = reader.ReadUInt32();
					state.IntervalTicks = reader.ReadUInt32();
					state.MaximumStalenessTicks = reader.ReadUInt32();
					state.Phase = reader.ReadUInt32();
					const bool validCommitted =
						state.Generation == 0 ||
						(state.PolicyRevision != 0 && state.SourceRevision != 0 &&
						 ValidInterval({state.IntervalTicks, state.MaximumStalenessTicks}) &&
						 state.Phase < state.IntervalTicks);
					if (!validCommitted || (state.CriticalReasons & ~ALL_CRITICAL_REASONS) != 0) {
						reader.Fail();
						return;
					}
				}
				if (reader.Failed()) return;
				states[index] = candidate;
			}
		}
	}

	std::string_view UpdateDomainName(UpdateDomain domain) {
		const size_t index = static_cast<size_t>(domain);
		return index < DOMAIN_NAMES.size() ? DOMAIN_NAMES[index] : std::string_view{};
	}

	bool ValidUpdateCadence(const UpdateCadence &policy) {
		if (policy.Revision == 0 || policy.DemotionDelayTicks > MAXIMUM_CADENCE_TICKS ||
			(policy.CriticalReasons & ~ALL_CRITICAL_REASONS) != 0 ||
			policy.Identity.Text().size() > MAXIMUM_CADENCE_IDENTITY_BYTES)
			return false;
		for (const auto &interval : policy.Domains) {
			if (!ValidInterval(interval)) return false;
		}
		return true;
	}

	uint32_t CadencePhase(std::string_view identity, uint32_t intervalTicks) {
		if (intervalTicks == 0) return 0;
		uint64_t hash = 14695981039346656037ull;
		for (char byte : identity) {
			hash ^= static_cast<uint8_t>(byte);
			hash *= 1099511628211ull;
		}
		return static_cast<uint32_t>(hash % intervalTicks);
	}

	CadenceDecision CadenceDue(
		const UpdateCadence &policy,
		const DomainCadenceState &state,
		UpdateDomain domain,
		uint64_t tick,
		uint64_t sourceRevision,
		uint32_t criticalReasons
	) {
		CadenceDecision decision;
		decision.Tick = tick;
		decision.SourceRevision = sourceRevision;
		decision.PolicyRevision = policy.Revision;
		decision.ExpectedGeneration = state.Generation;
		decision.AgeTicks =
			state.Generation != 0 && tick >= state.LastCommittedTick ? tick - state.LastCommittedTick : 0;
		const size_t index = static_cast<size_t>(domain);
		if (index >= UPDATE_DOMAIN_COUNT || !ValidUpdateCadence(policy) || sourceRevision == 0 ||
			(criticalReasons & ~ALL_CRITICAL_REASONS) != 0) {
			decision.Reason = CadenceReason::InvalidPolicy;
			return decision;
		}

		const auto &interval = policy.Domains[index];
		decision.IntervalTicks = interval.IntervalTicks;
		decision.MaximumStalenessTicks = interval.MaximumStalenessTicks;
		decision.Phase = CadencePhase(policy.Identity.Text(), interval.IntervalTicks);
		decision.CriticalReasons = criticalReasons | policy.CriticalReasons;
		decision.HoldUntilTick = tick < state.LastCommittedTick ? tick : state.HoldUntilTick;
		if (decision.CriticalReasons != 0) {
			const uint64_t remaining = std::numeric_limits<uint64_t>::max() - tick;
			decision.HoldUntilTick =
				tick + (remaining < policy.DemotionDelayTicks ? remaining : policy.DemotionDelayTicks);
		}

		const bool changed =
			state.SourceRevision != sourceRevision || state.PolicyRevision != policy.Revision ||
			state.IntervalTicks != interval.IntervalTicks ||
			state.MaximumStalenessTicks != interval.MaximumStalenessTicks || state.Phase != decision.Phase;
		const bool promoted = (decision.CriticalReasons & ~state.CriticalReasons) != 0;
		decision.Due = true;
		if (state.Generation == 0)
			decision.Reason = CadenceReason::FirstUpdate;
		else if (tick < state.LastCommittedTick)
			decision.Reason = CadenceReason::ClockReset;
		else if (promoted)
			decision.Reason = CadenceReason::Critical;
		else if (changed)
			decision.Reason = CadenceReason::RevisionChanged;
		else if (tick == state.LastCommittedTick)
			decision.Due = false;
		else if (decision.CriticalReasons != 0)
			decision.Reason = CadenceReason::Critical;
		else if (tick <= state.HoldUntilTick)
			decision.Reason = CadenceReason::DemotionHold;
		else if (!policy.Identity.IsValid() || interval.IntervalTicks == 1)
			decision.Reason = CadenceReason::Scheduled;
		else if (decision.AgeTicks >= interval.MaximumStalenessTicks)
			decision.Reason = CadenceReason::MaximumStaleness;
		else if (tick % interval.IntervalTicks == decision.Phase)
			decision.Reason = CadenceReason::Scheduled;
		else
			decision.Due = false;
		return decision;
	}

	bool CommitUpdate(
		DomainCadenceState &state,
		const CadenceDecision &decision,
		uint64_t currentTick,
		uint64_t currentSourceRevision,
		uint64_t currentPolicyRevision,
		bool completed
	) {
		if (!completed || !decision.Due || decision.Reason == CadenceReason::InvalidPolicy ||
			decision.Reason == CadenceReason::Current || decision.Reason > CadenceReason::ClockReset ||
			decision.SourceRevision == 0 || decision.PolicyRevision == 0 ||
			!ValidInterval({decision.IntervalTicks, decision.MaximumStalenessTicks}) ||
			decision.Phase >= decision.IntervalTicks ||
			(decision.CriticalReasons & ~ALL_CRITICAL_REASONS) != 0 || decision.Tick != currentTick ||
			decision.SourceRevision != currentSourceRevision ||
			decision.PolicyRevision != currentPolicyRevision ||
			decision.ExpectedGeneration != state.Generation ||
			state.Generation == std::numeric_limits<uint64_t>::max())
			return false;
		state.LastCommittedTick = decision.Tick;
		state.SourceRevision = decision.SourceRevision;
		state.PolicyRevision = decision.PolicyRevision;
		state.HoldUntilTick = decision.HoldUntilTick;
		state.Generation++;
		state.CriticalReasons = decision.CriticalReasons;
		state.IntervalTicks = decision.IntervalTicks;
		state.MaximumStalenessTicks = decision.MaximumStalenessTicks;
		state.Phase = decision.Phase;
		return true;
	}

	void RegisterUpdateCadenceComponents() {
		ecs::Components::Register<UpdateCadence>("scene.UpdateCadence", WritePolicies, ReadPolicies, 512);
		ecs::Components::Register<UpdateCadenceState>(
			"scene.UpdateCadenceState", WriteStates, ReadStates, 512
		);
		ecs::Components::Register<PoseCadenceHistory>(
			"presentation.PoseCadenceHistory",
			[](core::ByteWriter &, const void *, size_t) {},
			[](core::ByteReader &, void *destination, size_t count) {
				auto *histories = static_cast<PoseCadenceHistory *>(destination);
				for (size_t index = 0; index < count; ++index)
					histories[index] = {};
			}
		);
	}
}
