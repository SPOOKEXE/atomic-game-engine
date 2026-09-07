#include <engine/game/PortalSession.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.game.portalsession")

namespace {
	using namespace engine;

	game::PortalResume Claim(uint64_t sequence = 7) {
		game::PortalResume claim;
		claim.Transfer = {"near", 100, sequence};
		claim.Destination = "far";
		claim.DestinationIncarnation = 200;
		claim.SourceSession = 300;
		claim.Capability.fill(std::byte{42});
		return claim;
	}

	assets::PublicKey Identity() {
		assets::PublicKey key;
		key.Value.fill(23);
		return key;
	}
}

TEST_CASE(
	"portal session messages preserve owned values and refuse partial decoding", "[game][portal-session]"
) {
	for (const auto kind :
		 {game::PortalSessionKind::Fresh,
		  game::PortalSessionKind::Resume,
		  game::PortalSessionKind::Ready,
		  game::PortalSessionKind::Commit,
		  game::PortalSessionKind::Committed,
		  game::PortalSessionKind::Transfer,
		  game::PortalSessionKind::Refused,
		  game::PortalSessionKind::LeaseRequest,
		  game::PortalSessionKind::LeaseRoute,
		  game::PortalSessionKind::Proceed,
		  game::PortalSessionKind::Crossed,
		  game::PortalSessionKind::Motion}) {
		CAPTURE(kind);
		game::PortalSessionMessage message;
		message.Kind = kind;
		message.Attempt = 77;
		message.Player = ecs::Entity(53);
		message.World = "far";
		message.Claim = Claim();
		message.Through.Origin = {1, 2, 3};
		message.Through.Frame = core::CFrame({4, 5, 6}) * core::CFrame::Angles(.2f, .3f, .4f);
		message.Through.Scale = 1.5f;
		message.Identity = Identity();
		message.Port = 9000;
		message.Diagnostic = "destination was recreated";
		if (kind == game::PortalSessionKind::Motion) {
			script::PortalTransferMotion motion;
			motion.DestinationIncarnation = message.Claim.DestinationIncarnation;
			motion.DestinationTick = 123;
			motion.InputTick = 9000;
			motion.Frame = message.Through.Frame;
			motion.Linear = {1, 2, 3};
			motion.Angular = {4, 5, 6};
			motion.WalkSpeed = 16;
			motion.JumpSpeed = 7;
			motion.Grounded = true;
			message.Motion = motion;
		}
		const auto bytes = game::EncodePortalSession(message);
		REQUIRE_FALSE(bytes.empty());
		game::PortalSessionMessage decoded;
		REQUIRE(game::DecodePortalSession(bytes, decoded));
		CHECK(decoded.Kind == kind);
		CHECK(decoded.Attempt == message.Attempt);
		CHECK(game::EncodePortalSession(decoded) == bytes);
		for (size_t length = 0; length < bytes.size(); length++) {
			decoded.Attempt = 999;
			CHECK_FALSE(game::DecodePortalSession(std::span(bytes).first(length), decoded));
			CHECK(decoded.Attempt == 999);
		}
		auto trailing = bytes;
		trailing.push_back(std::byte{0});
		CHECK_FALSE(game::DecodePortalSession(trailing, decoded));
	}
}

TEST_CASE("portal session wire validates names mapping identity and capability", "[game][portal-session]") {
	game::PortalSessionMessage message;
	message.Kind = game::PortalSessionKind::Transfer;
	message.Attempt = 1;
	message.Claim = Claim();
	message.Identity = Identity();
	message.Port = 9000;
	REQUIRE_FALSE(game::EncodePortalSession(message).empty());
	SECTION("nonfinite or invalid mapping") {
		message.Through.Scale = std::numeric_limits<float>::infinity();
		CHECK(game::EncodePortalSession(message).empty());
		message.Through.Scale = -1;
		CHECK(game::EncodePortalSession(message).empty());
	}
	SECTION("unbounded world name") {
		message.Claim.Destination.assign(257, 'a');
		CHECK(game::EncodePortalSession(message).empty());
	}
	SECTION("empty capability or unknown destination incarnation") {
		message.Claim.Capability.fill(std::byte{0});
		CHECK(game::EncodePortalSession(message).empty());
		message.Claim = Claim();
		message.Claim.DestinationIncarnation = 0;
		CHECK(game::EncodePortalSession(message).empty());
		message.Kind = game::PortalSessionKind::LeaseRequest;
		CHECK_FALSE(game::EncodePortalSession(message).empty());
	}
}

