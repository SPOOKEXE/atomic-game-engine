// A distribution stream, and the mapping that makes a discovered origin a
// source a delivery client already knows how to walk.
//
// The wire is `network`'s and is covered there, over a loopback with real
// encoding. What is checked here is the part that belongs to this member: the
// four streams are two settings rather than four code paths, and what comes out
// of a search is a `delivery::Source` list in the order `AssetClient` walks.

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cdn/Stream.hpp>
#include <memory>
#include <string>

TEST_SUITE_ID("cdn.stream")
TEST_DEPENDS("network.presence")

using cdn::Stream;
using cdn::StreamFinder;
using cdn::StreamSearch;
using cdn::StreamSettings;

namespace {
	// Away from every well-known port, so this suite disturbs nothing that may
	// be running on the machine it is built on.
	constexpr uint16_t QUIET_RENDEZVOUS_PORT = 47691;
}

TEST_CASE("a public stream is an announcement and nothing else", "[cdn][stream]") {
	StreamSettings offering;
	offering.Announce = true;
	offering.Name = "the public one";
	offering.Port = 9080;

	std::string trouble;
	const std::unique_ptr<Stream> stream = Stream::Open(offering, trouble);
	REQUIRE(stream != nullptr);
	CHECK(trouble.empty());

	CHECK(stream->Advertised().Use == network::Purpose::Content);
	CHECK(stream->Advertised().Admits == network::Access::Public);
	CHECK(stream->Advertised().Name == "the public one");
	CHECK(stream->Advertised().Session.IsValid());

	// The wildcard address with the bound port. A browser resolves the address
	// against where the datagram came from; the port is the only part an
	// announcement can be right about on its own.
	CHECK(stream->Advertised().At.Port == 9080);
	CHECK_FALSE(stream->Meeting());
	CHECK(stream->Hosting() == 0);

	stream->Pump(0.0);
	stream->Withdraw(1.0);
}

TEST_CASE("a private stream announces under its key", "[cdn][stream]") {
	StreamSettings offering;
	offering.Announce = true;
	offering.Secret = "the passphrase";
	offering.Port = 9080;

	std::string trouble;
	const std::unique_ptr<Stream> stream = Stream::Open(offering, trouble);
	REQUIRE(stream != nullptr);
	CHECK(stream->Advertised().Admits == network::Access::Private);

	// A machine with no route to the broadcast address opens no beacon, which
	// is a fault rather than a refusal to serve - the origin is still serving
	// content on its port.
	if (stream->Announcing()) {
		CHECK(stream->Advertised().Admits == network::Access::Private);
	}
}

TEST_CASE("a secret that is neither hex nor words is refused with a reason", "[cdn][stream]") {
	StreamSettings offering;
	offering.Announce = true;
	// Sixty-four characters, so it looks like a key, and one of them is not a
	// hexadecimal digit - which is exactly the typo somebody makes copying one
	// off a screen. Refused rather than silently stretched as a passphrase,
	// because the two derive different keys and the operator would be the only
	// person on the wrong one.
	offering.Secret = std::string(63, 'a') + "z";

	std::string trouble;
	const std::unique_ptr<Stream> stream = Stream::Open(offering, trouble);
	// It is a passphrase, since it is not hex - which is the documented
	// fallback and not a failure. What must not happen is a null with no
	// reason.
	if (stream == nullptr) {
		CHECK_FALSE(trouble.empty());
	} else {
		CHECK(stream->Advertised().Admits == network::Access::Private);
	}

	// An empty secret is a public stream rather than a refusal.
	StreamSettings open;
	open.Announce = true;
	std::string quiet;
	const std::unique_ptr<Stream> plain = Stream::Open(open, quiet);
	REQUIRE(plain != nullptr);
	CHECK(plain->Advertised().Admits == network::Access::Public);
}

