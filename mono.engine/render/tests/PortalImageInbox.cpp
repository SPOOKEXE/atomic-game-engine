#include <engine/render/PortalImageInbox.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <limits>

TEST_SUITE_ID("engine.render.portalimageinbox")

using namespace engine::render;
using namespace std::chrono_literals;

namespace {
	constexpr PortalEndpointView LOCAL{"Origin", "portals", 1, 2};
	constexpr PortalEndpointView REMOTE{"Destination", "portals", 3, 4};
	constexpr PortalImageInbox::Time START{};

	PortalImageRequest Request(std::string portal = "Door") {
		PortalImageRequest request;
		request.Key.PortalKey = std::move(portal);
		request.Key.CameraRevision = 1;
		request.Key.SeamRevision = 2;
		request.Width = 2;
		request.Height = 2;
		request.PixelBudget = 4;
		return request;
	}
	std::vector<std::byte> Reply(
		const PortalIssueResult &issued,
		uint64_t tick = 10,
		PortalImageStatus status = PortalImageStatus::Ok,
		uint32_t width = 0
	) {
		PortalImageReply reply;
		reply.Key = issued.Request.Key;
		reply.Scope = issued.Request.Scope;
		reply.Status = status;
		reply.CaptureTick = tick;
		if (status == PortalImageStatus::Ok) {
			reply.Width = width == 0 ? issued.Request.Width : width;
			reply.Height = issued.Request.Height;
			reply.RowStride = reply.Width * 8;
			reply.Pixels.assign(size_t(reply.RowStride) * reply.Height, std::byte{0});
			reply.PixelHash = engine::assets::Hasher::Of(reply.Pixels);
		} else {
			reply.Diagnostic = "destination cannot capture";
		}
		std::string error;
		std::vector<std::byte> wire;
		REQUIRE(EncodePortalImageReply(reply, wire, error));
		return wire;
	}
}

TEST_CASE(
	"portal inbox correlates authenticated endpoints before reading reply bytes", "[render][portal-inbox]"
) {
	PortalImageInbox inbox;
	const auto issued = inbox.Issue(LOCAL, REMOTE, Request(), START);
	REQUIRE(issued.Status == PortalInboxStatus::Issued);
	CHECK(issued.Request.Key.RequestId == 1);
	PortalImageRequest decoded;
	std::string error;
	REQUIRE(DecodePortalImageRequest(issued.Wire, decoded, error));
	CHECK(decoded == issued.Request);
	const std::array junk{std::byte{0xff}};
	CHECK(
		inbox.AcceptAuthenticated(REMOTE, LOCAL, 999, junk, START).Status == PortalInboxStatus::Unsolicited
	);
	for (int field = 0; field < 8; ++field) {
		auto from = REMOTE;
		auto to = LOCAL;
		switch (field) {
		case 0:
			from.World = "Other";
			break;
		case 1:
			from.Channel = "Other";
			break;
		case 2:
			from.Session++;
			break;
		case 3:
			from.Generation++;
			break;
		case 4:
			to.World = "Other";
			break;
		case 5:
			to.Channel = "Other";
			break;
		case 6:
			to.Session++;
			break;
		case 7:
			to.Generation++;
			break;
		}
		const auto rejected = inbox.AcceptAuthenticated(from, to, issued.Request.Key.RequestId, junk, START);
		CHECK(rejected.Status == PortalInboxStatus::EndpointMismatch);
		CHECK(rejected.Error.empty());
		CHECK(inbox.Usage().PendingCount == 1);
	}
	const auto wire = Reply(issued);
	REQUIRE(
		inbox.AcceptAuthenticated(REMOTE, LOCAL, issued.Request.Key.RequestId, wire, START).Status ==
		PortalInboxStatus::Accepted
	);
	CHECK(inbox.Usage().PendingCount == 0);
	CHECK(inbox.Usage().HeldCount == 1);
	CHECK(
		inbox.AcceptAuthenticated(REMOTE, LOCAL, issued.Request.Key.RequestId, wire, START).Status ==
		PortalInboxStatus::Unsolicited
	);
	auto held = inbox.Take(LOCAL, REMOTE, "Door", START);
	REQUIRE(held);
	CHECK(held->Key == issued.Request.Key);
	CHECK(held->Pixels.size() == 32);
	CHECK(inbox.Usage().HeldBytes == 0);
	CHECK_FALSE(inbox.Take(LOCAL, REMOTE, "Door", START));
}

