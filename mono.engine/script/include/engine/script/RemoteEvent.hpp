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

	struct RemoteEventMessage {
		std::string Event;
		std::vector<std::byte> Payload;
	};

	inline constexpr size_t REMOTE_EVENT_MAXIMUM_PAYLOAD_BYTES = 4096;
	inline constexpr size_t REMOTE_EVENT_MAXIMUM_MESSAGE_BYTES = 4352;
	inline constexpr uint32_t REMOTE_EVENT_MAGIC = 0x52455654u;
	inline constexpr uint16_t REMOTE_EVENT_VERSION = 1;

	// Encodes one client request. An empty event path or an over-limit payload
	// is refused before it reaches a transport outbox.
	bool EncodeRemoteEvent(std::string_view event, std::span<const std::byte> payload, std::vector<std::byte> &out);

	// Decodes one complete request. Extra bytes, invalid names, and over-limit
	// fields are refused so an authority never dispatches an ambiguous payload.
	bool DecodeRemoteEvent(std::span<const std::byte> bytes, RemoteEventMessage &out);
}
