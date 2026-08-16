#include <engine/core/Bytes.hpp>
#include <engine/game/Content.hpp>
#include <engine/game/Play.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.game.content")

using engine::game::ContentChunk;
using engine::game::ContentDirectory;
using engine::game::ContentEndpoint;
using engine::game::ContentRefusal;
using engine::game::ContentRouteRequest;
using engine::game::DecodeContentChunk;
using engine::game::DecodeContentDirectory;
using engine::game::DecodeContentRefusal;
using engine::game::DecodeContentRequest;
using engine::game::EncodeContentChunk;
using engine::game::EncodeContentDirectory;
using engine::game::EncodeContentRefusal;
using engine::game::EncodeContentRequest;
using engine::game::MAXIMUM_CONTENT_CHUNK_BYTES;
using engine::game::MAXIMUM_CONTENT_ENDPOINTS;
using engine::game::MAXIMUM_ROUTE_BYTES;
using engine::game::PlayMessage;

namespace {
	std::vector<std::byte> Bytes(std::string_view text) {
		std::vector<std::byte> data(text.size());
		std::memcpy(data.data(), text.data(), text.size());
		return data;
	}

	// A payload with a tag nothing here owns, which is what somebody else's
	// message on the shared user channel looks like.
	std::vector<std::byte> Foreign() {
		engine::core::ByteWriter writer;
		writer.WriteUInt8(200);
		writer.WriteUInt64(1);
		const std::span<const std::byte> bytes = writer.Bytes();
		return {bytes.begin(), bytes.end()};
	}
}

TEST_CASE("a content directory round-trips", "[game][content]") {
	ContentDirectory sent;
	sent.Endpoints.push_back(ContentEndpoint{.Name = "edge-eu", .Kind = "http", .Location = "10.0.0.4:9080"});
	sent.Endpoints.push_back(ContentEndpoint{.Name = "beside-me", .Kind = "dir", .Location = "/srv/store"});
	sent.Grant = Bytes("a grant this module does not read");
	sent.PublisherKey = std::string(64, 'a');

	ContentDirectory read;
	REQUIRE(DecodeContentDirectory(EncodeContentDirectory(sent), read));

	REQUIRE(read.Endpoints.size() == 2);
	CHECK(read.Endpoints[0].Name == "edge-eu");
	CHECK(read.Endpoints[0].Kind == "http");
	CHECK(read.Endpoints[0].Location == "10.0.0.4:9080");
	CHECK(read.Endpoints[1].Name == "beside-me");
	CHECK(read.Endpoints[1].Kind == "dir");
	CHECK(read.Grant == sent.Grant);
	CHECK(read.PublisherKey == sent.PublisherKey);
}

TEST_CASE("a directory with nothing in it is still a directory", "[game][content]") {
	// **Relay mode's message.** There is nowhere to point a client at and it
	// still needs a root of trust, so an empty endpoint list is a legitimate
	// document rather than a malformed one.
	ContentDirectory sent;
	sent.PublisherKey = std::string(64, 'b');

	ContentDirectory read;
	REQUIRE(DecodeContentDirectory(EncodeContentDirectory(sent), read));
	CHECK(read.Endpoints.empty());
	CHECK(read.Grant.empty());
	CHECK(read.PublisherKey == sent.PublisherKey);
}

TEST_CASE("a directory naming more origins than the bound is refused", "[game][content]") {
	ContentDirectory sent;
	for (size_t index = 0; index < MAXIMUM_CONTENT_ENDPOINTS + 4; ++index) {
		sent.Endpoints.push_back(
			ContentEndpoint{.Name = "o" + std::to_string(index), .Kind = "http", .Location = "1.2.3.4:9080"}
		);
	}

	// The encoder truncates rather than writing a count it will then refuse to
	// read back, so what goes out is always something the other end accepts.
	ContentDirectory read;
	REQUIRE(DecodeContentDirectory(EncodeContentDirectory(sent), read));
	CHECK(read.Endpoints.size() == MAXIMUM_CONTENT_ENDPOINTS);

	// A count somebody wrote by hand is a different question, and it is refused
	// before anything is reserved for it.
	engine::core::ByteWriter forged;
	forged.WriteUInt8(static_cast<uint8_t>(PlayMessage::ContentDirectory));
	forged.WriteUInt16(static_cast<uint16_t>(MAXIMUM_CONTENT_ENDPOINTS + 1));
	const std::span<const std::byte> bytes = forged.Bytes();
	CHECK_FALSE(DecodeContentDirectory(std::vector<std::byte>(bytes.begin(), bytes.end()), read));
}

TEST_CASE("a route request round-trips and is bounded", "[game][content]") {
	ContentRouteRequest sent{.Ticket = 7, .Route = "/bundle/abc"};

	ContentRouteRequest read;
	REQUIRE(DecodeContentRequest(EncodeContentRequest(sent), read));
	CHECK(read.Ticket == 7);
	CHECK(read.Route == "/bundle/abc");

	// A route is text that arrived over a wire, so the length is bounded at the
	// parse rather than by whoever eventually looks at it.
	const ContentRouteRequest long_{.Ticket = 1, .Route = std::string(MAXIMUM_ROUTE_BYTES + 1, 'x')};
	CHECK_FALSE(DecodeContentRequest(EncodeContentRequest(long_), read));
}

