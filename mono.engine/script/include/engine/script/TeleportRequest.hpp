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

	// Authority outcomes for one teleport request.
	enum class TeleportRequestDecision : uint8_t {
		NotProcessed,
		Denied,
		Processed,
	};

	// Returns the stable script-visible spelling of an authority outcome.
	std::string_view TeleportRequestDecisionName(TeleportRequestDecision decision);

	// Client or authority request to move one player to a named place.
	struct TeleportRequest {
		// Player whose authority or replica initiated the request.
		ecs::Entity Player;
		// Destination place name.
		std::string Place;
		// VM-neutral serialized value.
		ScriptValue Data;
	};

	// Authority response delivered to the requesting script.
	struct TeleportRequestResult {
		// Authority outcome that determines whether the request proceeds.
		TeleportRequestDecision Decision = TeleportRequestDecision::NotProcessed;
		// Human-readable refusal detail supplied by the authority.
		std::string Message;
	};

	// One authority result held for the client runtime's next barrier.
	// `Id` is retained until this arrives, so an unsolicited or repeated network
	// reply cannot call a script.
	struct TeleportResult {
		// Locally assigned request id used to reject unsolicited replies.
		uint64_t Id = 0;
		// Authority decision.
		TeleportRequestDecision Decision = TeleportRequestDecision::NotProcessed;
		// Authority message.
		std::string Message;
	};

	// The one callback assigned to a world's TeleportService.
	struct TeleportRequestHandler {
		// VM-owned callback handle.
		HostCallback Callback;
	};

	// A client-local request waiting for `Connector::SendUser`. It is separate
	// from the authority handler because a replica may ask but may never act.
	struct PendingTeleportRequest {
		// Stable identifier.
		uint64_t Id = 0;
		// Destination place name.
		std::string Place;
		// VM-neutral serialized value.
		std::vector<std::byte> Data;
	};

	// A teleport  request outbox value.
	struct TeleportRequestOutbox {
		// Requests awaiting transport.
		std::vector<PendingTeleportRequest> Pending;
		// Awaiting results.
		std::vector<uint64_t> AwaitingResults;
		// Next local request identifier.
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

	// Returns client requests that still need connector transport.
	std::span<const PendingTeleportRequest> PendingTeleportRequests(const ecs::Store &store);
	// Moves queued requests into the result-waiting set after transport accepts them.
	void MarkTeleportRequestSent(ecs::Store &store);
	// Accepts teleport result.
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
