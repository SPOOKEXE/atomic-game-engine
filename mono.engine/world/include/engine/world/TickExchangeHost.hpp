#pragma once

#include <engine/world/TickExchange.hpp>

namespace engine::world {
	class Universe;
	// Maximum logical exchange rounds accepted in one driver frame.
	inline constexpr uint32_t MAXIMUM_HOST_EXCHANGE_ROUNDS = 1024;

	// Driver commands that advance one host-owned exchange frame.
	enum class TickExchangeOperation : uint8_t { Begin, Collect, Serve, Apply, End, Cancel };

	// One driver-owned phase command. Frame is monotonic for this host link;
	// Round starts at zero. Only Serve carries requests and Apply carries replies.
	struct TickExchangeCommand {
		// Phase operation the host must execute.
		TickExchangeOperation Operation = TickExchangeOperation::Begin;
		// Monotonic driver frame number.
		uint64_t Frame = 0;
		// Zero-based round within Frame.
		uint32_t Round = 0;
		// Wall-frame duration charged to the exchange frame.
		float FrameSeconds = 0;
		// Requests supplied only for the Serve operation.
		std::vector<TickExchangeRequest> Requests;
		// Replies supplied only for the Apply operation.
		std::vector<TickExchangeReply> Replies;
	};

	// Host result for one driver-owned exchange phase.
	struct TickExchangeResult {
		// Operation acknowledged by this result.
		TickExchangeOperation Operation = TickExchangeOperation::Begin;
		// Frame number copied from the command.
		uint64_t Frame = 0;
		// Round number copied from the command.
		uint32_t Round = 0;
		// Whether the operation was valid in the current host phase.
		bool Success = false;
		// Completed rounds at the end of this operation.
		uint32_t Rounds = 0;
		// Requests collected during the Collect operation.
		std::vector<TickExchangeRequest> Requests;
		// Replies produced during the Serve operation.
		std::vector<TickExchangeReply> Replies;
	};

	// Bounded, transactional control codecs. A command cannot decode as a result.
	// Encodes a driver command without permitting it to decode as a result.
	bool WriteTickExchangeControl(core::ByteWriter &writer, const TickExchangeCommand &command);
	// Decodes a complete driver command transactionally.
	bool ReadTickExchangeControl(core::ByteReader &reader, TickExchangeCommand &command);
	// Encodes a host result without permitting it to decode as a command.
	bool WriteTickExchangeControl(core::ByteWriter &writer, const TickExchangeResult &result);
	// Decodes a complete host result transactionally.
	bool ReadTickExchangeControl(core::ByteReader &reader, TickExchangeResult &result);

	// The render-independent host side of the phase handshake. The universe must
	// outlive this adapter. Driver ownership and destination routing are checked
	// outside it; all calls run on the universe's driver thread.
	class TickExchangeHost {
	  public:
		// Binds the adapter to the driver-thread universe it will advance.
		explicit TickExchangeHost(Universe &universe);
		// Cancels any incomplete exchange frame before releasing the adapter.
		~TickExchangeHost();
		TickExchangeHost(const TickExchangeHost &) = delete;
		TickExchangeHost &operator=(const TickExchangeHost &) = delete;
		// Applies one valid driver command and returns its phase result.
		TickExchangeResult Handle(const TickExchangeCommand &command);
		// Cancels an open exchange frame after its driver disconnects.
		void Disconnect();

	  private:
		enum class Phase : uint8_t { Idle, Between, Collected, Served };
		Universe &Worlds;
		Phase Stage = Phase::Idle;
		uint64_t LastFrame = 0;
		uint64_t ActiveFrame = 0;
		uint32_t NextRound = 0;
		std::vector<std::byte> LastCommand;
		TickExchangeResult LastResult;
	};
}
