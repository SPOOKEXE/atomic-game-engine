#include <engine/delivery/Relay.hpp>
#include <engine/delivery/Source.hpp>
#include <engine/game/Content.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <client/ContentLink.hpp>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("client.contentlink")
TEST_DEPENDS("engine.game.content")
TEST_DEPENDS("engine.delivery.relay")

using client::AcceptOfferedContent;
using client::ContentLink;
using client::MAXIMUM_RELAYED_ROUTE_BYTES;
using client::MergeContentSources;
using client::OfferedContent;
using engine::delivery::RelayAnswer;
using engine::delivery::Source;
using engine::delivery::SourceKind;
using engine::game::ContentChunk;
using engine::game::ContentDirectory;
using engine::game::ContentEndpoint;
using engine::game::ContentRefusal;
using engine::game::ContentRouteRequest;
using engine::game::DecodeContentRequest;
using engine::game::EncodeContentChunk;
using engine::game::EncodeContentRefusal;

namespace {
	std::vector<std::byte> Bytes(std::string_view text) {
		std::vector<std::byte> data(text.size());
		std::memcpy(data.data(), text.data(), text.size());
		return data;
	}

	// Collects what the link tried to send, and can refuse to carry it.
	struct Wire {
		std::vector<ContentRouteRequest> Asked;
		bool Refusing = false;

		ContentLink::Sender Sender() {
			return [this](std::span<const std::byte> payload) {
				if (Refusing) {
					return false;
				}
				ContentRouteRequest request;
				REQUIRE(DecodeContentRequest(payload, request));
				Asked.push_back(std::move(request));
				return true;
			};
		}
	};

	std::vector<std::byte> Piece(uint64_t ticket, uint32_t total, uint32_t offset, std::string_view text) {
		ContentChunk chunk;
		chunk.Ticket = ticket;
		chunk.TotalBytes = total;
		chunk.Offset = offset;
		chunk.Bytes = Bytes(text);
		return EncodeContentChunk(chunk);
	}
}

TEST_CASE("a route asked for arrives as one buffer", "[client][content]") {
	Wire wire;
	ContentLink link(wire.Sender());

	REQUIRE(link.Ask(7, "/manifest"));
	REQUIRE(wire.Asked.size() == 1);
	CHECK(wire.Asked.front().Ticket == 7);
	CHECK(wire.Asked.front().Route == "/manifest");

	CHECK(link.Receive(Piece(7, 9, 0, "abcd")));
	CHECK(link.Receive(Piece(7, 9, 4, "efghi")));

	std::vector<RelayAnswer> answers;
	link.Collect(answers);
	REQUIRE(answers.size() == 1);
	CHECK(answers.front().Ticket == 7);
	CHECK(answers.front().Served);
	CHECK(answers.front().Bytes == Bytes("abcdefghi"));
	CHECK(link.Stats().Completed == 1);
	CHECK(link.Assembling() == 0);
}

TEST_CASE("a link that will not carry a request has not asked", "[client][content]") {
	Wire wire;
	wire.Refusing = true;
	ContentLink link(wire.Sender());

	// Ordinary backpressure. Nothing is recorded, so the fetcher offers it again
	// rather than waiting for an answer to a question that was never asked.
	CHECK_FALSE(link.Ask(1, "/manifest"));
	CHECK(link.Assembling() == 0);
	CHECK(link.Stats().Requests == 0);
}

TEST_CASE("a refusal ends the route rather than hanging it", "[client][content]") {
	Wire wire;
	ContentLink link(wire.Sender());
	REQUIRE(link.Ask(4, "/dictionary"));

	CHECK(link.Receive(EncodeContentRefusal(ContentRefusal{.Ticket = 4})));

	std::vector<RelayAnswer> answers;
	link.Collect(answers);
	REQUIRE(answers.size() == 1);
	CHECK_FALSE(answers.front().Served);
	CHECK(answers.front().Bytes.empty());
	CHECK(link.Stats().Refused == 1);
}

TEST_CASE("every hostile shape a server can send is dropped and counted", "[client][content]") {
	Wire wire;
	ContentLink link(wire.Sender());
	REQUIRE(link.Ask(1, "/manifest"));

	SECTION("a ticket this end never issued") {
		CHECK(link.Receive(Piece(99, 4, 0, "abcd")));
		CHECK(link.Stats().Discarded == 1);
		CHECK(link.Stats().Chunks == 0);
	}

	SECTION("a total this client will not hold") {
		ContentChunk huge;
		huge.Ticket = 1;
		huge.TotalBytes = MAXIMUM_RELAYED_ROUTE_BYTES + 1;
		huge.Offset = 0;
		huge.Bytes = Bytes("a few bytes claiming to be a gigabyte");
		CHECK(link.Receive(EncodeContentChunk(huge)));
		CHECK(link.Stats().Discarded == 1);

		// **The route ends rather than waiting for ever.** A request left
		// pending would stall the source walk on an origin that answered.
		std::vector<RelayAnswer> answers;
		link.Collect(answers);
		REQUIRE(answers.size() == 1);
		CHECK_FALSE(answers.front().Served);
	}

	SECTION("a piece that does not start where the last one ended") {
		CHECK(link.Receive(Piece(1, 8, 0, "abcd")));
		CHECK(link.Receive(Piece(1, 8, 6, "gh")));
		CHECK(link.Stats().Discarded == 1);
		CHECK(link.Stats().Chunks == 1);
		CHECK(link.Stats().Completed == 0);
	}

	SECTION("a total that changes mid-route") {
		CHECK(link.Receive(Piece(1, 8, 0, "abcd")));
		// Legal on its own - it fits the total it names - and wrong here,
		// because resizing the buffer now would move bytes already held.
		CHECK(link.Receive(Piece(1, 12, 4, "efgh")));
		CHECK(link.Stats().Discarded == 1);
		CHECK(link.Stats().Chunks == 1);
	}

	SECTION("somebody else's message on the shared channel") {
		CHECK_FALSE(link.Receive(Bytes("a move, or a studio edit")));
		CHECK(link.Stats().Discarded == 0);
	}
}