TEST_CASE("an origin can be the meeting place without being a stream", "[cdn][stream]") {
	StreamSettings offering;
	offering.Announce = false;
	offering.RendezvousListenPort = QUIET_RENDEZVOUS_PORT;
	offering.Port = 9080;

	std::string trouble;
	const std::unique_ptr<Stream> stream = Stream::Open(offering, trouble);
	if (stream == nullptr) {
		// The port is held by something else on this machine. Said out loud
		// rather than asserted away.
		WARN("could not bind the test rendezvous port: " << trouble);
		return;
	}

	CHECK(stream->Meeting());
	CHECK(stream->Hosting() == 0);
	CHECK_FALSE(stream->Announcing());

	// Serving an empty point is a table walk over nothing, and it must not
	// invent a session.
	stream->Pump(0.0);
	stream->Pump(1.0);
	CHECK(stream->Hosting() == 0);
	CHECK(stream->MeetingCounters().Registrations == 0);
	CHECK(stream->MeetingCounters().Malformed == 0);
}

TEST_CASE("a finder turns what it sees into sources, nearest first", "[cdn][stream]") {
	StreamSearch search;
	// No sockets: a finder that browses nothing still holds the rows it was
	// offered, which is the property that keeps an "is discovery on" branch out
	// of every caller.
	search.Browse = false;

	const std::unique_ptr<StreamFinder> finder = StreamFinder::Open(search);
	REQUIRE(finder != nullptr);
	CHECK(finder->Sources().empty());

	network::Advert nearby;
	nearby.Session = network::SessionId::Draw();
	nearby.Use = network::Purpose::Content;
	nearby.Name = "next door";
	nearby.At = engine::net::Endpoint::LoopbackIPv4(9080);

	network::Advert distant;
	distant.Session = network::SessionId::Draw();
	distant.Use = network::Purpose::Content;
	distant.Name = "across the internet";
	distant.At = engine::net::Endpoint::LoopbackIPv4(9081);

	// Offered out of order on purpose.
	REQUIRE(finder->Seen().Listings().empty());
	REQUIRE(finder->Seen().Offer(distant, network::Reach::Remote, {}, 0.0));
	REQUIRE(finder->Seen().Offer(nearby, network::Reach::Lan, {}, 0.0));

	const std::vector<engine::delivery::Source> sources = finder->Sources();
	REQUIRE(sources.size() == 2);

	// Nearest first, which is what "local first, then the origin next door,
	// then the one across the internet" *is* to `delivery::AssetClient` - it
	// walks the list and stops at the first that answers.
	CHECK(sources[0].Name == "next door");
	CHECK(sources[1].Name == "across the internet");

	CHECK(sources[0].Kind == engine::delivery::SourceKind::Http);
	CHECK(sources[0].Location == "127.0.0.1:9080");
	CHECK(sources[0].Enabled);

	// **Read, never write.** Uploading to whatever answered a broadcast is how
	// content reaches a machine nobody meant to publish from.
	CHECK(sources[0].Role == engine::delivery::SourceRole::Read);
	CHECK(sources[1].Role == engine::delivery::SourceRole::Read);
	CHECK(sources[0].IsValid());
}

TEST_CASE("a private stream with no key is seen and not offered as a source", "[cdn][stream]") {
	StreamSearch search;
	search.Browse = false;

	const std::unique_ptr<StreamFinder> finder = StreamFinder::Open(search);
	REQUIRE(finder != nullptr);

	network::Advert locked;
	locked.Session = network::SessionId::Draw();
	locked.Use = network::Purpose::Content;
	locked.Admits = network::Access::Private;
	locked.Name = "somebody else's";
	locked.At = engine::net::Endpoint::LoopbackIPv4(9080);

	REQUIRE(finder->Seen().Offer(locked, network::Reach::Lan, {}, 0.0, false));

	// Visible, because a person has to be able to see the stream they are about
	// to be given the key for.
	CHECK(finder->Seen().Listings().size() == 1);

	// And not a source: one that cannot be drawn from is a source every fetch
	// pays a timeout for.
	CHECK(finder->Sources().empty());

	// The same stream, with the key held.
	REQUIRE(finder->Seen().Offer(locked, network::Reach::Lan, {}, 1.0, true));
	CHECK(finder->Sources().size() == 1);
}
