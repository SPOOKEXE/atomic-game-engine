#pragma once

#include <engine/core/Bytes.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::world {
	inline constexpr size_t MAXIMUM_TICK_EXCHANGE_MESSAGES = 256;
	inline constexpr size_t MAXIMUM_TICK_EXCHANGE_BYTES = 1024 * 1024;

	// All identities are host-stamped. Sequence identifies a request within this
	// source endpoint's logical tick, never an entity in another world.
	struct TickExchangeStamp {
		std::string SourceWorld;
		std::string DestinationWorld;
		std::string Channel;
		uint64_t SourceIncarnation = 0;
		uint64_t SourceTick = 0;
		uint64_t Sequence = 0;
		bool operator==(const TickExchangeStamp &) const = default;
	};

	struct TickExchangeRequest {
		TickExchangeStamp Stamp;
		std::vector<std::byte> Payload;
	};

	enum class TickExchangeStatus : uint8_t { Complete, Unavailable, Stale, Overflow, Refused, Cancelled };

	struct TickExchangeReply {
		TickExchangeStamp Stamp;
		uint64_t DestinationIncarnation = 0;
		uint64_t DestinationTick = 0;
		TickExchangeStatus Status = TickExchangeStatus::Unavailable;
		std::vector<std::byte> Payload;
	};

	// These run only at a joined phase boundary and only on the addressed world's
	// owning lane. Callbacks never receive another world's storage.
	struct TickExchangeChannel {
		std::string Name;
		void (*Collect)(ecs::Store &, std::vector<TickExchangeRequest> &) = nullptr;
		TickExchangeStatus (*Serve)(ecs::Store &, const TickExchangeRequest &, std::vector<std::byte> &) =
			nullptr;
		void (*Apply)(ecs::Store &, std::span<const TickExchangeReply>) = nullptr;
	};

	bool RegisterTickExchangeChannel(const TickExchangeChannel &channel);
	void RegisterTickExchangeComponents();
	bool OpenTickExchange(ecs::Store &store, std::string_view channel, uint64_t incarnation);
	bool HasTickExchanges(const ecs::Store &store);

	// A failed collection is a failed logical round, never an empty clear path.
	bool
	CollectTickExchanges(ecs::Store &store, std::string_view world, std::vector<TickExchangeRequest> &out);
	TickExchangeReply ServeTickExchange(ecs::Store &store, const TickExchangeRequest &request);
	bool ApplyTickExchanges(ecs::Store &store, std::span<const TickExchangeReply> replies);

	bool WriteTickExchange(core::ByteWriter &writer, const TickExchangeRequest &request);
	bool ReadTickExchange(core::ByteReader &reader, TickExchangeRequest &request);
	bool WriteTickExchange(core::ByteWriter &writer, const TickExchangeReply &reply);
	bool ReadTickExchange(core::ByteReader &reader, TickExchangeReply &reply);
}
