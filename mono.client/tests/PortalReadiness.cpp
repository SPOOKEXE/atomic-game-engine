#include "PortalReadiness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("client.portalreadiness")

namespace {
	engine::assets::ContentHash BaselineHash() {
		engine::assets::ContentHash hash;
		hash.Digest[0] = 1;
		return hash;
	}

	client::PortalReadinessEvidence ReadyEvidence() {
		return {
			.RequiredBaseline = 17,
			.ReplicaBaseline = 17,
			.RequiredBaselineHash = BaselineHash(),
			.ReplicaBaselineHash = BaselineHash(),
			.RequiredTopologyRevision = 9,
			.ReplicaTopologyRevision = 9,
			.RequiredAuthorityEpoch = 2,
			.ReplicaAuthorityEpoch = 2,
			.RequiredPrepareRevision = 3,
			.ReplicaPrepareRevision = 3,
			.RequiredClockDomain = "portal-clock",
			.ReplicaClockDomain = "portal-clock",
			.RequiredSourceTick = 20,
			.ReplicaSourceTick = 20,
			.RequiredDestinationTick = 24,
			.ReplicaDestinationTick = 24,
			.RequiredAssetRevision = 4,
			.ResidentAssetRevision = 4,
			.RequiredPoseBegin = 20,
			.RequiredPoseEnd = 24,
			.ReplicaPoseBegin = 20,
			.ReplicaPoseEnd = 24,
			.Distance = 3,
			.AssetsResident = true,
			.CapacityReserved = true
		};
	}

	client::PortalReadinessReservation SmallReservation() {
		return {.DestinationEyeBytes = 128, .ReturnEyeBytes = 64, .AssetUploadBytes = 64};
	}
}

TEST_CASE("Portal readiness promotes only a complete local replica") {
	client::PortalReadinessController readiness(
		{.EnterDistance = 4, .ExitDistance = 6, .CapacityBytes = 1024}
	);
	auto evidence = ReadyEvidence();
	REQUIRE(readiness.Reserve(SmallReservation()));

	const auto live = readiness.Evaluate(evidence);
	CHECK(live.Live);
	CHECK(live.ImageOnly == client::PortalImageOnlyReason::None);

	evidence.ReplicaBaseline = 16;
	const auto stale = readiness.Evaluate(evidence);
	CHECK_FALSE(stale.Live);
	CHECK(stale.ImageOnly == client::PortalImageOnlyReason::ReplicaBaseline);
}

TEST_CASE("Portal readiness refuses a stale same-tick sealed baseline hash") {
	client::PortalReadinessController readiness(
		{.EnterDistance = 4, .ExitDistance = 6, .CapacityBytes = 1024}
	);
	auto evidence = ReadyEvidence();
	REQUIRE(readiness.Reserve(SmallReservation()));
	evidence.ReplicaBaselineHash.Digest[0] = 2;
	CHECK(readiness.Evaluate(evidence).ImageOnly == client::PortalImageOnlyReason::ReplicaBaseline);
}

TEST_CASE("Portal readiness refuses a destination topology revision mismatch") {
	client::PortalReadinessController readiness(
		{.EnterDistance = 4, .ExitDistance = 6, .CapacityBytes = 1024}
	);
	auto evidence = ReadyEvidence();
	REQUIRE(readiness.Reserve(SmallReservation()));
	evidence.ReplicaTopologyRevision++;
	CHECK(readiness.Evaluate(evidence).ImageOnly == client::PortalImageOnlyReason::Topology);
}

TEST_CASE("Portal readiness keeps live geometry through the exit hysteresis band") {
	client::PortalReadinessController readiness(
		{.EnterDistance = 4, .ExitDistance = 6, .CapacityBytes = 1024}
	);
	auto evidence = ReadyEvidence();
	REQUIRE(readiness.Reserve(SmallReservation()));
	CHECK(readiness.Evaluate(evidence).Live);

	evidence.Distance = 5;
	CHECK(readiness.Evaluate(evidence).Live);

	evidence.Distance = 7;
	const auto image = readiness.Evaluate(evidence);
	CHECK_FALSE(image.Live);
	CHECK(image.ImageOnly == client::PortalImageOnlyReason::OutsideEnterRange);

	evidence.Distance = 5;
	CHECK_FALSE(readiness.Evaluate(evidence).Live);
}

