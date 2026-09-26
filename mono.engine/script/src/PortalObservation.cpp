#include "PortalObservation.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/PortalCrossing.hpp>

#include <array>

namespace engine::script {

	namespace {
		constexpr std::array<std::string_view, 4> HOOKS = {
			PORTAL_CROSSING_OBSERVATION,
			PORTAL_ARRIVAL_OBSERVATION,
			PORTAL_INPUT_OBSERVATION,
			PORTAL_HANDOFF_OBSERVATION,
		};

		// Observation is process-local evidence, not world state: a snapshot keeps
		// none of it and a restored world starts with an empty log.
		void WriteLogs(core::ByteWriter &, const void *, size_t) {}
		void ReadLogs(core::ByteReader &, void *destination, size_t count) {
			auto *logs = static_cast<PortalObservationLog *>(destination);
			for (size_t index = 0; index < count; ++index)
				logs[index] = PortalObservationLog{};
		}

		uint64_t TraceOf(const ecs::Store &store, ecs::Entity entity) {
			if (entity == ecs::NULL_ENTITY || !store.Alive(entity)) return 0;
			const auto *trace = store.Get<scene::ObservationTrace>(entity);
			return trace ? trace->Id : 0;
		}
	}

	std::span<const std::string_view> PortalObservationHooks() {
		return HOOKS;
	}

	std::string_view Describe(PortalInputRoute route) {
		switch (route) {
		case PortalInputRoute::Native:
			return "native";
		case PortalInputRoute::Scheduled:
			return "scheduled";
		case PortalInputRoute::Forwarded:
			return "forwarded";
		case PortalInputRoute::Included:
			return "included";
		case PortalInputRoute::CaughtUp:
			return "caught-up";
		}
		return {};
	}

	std::string_view Describe(PortalHandoffEvent event) {
		switch (event) {
		case PortalHandoffEvent::Offered:
			return "offered";
		case PortalHandoffEvent::Sealed:
			return "sealed";
		case PortalHandoffEvent::Committing:
			return "committing";
		case PortalHandoffEvent::Committed:
			return "committed";
		case PortalHandoffEvent::Refused:
			return "refused";
		case PortalHandoffEvent::InputClosed:
			return "input-closed";
		case PortalHandoffEvent::Routed:
			return "routed";
		case PortalHandoffEvent::ClientNotified:
			return "client-notified";
		case PortalHandoffEvent::Admitted:
			return "admitted";
		case PortalHandoffEvent::CrossedNotified:
			return "crossed-notified";
		}
		return {};
	}

	void SetPortalObservationTrace(ecs::Store &store, uint64_t trace) {
		portal_observation::Prepare(store);
		if (auto *log = store.ResourceMutable<PortalObservationLog>()) log->DefaultTrace = trace;
	}

	PortalObservationCopy CopyPortalObservations(const ecs::Store &store) {
		PortalObservationCopy copy;
		if (!ecs::Components::Assigned<PortalObservationLog>().IsValid()) return copy;
		const auto *log = store.Resource<PortalObservationLog>();
		if (log == nullptr) return copy;
		copy.Crossings = log->Crossings.Copy();
		copy.Arrivals = log->Arrivals.Copy();
		copy.Inputs = log->Inputs.Copy();
		copy.Handoffs = log->Handoffs.Copy();
		copy.Overwritten = log->Crossings.Overwritten() + log->Arrivals.Overwritten() +
						   log->Inputs.Overwritten() + log->Handoffs.Overwritten();
		return copy;
	}

	void ObservePortalSession(
		ecs::Store &store,
		ecs::Entity player,
		PortalHandoffEvent event,
		uint64_t transfer,
		std::string_view peer
	) {
		portal_observation::Handoff(store, player, ecs::NULL_ENTITY, event, transfer, 0, peer);
	}

	void ObservePortalNativeInput(
		ecs::Store &store, ecs::Entity player, uint64_t inputTick, const core::Vector3 &direction
	) {
		portal_observation::Input(store, player, PortalInputRoute::Native, inputTick, direction);
	}

	namespace portal_observation {

		void Register() {
			if (!ecs::Components::Assigned<PortalObservationLog>().IsValid())
				ecs::Components::Register<PortalObservationLog>(
					"script.PortalObservationLog", WriteLogs, ReadLogs
				);
		}

		void Prepare(ecs::Store &store) {
			Register();
			if (!store.HasResource<PortalObservationLog>()) store.SetResource(PortalObservationLog{});
		}

		PortalObservationLog *
		Begin(ecs::Store &store, ecs::Entity subject, ecs::Entity root, PortalObservationStamp &stamp) {
			if (!ecs::Components::Assigned<PortalObservationLog>().IsValid()) return nullptr;
			auto *log = store.ResourceMutable<PortalObservationLog>();
			if (log == nullptr) return nullptr;
			stamp.Tick = store.Time().Tick;
			stamp.Sequence = log->NextSequence++;
			stamp.Subject = subject.Id;
			stamp.Trace = TraceOf(store, subject);
			if (stamp.Trace == 0) stamp.Trace = TraceOf(store, root);
			if (stamp.Trace == 0) stamp.Trace = log->DefaultTrace;
			return log;
		}

		void Handoff(
			ecs::Store &store,
			ecs::Entity subject,
			ecs::Entity root,
			PortalHandoffEvent event,
			uint64_t transfer,
			uint64_t inputTick,
			std::string_view peer
		) {
			PortalHandoffRecord record;
			auto *log = Begin(store, subject, root, record.Stamp);
			if (log == nullptr) return;
			record.Event = event;
			record.Transfer = transfer;
			record.InputTick = inputTick;
			record.Peer.Assign(peer);
			log->Handoffs.Append(record);
		}

		void Input(
			ecs::Store &store,
			ecs::Entity subject,
			PortalInputRoute route,
			uint64_t inputTick,
			const core::Vector3 &direction
		) {
			PortalInputRecord record;
			const auto *rig = store.Alive(subject)
								  ? store.Get<scene::Character>(scene::CharacterOf(store, subject))
								  : nullptr;
			auto *log = Begin(store, subject, rig ? rig->Root : ecs::NULL_ENTITY, record.Stamp);
			if (log == nullptr) return;
			record.Route = route;
			record.InputTick = inputTick;
			record.Direction = direction;
			log->Inputs.Append(record);
		}
	}
}
