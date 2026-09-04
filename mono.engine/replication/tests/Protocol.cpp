#include <engine/core/Random.hpp>
#include <engine/replication/Protocol.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

TEST_SUITE_ID("engine.replication.protocol")
TEST_DEPENDS("engine.core.bytes")
TEST_DEPENDS("engine.core.random")

using engine::core::ByteReader;
using engine::core::ByteWriter;
using engine::core::Random;
using engine::replication::Applied;
using engine::replication::Message;
using engine::replication::MessageKind;
using engine::replication::ReadMessage;
using engine::replication::User;
using engine::replication::WriteMessage;

namespace {
	Message Sentinel() {
		Message message;
		message.Kind = MessageKind::User;
		message.User.Bytes = {std::byte{0xC0}, std::byte{0xDE}};
		return message;
	}

	void CheckSentinel(const Message &message) {
		CHECK(message.Kind == MessageKind::User);
		REQUIRE(message.User.Bytes.size() == 2);
		CHECK(message.User.Bytes[0] == std::byte{0xC0});
		CHECK(message.User.Bytes[1] == std::byte{0xDE});
	}
}

TEST_CASE("a replication message refuses a trailing byte", "[replication][protocol]") {
	ByteWriter writer;
	WriteMessage(writer, Applied{.Tick = 42});
	std::vector<std::byte> bytes(writer.Bytes().begin(), writer.Bytes().end());
	bytes.push_back(std::byte{0xA5});

	ByteReader reader(bytes);
	Message message = Sentinel();
	CHECK_FALSE(ReadMessage(reader, message));
	CheckSentinel(message);
}

TEST_CASE("arbitrary replication input cannot produce a partial message", "[.][sandbox][fuzz]") {
	// A failed seed is reproducible by its loop index, and the sentinel makes
	// the public all-or-nothing output contract observable.
	constexpr uint32_t ITERATIONS = 2'000;
	for (uint32_t iteration = 0; iteration < ITERATIONS; iteration++) {
		CAPTURE(iteration);
		const size_t size = Random::Bits(iteration, 1) % 256;
		std::vector<std::byte> bytes(size);
		for (size_t index = 0; index < bytes.size(); index++) {
			bytes[index] = static_cast<std::byte>(Random::Bits(iteration, static_cast<uint32_t>(index) + 2));
		}

		ByteReader reader(bytes);
		Message message = Sentinel();
		if (ReadMessage(reader, message)) {
			CHECK(reader.AtEnd());
		} else {
			CheckSentinel(message);
		}
	}
}