TEST_CASE("a server-named origin passes the allow-list before it is used", "[client][content]") {
	ContentDirectory directory;
	directory.Endpoints.push_back(
		ContentEndpoint{.Name = "permitted", .Kind = "http", .Location = "10.0.0.4:9080"}
	);
	directory.Endpoints.push_back(
		ContentEndpoint{.Name = "elsewhere", .Kind = "http", .Location = "203.0.113.9:9080"}
	);

	SECTION("an empty list restricts nothing, which is what an unset one means") {
		const OfferedContent accepted = AcceptOfferedContent(directory, {});
		CHECK(accepted.Permitted.size() == 2);
		CHECK(accepted.RefusedByAllowList == 0);
	}

	SECTION("a configured list refuses everything it does not name") {
		const OfferedContent accepted = AcceptOfferedContent(directory, {"10.0.0.4"});
		REQUIRE(accepted.Permitted.size() == 1);
		CHECK(accepted.Permitted.front().Name == "permitted");
		CHECK(accepted.RefusedByAllowList == 1);
	}
}

TEST_CASE("a server-named host name is skipped rather than fetched from", "[client][content]") {
	ContentDirectory directory;
	directory.Endpoints.push_back(
		ContentEndpoint{.Name = "by-name", .Kind = "http", .Location = "cdn.example.com:9080"}
	);
	directory.Endpoints.push_back(
		ContentEndpoint{.Name = "also-by-name", .Kind = "http", .Location = "other.example.com:9080"}
	);
	directory.Endpoints.push_back(
		ContentEndpoint{.Name = "by-address", .Kind = "http", .Location = "10.0.0.4:9080"}
	);

	// **`Endpoint::Parse` refuses a host name on purpose** - resolving one
	// blocks on a network service and nothing on the fetch path may block - so
	// these are dropped at the door and counted once, rather than producing a
	// stream of individually plausible fetch failures later.
	const OfferedContent accepted = AcceptOfferedContent(directory, {});
	REQUIRE(accepted.Permitted.size() == 1);
	CHECK(accepted.Permitted.front().Name == "by-address");
	CHECK(accepted.UnresolvedNames == 2);
}

TEST_CASE("a kind this build has no row for costs nothing else", "[client][content]") {
	ContentDirectory directory;
	directory.Endpoints.push_back(ContentEndpoint{.Name = "future", .Kind = "s3", .Location = "bucket"});
	directory.Endpoints.push_back(ContentEndpoint{.Name = "known", .Kind = "dir", .Location = "/srv/store"});
	directory.Endpoints.push_back(ContentEndpoint{.Name = "", .Kind = "dir", .Location = "/srv/other"});

	const OfferedContent accepted = AcceptOfferedContent(directory, {});
	REQUIRE(accepted.Permitted.size() == 1);
	CHECK(accepted.Permitted.front().Kind == SourceKind::Directory);
	CHECK(accepted.UnknownKinds == 2);
}

TEST_CASE("this client's own sources come first and the link comes last", "[client][content]") {
	const std::vector<std::string> configured{"dir:/home/me/store", "127.0.0.1:9080"};
	const std::vector<Source> offered{
		Source{.Name = "edge", .Kind = SourceKind::Http, .Location = "10.0.0.4:9080", .Enabled = true},
	};

	const std::vector<Source> merged = MergeContentSources(configured, offered, "10.0.0.1:9000");

	// **Precedence is decided by where a value came from.** These two were
	// typed by the person running the program; the third arrived over a wire;
	// the fourth is the link itself, which is the source that always exists and
	// therefore the one that answers when nothing before it did.
	REQUIRE(merged.size() == 4);
	CHECK(merged[0].Kind == SourceKind::Directory);
	CHECK(merged[0].Location == "/home/me/store");
	CHECK(merged[1].Location == "127.0.0.1:9080");
	CHECK(merged[2].Name == "edge");
	CHECK(merged[3].Kind == SourceKind::Relay);
	CHECK(merged[3].Name == "server");
	CHECK(merged[3].Location == "10.0.0.1:9000");
}

TEST_CASE("a server's list is appended and never replaces what was configured", "[client][content]") {
	const std::vector<std::string> configured{"dir:/home/me/store"};
	const std::vector<Source> offered{
		Source{.Name = "edge", .Kind = SourceKind::Http, .Location = "10.0.0.4:9080", .Enabled = true},
	};

	const std::vector<Source> merged = MergeContentSources(configured, offered, {});
	REQUIRE(merged.size() == 2);
	CHECK(merged[0].Location == "/home/me/store");
	CHECK(merged[1].Name == "edge");

	// With no link there is no relay entry at all, which is what a
	// single-player run gets.
	for (const Source &source : merged) {
		CHECK(source.Kind != SourceKind::Relay);
	}
}

TEST_CASE("a relay is never written to", "[client][content]") {
	const std::vector<Source> merged = MergeContentSources({}, {}, "server");
	REQUIRE(merged.size() == 1);

	// The routes a relay carries are the three an origin *serves*, and `net/http`
	// is `GET` and `HEAD` only - so there is no upload to relay, and a client
	// that tried would be asking a game server to publish on its behalf.
	CHECK(merged.front().Readable());
	CHECK_FALSE(merged.front().Writable());
}