TEST_CASE("Portal readiness reports capacity and prevents retained capture duplication") {
	client::PortalReadinessController readiness(
		{.EnterDistance = 4, .ExitDistance = 6, .CapacityBytes = 1024}
	);
	auto evidence = ReadyEvidence();
	REQUIRE(readiness.Reserve(SmallReservation()));
	evidence.CapacityReserved = false;
	CHECK(readiness.Evaluate(evidence).ImageOnly == client::PortalImageOnlyReason::Capacity);

	evidence.CapacityReserved = true;
	evidence.RetainedCapture = true;
	const auto live = readiness.Evaluate(evidence);
	CHECK(live.Live);
	CHECK(live.SuppressRetainedCapture);
}

TEST_CASE("Portal readiness requires assets and a complete presentation pose range") {
	client::PortalReadinessController readiness(
		{.EnterDistance = 4, .ExitDistance = 6, .CapacityBytes = 1024}
	);
	auto evidence = ReadyEvidence();
	REQUIRE(readiness.Reserve(SmallReservation()));
	evidence.ResidentAssetRevision = 3;
	CHECK(readiness.Evaluate(evidence).ImageOnly == client::PortalImageOnlyReason::Assets);

	evidence.ResidentAssetRevision = 4;
	evidence.ReplicaPoseEnd = 23;
	CHECK(readiness.Evaluate(evidence).ImageOnly == client::PortalImageOnlyReason::PoseRange);
}

TEST_CASE("Portal readiness keeps an early Ready fence image-only until the local baseline applies") {
	client::PortalReadinessController readiness(
		{.EnterDistance = 4, .ExitDistance = 6, .CapacityBytes = 1024}
	);
	auto evidence = ReadyEvidence();
	REQUIRE(readiness.Reserve(SmallReservation()));
	// The host reported the exact sealed fence, but this client's replica has
	// applied only the tick before its destination baseline.
	evidence.ReplicaPoseBegin = 0;
	evidence.ReplicaPoseEnd = evidence.RequiredDestinationTick - 1;
	CHECK(readiness.Evaluate(evidence).ImageOnly == client::PortalImageOnlyReason::PoseRange);
}

TEST_CASE("Portal readiness holds image-only when the prewarm budget is exhausted") {
	client::PortalReadinessController readiness(
		{.EnterDistance = 4, .ExitDistance = 6, .CapacityBytes = 512}
	);
	auto evidence = ReadyEvidence();
	CHECK_FALSE(
		readiness.Reserve({.DestinationEyeBytes = 512, .ReturnEyeBytes = 256, .AssetUploadBytes = 256})
	);
	CHECK(readiness.Evaluate(evidence).ImageOnly == client::PortalImageOnlyReason::Capacity);
}

TEST_CASE("Portal readiness rejects a stale retained image near a traversable seam") {
	client::PortalReadinessController readiness(
		{.EnterDistance = 4, .ExitDistance = 6, .CapacityBytes = 1024}
	);
	auto evidence = ReadyEvidence();
	REQUIRE(readiness.Reserve(SmallReservation()));
	evidence.AssetsResident = false;
	evidence.RetainedCapture = true;
	evidence.RetainedCaptureFresh = false;
	CHECK(readiness.Evaluate(evidence).ImageOnly == client::PortalImageOnlyReason::StaleCapture);

	evidence.Distance = 7;
	CHECK(readiness.Evaluate(evidence).ImageOnly == client::PortalImageOnlyReason::OutsideEnterRange);
}

TEST_CASE("Portal readiness releases a closed or reversed crossing") {
	client::PortalReadinessController readiness(
		{.EnterDistance = 4, .ExitDistance = 6, .CapacityBytes = 1024}
	);
	auto evidence = ReadyEvidence();
	REQUIRE(readiness.Reserve(SmallReservation()));
	CHECK(readiness.Evaluate(evidence).Live);

	readiness.Release();
	CHECK(readiness.Reserved() == 0);
	CHECK(readiness.Evaluate(evidence).ImageOnly == client::PortalImageOnlyReason::Capacity);
}

