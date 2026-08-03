#include <engine/net/ConnectionId.hpp>
#include <engine/net/ConnectionStats.hpp>
#include <engine/net/Enums.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

TEST_SUITE_ID("engine.net.enums")

using engine::net::ChannelKind;
using engine::net::ConnectionId;
using engine::net::ConnectionState;
using engine::net::ConnectionStats;
using engine::net::Describe;
using engine::net::DisconnectReason;

TEST_CASE("every state has a name", "[net][enums]") {
	// Names are the format and numbers are not, so every value has to have one —
	// a `?` in a log is a value somebody has to trace back to a switch.
	for (const ConnectionState state :
		 {ConnectionState::Connecting,
		  ConnectionState::Connected,
		  ConnectionState::Disconnecting,
		  ConnectionState::Disconnected}) {
		CHECK(std::string(Describe(state)) != "?");
	}

	for (const DisconnectReason reason :
		 {DisconnectReason::None,
		  DisconnectReason::Requested,
		  DisconnectReason::TimedOut,
		  DisconnectReason::HandshakeFailed,
		  DisconnectReason::ProtocolError,
		  DisconnectReason::BudgetExceeded,
		  DisconnectReason::Shutdown}) {
		CHECK(std::string(Describe(reason)) != "?");
	}

	for (const ChannelKind kind : {ChannelKind::Unreliable, ChannelKind::Reliable, ChannelKind::Handshake}) {
		CHECK(std::string(Describe(kind)) != "?");
	}
}

TEST_CASE("names are distinct", "[net][enums]") {
	// Two states sharing a name would make a log ambiguous in exactly the
	// situation a log is being read.
	const std::vector<std::string_view> names{
		Describe(ConnectionState::Connecting),
		Describe(ConnectionState::Connected),
		Describe(ConnectionState::Disconnecting),
		Describe(ConnectionState::Disconnected),
		Describe(DisconnectReason::Requested),
		Describe(DisconnectReason::TimedOut),
		Describe(DisconnectReason::HandshakeFailed),
		Describe(DisconnectReason::ProtocolError),
		Describe(DisconnectReason::BudgetExceeded),
		Describe(DisconnectReason::Shutdown),
	};

	const std::unordered_set<std::string_view> unique(names.begin(), names.end());
	CHECK(unique.size() == names.size());
}

TEST_CASE("unreliable is the default channel", "[net][enums]") {
	// A design commitment rather than an accident of ordering: a late position
	// update is worse than a dropped one, because the next is already on its way
	// and is more correct than the one being waited for.
	CHECK(ChannelKind{} == ChannelKind::Unreliable);
}

TEST_CASE("a default disconnect reason is None", "[net][enums]") {
	// So a partly filled record does not read as a graceful close.
	CHECK(DisconnectReason{} == DisconnectReason::None);
}

TEST_CASE("a default connection id is not valid", "[net][enums]") {
	CHECK_FALSE(ConnectionId{}.IsValid());
	CHECK(ConnectionId{0, 1}.IsValid());
	CHECK(ConnectionId{5, 3}.IsValid());

	// Generation zero is never issued, so a zeroed handle cannot be mistaken for
	// slot zero's first connection.
	CHECK_FALSE(ConnectionId{5, 0}.IsValid());
}

TEST_CASE("a reconnect is a different handle", "[net][enums]") {
	const ConnectionId before{7, 1};
	const ConnectionId after{7, 2};

	// The same slot, reused. A caller holding the old handle gets a refusal
	// rather than the new player's connection.
	CHECK_FALSE(before == after);
	CHECK(before < after);
}

TEST_CASE("connection ids hash and compare", "[net][enums]") {
	std::unordered_set<ConnectionId> live;
	live.insert(ConnectionId{1, 1});
	live.insert(ConnectionId{1, 2});
	live.insert(ConnectionId{2, 1});
	live.insert(ConnectionId{1, 1});

	// Three distinct connections. A hash folding only the index would collapse
	// the first two, and a host that reused a slot would then lose track of the
	// connection sitting in it.
	CHECK(live.size() == 3);
	CHECK(live.count(ConnectionId{1, 2}) == 1);
	CHECK(live.count(ConnectionId{9, 9}) == 0);
}

TEST_CASE("stats start at zero", "[net][enums]") {
	const ConnectionStats stats;

	// A freshly opened connection has lost nothing and overrun nothing. Any
	// non-zero default here would read as a fault on every new player.
	CHECK(stats.BytesReceived == 0);
	CHECK(stats.BytesSent == 0);
	CHECK(stats.PacketsRefused == 0);
	CHECK(stats.PacketsStale == 0);
	CHECK(stats.SendsOverBudget == 0);
	CHECK(stats.PacketsLost == 0);
	CHECK(stats.RoundTripMilliseconds == 0.0f);
}
