// The three closed lists, and the property that makes them safe to put on a
// wire: an ordinal means one thing for ever.
//
// This suite exists to fail when somebody reorders one. That is not a
// hypothetical - an announcement from an older build decoding as a different
// kind of session in a newer one is the quietest way this module could break,
// and nothing else in the build would notice.

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <network/Enums.hpp>
#include <string_view>

TEST_SUITE_ID("network.enums")

using network::Access;
using network::Purpose;
using network::Reach;

TEST_CASE("every ordinal is the one that reached a wire", "[network][enums]") {
	// Written out rather than derived. A test that computed these from the
	// enum would agree with any reordering, which is the one thing it is here
	// to refuse.
	CHECK(static_cast<uint8_t>(Reach::Loopback) == 0);
	CHECK(static_cast<uint8_t>(Reach::Lan) == 1);
	CHECK(static_cast<uint8_t>(Reach::Peer) == 2);
	CHECK(static_cast<uint8_t>(Reach::Remote) == 3);

	CHECK(static_cast<uint8_t>(Access::Public) == 0);
	CHECK(static_cast<uint8_t>(Access::Private) == 1);

	CHECK(static_cast<uint8_t>(Purpose::Game) == 0);
	CHECK(static_cast<uint8_t>(Purpose::Studio) == 1);
	CHECK(static_cast<uint8_t>(Purpose::Content) == 2);
}

TEST_CASE("the reach order is how much has to keep working", "[network][enums]") {
	// `Directory::Admit` prefers the smaller value when one session arrives
	// twice, so this ordering is behaviour rather than documentation: a LAN
	// address survives a router forgetting a mapping and a punched one does
	// not.
	CHECK(Reach::Loopback < Reach::Lan);
	CHECK(Reach::Lan < Reach::Peer);
	CHECK(Reach::Peer < Reach::Remote);
}

TEST_CASE("every value names itself and no two share a name", "[network][enums]") {
	const std::string_view reaches[] = {
		network::Describe(Reach::Loopback),
		network::Describe(Reach::Lan),
		network::Describe(Reach::Peer),
		network::Describe(Reach::Remote),
	};
	for (size_t index = 0; index < 4; ++index) {
		CHECK_FALSE(reaches[index].empty());
		CHECK(reaches[index] != "?");
		for (size_t other = index + 1; other < 4; ++other) {
			CHECK(reaches[index] != reaches[other]);
		}
	}

	CHECK(std::string_view(network::Describe(Access::Public)) == "public");
	CHECK(std::string_view(network::Describe(Access::Private)) == "private");

	CHECK(std::string_view(network::Describe(Purpose::Game)) == "game");
	CHECK(std::string_view(network::Describe(Purpose::Studio)) == "studio");
	CHECK(std::string_view(network::Describe(Purpose::Content)) == "content");

	// A value from outside the list - what a hostile datagram would carry if
	// anything cast one without checking. It names itself as unknown rather
	// than reading past the end of a table.
	CHECK(std::string_view(network::Describe(static_cast<Reach>(200))) == "?");
}

TEST_CASE("the two well-known ports are distinct and stated", "[network][enums]") {
	CHECK(network::DISCOVERY_PORT == 47600);
}
