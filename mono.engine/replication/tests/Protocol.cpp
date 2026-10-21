#include <engine/core/Random.hpp>
#include <engine/ecs/Schema.hpp>
#include <engine/replication/Protocol.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

TEST_SUITE_ID("engine.replication.protocol")
TEST_DEPENDS("engine.core.bytes")
TEST_DEPENDS("engine.core.random")
TEST_TIER("fast")

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

TEST_CASE("entity lists retain the scalar little endian wire", "[replication][protocol]") {
	for (const size_t count : {size_t{0}, size_t{1}, size_t{257}, size_t{4096}}) {
		CAPTURE(count);
		engine::replication::Structure structure;
		structure.Tick = 0x0123456789ABCDEF;
		for (size_t index = 0; index < count; index++) {
			structure.Created.emplace_back(std::numeric_limits<uint64_t>::max() - index * 0x0102030405ULL);
		}
		structure.Destroyed = {engine::ecs::Entity{0}, engine::ecs::Entity{0x8877665544332211}};
		structure.Forgotten = {engine::ecs::Entity{0xFEDCBA9876543210}};

		ByteWriter scalar;
		scalar.WriteUInt16(engine::replication::PROTOCOL_VERSION);
		scalar.WriteUInt8(static_cast<uint8_t>(MessageKind::Structure));
		scalar.WriteUInt64(structure.Tick);
		for (const auto *entities : {&structure.Created, &structure.Destroyed, &structure.Forgotten}) {
			scalar.WriteUInt32(static_cast<uint32_t>(entities->size()));
			for (const auto entity : *entities)
				scalar.WriteUInt64(entity.Id);
		}
		ByteWriter encoded;
		WriteMessage(encoded, structure);
		CHECK(
			std::vector<std::byte>(encoded.Bytes().begin(), encoded.Bytes().end()) ==
			std::vector<std::byte>(scalar.Bytes().begin(), scalar.Bytes().end())
		);
		ByteReader reader(scalar.Bytes());
		Message decoded;
		REQUIRE(ReadMessage(reader, decoded));
		CHECK(decoded.Structure.Tick == structure.Tick);
		CHECK(decoded.Structure.Created == structure.Created);
		CHECK(decoded.Structure.Destroyed == structure.Destroyed);
		CHECK(decoded.Structure.Forgotten == structure.Forgotten);

		for (size_t length = 0; length < encoded.Size(); length++) {
			ByteReader truncated(encoded.Bytes().first(length));
			Message message = Sentinel();
			CHECK_FALSE(ReadMessage(truncated, message));
			CheckSentinel(message);
		}
	}
}

TEST_CASE("entity lists refuse excessive counts before reading rows", "[replication][protocol]") {
	ByteWriter writer;
	writer.WriteUInt16(engine::replication::PROTOCOL_VERSION);
	writer.WriteUInt8(static_cast<uint8_t>(MessageKind::Structure));
	writer.WriteUInt64(1);
	writer.WriteUInt32(engine::replication::MAXIMUM_ENTRIES + 1);
	writer.WriteUInt64(0x8877665544332211);
	ByteReader reader(writer.Bytes());
	Message message = Sentinel();
	CHECK_FALSE(ReadMessage(reader, message));
	CHECK(reader.Remaining() == sizeof(uint64_t));
	CheckSentinel(message);
}

TEST_CASE("a schema chunk refuses an oversized payload before reading it", "[replication][protocol]") {
	const std::vector<std::byte> payload(engine::ecs::Schemas::MAXIMUM_DEFINITION_BYTES + 1);
	ByteWriter writer;
	writer.WriteUInt16(engine::replication::PROTOCOL_VERSION);
	writer.WriteUInt8(static_cast<uint8_t>(MessageKind::Schemas));
	writer.WriteString("test.SchemaPayloadBound");
	writer.WriteUInt32(sizeof(uint32_t));
	writer.WriteUInt32(0);
	writer.WriteUInt32(static_cast<uint32_t>(payload.size()));
	writer.WriteRaw(payload.data(), payload.size());

	ByteReader reader(writer.Bytes());
	Message message = Sentinel();
	CHECK_FALSE(ReadMessage(reader, message));
	CHECK(reader.Remaining() == payload.size());
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