TEST_CASE(
	"portal inbox supersedes revisions and refuses stale keys shapes and captures", "[render][portal-inbox]"
) {
	PortalImageInbox inbox;
	auto request = Request();
	const auto first = inbox.Issue(LOCAL, REMOTE, request, START);
	REQUIRE(first.Status == PortalInboxStatus::Issued);
	CHECK(inbox.Issue(LOCAL, REMOTE, request, START).Status == PortalInboxStatus::Busy);
	request.Key.CameraRevision++;
	const auto second = inbox.Issue(LOCAL, REMOTE, request, START);
	REQUIRE(second.Status == PortalInboxStatus::Issued);
	CHECK(second.Request.Key.RequestId > first.Request.Key.RequestId);
	CHECK(inbox.Usage().PendingCount == 1);
	CHECK(
		inbox.AcceptAuthenticated(REMOTE, LOCAL, first.Request.Key.RequestId, Reply(first), START).Status ==
		PortalInboxStatus::Unsolicited
	);
	for (int field = 0; field < 4; ++field) {
		auto wrong = second;
		switch (field) {
		case 0:
			wrong.Request.Key.PortalKey = "Other";
			break;
		case 1:
			wrong.Request.Key.CameraRevision++;
			break;
		case 2:
			wrong.Request.Key.SeamRevision++;
			break;
		case 3:
			wrong.Request.Key.RequestId++;
			break;
		}
		CHECK(
			inbox.AcceptAuthenticated(REMOTE, LOCAL, second.Request.Key.RequestId, Reply(wrong), START)
				.Status == PortalInboxStatus::Stale
		);
		CHECK(inbox.Usage().PendingCount == 1);
	}
	CHECK(
		inbox
			.AcceptAuthenticated(
				REMOTE,
				LOCAL,
				second.Request.Key.RequestId,
				Reply(second, 10, PortalImageStatus::Ok, 512),
				START
			)
			.Status == PortalInboxStatus::Stale
	);
	auto broken = Reply(second);
	broken.back() = std::byte{1};
	CHECK(
		inbox.AcceptAuthenticated(REMOTE, LOCAL, second.Request.Key.RequestId, broken, START).Status ==
		PortalInboxStatus::Malformed
	);
	REQUIRE(
		inbox.AcceptAuthenticated(REMOTE, LOCAL, second.Request.Key.RequestId, Reply(second, 20), START)
			.Status == PortalInboxStatus::Accepted
	);
	request.Key.SeamRevision++;
	const auto third = inbox.Issue(LOCAL, REMOTE, request, START);
	REQUIRE(third.Status == PortalInboxStatus::Issued);
	CHECK(
		inbox.AcceptAuthenticated(REMOTE, LOCAL, third.Request.Key.RequestId, Reply(third, 19), START)
			.Status == PortalInboxStatus::Stale
	);
	REQUIRE(
		inbox.AcceptAuthenticated(REMOTE, LOCAL, third.Request.Key.RequestId, Reply(third, 21), START)
			.Status == PortalInboxStatus::Accepted
	);
	const auto held = inbox.Take(LOCAL, REMOTE, "Door", START);
	REQUIRE(held);
	CHECK(held->CaptureTick == 21);
}

