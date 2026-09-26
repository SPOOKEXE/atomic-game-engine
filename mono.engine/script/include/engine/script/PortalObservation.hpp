#pragma once

// Fixed-boundary, read-only observations of portal transfers and the input that
// follows a player through them.
//
// Records rather than callbacks, for the reason `physics/Observation.hpp` gives:
// the transfer service copies a small value into a bounded world resource at a
// declared point, and a host copies completed records after the tick. Nothing
// here runs host code inside the tick or exposes world state.
//
// Each record carries an optional trace id. It comes from the subject's
// `scene::ObservationTrace` when present, which travels with the body through
// a portal, and otherwise from the world default a host sets. A host that gives
// every process the same default can read one crossing across all of them.
//
// @tier L9 · shared

#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::script {

	// Stable discovery names. These strings, never the enum values, identify
	// portal observations outside this process.
	inline constexpr std::string_view PORTAL_CROSSING_OBSERVATION = "portal.crossing";
	// Discovery name for a transferred body admitted by its destination.
	inline constexpr std::string_view PORTAL_ARRIVAL_OBSERVATION = "portal.arrival";
	// Discovery name for player input applied, scheduled or skipped around a transfer.
	inline constexpr std::string_view PORTAL_INPUT_OBSERVATION = "portal.input";
	// Discovery name for transfer handoff stage changes on either side.
	inline constexpr std::string_view PORTAL_HANDOFF_OBSERVATION = "portal.handoff";

	// The complete discovery surface in declaration order.
	std::span<const std::string_view> PortalObservationHooks();

	// Bounded, null-terminated text copied into a record.
	template <size_t Bytes> struct ObservationText {
		std::array<char, Bytes> Value{};
		void Assign(std::string_view text) {
			const size_t copied = text.size() < Bytes - 1 ? text.size() : Bytes - 1;
			for (size_t index = 0; index < copied; ++index)
				Value[index] = text[index];
			Value[copied] = '\0';
		}
		std::string_view View() const {
			return Value.data();
		}
	};

	// Shared by every portal record.
	struct PortalObservationStamp {
		// World tick the record was taken in.
		uint64_t Tick = 0;
		// Monotonic per world log, across all four hooks, starting at 1.
		uint64_t Sequence = 0;
		// Optional trace id; zero when neither the subject nor the world has one.
		uint64_t Trace = 0;
		// Local entity id of the subject in the recording world.
		uint64_t Subject = 0;
	};

	// A body's path met a pane and the source tried to start a transfer.
	struct PortalCrossingRecord {
		PortalObservationStamp Stamp;
		// True when a transfer began; false when `Reason` says why it did not.
		bool Begun = false;
		// True when the segment from the previous pose crossed; false when it was
		// reconstructed from velocity.
		bool Direct = false;
		// Transfer sequence when `Begun`.
		uint64_t Transfer = 0;
		// Signed distances from the pane plane before and after the step.
		float PriorOffset = 0;
		float CurrentOffset = 0;
		core::Vector3 Prior;
		core::Vector3 Current;
		ObservationText<65> Destination;
		ObservationText<97> Reason;
	};

	// The destination admitted a transferred body.
	struct PortalArrivalRecord {
		PortalObservationStamp Stamp;
		uint64_t Transfer = 0;
		// Where the body was placed after the destination suffix resolved.
		core::Vector3 Position;
		// Input the committed baseline already includes.
		uint64_t BaselineInputTick = 0;
		ObservationText<65> Source;
	};

	// How one player input reached the humanoid.
	enum class PortalInputRoute : uint8_t {
		// Applied on arrival with no portal clock.
		Native,
		// Queued against the portal clock and applied at its due tick.
		Scheduled,
		// Forwarded by the previous host and applied here.
		Forwarded,
		// Forwarded, but already contained in the committed baseline, so skipped.
		Included,
		// Forwarded while the source body was sealed and frozen, and moved through
		// at once so this world catches up with the client instead of lagging it.
		CaughtUp,
	};

	// One player input applied or skipped.
	struct PortalInputRecord {
		PortalObservationStamp Stamp;
		PortalInputRoute Route = PortalInputRoute::Native;
		// Client input tick.
		uint64_t InputTick = 0;
		// Direction written to the humanoid, in this world's frame.
		core::Vector3 Direction;
	};

	// A handoff stage change, recorded by whichever side made it.
	enum class PortalHandoffEvent : uint8_t {
		// Source offered the body to the destination.
		Offered,
		// Source sealed the baseline H and stopped simulating the body.
		Sealed,
		// Source retired its body and sent Commit.
		Committing,
		// Destination admitted the committed body.
		Committed,
		// Source restored the body after a refusal or cancellation.
		Refused,
		// Forwarded input for this transfer closed on this side.
		InputClosed,
		// Host session: the destination answered the source with a player route.
		Routed,
		// Host session: the source told its client to join the destination.
		ClientNotified,
		// Host session: the client proceeded and the source admitted the transfer.
		Admitted,
		// Host session: the source told its client the body has crossed.
		CrossedNotified,
	};

	// One handoff stage change.
	struct PortalHandoffRecord {
		PortalObservationStamp Stamp;
		PortalHandoffEvent Event = PortalHandoffEvent::Offered;
		uint64_t Transfer = 0;
		// Input tick the event concerns: the sealed baseline's for Sealed and
		// Committed, the last applied for InputClosed, zero otherwise.
		uint64_t InputTick = 0;
		// The other world of the transfer.
		ObservationText<65> Peer;
	};

	// Stable text for records that leave the process.
	std::string_view Describe(PortalInputRoute route);
	std::string_view Describe(PortalHandoffEvent event);

	// Per-world, overwrite-on-full storage with one ring per hook. A fixed capacity
	// keeps observation from becoming a backlog when no consumer is reading.
	template <class Record, size_t Capacity> class PortalObservationRing {
	  public:
		void Append(const Record &record) {
			if (Count == Capacity) {
				First = (First + 1) % Capacity;
				--Count;
				++OverwrittenRecords;
			}
			Records[(First + Count) % Capacity] = record;
			++Count;
		}
		std::vector<Record> Copy() const {
			std::vector<Record> copied;
			copied.reserve(Count);
			for (size_t index = 0; index < Count; ++index)
				copied.push_back(Records[(First + index) % Capacity]);
			return copied;
		}
		uint64_t Overwritten() const {
			return OverwrittenRecords;
		}

	  private:
		std::array<Record, Capacity> Records{};
		size_t First = 0;
		size_t Count = 0;
		uint64_t OverwrittenRecords = 0;
	};

	// The world resource holding every portal ring, about 64 KiB per world.
	struct PortalObservationLog {
		// Crossings, arrivals and handoffs are a few rows per transfer.
		static constexpr size_t EVENT_CAPACITY = 64;
		// Input is one row per applied tick per player: several seconds of one.
		static constexpr size_t INPUT_CAPACITY = 512;

		// Trace id for records whose subject carries none.
		uint64_t DefaultTrace = 0;
		uint64_t NextSequence = 1;
		PortalObservationRing<PortalCrossingRecord, EVENT_CAPACITY> Crossings;
		PortalObservationRing<PortalArrivalRecord, EVENT_CAPACITY> Arrivals;
		PortalObservationRing<PortalInputRecord, INPUT_CAPACITY> Inputs;
		PortalObservationRing<PortalHandoffRecord, EVENT_CAPACITY> Handoffs;
	};

	// Value copies of every retained record, each list in production order.
	struct PortalObservationCopy {
		std::vector<PortalCrossingRecord> Crossings;
		std::vector<PortalArrivalRecord> Arrivals;
		std::vector<PortalInputRecord> Inputs;
		std::vector<PortalHandoffRecord> Handoffs;
		// Records lost because a ring was full, summed over the four hooks.
		uint64_t Overwritten = 0;
	};

	// Sets the trace id used when a subject carries no `scene::ObservationTrace`.
	// Creates the log when a world configured for transfers has none yet.
	void SetPortalObservationTrace(ecs::Store &store, uint64_t trace);

	// Copies retained records. The caller owns the result and receives no access
	// to transfer state.
	PortalObservationCopy CopyPortalObservations(const ecs::Store &store);

	// Records a host session stage (`Routed` through `CrossedNotified`) for a
	// transfer, so the player session and the transfer read as one trail.
	void ObservePortalSession(
		ecs::Store &store,
		ecs::Entity player,
		PortalHandoffEvent event,
		uint64_t transfer,
		std::string_view peer
	);

	// Records one input a host applied straight to the humanoid, outside the
	// portal clock. Hosts call this; the transfer service records its own routes.
	void ObservePortalNativeInput(
		ecs::Store &store, ecs::Entity player, uint64_t inputTick, const core::Vector3 &direction
	);
}
