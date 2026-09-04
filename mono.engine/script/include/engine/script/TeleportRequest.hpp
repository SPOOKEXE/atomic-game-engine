#pragma once

// The authority callback behind `TeleportService.TeleportRequested`.
//
// A client may ask to move only itself. The server gives the request to one
// script callback and only a `Processed` result permits the actual teleport.
// The callback is retained in the runtime that owns this world, never sent over
// a bus or stored in replicated state.

#include <engine/ecs/Entity.hpp>
#include <engine/script/Codec.hpp>
#include <engine/script/Host.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::script {
	class Runtime;

	enum class TeleportRequestDecision : uint8_t {
		NotProcessed,
		Denied,
		Processed,
	};

	std::string_view TeleportRequestDecisionName(TeleportRequestDecision decision);

	struct TeleportRequest {
		ecs::Entity Player;
		std::string Place;
		ScriptValue Data;
	};

	struct TeleportRequestResult {
		TeleportRequestDecision Decision = TeleportRequestDecision::NotProcessed;
		std::string Message;
	};

	// One authority result held for the client runtime's next barrier.
	// `Id` is retained until this arrives, so an unsolicited or repeated network
	// reply cannot call a script.
	struct TeleportResult {
		uint64_t Id = 0;
		TeleportRequestDecision Decision = TeleportRequestDecision::NotProcessed;
		std::string Message;
	};

	// The one callback assigned to a world's TeleportService.
	struct TeleportRequestHandler {
		HostCallback Callback;
	};

	// A client-local request waiting for `Connector::SendUser`. It is separate
	// from the authority handler because a replica may ask but may never act.
	struct PendingTeleportRequest {
		uint64_t Id = 0;
		std::string Place;
		std::vector<std::byte> Data;
	};

	struct TeleportRequestOutbox {
		std::vector<PendingTeleportRequest> Pending;
		std::vector<uint64_t> AwaitingResults;
		uint64_t NextId = 1;
	};

	// Registers the retained callback resource and its script-visible decision
	// enum before the component table is sealed.
	void RegisterTeleportRequestComponents();

	// Encodes and queues one local player's request. The request stays queued
	// until the client transport accepted it.
	bool QueueTeleportRequest(
		ecs::Store &store, std::string_view place, const ScriptValue &data, std::string &failure
	);

	std::span<const PendingTeleportRequest> PendingTeleportRequests(const ecs::Store &store);
	void MarkTeleportRequestSent(ecs::Store &store);
	bool AcceptTeleportResult(ecs::Store &store, uint64_t id);

	// Queues an authoritative teleport and removes the player only after the
	// router has copied the envelope. `failure` is set when no teleport occurred.
	bool TeleportPlayer(
		ecs::Store &store,
		std::string_view place,
		ecs::Entity player,
		const ScriptValue *data,
		std::string &failure
	);

	// Runs the assigned handler and validates its returned contract object.
	// A missing, failing, or malformed callback is NotProcessed.
	TeleportRequestResult
	DispatchTeleportRequest(Runtime &runtime, ecs::Store &store, const TeleportRequest &request);
}
