#pragma once

#include <engine/world/TickExchange.hpp>

namespace engine::world {
	class Universe;
	inline constexpr uint32_t MAXIMUM_HOST_EXCHANGE_ROUNDS = 1024;

	enum class TickExchangeOperation : uint8_t { Begin, Collect, Serve, Apply, End, Cancel };

	// One driver-owned phase command. Frame is monotonic for this host link;
	// Round starts at zero. Only Serve carries requests and Apply carries replies.
	struct TickExchangeCommand {
		TickExchangeOperation Operation = TickExchangeOperation::Begin;
		uint64_t Frame = 0;
		uint32_t Round = 0;
		float FrameSeconds = 0;
		std::vector<TickExchangeRequest> Requests;
		std::vector<TickExchangeReply> Replies;
	};

	struct TickExchangeResult {
		TickExchangeOperation Operation = TickExchangeOperation::Begin;
		uint64_t Frame = 0;
		uint32_t Round = 0;
		bool Success = false;
		uint32_t Rounds = 0;
		std::vector<TickExchangeRequest> Requests;
		std::vector<TickExchangeReply> Replies;
	};

	// Bounded, transactional control codecs. A command cannot decode as a result.
	bool WriteTickExchangeControl(core::ByteWriter &writer, const TickExchangeCommand &command);
	bool ReadTickExchangeControl(core::ByteReader &reader, TickExchangeCommand &command);
	bool WriteTickExchangeControl(core::ByteWriter &writer, const TickExchangeResult &result);
	bool ReadTickExchangeControl(core::ByteReader &reader, TickExchangeResult &result);

	// The render-independent host side of the phase handshake. The universe must
	// outlive this adapter. Driver ownership and destination routing are checked
	// outside it; all calls run on the universe's driver thread.
	class TickExchangeHost {
	  public:
		explicit TickExchangeHost(Universe &universe);
		~TickExchangeHost();
		TickExchangeHost(const TickExchangeHost &) = delete;
		TickExchangeHost &operator=(const TickExchangeHost &) = delete;
		TickExchangeResult Handle(const TickExchangeCommand &command);
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