TEST_CASE("portal admission binds exact incarnation identity and exclusive peer", "[game][portal-session]") {
	game::PortalSessionLeases leases;
	const auto claim = Claim();
	const auto identity = Identity();
	const ecs::Entity player(53);
	REQUIRE(leases.Offer(claim, identity, 10));
	SECTION("wrong identity") {
		auto wrong = identity;
		wrong.Value[0]++;
		CHECK_FALSE(leases.Reserve(claim, wrong, 1, player, 11));
	}
	SECTION("recreated destination") {
		auto wrong = claim;
		wrong.DestinationIncarnation++;
		CHECK_FALSE(leases.Reserve(wrong, identity, 1, player, 11));
	}
	SECTION("wrong source session or capability") {
		auto wrong = claim;
		wrong.SourceSession++;
		CHECK_FALSE(leases.Reserve(wrong, identity, 1, player, 11));
		wrong = claim;
		wrong.Capability[31] ^= std::byte{1};
		CHECK_FALSE(leases.Reserve(wrong, identity, 1, player, 11));
	}
	SECTION("exclusive idempotent commit and reconnect after lost acknowledgement") {
		REQUIRE(leases.Reserve(claim, identity, 1, player, 11));
		CHECK_FALSE(leases.Committed(claim, 1, 11));
		CHECK_FALSE(leases.Reserve(claim, identity, 2, player, 11));
		REQUIRE(leases.Commit(claim, 1, 12));
		CHECK(leases.Commit(claim, 1, 13));
		CHECK(leases.Committed(claim, 1, 13));
		leases.Drop(1);
		REQUIRE(leases.Reserve(claim, identity, 2, player, 14));
		CHECK(leases.Committed(claim, 2, 14));
		CHECK(leases.Player(claim, 2, 14) == player);
		CHECK_FALSE(leases.Reserve(claim, identity, 2, ecs::Entity(54), 14));
	}
	SECTION("failed join releases reservation and expiry does not depend on simulation ticks") {
		REQUIRE(leases.Reserve(claim, identity, 1, player, 11));
		leases.Drop(1);
		REQUIRE(leases.Reserve(claim, identity, 2, player, 12));
		CHECK_FALSE(leases.Commit(claim, 2, 40));
		CHECK_FALSE(leases.Player(claim, 2, 40).has_value());
		leases.Expire(40);
		CHECK(leases.Size() == 0);
	}
}

TEST_CASE(
	"portal admission caps resident leases without losing existing reservations", "[game][portal-session]"
) {
	game::PortalSessionLeases leases;
	for (size_t index = 0; index < game::PortalSessionLeases::MAXIMUM_LEASES; index++) {
		REQUIRE(leases.Offer(Claim(index + 1), Identity(), 1));
	}
	CHECK_FALSE(leases.Offer(Claim(1000), Identity(), 1));
	CHECK(leases.Reserve(Claim(1), Identity(), 1, ecs::Entity(53), 2));
	leases.Expire(31);
	CHECK(leases.Size() == 0);
	CHECK(leases.Offer(Claim(1000), Identity(), 31));
}

TEST_CASE("one portal connection reserves only one transferred player", "[game][portal-session]") {
	game::PortalSessionLeases leases;
	REQUIRE(leases.Offer(Claim(1), Identity(), 1));
	REQUIRE(leases.Offer(Claim(2), Identity(), 1));
	CHECK_FALSE(leases.Reserved(7, 1));
	REQUIRE(leases.Reserve(Claim(1), Identity(), 7, ecs::Entity(53), 2));
	CHECK(leases.Reserved(7, 2));
	CHECK_FALSE(leases.Reserve(Claim(2), Identity(), 7, ecs::Entity(54), 2));
	leases.Drop(7);
	CHECK_FALSE(leases.Reserved(7, 2));
	CHECK(leases.Reserve(Claim(2), Identity(), 7, ecs::Entity(54), 2));
	CHECK_FALSE(leases.Reserved(7, 31));
}

TEST_CASE(
	"portal session motion requires the matching destination and finite pose", "[game][portal-session]"
) {
	using namespace engine;
	game::PortalSessionMessage message;
	message.Kind = game::PortalSessionKind::Motion;
	message.Attempt = 1;
	message.Claim = Claim();
	CHECK(game::EncodePortalSession(message).empty());
	script::PortalTransferMotion motion;
	motion.DestinationIncarnation = message.Claim.DestinationIncarnation;
	motion.DestinationTick = 1;
	message.Motion = motion;
	REQUIRE_FALSE(game::EncodePortalSession(message).empty());
	++message.Motion->DestinationIncarnation;
	CHECK(game::EncodePortalSession(message).empty());
	message.Motion = motion;
	message.Motion->Linear.X = std::numeric_limits<float>::quiet_NaN();
	CHECK(game::EncodePortalSession(message).empty());
	message.Motion = motion;
	message.Motion->DestinationTick = 0;
	CHECK(game::EncodePortalSession(message).empty());
}
