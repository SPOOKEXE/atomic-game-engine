#include <engine/script/RemoteEvent.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_SUITE_ID("engine.script.remoteevent")

TEST_CASE("RemoteEvent framing bounds the complete path and payload", "[script][remote-event]") {
	using namespace engine::script;
	const std::vector<std::byte> payload(REMOTE_EVENT_MAXIMUM_PAYLOAD_BYTES, std::byte{0x7f});
	const std::string fittingPath(REMOTE_EVENT_MAXIMUM_MESSAGE_BYTES - payload.size() - 14, 'p');
	std::vector<std::byte> encoded;
	REQUIRE(EncodeRemoteEvent(fittingPath, payload, encoded));
	REQUIRE(encoded.size() == REMOTE_EVENT_MAXIMUM_MESSAGE_BYTES);
	RemoteEventMessage decoded;
	REQUIRE(DecodeRemoteEvent(encoded, decoded));
	CHECK(decoded.Event == fittingPath);
	CHECK(decoded.Payload == payload);
	CHECK_FALSE(EncodeRemoteEvent(fittingPath + "p", payload, encoded));
	CHECK(encoded.empty());
	CHECK_FALSE(EncodeRemoteEvent(std::string(REMOTE_EVENT_MAXIMUM_MESSAGE_BYTES, 'p'), {}, encoded));
	CHECK(encoded.empty());
}
