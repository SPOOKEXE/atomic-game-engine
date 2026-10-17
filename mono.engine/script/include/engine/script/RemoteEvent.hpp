#pragma once

// The bounded value envelope carried by a RemoteEvent over replication's opaque
// user lane. This module owns its meaning; replication only carries the bytes.
//
// @tier L9 · shared

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::script {

	// A decoded client request addressed to one event in the authority's world.
	struct RemoteEventMessage {
		// Full instance path, resolved against the authority's class tree.
		std::string Event;
		// Copied application payload, interpreted by the event's listener.
		std::vector<std::byte> Payload;
	};

	// Maximum application payload in one event request.
	inline constexpr size_t REMOTE_EVENT_MAXIMUM_PAYLOAD_BYTES = 4096;
	// Maximum framed request size, including the event path.
	inline constexpr size_t REMOTE_EVENT_MAXIMUM_MESSAGE_BYTES = 4352;
	// Identifies this user-lane message before the authority dispatches it.
	inline constexpr uint32_t REMOTE_EVENT_MAGIC = 0x52455654u;
	// Format version for the framed request.
	inline constexpr uint16_t REMOTE_EVENT_VERSION = 1;

	// Encodes one client request. An empty event path or an over-limit payload
	// is refused before it reaches a transport outbox.
	// @param event Full path of the target RemoteEvent.
	// @param payload Application bytes to copy into the envelope.
	// @param out Receives the framed request on success and is cleared on refusal.
	// @return Whether the request fits the envelope bounds.
	bool EncodeRemoteEvent(
		std::string_view event, std::span<const std::byte> payload, std::vector<std::byte> &out
	);

	// Decodes one complete request. Extra bytes, invalid names, and over-limit
	// fields are refused so an authority never dispatches an ambiguous payload.
	// @param bytes Complete user-lane message to inspect.
	// @param out Receives the decoded request only on success.
	// @return Whether the message is a valid RemoteEvent request.
	bool DecodeRemoteEvent(std::span<const std::byte> bytes, RemoteEventMessage &out);
}
