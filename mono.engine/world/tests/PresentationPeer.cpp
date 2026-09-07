#include <engine/testing/Suite.hpp>
#include <engine/world/PresentationPeer.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.world.presentationpeer")
TEST_DEPENDS("engine.world.presentationbus")

namespace {
	using namespace engine;
	using namespace engine::world;
	using core::Name;
	using Status = PresentationStatus;
	PresentationPeerChannels Channels() {
		return {{"replies", "topology-replies"}, {"replies/"}, {"images", "topology"}};
	}
	struct Fixture {
		Universe Worlds;
		WorldId Far = Worlds.Create({.Name = Name("far")});
		PresentationAddress Image, Sessions;
		Fixture() {
			REQUIRE(Worlds.ConfigurePresentation(100));
			Image = Worlds.OpenPresentation(Far, Name("images")).Address;
			Sessions = Worlds.OpenPresentation(Far, Name("portal-sessions")).Address;
		}
	};
}

TEST_CASE(
	"presentation peers isolate identical client addresses and preserve reply channels",
	"[world][presentation-peer]"
) {
	Fixture fixture;
	PresentationPeer first(fixture.Worlds, fixture.Far, 64, Channels());
	PresentationPeer second(fixture.Worlds, fixture.Far, 128, Channels());
	const PresentationAddress original{"client.replica", "replies", 1, 1};
	const PresentationDirectory directory{1, 1, {original}};
	REQUIRE(first.Apply(directory) == Status::Ok);
	REQUIRE(second.Apply(directory) == Status::Ok);
	const PresentationMessage request{original, fixture.Image, 1, 7, {std::byte{42}}};
	REQUIRE(first.Accept(request) == Status::Ok);
	REQUIRE(second.Accept(request) == Status::Ok);
	const auto requests = fixture.Worlds.TakePresentation(fixture.Image);
	REQUIRE(requests.size() == 2);
	CHECK(requests[0].From.Channel != requests[1].From.Channel);
	CHECK(requests[0].From.Session == 100);
	CHECK(requests[1].From.Session == 100);
	for (const auto &incoming : requests) {
		CHECK(incoming.From.Channel.starts_with(original.Channel + "/"));
		REQUIRE(
			fixture.Worlds.SendPresentation(
				fixture.Far, fixture.Image, incoming.From, incoming.Correlation, incoming.Payload
			) == Status::Ok
		);
	}
	auto replies = first.Take();
	REQUIRE(replies.size() == 1);
	auto other = second.Take();
	REQUIRE(other.size() == 1);
	replies.push_back(std::move(other[0]));
	for (const auto &reply : replies) {
		CHECK(reply.To == original);
		CHECK(reply.From == fixture.Image);
		CHECK(reply.Payload == request.Payload);
	}
	CHECK(first.Routes().Endpoints == std::vector<PresentationAddress>{fixture.Image});
	first.Close();
	CHECK(fixture.Worlds.LookupPresentation(fixture.Far, requests[0].From.Channel).Session == 0);
	CHECK(fixture.Worlds.LookupPresentation(fixture.Far, requests[1].From.Channel) == requests[1].From);
	CHECK(fixture.Worlds.LookupPresentation(fixture.Far, "portal-sessions") == fixture.Sessions);
}

TEST_CASE(
	"presentation peers refuse control channels and stale endpoint reuse", "[world][presentation-peer]"
) {
	Fixture fixture;
	PresentationPeer peer(fixture.Worlds, fixture.Far, 64, Channels());
	const PresentationAddress source{"client.replica", "replies", 1, 1};
	REQUIRE(peer.Apply({1, 1, {source}}) == Status::Ok);
	CHECK(peer.Accept({source, fixture.Sessions, 1, 1, {}}) == Status::WrongHost);
	auto forged = source;
	forged.World = "far";
	CHECK(peer.Accept({forged, fixture.Image, 1, 1, {}}) == Status::WrongHost);
	REQUIRE(peer.Accept({source, fixture.Image, 1, 1, {}}) == Status::Ok);
	const auto requests = fixture.Worlds.TakePresentation(fixture.Image);
	REQUIRE(requests.size() == 1);
	PresentationMessage oldReply{fixture.Image, requests[0].From, 1, 1, {}};
	REQUIRE(peer.Apply({1, 2, {}}) == Status::Ok);
	REQUIRE(peer.Apply({1, 3, {source}}) == Status::Ok);
	CHECK(
		fixture.Worlds.SendPresentation(fixture.Far, fixture.Image, oldReply.To, 1, {}) ==
		Status::StaleEndpoint
	);
	CHECK(peer.Take().empty());
	REQUIRE(peer.Accept({source, fixture.Image, 1, 2, {}}) == Status::Ok);
	const auto fresh = fixture.Worlds.TakePresentation(fixture.Image);
	REQUIRE(fresh.size() == 1);
	CHECK(fresh[0].From.Generation != requests[0].From.Generation);
	CHECK(peer.Apply({1, 4, {{"far", "portal-sessions", 1, 1}}}) == Status::WrongHost);
	CHECK(peer.Apply({1, 2, {source}}) == Status::StaleEndpoint);
	CHECK(fixture.Worlds.LookupPresentation(fixture.Far, "portal-sessions") == fixture.Sessions);
}

TEST_CASE(
	"presentation peer reuses bounded alias names across directory churn", "[world][presentation-peer]"
) {
	Fixture fixture;
	PresentationPeer peer(fixture.Worlds, fixture.Far, 64, Channels());
	for (uint64_t i = 0; i < 128; ++i) {
		REQUIRE(peer.Apply({1, i + 1, {{"source" + std::to_string(i), "replies/2", 1, 1}}}) == Status::Ok);
		CHECK(fixture.Worlds.LocalPresentationDirectory().Endpoints.size() == 3);
	}
	CHECK(fixture.Worlds.Worlds().size() == 1);
	peer.Close();
	CHECK(fixture.Worlds.LocalPresentationDirectory().Endpoints.size() == 2);
	CHECK(peer.Apply({1, 129, {}}) == Status::Invalid);
}
