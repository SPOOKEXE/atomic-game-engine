#include <engine/core/Bytes.hpp>
#include <engine/render/PortalShadowTransport.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

TEST_SUITE_ID("engine.render.portalshadowtransport")
TEST_DEPENDS("engine.render.portalshadowimagewire")

namespace {
	using namespace engine;
	using namespace engine::render;

	PortalShadowSnapshot Snapshot() {
		PortalShadowSnapshot snapshot;
		snapshot.Producer = {"room", "portal-image-requests", 7, 9};
		snapshot.Eye = {11, "nested/door", 13, 17};
		snapshot.EyePixelHash = assets::Hasher::Of(std::as_bytes(std::span("eye", 3)));
		snapshot.DepthHash = assets::Hasher::Of(std::as_bytes(std::span("depth", 5)));
		snapshot.SourceBounds = snapshot.DomainBounds = {-1, -1, -1, 1, 1, 1};
		snapshot.LightViewProjection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
		return snapshot;
	}
	PortalShadowPull Pull() {
		const auto snapshot = Snapshot();
		return {
			.ParentEye = {21, "viewport-eye", 23, 29},
			.TargetProducer = snapshot.Producer,
			.TargetEye = snapshot.Eye,
			.BodyBounds = {},
			.Part = 0
		};
	}
	PortalShadowPacket Manifest() {
		PortalShadowPacket packet{Pull(), PortalImageStatus::Ok, {}};
		std::string error;
		REQUIRE(EncodePortalShadowManifest(Snapshot(), packet.Payload, error));
		return packet;
	}
	std::vector<std::byte> UncheckedPacket(const PortalShadowPacket &packet) {
		std::string error;
		std::vector<std::byte> bytes;
		REQUIRE(EncodePortalShadowPull(packet.Pull, bytes, error));
		bytes[6] = std::byte{2};
		core::ByteWriter tail;
		tail.WriteUInt8(static_cast<uint8_t>(packet.Status));
		tail.WriteUInt32(static_cast<uint32_t>(packet.Payload.size()));
		tail.WriteRaw(packet.Payload.data(), packet.Payload.size());
		bytes.insert(bytes.end(), tail.Bytes().begin(), tail.Bytes().end());
		return bytes;
	}
}

TEST_CASE(
	"shadow pulls preserve the exact parent and target without requester claims",
	"[render][portal-shadow-transport]"
) {
	std::vector<std::byte> bytes;
	std::string error;
	for (uint8_t part = 0; part <= PORTAL_SHADOW_CANCEL_PART; ++part) {
		auto pull = Pull();
		pull.Part = part;
		REQUIRE(EncodePortalShadowPull(pull, bytes, error));
		CHECK_FALSE(IsPortalShadowPacket(bytes));
		PortalShadowPull decoded;
		REQUIRE(DecodePortalShadowPull(bytes, decoded, error));
		CHECK(decoded == pull);
	}
	const auto valid = Pull();
	REQUIRE(EncodePortalShadowPull(valid, bytes, error));
	PortalShadowPull decoded = valid;
	for (size_t size = 0; size < bytes.size(); ++size) {
		CHECK_FALSE(DecodePortalShadowPull(std::span(bytes).first(size), decoded, error));
		CHECK(decoded == valid);
	}
	auto trailing = bytes;
	trailing.push_back(std::byte{});
	CHECK_FALSE(DecodePortalShadowPull(trailing, decoded, error));
	for (size_t offset : {size_t(0), size_t(4), size_t(6), size_t(7), bytes.size() - 1}) {
		auto invalid = bytes;
		invalid[offset] = std::byte{0xff};
		CHECK_FALSE(DecodePortalShadowPull(invalid, decoded, error));
	}
	// The first string length follows header and parent request id.
	auto invalid = bytes;
	std::fill_n(invalid.begin() + 16, 4, std::byte{0xff});
	CHECK_FALSE(DecodePortalShadowPull(invalid, decoded, error));
	for (size_t change = 0; change < 6; ++change) {
		auto pull = valid;
		switch (change) {
		case 0:
			pull.ParentEye.RequestId = 0;
			break;
		case 1:
			pull.TargetEye.RequestId = 0;
			break;
		case 2:
			pull.TargetProducer.Session = 0;
			break;
		case 3:
			pull.TargetProducer.Generation = 0;
			break;
		case 4:
			pull.TargetProducer.Channel.push_back('\0');
			break;
		case 5:
			pull.ParentEye.PortalKey.assign(257, 'x');
			break;
		}
		CHECK_FALSE(EncodePortalShadowPull(pull, invalid, error));
	}
}