TEST_CASE(
	"portal inbox bounds queues and keeps failures separate from held images", "[render][portal-inbox]"
) {
	PortalInboxLimits limits;
	limits.PendingCount = 1;
	limits.HeldCount = 1;
	PortalImageInbox inbox(limits);
	const auto first = inbox.Issue(LOCAL, REMOTE, Request(), START);
	REQUIRE(first.Status == PortalInboxStatus::Issued);
	CHECK(inbox.Issue(LOCAL, REMOTE, Request("Other"), START).Status == PortalInboxStatus::Full);
	REQUIRE(
		inbox.AcceptAuthenticated(REMOTE, LOCAL, first.Request.Key.RequestId, Reply(first), START).Status ==
		PortalInboxStatus::Accepted
	);
	const auto other = inbox.Issue(LOCAL, REMOTE, Request("Other"), START);
	REQUIRE(other.Status == PortalInboxStatus::Issued);
	CHECK(
		inbox.AcceptAuthenticated(REMOTE, LOCAL, other.Request.Key.RequestId, Reply(other), START).Status ==
		PortalInboxStatus::Full
	);
	const auto failure = inbox.AcceptAuthenticated(
		REMOTE, LOCAL, other.Request.Key.RequestId, Reply(other, 0, PortalImageStatus::Unsupported), START
	);
	REQUIRE(failure.Status == PortalInboxStatus::CompletedFailure);
	REQUIRE(failure.Failure);
	CHECK(failure.Failure->Key == other.Request.Key);
	CHECK(failure.Failure->Status == PortalImageStatus::Unsupported);
	CHECK_FALSE(failure.Failure->Diagnostic.empty());
	CHECK(inbox.Usage().HeldCount == 1);
	CHECK(inbox.Usage().PendingCount == 0);
	CHECK(inbox.Take(LOCAL, REMOTE, "Door", START).has_value());

	limits.HeldBytes = 1;
	PortalImageInbox small(limits);
	const auto bounded = small.Issue(LOCAL, REMOTE, Request(), START);
	REQUIRE(bounded.Status == PortalInboxStatus::Issued);
	CHECK(
		small.AcceptAuthenticated(REMOTE, LOCAL, bounded.Request.Key.RequestId, Reply(bounded), START)
			.Status == PortalInboxStatus::Full
	);
	CHECK(small.Usage().HeldBytes == 0);
	CHECK(
		small
			.AcceptAuthenticated(
				REMOTE,
				LOCAL,
				bounded.Request.Key.RequestId,
				Reply(bounded, 0, PortalImageStatus::Failed),
				START
			)
			.Status == PortalInboxStatus::CompletedFailure
	);
	limits.PendingBytes = 1;
	PortalImageInbox noMetadata(limits);
	CHECK(noMetadata.Issue(LOCAL, REMOTE, Request(), START).Status == PortalInboxStatus::Full);
	CHECK(noMetadata.Usage().PendingBytes == 0);
}

TEST_CASE(
	"portal inbox separates local worlds and invalidates exact endpoint incarnations",
	"[render][portal-inbox]"
) {
	PortalImageInbox inbox;
	const PortalEndpointView otherLocal{"OtherOrigin", "portals", 1, 2};
	const auto first = inbox.Issue(LOCAL, REMOTE, Request(), START);
	const auto second = inbox.Issue(otherLocal, REMOTE, Request(), START);
	REQUIRE(first.Status == PortalInboxStatus::Issued);
	REQUIRE(second.Status == PortalInboxStatus::Issued);
	REQUIRE(
		inbox.AcceptAuthenticated(REMOTE, otherLocal, second.Request.Key.RequestId, Reply(second), START)
			.Status == PortalInboxStatus::Accepted
	);
	inbox.InvalidatePortal(LOCAL, "Door");
	CHECK(inbox.Usage().PendingCount == 0);
	CHECK(inbox.Usage().HeldCount == 1);
	auto wrongIncarnation = REMOTE;
	wrongIncarnation.Generation++;
	inbox.InvalidateEndpoint(wrongIncarnation);
	CHECK(inbox.Usage().HeldCount == 1);
	inbox.InvalidateEndpoint(REMOTE);
	CHECK(inbox.Usage().HeldCount == 0);
	inbox.Clear();
	const auto next = inbox.Issue(LOCAL, REMOTE, Request(), START);
	CHECK(next.Request.Key.RequestId > second.Request.Key.RequestId);
	CHECK(
		inbox.AcceptAuthenticated(REMOTE, LOCAL, first.Request.Key.RequestId, Reply(first), START).Status ==
		PortalInboxStatus::Unsolicited
	);
}

