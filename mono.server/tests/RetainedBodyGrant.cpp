#include "RetainedBodyGrant.hpp"

#include <engine/testing/Suite.hpp>
#include <engine/world/PresentationPeer.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("server.retainedbodygrant")
TEST_DEPENDS("engine.world.presentationpeer")

namespace {
	using namespace engine;
	using namespace engine::world;

	PresentationPeerChannels Channels() {
		return {{"replies"}, {}, {"images"}};
	}

	struct Fixture {
		Universe Worlds;
		WorldId World = Worlds.Create({.Name = core::Name("grant-world")});
		PresentationAddress Images;
		PresentationPeer Peer{Worlds, World, 64, Channels()};
		PresentationPeer OtherPeer{Worlds, World, 128, Channels()};

		Fixture() {
			REQUIRE(Worlds.ConfigurePresentation(100));
			Images = Worlds.OpenPresentation(World, core::Name("images")).Address;
			const PresentationAddress original{"client", "replies", 1, 1};
			REQUIRE(Peer.Apply({1, 1, {original}}) == PresentationStatus::Ok);
			REQUIRE(OtherPeer.Apply({1, 1, {original}}) == PresentationStatus::Ok);
		}

		PresentationAddress Receipt(PresentationPeer &peer, uint64_t correlation) {
			const PresentationAddress original{"client", "replies", 1, 1};
			REQUIRE(peer.Accept({original, Images, correlation, correlation, {}}) == PresentationStatus::Ok);
			auto requests = Worlds.TakePresentation(Images);
			REQUIRE(requests.size() == 1);
			return requests.front().From;
		}
	};
}

TEST_CASE(
	"retained body grants require a live receipt and the exact committed transfer target",
	"[server][portal][retained-body]"
) {
	Fixture fixture;
	server::RetainedBodyGrants grants;
	const replication::ClientId client{7, 3};
	const script::PortalTransferId transfer{"source", 41, 9};
	REQUIRE(grants.Issue({client, transfer, 71, 91}));
	const auto receipt = fixture.Receipt(fixture.Peer, 1);
	const auto committed = [&](const server::RetainedBodyGrants::Grant &grant) {
		return grant.Transfer == transfer && grant.DestinationIncarnation == 71 && grant.UserId == 91;
	};

	CHECK(grants.Authorizes(client, fixture.Peer, receipt, "91", committed));
	CHECK_FALSE(grants.Authorizes(client, fixture.Peer, receipt, "091", committed));
	CHECK_FALSE(
		grants.Authorizes({client.Index, client.Generation + 1}, fixture.Peer, receipt, "91", committed)
	);
	CHECK_FALSE(grants.Authorizes(client, fixture.OtherPeer, receipt, "91", committed));

	const auto otherReceipt = fixture.Receipt(fixture.OtherPeer, 2);
	CHECK_FALSE(grants.Authorizes(client, fixture.Peer, otherReceipt, "91", committed));
	CHECK_FALSE(grants.Authorizes(client, fixture.Peer, receipt, "91", [&](const auto &grant) {
		return grant.Transfer.SourceIncarnation == 42;
	}));
	CHECK_FALSE(grants.Authorizes(client, fixture.Peer, receipt, "91", [&](const auto &grant) {
		return grant.DestinationIncarnation == 72;
	}));

	grants.Drop(client);
	CHECK_FALSE(grants.Authorizes(client, fixture.Peer, receipt, "91", committed));
}
