#pragma once

#include <engine/core/Bytes.hpp>

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::world {
	// Maximum requests or replies admitted in one exchange round.
	inline constexpr size_t MAXIMUM_TICK_EXCHANGE_MESSAGES = 256;
	// Maximum aggregate payload bytes admitted in one exchange round.
	inline constexpr size_t MAXIMUM_TICK_EXCHANGE_BYTES = 1024 * 1024;

	// All identities are host-stamped. Sequence identifies a request within this
	// source endpoint's logical tick, never an entity in another world.
	struct TickExchangeStamp {
		// Stable name of the world that collected the request.
		std::string SourceWorld;
		// Stable name of the world that must serve the request.
		std::string DestinationWorld;
		// Named exchange channel opened by both worlds.
		std::string Channel;
		// Source endpoint incarnation that invalidates stale requests.
		uint64_t SourceIncarnation = 0;
		// Source logical tick that owns this request sequence.
		uint64_t SourceTick = 0;
		// Per-source-tick request order number.
		uint64_t Sequence = 0;
		// Compares the full cross-world request identity.
		bool operator==(const TickExchangeStamp &) const = default;
	};

	// Collected request routed to one addressed world at a phase boundary.
	struct TickExchangeRequest {
		// Source, destination, channel, and logical ordering identity.
		TickExchangeStamp Stamp;
		// Owned channel-defined request bytes.
		std::vector<std::byte> Payload;
	};

	// Per-request outcome reported after the destination phase.
	enum class TickExchangeStatus : uint8_t { Complete, Unavailable, Stale, Overflow, Refused, Cancelled };

	// Destination response paired with one collected request.
	struct TickExchangeReply {
		// Identity copied from the matched request.
		TickExchangeStamp Stamp;
		// Destination endpoint incarnation that served the request.
		uint64_t DestinationIncarnation = 0;
		// Destination logical tick at which the request was served.
		uint64_t DestinationTick = 0;
		// Completion or refusal outcome.
		TickExchangeStatus Status = TickExchangeStatus::Unavailable;
		// Owned channel-defined reply bytes.
		std::vector<std::byte> Payload;
	};

	// One opaque value record emitted at the joined fixed-step barrier. The
	// generic world layer routes bytes only; game-level owners interpret them.
	struct FixedStepBarrierRecord {
		// Stable source world name.
		std::string World;
		// Opaque value emitted by the source world.
		std::vector<std::byte> Payload;
	};

	// Opaque hooks around the fixed-step boundary between Simulation and
	// Physics. World owns the order and transport; the caller owns the byte
	// format and the one global resolve step.
	struct FixedStepBarrierCallbacks {
		// Collects one world's opaque barrier payload.
		std::function<void(ecs::Store &, std::vector<std::byte> &)> Collect;
		// Resolves collected payloads into addressed records.
		std::function<bool(std::span<const FixedStepBarrierRecord>, std::vector<FixedStepBarrierRecord> &)>
			Resolve;
		// Applies one resolved payload to its owning world.
		std::function<bool(ecs::Store &, std::span<const std::byte>)> Apply;

		// Reports whether all three barrier callbacks are callable.
		bool Valid() const {
			return Collect && Resolve && Apply;
		}
	};

	// These run only at a joined phase boundary and only on the addressed world's
	// owning lane. Callbacks never receive another world's storage.
	struct TickExchangeChannel {
		// Stable channel name used in cross-world stamps.
		std::string Name;
		// Collects source-world requests for the current logical tick.
		void (*Collect)(ecs::Store &, std::vector<TickExchangeRequest> &) = nullptr;
		// Serves one addressed request on the destination lane.
		TickExchangeStatus (*Serve)(ecs::Store &, const TickExchangeRequest &, std::vector<std::byte> &) =
			nullptr;
		// Applies replies on the source world's owning lane.
		void (*Apply)(ecs::Store &, std::span<const TickExchangeReply>) = nullptr;
	};

	// Registers one globally named exchange channel before worlds tick.
	bool RegisterTickExchangeChannel(const TickExchangeChannel &channel);
	// Registers ECS components used to retain exchange endpoints.
	void RegisterTickExchangeComponents();
	// Opens a registered exchange channel for one world incarnation.
	bool OpenTickExchange(ecs::Store &store, std::string_view channel, uint64_t incarnation);
	// Reports whether the world has any open exchange channels.
	bool HasTickExchanges(const ecs::Store &store);

	// A failed collection is a failed logical round, never an empty clear path.
	bool
	CollectTickExchanges(ecs::Store &store, std::string_view world, std::vector<TickExchangeRequest> &out);
	// Serves one validated request on its addressed world's lane.
	TickExchangeReply ServeTickExchange(ecs::Store &store, const TickExchangeRequest &request);
	// Applies all replies belonging to one source-world phase.
	bool ApplyTickExchanges(ecs::Store &store, std::span<const TickExchangeReply> replies);

	// Encodes a bounded exchange request.
	bool WriteTickExchange(core::ByteWriter &writer, const TickExchangeRequest &request);
	// Decodes one exchange request transactionally.
	bool ReadTickExchange(core::ByteReader &reader, TickExchangeRequest &request);
	// Encodes a bounded exchange reply.
	bool WriteTickExchange(core::ByteWriter &writer, const TickExchangeReply &reply);
	// Decodes one exchange reply transactionally.
	bool ReadTickExchange(core::ByteReader &reader, TickExchangeReply &reply);
}