TEST_CASE(
	"portal inbox expiry and invalid limits fail closed without clock reads", "[render][portal-inbox]"
) {
	PortalInboxLimits limits;
	limits.Timeout = 10ms;
	PortalImageInbox inbox(limits);
	const auto issued = inbox.Issue(LOCAL, REMOTE, Request(), START);
	REQUIRE(issued.Status == PortalInboxStatus::Issued);
	REQUIRE(inbox.Expire(START + 9ms));
	CHECK(inbox.Usage().PendingCount == 1);
	CHECK_FALSE(inbox.Expire(START));
	CHECK(inbox.Usage().PendingCount == 1);
	REQUIRE(inbox.Expire(START + 10ms));
	CHECK(inbox.Usage().PendingCount == 0);
	CHECK(
		inbox.AcceptAuthenticated(REMOTE, LOCAL, issued.Request.Key.RequestId, Reply(issued), START + 10ms)
			.Status == PortalInboxStatus::Unsolicited
	);
	const auto next = inbox.Issue(LOCAL, REMOTE, Request(), START + 10ms);
	REQUIRE(next.Status == PortalInboxStatus::Issued);
	REQUIRE(
		inbox.AcceptAuthenticated(REMOTE, LOCAL, next.Request.Key.RequestId, Reply(next), START + 11ms)
			.Status == PortalInboxStatus::Accepted
	);
	CHECK_FALSE(inbox.Take(LOCAL, REMOTE, "Door", START + 21ms));
	CHECK(inbox.Usage().HeldCount == 0);
	CHECK_FALSE(inbox.Expire(PortalImageInbox::Time::max()));
	for (int invalid = 0; invalid < 4; ++invalid) {
		auto bad = limits;
		switch (invalid) {
		case 0:
			bad.PendingCount = std::numeric_limits<size_t>::max();
			break;
		case 1:
			bad.HeldBytes = std::numeric_limits<size_t>::max();
			break;
		case 2:
			bad.Timeout = std::chrono::milliseconds::max();
			break;
		case 3:
			bad.Timeout = 0ms;
			break;
		}
		PortalImageInbox refused(bad);
		CHECK(refused.Issue(LOCAL, REMOTE, Request(), START).Status == PortalInboxStatus::Invalid);
		CHECK(refused.Usage().PendingCount == 0);
	}
}

TEST_CASE(
	"portal inbox reserves replacement pixels while the old image is still held", "[render][portal-inbox]"
) {
	const uint32_t extent = GENERATE(2u, 128u);
	PortalInboxLimits limits;
	limits.HeldBytes = LOCAL.World.size() + LOCAL.Channel.size() + REMOTE.World.size() +
					   REMOTE.Channel.size() + 4 + extent * extent * 8;
	PortalImageInbox inbox(limits);
	auto request = Request();
	request.Width = request.Height = extent;
	request.PixelBudget = extent * extent;
	const auto first = inbox.Issue(LOCAL, REMOTE, request, START);
	REQUIRE(first.Status == PortalInboxStatus::Issued);
	REQUIRE(
		inbox.AcceptAuthenticated(REMOTE, LOCAL, first.Request.Key.RequestId, Reply(first), START).Status ==
		PortalInboxStatus::Accepted
	);
	request.Key.CameraRevision++;
	const auto second = inbox.Issue(LOCAL, REMOTE, request, START);
	REQUIRE(second.Status == PortalInboxStatus::Issued);
	CHECK(
		inbox.AcceptAuthenticated(REMOTE, LOCAL, second.Request.Key.RequestId, Reply(second), START).Status ==
		PortalInboxStatus::Full
	);
	const auto held = inbox.Take(LOCAL, REMOTE, "Door", START);
	REQUIRE(held);
	CHECK(held->Key == first.Request.Key);
	CHECK(
		inbox.AcceptAuthenticated(REMOTE, LOCAL, second.Request.Key.RequestId, Reply(second), START).Status ==
		PortalInboxStatus::Accepted
	);
}

