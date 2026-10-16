#include <engine/core/Bytes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/ServiceSurface.hpp>
#include <engine/script/UpdateCadenceService.hpp>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace engine::script {
	namespace {
		constexpr uint64_t MAXIMUM_SCRIPT_TOKEN = (uint64_t{1} << 53) - 1;
		constexpr uint64_t MAXIMUM_SOURCE_REVISION = MAXIMUM_SCRIPT_TOKEN;
		constexpr size_t MAXIMUM_NPCS = 4096;
		constexpr uint32_t COMBAT = static_cast<uint32_t>(scene::UpdateCriticality::Combat);
		constexpr uint32_t PLAYER_OWNED = static_cast<uint32_t>(scene::UpdateCriticality::PlayerOwned);
		std::atomic<uint64_t> NextToken{1};

		bool NpcFinite(const core::Vector3 &value) {
			return std::isfinite(value.X) && std::isfinite(value.Y) && std::isfinite(value.Z);
		}

		bool NpcValidInterval(const scene::CadenceInterval &interval) {
			return interval.IntervalTicks > 0 && interval.IntervalTicks <= scene::MAXIMUM_CADENCE_TICKS &&
				   interval.MaximumStalenessTicks >= interval.IntervalTicks &&
				   interval.MaximumStalenessTicks <= scene::MAXIMUM_CADENCE_TICKS;
		}

		bool ValidNpcDomain(scene::UpdateDomain domain) {
			return domain == scene::UpdateDomain::Perception || domain == scene::UpdateDomain::Behaviour;
		}

		size_t NpcDomainIndex(scene::UpdateDomain domain) {
			return static_cast<size_t>(domain);
		}

		uint64_t IssueNpcToken() {
			uint64_t next = NextToken.load(std::memory_order_relaxed);
			while (next <= MAXIMUM_SCRIPT_TOKEN) {
				if (NextToken.compare_exchange_weak(next, next + 1, std::memory_order_relaxed)) return next;
			}
			return 0;
		}

		bool NpcPlayerOwned(const ecs::Store &store, ecs::Entity actor) {
			for (ecs::Entity walk = actor; walk != ecs::NULL_ENTITY; walk = store.ParentOf(walk)) {
				const auto *character = store.Get<scene::Character>(walk);
				const auto *owner = store.Get<scene::NetworkOwner>(walk);
				if ((character != nullptr && character->Owner != ecs::NULL_ENTITY) ||
					(owner != nullptr && owner->Player != ecs::NULL_ENTITY))
					return true;
			}
			return scene::PlayerOwning(store, actor) != ecs::NULL_ENTITY;
		}

		ecs::Entity FindNpcHumanoid(const ecs::Store &store, ecs::Entity actor) {
			if (!store.Alive(actor)) return ecs::NULL_ENTITY;
			if (store.Get<scene::Humanoid>(actor) != nullptr) return actor;
			const auto child = store.FindFirstChildWhichIsA(actor, scene::HumanoidClass(), true);
			return child != ecs::NULL_ENTITY && store.Get<scene::Humanoid>(child) != nullptr
					   ? child
					   : ecs::NULL_ENTITY;
		}

		bool SameNpcDecision(const scene::CadenceDecision &left, const scene::CadenceDecision &right) {
			return left.Due == right.Due && left.Reason == right.Reason && left.Tick == right.Tick &&
				   left.AgeTicks == right.AgeTicks && left.SourceRevision == right.SourceRevision &&
				   left.PolicyRevision == right.PolicyRevision &&
				   left.ExpectedGeneration == right.ExpectedGeneration &&
				   left.HoldUntilTick == right.HoldUntilTick &&
				   left.CriticalReasons == right.CriticalReasons &&
				   left.IntervalTicks == right.IntervalTicks &&
				   left.MaximumStalenessTicks == right.MaximumStalenessTicks && left.Phase == right.Phase;
		}

		uint32_t NpcCriticalReasons(const ecs::Store &store, ecs::Entity actor) {
			return NpcPlayerOwned(store, actor) ? PLAYER_OWNED : 0;
		}

		const scene::UpdateCadence *NpcPolicy(const ecs::Store &store, ecs::Entity actor) {
			return store.Alive(actor) ? store.Get<scene::UpdateCadence>(actor) : nullptr;
		}

		bool NpcCommitReady(
			ecs::Store &store,
			uint64_t runtimeIdentity,
			ecs::Entity actor,
			scene::UpdateDomain domain,
			uint64_t token,
			uint64_t sourceRevision,
			bool completed,
			scene::DomainCadenceState &candidate
		) {
			if (!completed || store.AdoptOnly() || runtimeIdentity == 0 || token == 0 ||
				sourceRevision == 0 || sourceRevision > MAXIMUM_SOURCE_REVISION || !store.Alive(actor))
				return false;
			const auto *policy = NpcPolicy(store, actor);
			const auto *state = store.Get<scene::UpdateCadenceState>(actor);
			const auto *pending = store.Get<NpcCadencePending>(actor);
			if (policy == nullptr || state == nullptr || pending == nullptr) return false;
			const size_t index = NpcDomainIndex(domain);
			const auto &slot = pending->Domains[index];
			if (slot.Token != token || slot.Actor != actor || slot.RuntimeIdentity != runtimeIdentity ||
				slot.StoreIdentity != store.Identity() || slot.Decision.SourceRevision != sourceRevision ||
				slot.Decision.Tick != store.Time().Tick)
				return false;
			const auto current = scene::CadenceDue(
				*policy,
				state->Domains[index],
				domain,
				store.Time().Tick,
				sourceRevision,
				NpcCriticalReasons(store, actor)
			);
			if (!SameNpcDecision(slot.Decision, current)) return false;
			candidate = state->Domains[index];
			return scene::CommitUpdate(
				candidate, slot.Decision, store.Time().Tick, sourceRevision, policy->Revision, true
			);
		}

		void WriteNpcPerceptions(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *rows = static_cast<const NpcPerception *>(source);
			for (size_t index = 0; index < count; ++index) {
				writer.WriteFloat(rows[index].TargetPosition.X);
				writer.WriteFloat(rows[index].TargetPosition.Y);
				writer.WriteFloat(rows[index].TargetPosition.Z);
				writer.WriteBool(rows[index].Visible);
			}
		}

		void ReadNpcPerceptions(core::ByteReader &reader, void *destination, size_t count) {
			auto *rows = static_cast<NpcPerception *>(destination);
			for (size_t index = 0; index < count; ++index) {
				NpcPerception value;
				value.TargetPosition.X = reader.ReadFloat();
				value.TargetPosition.Y = reader.ReadFloat();
				value.TargetPosition.Z = reader.ReadFloat();
				value.Visible = reader.ReadBool();
				if (reader.Failed() || !NpcFinite(value.TargetPosition)) {
					reader.Fail();
					return;
				}
				rows[index] = value;
			}
		}

		void WriteNpcBehaviours(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *rows = static_cast<const NpcBehaviour *>(source);
			for (size_t index = 0; index < count; ++index) {
				writer.WriteFloat(rows[index].MoveDirection.X);
				writer.WriteFloat(rows[index].MoveDirection.Y);
				writer.WriteFloat(rows[index].MoveDirection.Z);
			}
		}

		void ReadNpcBehaviours(core::ByteReader &reader, void *destination, size_t count) {
			auto *rows = static_cast<NpcBehaviour *>(destination);
			for (size_t index = 0; index < count; ++index) {
				NpcBehaviour value;
				value.MoveDirection.X = reader.ReadFloat();
				value.MoveDirection.Y = reader.ReadFloat();
				value.MoveDirection.Z = reader.ReadFloat();
				if (reader.Failed() || !NpcFinite(value.MoveDirection) ||
					value.MoveDirection.Dot(value.MoveDirection) > 1.0001f) {
					reader.Fail();
					return;
				}
				rows[index] = value;
			}
		}

		void WriteNpcPending(core::ByteWriter &, const void *, size_t) {}
		void ReadNpcPending(core::ByteReader &, void *destination, size_t count) {
			auto *rows = static_cast<NpcCadencePending *>(destination);
			for (size_t index = 0; index < count; ++index)
				rows[index] = {};
		}
	}

	void RegisterUpdateCadenceServiceComponents() {
		ecs::Components::Register<NpcPerception>(
			"script.NpcPerception", WriteNpcPerceptions, ReadNpcPerceptions, 13
		);
		ecs::Components::Register<NpcBehaviour>(
			"script.NpcBehaviour", WriteNpcBehaviours, ReadNpcBehaviours, 12
		);
		ecs::Components::Register<NpcCadencePending>(
			"script.NpcCadencePending", WriteNpcPending, ReadNpcPending
		);
	}

	bool ConfigureNpcCadence(
		ecs::Store &store,
		ecs::Entity actor,
		std::string_view persistentIdentity,
		scene::CadenceInterval perception,
		scene::CadenceInterval behaviour,
		uint32_t demotionDelayTicks
	) {
		if (store.AdoptOnly() || !store.Alive(actor) || FindNpcHumanoid(store, actor) == ecs::NULL_ENTITY ||
			persistentIdentity.empty() || persistentIdentity.size() > scene::MAXIMUM_CADENCE_IDENTITY_BYTES ||
			!NpcValidInterval(perception) || !NpcValidInterval(behaviour) ||
			demotionDelayTicks > scene::MAXIMUM_CADENCE_TICKS)
			return false;
		if (!store.Has<NpcCadencePending>(actor)) {
			size_t configured = 0;
			store.Each<const NpcCadencePending>([&](ecs::Entity, const NpcCadencePending &) {
				++configured;
			});
			if (configured >= MAXIMUM_NPCS) return false;
		}
		const auto *existing = store.Get<scene::UpdateCadence>(actor);
		if (existing != nullptr && existing->Identity.IsValid() &&
			existing->Identity.Text() != persistentIdentity)
			return false;
		if (existing != nullptr && existing->Owner != ecs::NULL_ENTITY && existing->Owner != actor)
			return false;

		scene::UpdateCadence policy = existing != nullptr ? *existing : scene::UpdateCadence{};
		const core::Name identity(persistentIdentity);
		const bool changed =
			existing == nullptr || policy.Identity != identity || policy.Owner != actor ||
			policy.Domains[NpcDomainIndex(scene::UpdateDomain::Perception)].IntervalTicks !=
				perception.IntervalTicks ||
			policy.Domains[NpcDomainIndex(scene::UpdateDomain::Perception)].MaximumStalenessTicks !=
				perception.MaximumStalenessTicks ||
			policy.Domains[NpcDomainIndex(scene::UpdateDomain::Behaviour)].IntervalTicks !=
				behaviour.IntervalTicks ||
			policy.Domains[NpcDomainIndex(scene::UpdateDomain::Behaviour)].MaximumStalenessTicks !=
				behaviour.MaximumStalenessTicks ||
			policy.DemotionDelayTicks != demotionDelayTicks;
		if (changed && existing != nullptr && policy.Revision == std::numeric_limits<uint64_t>::max())
			return false;
		policy.Identity = identity;
		policy.Owner = actor;
		policy.Domains[NpcDomainIndex(scene::UpdateDomain::Perception)] = perception;
		policy.Domains[NpcDomainIndex(scene::UpdateDomain::Behaviour)] = behaviour;
		policy.DemotionDelayTicks = demotionDelayTicks;
		if (changed && existing != nullptr) policy.Revision++;
		if (!scene::ValidUpdateCadence(policy)) return false;

		if (changed) store.Set(actor, policy);
		if (!store.Has<scene::UpdateCadenceState>(actor)) store.Set(actor, scene::UpdateCadenceState{});
		if (!store.Has<NpcPerception>(actor)) store.Set(actor, NpcPerception{});
		if (!store.Has<NpcBehaviour>(actor)) store.Set(actor, NpcBehaviour{});
		if (changed || !store.Has<NpcCadencePending>(actor)) store.Set(actor, NpcCadencePending{});
		return true;
	}

	bool SetNpcCombat(ecs::Store &store, ecs::Entity actor, bool active) {
		if (store.AdoptOnly() || !store.Alive(actor) || !store.Has<NpcCadencePending>(actor)) return false;
		const auto *existing = store.Get<scene::UpdateCadence>(actor);
		if (existing == nullptr) return false;
		const uint32_t reasons =
			active ? existing->CriticalReasons | COMBAT : existing->CriticalReasons & ~COMBAT;
		if (reasons == existing->CriticalReasons) return true;
		if (existing->Revision == std::numeric_limits<uint64_t>::max()) return false;
		scene::UpdateCadence policy = *existing;
		policy.CriticalReasons = reasons;
		policy.Revision++;
		store.Set(actor, policy);
		store.Set(actor, NpcCadencePending{});
		return true;
	}

	NpcCadenceAdmission BeginNpcUpdate(
		ecs::Store &store,
		uint64_t runtimeIdentity,
		ecs::Entity actor,
		scene::UpdateDomain domain,
		uint64_t sourceRevision
	) {
		NpcCadenceAdmission admission;
		admission.Tick = store.Time().Tick;
		if (store.AdoptOnly() || runtimeIdentity == 0 || !ValidNpcDomain(domain) || sourceRevision == 0 ||
			sourceRevision > MAXIMUM_SOURCE_REVISION || !store.Alive(actor))
			return admission;
		const auto *policy = store.Get<scene::UpdateCadence>(actor);
		const auto *state = store.Get<scene::UpdateCadenceState>(actor);
		if (policy == nullptr || state == nullptr || !store.Has<NpcCadencePending>(actor) ||
			!store.Has<NpcPerception>(actor) || !store.Has<NpcBehaviour>(actor))
			return admission;
		const size_t index = NpcDomainIndex(domain);
		const auto decision = scene::CadenceDue(
			*policy,
			state->Domains[index],
			domain,
			admission.Tick,
			sourceRevision,
			NpcCriticalReasons(store, actor)
		);
		admission.Reason = decision.Reason;
		auto *pending = store.GetMutable<NpcCadencePending>(actor);
		if (pending == nullptr) return admission;
		pending->Domains[index] = {};
		if (!decision.Due) return admission;
		const uint64_t token = IssueNpcToken();
		if (token == 0) return admission;
		pending->Domains[index] = {
			.Decision = decision,
			.Actor = actor,
			.Token = token,
			.RuntimeIdentity = runtimeIdentity,
			.StoreIdentity = store.Identity(),
		};
		admission.Due = true;
		admission.Token = token;
		return admission;
	}

	bool CommitNpcPerception(
		ecs::Store &store,
		uint64_t runtimeIdentity,
		ecs::Entity actor,
		uint64_t token,
		uint64_t sourceRevision,
		bool visible,
		const core::Vector3 &targetPosition,
		bool completed
	) {
		if (!NpcFinite(targetPosition)) return false;
		scene::DomainCadenceState candidate;
		if (!NpcCommitReady(
				store,
				runtimeIdentity,
				actor,
				scene::UpdateDomain::Perception,
				token,
				sourceRevision,
				completed,
				candidate
			))
			return false;
		if (!store.Has<NpcPerception>(actor) || !store.Has<scene::UpdateCadenceState>(actor) ||
			!store.Has<NpcCadencePending>(actor))
			return false;
		auto *output = store.GetMutable<NpcPerception>(actor);
		auto *state = store.GetMutable<scene::UpdateCadenceState>(actor);
		auto *pending = store.GetMutable<NpcCadencePending>(actor);
		if (output == nullptr || state == nullptr || pending == nullptr) return false;
		*output = {targetPosition, visible};
		state->Domains[NpcDomainIndex(scene::UpdateDomain::Perception)] = candidate;
		pending->Domains[NpcDomainIndex(scene::UpdateDomain::Perception)] = {};
		return true;
	}

	bool CommitNpcBehaviour(
		ecs::Store &store,
		uint64_t runtimeIdentity,
		ecs::Entity actor,
		uint64_t token,
		uint64_t sourceRevision,
		const core::Vector3 &moveDirection,
		bool completed
	) {
		if (!NpcFinite(moveDirection) || moveDirection.Dot(moveDirection) > 1.0001f) return false;
		scene::DomainCadenceState candidate;
		if (!NpcCommitReady(
				store,
				runtimeIdentity,
				actor,
				scene::UpdateDomain::Behaviour,
				token,
				sourceRevision,
				completed,
				candidate
			))
			return false;
		const ecs::Entity humanoid = FindNpcHumanoid(store, actor);
		if (humanoid == ecs::NULL_ENTITY || !store.Has<NpcBehaviour>(actor) ||
			!store.Has<scene::UpdateCadenceState>(actor) || !store.Has<NpcCadencePending>(actor))
			return false;
		auto *output = store.GetMutable<NpcBehaviour>(actor);
		auto *steering = store.GetMutable<scene::Humanoid>(humanoid);
		auto *state = store.GetMutable<scene::UpdateCadenceState>(actor);
		auto *pending = store.GetMutable<NpcCadencePending>(actor);
		if (output == nullptr || steering == nullptr || state == nullptr || pending == nullptr) return false;
		output->MoveDirection = moveDirection;
		steering->MoveDirection = moveDirection;
		state->Domains[NpcDomainIndex(scene::UpdateDomain::Behaviour)] = candidate;
		pending->Domains[NpcDomainIndex(scene::UpdateDomain::Behaviour)] = {};
		return true;
	}

	bool NpcCombatReady(const ecs::Store &store, ecs::Entity actor, uint64_t sourceRevision) {
		if (store.AdoptOnly() || sourceRevision == 0 || !store.Alive(actor)) return false;
		const auto *policy = store.Get<scene::UpdateCadence>(actor);
		const auto *state = store.Get<scene::UpdateCadenceState>(actor);
		const auto *perception = store.Get<NpcPerception>(actor);
		if (policy == nullptr || state == nullptr || perception == nullptr || !perception->Visible ||
			(policy->CriticalReasons & COMBAT) == 0)
			return false;
		const uint64_t tick = store.Time().Tick;
		for (const auto domain : {scene::UpdateDomain::Perception, scene::UpdateDomain::Behaviour}) {
			const auto &committed = state->Domains[NpcDomainIndex(domain)];
			if (committed.Generation == 0 || committed.LastCommittedTick != tick ||
				committed.PolicyRevision != policy->Revision || committed.SourceRevision != sourceRevision)
				return false;
		}
		return true;
	}

	namespace {
		uint64_t NpcScriptInteger(ScriptCall &call, size_t index, uint64_t minimum, uint64_t maximum) {
			const double number = call.AsNumber(index);
			if (!std::isfinite(number) || std::floor(number) != number ||
				number < static_cast<double>(minimum) || number > static_cast<double>(maximum))
				call.Raise("cadence argument must be a bounded integer");
			return static_cast<uint64_t>(number);
		}

		bool NpcScriptBoolean(ScriptCall &call, size_t index) {
			ScriptValue value;
			CodecStatus status = CodecStatus::Ok;
			if (!call.ReadValue(index, value, status) ||
				(value.Tag != ValueTag::True && value.Tag != ValueTag::False))
				call.Raise("cadence argument must be a boolean");
			return value.Tag == ValueTag::True;
		}

		core::Vector3 NpcScriptVector(ScriptCall &call, size_t index) {
			ScriptValue value;
			CodecStatus status = CodecStatus::Ok;
			if (!call.ReadValue(index, value, status) || value.Tag != ValueTag::Vector3 ||
				!NpcFinite(value.Vector))
				call.Raise("cadence argument must be a finite Vector3");
			return value.Vector;
		}

		scene::UpdateDomain NpcScriptDomain(ScriptCall &call, size_t index) {
			const std::string name = call.AsBoundedString(index, 16);
			if (name == "perception") return scene::UpdateDomain::Perception;
			if (name == "behaviour") return scene::UpdateDomain::Behaviour;
			call.Raise("cadence domain must be perception or behaviour");
			return scene::UpdateDomain::Count;
		}

		const char *NpcReasonName(scene::CadenceReason reason) {
			switch (reason) {
			case scene::CadenceReason::Current:
				return "current";
			case scene::CadenceReason::FirstUpdate:
				return "first_update";
			case scene::CadenceReason::Scheduled:
				return "scheduled";
			case scene::CadenceReason::MaximumStaleness:
				return "maximum_staleness";
			case scene::CadenceReason::RevisionChanged:
				return "revision_changed";
			case scene::CadenceReason::Critical:
				return "critical";
			case scene::CadenceReason::DemotionHold:
				return "demotion_hold";
			case scene::CadenceReason::InvalidPolicy:
				return "invalid_policy";
			case scene::CadenceReason::ClockReset:
				return "clock_reset";
			}
			return "invalid_policy";
		}

		ScriptValue NpcValueBoolean(bool value) {
			ScriptValue result(value ? ValueTag::True : ValueTag::False);
			result.Boolean = value;
			return result;
		}

		ScriptValue NpcValueNumber(uint64_t value) {
			ScriptValue result(ValueTag::Number);
			result.Number = static_cast<double>(value);
			return result;
		}

		ScriptValue NpcValueString(std::string_view value) {
			ScriptValue result(ValueTag::String);
			result.Text = value;
			return result;
		}

		ScriptValue NpcValueVector(const core::Vector3 &value) {
			ScriptValue result(ValueTag::Vector3);
			result.Vector = value;
			return result;
		}

		void ConfigureNpcService(ScriptCall &call) {
			const ecs::Entity actor = call.AsInstance(0);
			const std::string identity = call.AsBoundedString(1, scene::MAXIMUM_CADENCE_IDENTITY_BYTES);
			const scene::CadenceInterval perception{
				static_cast<uint32_t>(NpcScriptInteger(call, 2, 1, scene::MAXIMUM_CADENCE_TICKS)),
				static_cast<uint32_t>(NpcScriptInteger(call, 3, 1, scene::MAXIMUM_CADENCE_TICKS))
			};
			const scene::CadenceInterval behaviour{
				static_cast<uint32_t>(NpcScriptInteger(call, 4, 1, scene::MAXIMUM_CADENCE_TICKS)),
				static_cast<uint32_t>(NpcScriptInteger(call, 5, 1, scene::MAXIMUM_CADENCE_TICKS))
			};
			const auto hold =
				static_cast<uint32_t>(NpcScriptInteger(call, 6, 0, scene::MAXIMUM_CADENCE_TICKS));
			call.ReturnBoolean(
				ConfigureNpcCadence(call.World(), actor, identity, perception, behaviour, hold)
			);
		}

		void SetNpcServiceCombat(ScriptCall &call) {
			call.ReturnBoolean(SetNpcCombat(call.World(), call.AsInstance(0), NpcScriptBoolean(call, 1)));
		}

		void BeginNpcServiceUpdate(ScriptCall &call) {
			const ecs::Entity actor = call.AsInstance(0);
			const auto domain = NpcScriptDomain(call, 1);
			const uint64_t sourceRevision = NpcScriptInteger(call, 2, 1, MAXIMUM_SOURCE_REVISION);
			const auto admission =
				BeginNpcUpdate(call.World(), call.RuntimeIdentity(), actor, domain, sourceRevision);
			ScriptValue result(ValueTag::Map);
			result.Entries = {
				{"due", NpcValueBoolean(admission.Due)},
				{"reason", NpcValueString(NpcReasonName(admission.Reason))},
				{"token", NpcValueNumber(admission.Token)},
			};
			call.ReturnValue(result);
		}

		void CommitNpcServicePerception(ScriptCall &call) {
			const ecs::Entity actor = call.AsInstance(0);
			const uint64_t token = NpcScriptInteger(call, 1, 1, MAXIMUM_SCRIPT_TOKEN);
			const uint64_t sourceRevision = NpcScriptInteger(call, 2, 1, MAXIMUM_SOURCE_REVISION);
			const bool visible = NpcScriptBoolean(call, 3);
			const core::Vector3 target = NpcScriptVector(call, 4);
			const bool completed = NpcScriptBoolean(call, 5);
			call.ReturnBoolean(CommitNpcPerception(
				call.World(), call.RuntimeIdentity(), actor, token, sourceRevision, visible, target, completed
			));
		}

		void CommitNpcServiceBehaviour(ScriptCall &call) {
			const ecs::Entity actor = call.AsInstance(0);
			const uint64_t token = NpcScriptInteger(call, 1, 1, MAXIMUM_SCRIPT_TOKEN);
			const uint64_t sourceRevision = NpcScriptInteger(call, 2, 1, MAXIMUM_SOURCE_REVISION);
			const core::Vector3 movement = NpcScriptVector(call, 3);
			const bool completed = NpcScriptBoolean(call, 4);
			call.ReturnBoolean(CommitNpcBehaviour(
				call.World(), call.RuntimeIdentity(), actor, token, sourceRevision, movement, completed
			));
		}

		void ReadNpcServicePerception(ScriptCall &call) {
			const auto *state = call.World().Get<NpcPerception>(call.AsInstance(0));
			if (state == nullptr) {
				call.ReturnNil();
				return;
			}
			ScriptValue result(ValueTag::Map);
			result.Entries = {
				{"visible", NpcValueBoolean(state->Visible)},
				{"target_position", NpcValueVector(state->TargetPosition)},
			};
			call.ReturnValue(result);
		}

		void ReadNpcServiceBehaviour(ScriptCall &call) {
			const auto *state = call.World().Get<NpcBehaviour>(call.AsInstance(0));
			if (state == nullptr) {
				call.ReturnNil();
				return;
			}
			call.ReturnVector3(state->MoveDirection);
		}

		void ReadNpcServiceCombatReady(ScriptCall &call) {
			const ecs::Entity actor = call.AsInstance(0);
			const uint64_t sourceRevision = NpcScriptInteger(call, 1, 1, MAXIMUM_SOURCE_REVISION);
			call.ReturnBoolean(NpcCombatReady(call.World(), actor, sourceRevision));
		}

		constexpr std::array<ServiceMethod, 8> NPC_CADENCE_METHODS{{
			{"Configure", ConfigureNpcService},
			{"SetCombat", SetNpcServiceCombat},
			{"BeginUpdate", BeginNpcServiceUpdate},
			{"CommitPerception", CommitNpcServicePerception},
			{"CommitBehaviour", CommitNpcServiceBehaviour},
			{"Perception", ReadNpcServicePerception},
			{"Behaviour", ReadNpcServiceBehaviour},
			{"CombatReady", ReadNpcServiceCombatReady},
		}};
	}

	const ServiceSurface &UpdateCadenceServiceSurface() {
		static const ServiceSurface SURFACE = [] {
			ServiceSurface surface;
			surface.Name = "UpdateCadenceService";
			surface.Methods = NPC_CADENCE_METHODS;
			return surface;
		}();
		return SURFACE;
	}
}