TEST_CASE("Portal approach promotes only its authenticated destination route") {
	engine::game::PortalSessionMessage approach;
	approach.Kind = engine::game::PortalSessionKind::Approach;
	approach.Destination = "far";
	approach.Port = 9000;
	approach.Identity.Value[0] = 7;
	engine::game::PortalSessionMessage transfer;
	transfer.Kind = engine::game::PortalSessionKind::Transfer;
	transfer.Claim.Destination = "far";
	transfer.Port = 9000;
	transfer.Identity = approach.Identity;

	CHECK(client::PortalApproachMatchesTransfer(approach, transfer));
	transfer.Port = 9001;
	CHECK_FALSE(client::PortalApproachMatchesTransfer(approach, transfer));
	transfer.Port = approach.Port;
	transfer.Identity.Value[0] = 8;
	CHECK_FALSE(client::PortalApproachMatchesTransfer(approach, transfer));
}

TEST_CASE("Portal readiness retains a near-field approach through its 20 metre exit range") {
	client::PortalReadinessController readiness(
		{.EnterDistance = 16, .ExitDistance = 20, .CapacityBytes = 1024}
	);
	auto evidence = ReadyEvidence();
	evidence.Distance = 15;
	REQUIRE(readiness.Reserve(SmallReservation()));
	CHECK(readiness.Evaluate(evidence).Live);
	evidence.Distance = 19;
	CHECK(readiness.Evaluate(evidence).Live);
	evidence.Distance = 21;
	CHECK_FALSE(readiness.Evaluate(evidence).Live);
}

TEST_CASE("Portal readiness accounts for both 1080p eyes and content uploads") {
	constexpr uint64_t MEBIBYTE = 1024u * 1024u;
	const auto reservation = client::PortalReadinessReservation::ForRgba16fEyes(1920, 1080, 64 * MEBIBYTE);
	REQUIRE(reservation);
	CHECK(reservation->DestinationEyeBytes == 1920ull * 1080ull * 8ull);
	CHECK(reservation->ReturnEyeBytes == 1920ull * 1080ull * 8ull);
	CHECK(reservation->AssetUploadBytes == 64 * MEBIBYTE);
	REQUIRE(reservation->TotalBytes());

	client::PortalReadinessController readiness(
		{.EnterDistance = 4, .ExitDistance = 6, .CapacityBytes = *reservation->TotalBytes()}
	);
	REQUIRE(readiness.Reserve(*reservation));
	CHECK(readiness.Reserved() == *reservation->TotalBytes());
}

TEST_CASE("Portal readiness default budget admits a 4K eye pair and bounded uploads") {
	constexpr uint64_t MEBIBYTE = 1024u * 1024u;
	const auto reservation = client::PortalReadinessReservation::ForRgba16fEyes(3840, 2160, 256 * MEBIBYTE);
	REQUIRE(reservation);
	REQUIRE(reservation->TotalBytes());
	CHECK(*reservation->TotalBytes() < 512 * MEBIBYTE);

	client::PortalReadinessController readiness(
		{.EnterDistance = 4, .ExitDistance = 6, .CapacityBytes = 512 * MEBIBYTE}
	);
	REQUIRE(readiness.Reserve(*reservation));
	CHECK(readiness.Evaluate(ReadyEvidence()).Live);
}

TEST_CASE("Portal readiness keeps a 4K crossing image-only when its configured budget is exhausted") {
	constexpr uint64_t MEBIBYTE = 1024u * 1024u;
	const auto reservation = client::PortalReadinessReservation::ForRgba16fEyes(3840, 2160, 256 * MEBIBYTE);
	REQUIRE(reservation);

	client::PortalReadinessController readiness(
		{.EnterDistance = 4, .ExitDistance = 6, .CapacityBytes = 380 * MEBIBYTE}
	);
	CHECK_FALSE(readiness.Reserve(*reservation));
	CHECK(readiness.Evaluate(ReadyEvidence()).ImageOnly == client::PortalImageOnlyReason::Capacity);
}