TEST_CASE(
	"portal send rollback preserves held image and scope cannot be downgraded", "[render][portal-inbox]"
) {
	PortalImageInbox inbox;
	auto request = Request();
	const auto first = inbox.Issue(LOCAL, REMOTE, request, START);
	REQUIRE(first.Status == PortalInboxStatus::Issued);
	REQUIRE(
		inbox.AcceptAuthenticated(REMOTE, LOCAL, first.Request.Key.RequestId, Reply(first), START).Status ==
		PortalInboxStatus::Accepted
	);
	request.Key.CameraRevision++;
	const auto second = inbox.Issue(LOCAL, REMOTE, request, START);
	REQUIRE(second.Status == PortalInboxStatus::Issued);
	CHECK(inbox.CancelRequest(second.Request.Key.RequestId));
	CHECK_FALSE(inbox.CancelRequest(second.Request.Key.RequestId));
	CHECK(inbox.Usage().PendingCount == 0);
	CHECK(inbox.Usage().HeldCount == 1);
	CHECK(
		inbox.AcceptAuthenticated(REMOTE, LOCAL, second.Request.Key.RequestId, Reply(second), START).Status ==
		PortalInboxStatus::Unsolicited
	);
	const auto third = inbox.Issue(LOCAL, REMOTE, request, START);
	REQUIRE(third.Status == PortalInboxStatus::Issued);
	CHECK(third.Request.Key.RequestId > second.Request.Key.RequestId);
	auto partial = third;
	partial.Request.Scope = PortalImageScope::OpaqueLighting;
	CHECK(
		inbox.AcceptAuthenticated(REMOTE, LOCAL, third.Request.Key.RequestId, Reply(partial), START).Status ==
		PortalInboxStatus::Stale
	);
	CHECK(inbox.Usage().PendingCount == 1);
	const auto held = inbox.Take(LOCAL, REMOTE, "Door", START);
	REQUIRE(held);
	CHECK(held->Key == first.Request.Key);
}

TEST_CASE("portal inbox charges paired depth before decoding", "[render][portal-inbox][portal-depth]") {
	PortalImageInbox measured;
	auto issued = measured.Issue(LOCAL, REMOTE, Request(), START);
	REQUIRE(issued.Status == PortalInboxStatus::Issued);
	const auto color = Reply(issued);
	REQUIRE(
		measured.AcceptAuthenticated(REMOTE, LOCAL, issued.Request.Key.RequestId, color, START).Status ==
		PortalInboxStatus::Accepted
	);
	const auto colorBytes = measured.Usage().HeldBytes;
	PortalImageReply reply;
	std::string error;
	REQUIRE(DecodePortalImageReply(color, reply, error));
	reply.Depth.assign(size_t(reply.Width) * reply.Height * 4, std::byte{});
	reply.DepthHash = engine::assets::Hasher::Of(reply.Depth);
	std::vector<std::byte> paired;
	REQUIRE(EncodePortalImageReply(reply, paired, error));
	for (const size_t shortBy : {1u, 0u}) {
		PortalInboxLimits limits;
		limits.HeldBytes = colorBytes + reply.Depth.size() - shortBy;
		PortalImageInbox inbox(limits);
		issued = inbox.Issue(LOCAL, REMOTE, Request(), START);
		REQUIRE(issued.Status == PortalInboxStatus::Issued);
		const auto accepted =
			inbox.AcceptAuthenticated(REMOTE, LOCAL, issued.Request.Key.RequestId, paired, START);
		if (shortBy) {
			CHECK(accepted.Status == PortalInboxStatus::Full);
			CHECK(inbox.Usage().HeldBytes == 0);
			CHECK(inbox.Usage().PendingCount == 1);
		} else {
			REQUIRE(accepted.Status == PortalInboxStatus::Accepted);
			CHECK(inbox.Usage().HeldBytes == limits.HeldBytes);
			const auto image = inbox.Take(LOCAL, REMOTE, issued.Request.Key.PortalKey, START);
			REQUIRE(image);
			CHECK(*image == reply);
			CHECK(inbox.Usage().HeldBytes == 0);
		}
	}
}