TEST_CASE(
	"shadow packets validate manifests cancellation and empty failures", "[render][portal-shadow-transport]"
) {
	const auto valid = Manifest();
	std::vector<std::byte> bytes;
	std::string error;
	REQUIRE(EncodePortalShadowPacket(valid, bytes, error));
	CHECK(IsPortalShadowPacket(bytes));
	PortalShadowPacket decoded;
	REQUIRE(DecodePortalShadowPacket(bytes, decoded, error));
	CHECK(decoded == valid);
	for (size_t size = 0; size < bytes.size(); ++size) {
		CHECK_FALSE(DecodePortalShadowPacket(std::span(bytes).first(size), decoded, error));
		CHECK(decoded == valid);
	}
	for (uint8_t status = 1; status <= static_cast<uint8_t>(PortalImageStatus::Failed); ++status) {
		auto failure = valid;
		failure.Status = static_cast<PortalImageStatus>(status);
		CHECK_FALSE(EncodePortalShadowPacket(failure, bytes, error));
		CHECK_FALSE(DecodePortalShadowPacket(UncheckedPacket(failure), decoded, error));
		failure.Payload.clear();
		REQUIRE(EncodePortalShadowPacket(failure, bytes, error));
		REQUIRE(DecodePortalShadowPacket(bytes, decoded, error));
		CHECK(decoded == failure);
	}
	auto cancel = valid;
	cancel.Pull.Part = PORTAL_SHADOW_CANCEL_PART;
	CHECK_FALSE(EncodePortalShadowPacket(cancel, bytes, error));
	cancel.Payload.clear();
	REQUIRE(EncodePortalShadowPacket(cancel, bytes, error));
	REQUIRE(DecodePortalShadowPacket(bytes, decoded, error));
	CHECK(decoded == cancel);
	auto wrong = valid;
	++wrong.Pull.TargetEye.CameraRevision;
	CHECK_FALSE(EncodePortalShadowPacket(wrong, bytes, error));
	CHECK_FALSE(DecodePortalShadowPacket(UncheckedPacket(wrong), decoded, error));
	wrong = valid;
	++wrong.Pull.TargetProducer.Generation;
	CHECK_FALSE(DecodePortalShadowPacket(UncheckedPacket(wrong), decoded, error));
	wrong = valid;
	wrong.Status = static_cast<PortalImageStatus>(255);
	CHECK_FALSE(DecodePortalShadowPacket(UncheckedPacket(wrong), decoded, error));
	bytes = UncheckedPacket(valid);
	bytes.push_back(std::byte{});
	CHECK_FALSE(DecodePortalShadowPacket(bytes, decoded, error));
	bytes.resize(MAX_PORTAL_EXCHANGE_BYTES + 1);
	CHECK_FALSE(DecodePortalShadowPacket(bytes, decoded, error));
	wrong = valid;
	wrong.Payload.resize(MAX_PORTAL_EXCHANGE_BYTES);
	CHECK_FALSE(EncodePortalShadowPacket(wrong, bytes, error));
	CHECK(decoded == cancel);
}

TEST_CASE(
	"shadow packets bind native tile index hash samples and snapshot", "[render][portal-shadow-transport]"
) {
	PortalShadowImage image{Snapshot(), std::vector<std::byte>(PORTAL_SHADOW_BYTES)};
	image.Snapshot.DepthHash = assets::Hasher::Of(image.Depth);
	PortalShadowPacket packet{Pull(), PortalImageStatus::Ok, {}};
	packet.Pull.Part = 16;
	std::string error;
	REQUIRE(EncodePortalShadowTile(image, 15, packet.Payload, error));
	PortalShadowSnapshot metadata;
	uint8_t tile = 0;
	REQUIRE(DecodePortalShadowTileMetadata(packet.Payload, metadata, tile, error));
	CHECK(metadata == image.Snapshot);
	CHECK(tile == 15);
	std::vector<std::byte> bytes;
	REQUIRE(EncodePortalShadowPacket(packet, bytes, error));
	CHECK(bytes.size() < MAX_PORTAL_EXCHANGE_BYTES);
	PortalShadowPacket decoded;
	REQUIRE(DecodePortalShadowPacket(bytes, decoded, error));
	CHECK(decoded == packet);
	auto wrong = packet;
	wrong.Pull.Part = 15;
	CHECK_FALSE(DecodePortalShadowPacket(UncheckedPacket(wrong), decoded, error));
	wrong = packet;
	wrong.Payload.back() ^= std::byte{1};
	CHECK_FALSE(DecodePortalShadowPacket(UncheckedPacket(wrong), decoded, error));
	CHECK_FALSE(DecodePortalShadowTileMetadata(wrong.Payload, metadata, tile, error));
	CHECK(metadata == image.Snapshot);
	CHECK(tile == 15);
	// A matching tile hash does not make a non-finite depth sample valid.
	wrong.Payload = packet.Payload;
	wrong.Payload.back() = std::byte{0x7f};
	wrong.Payload[wrong.Payload.size() - 2] = std::byte{0x80};
	const auto hash = assets::Hasher::Of(std::span(wrong.Payload).last(PORTAL_SHADOW_TILE_BYTES));
	const size_t hashAt = wrong.Payload.size() - PORTAL_SHADOW_TILE_BYTES - assets::ContentHash::BYTES;
	std::copy(
		std::as_bytes(std::span(hash.Digest)).begin(),
		std::as_bytes(std::span(hash.Digest)).end(),
		wrong.Payload.begin() + hashAt
	);
	CHECK_FALSE(DecodePortalShadowPacket(UncheckedPacket(wrong), decoded, error));
	for (size_t size : {size_t(0), size_t(8), bytes.size() / 2, bytes.size() - 1})
		CHECK_FALSE(DecodePortalShadowPacket(std::span(bytes).first(size), decoded, error));
	CHECK(decoded == packet);
}