TEST_CASE("a content chunk round-trips", "[game][content]") {
	ContentChunk sent;
	sent.Ticket = 3;
	sent.TotalBytes = 12;
	sent.Offset = 4;
	sent.Bytes = Bytes("five!");
	sent.TotalBytes = 9;

	ContentChunk read;
	REQUIRE(DecodeContentChunk(EncodeContentChunk(sent), read));
	CHECK(read.Ticket == 3);
	CHECK(read.TotalBytes == 9);
	CHECK(read.Offset == 4);
	CHECK(read.Bytes == sent.Bytes);
}

TEST_CASE("a chunk that does not fit the total it names is refused", "[game][content]") {
	// **Checked in the decoder, not by the reassembler.** There is one decoder
	// and there will be more than one caller, and a piece that ends past the
	// total is a peer asking a receiver to write outside a buffer sized from the
	// same message.
	ContentChunk past;
	past.Ticket = 1;
	past.TotalBytes = 4;
	past.Offset = 2;
	past.Bytes = Bytes("four");

	ContentChunk read;
	CHECK_FALSE(DecodeContentChunk(EncodeContentChunk(past), read));

	ContentChunk oversized;
	oversized.Ticket = 1;
	oversized.TotalBytes = MAXIMUM_CONTENT_CHUNK_BYTES + 1;
	oversized.Offset = 0;
	oversized.Bytes.assign(MAXIMUM_CONTENT_CHUNK_BYTES + 1, std::byte{7});
	CHECK_FALSE(DecodeContentChunk(EncodeContentChunk(oversized), read));

	// The largest legal piece still reads back, so the bound is a ceiling rather
	// than an off-by-one.
	ContentChunk full;
	full.Ticket = 1;
	full.TotalBytes = MAXIMUM_CONTENT_CHUNK_BYTES;
	full.Offset = 0;
	full.Bytes.assign(MAXIMUM_CONTENT_CHUNK_BYTES, std::byte{7});
	CHECK(DecodeContentChunk(EncodeContentChunk(full), read));
}

TEST_CASE("a refusal round-trips", "[game][content]") {
	ContentRefusal read;
	REQUIRE(DecodeContentRefusal(EncodeContentRefusal(ContentRefusal{.Ticket = 99}), read));
	CHECK(read.Ticket == 99);
}

TEST_CASE("every decoder refuses somebody else's message", "[game][content]") {
	// **The whole reason there is a tag.** Both directions of the user channel
	// are shared - a join notice goes down it, a move goes up it, and the studio
	// sends its own edits over one - so a payload that is not this message has
	// to be a non-event rather than a misread.
	const std::vector<std::byte> foreign = Foreign();

	ContentDirectory directory;
	ContentRouteRequest request;
	ContentChunk chunk;
	ContentRefusal refusal;

	CHECK_FALSE(DecodeContentDirectory(foreign, directory));
	CHECK_FALSE(DecodeContentRequest(foreign, request));
	CHECK_FALSE(DecodeContentChunk(foreign, chunk));
	CHECK_FALSE(DecodeContentRefusal(foreign, refusal));

	// And each other's, which is what a shared tag space has to guarantee.
	const std::vector<std::byte> aChunk =
		EncodeContentChunk(ContentChunk{.Ticket = 1, .TotalBytes = 0, .Offset = 0, .Bytes = {}});
	CHECK_FALSE(DecodeContentDirectory(aChunk, directory));
	CHECK_FALSE(DecodeContentRequest(aChunk, request));
	CHECK_FALSE(DecodeContentRefusal(aChunk, refusal));
	CHECK(DecodeContentChunk(aChunk, chunk));

	// Empty is not a message either, and is the shape a truncated one takes.
	CHECK_FALSE(DecodeContentChunk({}, chunk));
	CHECK_FALSE(DecodeContentRefusal({}, refusal));
}

TEST_CASE("the content tags do not collide with the play tags", "[game][content]") {
	// One tag space, one enum, and this is the check that says so. A second
	// enum starting at one again is the day a move is read as a refusal.
	CHECK(static_cast<uint8_t>(PlayMessage::AssignPlayer) == 1);
	CHECK(static_cast<uint8_t>(PlayMessage::Move) == 2);
	CHECK(static_cast<uint8_t>(PlayMessage::ContentDirectory) == 3);
	CHECK(static_cast<uint8_t>(PlayMessage::ContentRequest) == 4);
	CHECK(static_cast<uint8_t>(PlayMessage::ContentChunk) == 5);
	CHECK(static_cast<uint8_t>(PlayMessage::ContentRefusal) == 6);
}