TEST_CASE("ordered requests cannot complete with flattened pixels", "[render][portal-inbox]") {
	PortalImageInbox inbox;
	auto request = Request();
	request.OrderedLayers = true;
	request.Scope = PortalImageScope::OpaqueLighting;
	request.PixelBudget *= 4;
	const auto issued = inbox.Issue(LOCAL, REMOTE, request, START);
	REQUIRE(issued.Status == PortalInboxStatus::Issued);
	CHECK(
		inbox.AcceptAuthenticated(REMOTE, LOCAL, issued.Request.Key.RequestId, Reply(issued), START).Status ==
		PortalInboxStatus::Stale
	);
	CHECK(inbox.Usage().PendingCount == 1);
	CHECK(inbox.Usage().HeldCount == 0);
	const auto failed = Reply(issued, 10, PortalImageStatus::BudgetExceeded);
	CHECK(
		inbox.AcceptAuthenticated(REMOTE, LOCAL, issued.Request.Key.RequestId, failed, START).Status ==
		PortalInboxStatus::CompletedFailure
	);
	CHECK(inbox.Usage().PendingCount == 0);
}

TEST_CASE("ordered inbox admission preserves complete groups under pressure", "[render][portal-inbox]") {
	auto request = Request();
	request.Scope = PortalImageScope::OpaqueLighting;
	request.OrderedLayers = true;
	request.PixelBudget *= 4;
	const auto layerSet = [](const PortalIssueResult &issued, uint64_t tick) {
		PortalImageLayerSet layers;
		auto &image = layers.Opaque;
		image.Key = issued.Request.Key;
		image.Scope = PortalImageScope::OpaqueLighting;
		image.Status = PortalImageStatus::Ok;
		image.CaptureTick = tick;
		image.Width = image.Height = 2;
		image.RowStride = 16;
		image.Pixels.assign(32, std::byte{});
		image.Depth.assign(16, std::byte{});
		image.PixelHash = engine::assets::Hasher::Of(image.Pixels);
		image.DepthHash = engine::assets::Hasher::Of(image.Depth);
		layers.Transparent.assign(2, image);
		return layers;
	};
	const auto encode = [](const PortalImageLayerSet &layers) {
		std::vector<std::byte> wire;
		std::string error;
		REQUIRE(EncodePortalImageLayerSet(layers, wire, error));
		return wire;
	};
	const size_t heldBytes = LOCAL.World.size() + LOCAL.Channel.size() + REMOTE.World.size() +
							 REMOTE.Channel.size() + 3 * request.Key.PortalKey.size() + 3 * 4 * 12;
	PortalInboxLimits limits;
	limits.HeldBytes = heldBytes * 2 - 1;
	PortalImageInbox inbox(limits);
	const auto first = inbox.Issue(LOCAL, REMOTE, request, START);
	REQUIRE(first.Status == PortalInboxStatus::Issued);
	const auto original = layerSet(first, 10);
	const auto wire = encode(original);
	REQUIRE(MatchPortalImageLayerSet(wire, first.Request.Key, 2, 2));
	CHECK_FALSE(MatchPortalImageLayerSet(wire, first.Request.Key, 1, 2));
	auto incomplete = original;
	incomplete.Transparent.pop_back();
	const auto incompleteWire = encode(incomplete);
	CHECK_FALSE(MatchPortalImageLayerSet(incompleteWire, first.Request.Key, 2, 2));
	CHECK(
		inbox.AcceptAuthenticated(REMOTE, LOCAL, first.Request.Key.RequestId, incompleteWire, START).Status ==
		PortalInboxStatus::Stale
	);
	CHECK(inbox.Usage().PendingCount == 1);

	for (const bool sender : {false, true}) {
		auto from = REMOTE, to = LOCAL;
		if (sender)
			++from.Generation;
		else
			++to.Session;
		CHECK(
			inbox.AcceptAuthenticated(from, to, first.Request.Key.RequestId, wire, START).Status ==
			PortalInboxStatus::EndpointMismatch
		);
		CHECK(inbox.Usage().HeldCount == 0);
	}
	REQUIRE(
		inbox.AcceptAuthenticated(REMOTE, LOCAL, first.Request.Key.RequestId, wire, START).Status ==
		PortalInboxStatus::Accepted
	);
	CHECK(inbox.Usage().HeldCount == 1);
	CHECK(inbox.Usage().HeldBytes == heldBytes);
	CHECK_FALSE(inbox.Take(LOCAL, REMOTE, "Door", START));
	CHECK(inbox.Usage().HeldCount == 1);
	++request.Key.CameraRevision;
	const auto next = inbox.Issue(LOCAL, REMOTE, request, START);
	REQUIRE(next.Status == PortalInboxStatus::Issued);
	const auto replacement = layerSet(next, 11);
	const auto newWire = encode(replacement);
	CHECK(
		inbox.AcceptAuthenticated(REMOTE, LOCAL, next.Request.Key.RequestId, wire, START).Status ==
		PortalInboxStatus::Stale
	);
	CHECK(
		inbox.AcceptAuthenticated(REMOTE, LOCAL, next.Request.Key.RequestId, newWire, START).Status ==
		PortalInboxStatus::Full
	);
	CHECK(inbox.Usage().PendingCount == 1);
	CHECK(inbox.Usage().HeldBytes == heldBytes);
	auto corruptUnderPressure = newWire;
	corruptUnderPressure.back() ^= std::byte{1};
	CHECK(
		inbox.AcceptAuthenticated(REMOTE, LOCAL, next.Request.Key.RequestId, corruptUnderPressure, START)
			.Status == PortalInboxStatus::Full
	);

	REQUIRE(inbox.TakeLayers(LOCAL, REMOTE, "Door", START) == original);
	CHECK(inbox.Usage().HeldBytes == 0);
	auto damaged = newWire;
	damaged.back() ^= std::byte{1};
	CHECK(
		inbox.AcceptAuthenticated(REMOTE, LOCAL, next.Request.Key.RequestId, damaged, START).Status ==
		PortalInboxStatus::Malformed
	);
	CHECK(inbox.Usage().PendingCount == 1);
	CHECK(inbox.Usage().HeldBytes == 0);
	REQUIRE(
		inbox.AcceptAuthenticated(REMOTE, LOCAL, next.Request.Key.RequestId, newWire, START).Status ==
		PortalInboxStatus::Accepted
	);
	CHECK(inbox.Usage().HeldBytes == heldBytes);
	++request.Key.CameraRevision;
	const auto failed = inbox.Issue(LOCAL, REMOTE, request, START);
	REQUIRE(failed.Status == PortalInboxStatus::Issued);
	CHECK(
		inbox
			.AcceptAuthenticated(
				REMOTE,
				LOCAL,
				failed.Request.Key.RequestId,
				Reply(failed, 12, PortalImageStatus::BudgetExceeded),
				START
			)
			.Status == PortalInboxStatus::CompletedFailure
	);
	CHECK(inbox.Usage().HeldBytes == heldBytes);
	REQUIRE(inbox.TakeLayers(LOCAL, REMOTE, "Door", START) == replacement);
	CHECK(inbox.Usage().HeldBytes == 0);
	const auto last = inbox.Issue(LOCAL, REMOTE, request, START);
	REQUIRE(last.Status == PortalInboxStatus::Issued);
	CHECK(
		inbox
			.AcceptAuthenticated(REMOTE, LOCAL, last.Request.Key.RequestId, encode(layerSet(last, 13)), START)
			.Status == PortalInboxStatus::Accepted
	);
	inbox.InvalidateEndpoint(REMOTE);
	CHECK(inbox.Usage().HeldBytes == 0);
	CHECK_FALSE(inbox.TakeLayers(LOCAL, REMOTE, "Door", START));
	const auto expiring = inbox.Issue(LOCAL, REMOTE, request, START);
	REQUIRE(expiring.Status == PortalInboxStatus::Issued);
	REQUIRE(
		inbox
			.AcceptAuthenticated(
				REMOTE, LOCAL, expiring.Request.Key.RequestId, encode(layerSet(expiring, 14)), START
			)
			.Status == PortalInboxStatus::Accepted
	);
	CHECK_FALSE(inbox.TakeLayers(LOCAL, REMOTE, "Door", START + 1s));
	CHECK(inbox.Usage().HeldCount == 0);
	CHECK(inbox.Usage().HeldBytes == 0);
}
