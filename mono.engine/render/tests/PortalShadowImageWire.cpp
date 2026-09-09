#include <engine/core/Bytes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/graph/Shadow.hpp>
#include <engine/render/PortalCaptureTreeCompose.hpp>
#include <engine/render/PortalShadowImage.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>

TEST_SUITE_ID("engine.render.portalshadowimagewire")

namespace {
	using namespace engine;
	using namespace engine::render;

	PortalShadowImage Pattern() {
		PortalShadowImage image;
		auto &snapshot = image.Snapshot;
		snapshot.Producer = {"source-room", "shadow-replies", 7, 9};
		snapshot.Eye = {11, "Workspace/Door", 13, 17};
		snapshot.CaptureTick = 19;
		snapshot.ContentRevision = 23;
		snapshot.LightingRevision = 29;
		snapshot.ExcludedPlayer = "31";
		snapshot.EyePixelHash = assets::Hasher::Of(std::as_bytes(std::span("retained-eye", 12)));
		snapshot.SourceBounds = {-1, -2, -3, 1, 2, 3};
		snapshot.DomainBounds = {-4, -5, -6, 4, 5, 6};
		snapshot.LightViewProjection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
		core::ByteWriter writer;
		for (uint32_t y = 0; y < PORTAL_SHADOW_EXTENT; ++y)
			for (uint32_t x = 0; x < PORTAL_SHADOW_EXTENT; ++x)
				writer.WriteFloat(float((x * 13 + y * 37) % 1025) / 1024);
		image.Depth.assign(writer.Bytes().begin(), writer.Bytes().end());
		snapshot.DepthHash = assets::Hasher::Of(image.Depth);
		return image;
	}
	std::vector<std::byte> Tile(const PortalShadowImage &image, uint8_t index) {
		std::vector<std::byte> wire;
		std::string error;
		REQUIRE(EncodePortalShadowTile(image, index, wire, error));
		REQUIRE(error.empty());
		REQUIRE(wire.size() <= MAX_PORTAL_EXCHANGE_BYTES);
		return wire;
	}
	size_t PrefixSize(const std::vector<std::byte> &wire) {
		return wire.size() - PORTAL_SHADOW_TILE_BYTES - assets::ContentHash::BYTES - 1;
	}
	void RehashTile(std::vector<std::byte> &wire) {
		const auto hash = assets::Hasher::Of(std::span(wire).last(PORTAL_SHADOW_TILE_BYTES));
		std::memcpy(wire.data() + PrefixSize(wire) + 1, hash.Digest.data(), hash.Digest.size());
	}
	size_t WholeHashOffset(const std::vector<std::byte> &wire) {
		core::ByteReader reader(wire);
		reader.ReadRawView(8);
		reader.ReadString();
		reader.ReadString();
		reader.ReadRawView(3 * 8);
		reader.ReadString();
		reader.ReadRawView(5 * 8 + assets::ContentHash::BYTES);
		REQUIRE_FALSE(reader.Failed());
		return reader.Position();
	}
}

TEST_CASE(
	"shadow tiles assemble out of order with exact duplicate ownership", "[render][portal-shadow-wire]"
) {
	const auto image = Pattern();
	REQUIRE(ValidPortalShadowImage(image));
	PortalShadowAssembly assembly;
	std::string error;
	CHECK_FALSE(assembly.Begin(image.Snapshot, PORTAL_SHADOW_BYTES - 1, error));
	CHECK(assembly.Bytes() == 0);
	REQUIRE(assembly.Begin(image.Snapshot, PORTAL_SHADOW_BYTES, error));
	CHECK(assembly.Bytes() == PORTAL_SHADOW_BYTES);
	CHECK_FALSE(assembly.Begin(image.Snapshot, PORTAL_SHADOW_BYTES, error));
	for (size_t step = 0; step < PORTAL_SHADOW_TILE_COUNT; ++step) {
		const auto index = static_cast<uint8_t>((step * 5 + 7) % PORTAL_SHADOW_TILE_COUNT);
		const auto wire = Tile(image, index);
		REQUIRE(assembly.Accept(wire, error));
		REQUIRE(assembly.Accept(wire, error));
		CHECK(assembly.CompletedTiles() == step + 1);
		CHECK(assembly.Bytes() == PORTAL_SHADOW_BYTES);
		if (step + 1 != PORTAL_SHADOW_TILE_COUNT) CHECK_FALSE(assembly.Take());
	}
	const auto complete = assembly.Take();
	REQUIRE(complete);
	CHECK(*complete == image);
	CHECK(assembly.Bytes() == 0);
	CHECK(assembly.CompletedTiles() == 0);
	CHECK_FALSE(assembly.Take());
	REQUIRE(assembly.Begin(image.Snapshot, PORTAL_SHADOW_BYTES, error));
	REQUIRE(assembly.Accept(Tile(image, 0), error));
	assembly.Cancel();
	CHECK(assembly.Bytes() == 0);
	CHECK(assembly.CompletedTiles() == 0);
	CHECK_FALSE(assembly.Accept(Tile(image, 0), error));
}

TEST_CASE("shadow tile preflight binds every snapshot byte before copying", "[render][portal-shadow-wire]") {
	const auto image = Pattern();
	auto wire = Tile(image, 0);
	PortalShadowAssembly assembly;
	std::string error;
	REQUIRE(assembly.Begin(image.Snapshot, PORTAL_SHADOW_BYTES, error));
	const size_t prefix = PrefixSize(wire);
	// Endpoint, eye revisions, hashes, exclusion, bounds and matrix all remain exact.
	for (size_t offset = 0; offset < prefix; ++offset) {
		wire[offset] ^= std::byte{1};
		CHECK_FALSE(assembly.Accept(wire, error));
		wire[offset] ^= std::byte{1};
	}
	for (size_t length : {size_t(0), prefix, prefix + 1, prefix + 32, wire.size() / 2, wire.size() - 1})
		CHECK_FALSE(assembly.Accept(std::span(wire).first(length), error));
	wire.push_back(std::byte{});
	CHECK_FALSE(assembly.Accept(wire, error));
	wire.pop_back();
	wire[prefix] = std::byte{16};
	CHECK_FALSE(assembly.Accept(wire, error));
	wire[prefix] = std::byte{0};
	wire.back() ^= std::byte{1};
	CHECK_FALSE(assembly.Accept(wire, error));
	wire.back() ^= std::byte{1};
	CHECK(assembly.CompletedTiles() == 0);
	REQUIRE(assembly.Accept(wire, error));
	// A valid tile hash cannot replace an already accepted tile with different pixels.
	const size_t firstSample = wire.size() - PORTAL_SHADOW_TILE_BYTES;
	wire[firstSample] = wire[firstSample + 1] = wire[firstSample + 2] = std::byte{};
	wire[firstSample + 3] = std::byte{0x3f};
	RehashTile(wire);
	CHECK_FALSE(assembly.Accept(wire, error));
	CHECK(assembly.CompletedTiles() == 1);
	CHECK_FALSE(assembly.Take());
}

TEST_CASE(
	"shadow tiles reject hash-valid invalid D32 and mismatched whole images", "[render][portal-shadow-wire]"
) {
	const auto image = Pattern();
	const auto original = Tile(image, 0);
	PortalShadowAssembly assembly;
	std::string error;
	REQUIRE(assembly.Begin(image.Snapshot, PORTAL_SHADOW_BYTES, error));
	for (uint32_t word : {0x80000000u, 0xbf000000u, 0x3f800001u, 0x7f800000u, 0xff800000u, 0x7fc00000u}) {
		auto wire = original;
		const size_t offset = wire.size() - PORTAL_SHADOW_TILE_BYTES;
		for (size_t byte = 0; byte < 4; ++byte)
			wire[offset + byte] = std::byte((word >> (byte * 8)) & 255);
		RehashTile(wire);
		CHECK_FALSE(assembly.Accept(wire, error));
		CHECK(assembly.CompletedTiles() == 0);
	}
	assembly.Cancel();
	auto wrong = image.Snapshot;
	wrong.DepthHash.Digest[0] ^= 1;
	REQUIRE(assembly.Begin(wrong, PORTAL_SHADOW_BYTES, error));
	for (uint8_t tile = 0; tile < PORTAL_SHADOW_TILE_COUNT; ++tile) {
		auto wire = Tile(image, tile);
		std::memcpy(
			wire.data() + WholeHashOffset(wire), wrong.DepthHash.Digest.data(), assets::ContentHash::BYTES
		);
		CHECK(assembly.Accept(wire, error) == (tile + 1 < PORTAL_SHADOW_TILE_COUNT));
	}
	CHECK_FALSE(error.empty());
	CHECK_FALSE(assembly.Take());
	CHECK(assembly.Bytes() == 0);
}

TEST_CASE("initial world tick remains an authoritative shadow snapshot", "[render][portal-shadow-wire]") {
	ecs::Store store("initial-shadow-world");
	REQUIRE(store.Time().Tick == 0);
	auto image = Pattern();
	image.Snapshot.CaptureTick = store.Time().Tick;
	REQUIRE(ValidPortalShadowSnapshot(image.Snapshot));
	REQUIRE(ValidPortalShadowImage(image));
	PortalShadowAssembly assembly;
	std::string error;
	REQUIRE(assembly.Begin(image.Snapshot, PORTAL_SHADOW_BYTES, error));
	const auto initialTile = Tile(image, 0);
	REQUIRE(assembly.Accept(initialTile, error));
	CHECK(assembly.CompletedTiles() == 1);
	assembly.Cancel();
	auto nextTick = image.Snapshot;
	++nextTick.CaptureTick;
	REQUIRE(assembly.Begin(nextTick, PORTAL_SHADOW_BYTES, error));
	CHECK_FALSE(assembly.Accept(initialTile, error));
	CHECK(assembly.CompletedTiles() == 0);
}

TEST_CASE("shadow snapshot admission rejects invalid identity and geometry", "[render][portal-shadow-wire]") {
	const auto image = Pattern();
	std::string error;
	PortalShadowAssembly assembly;
	auto reject = [&](const PortalShadowSnapshot &snapshot) {
		CHECK_FALSE(ValidPortalShadowSnapshot(snapshot));
		CHECK_FALSE(assembly.Begin(snapshot, PORTAL_SHADOW_BYTES, error));
		CHECK(assembly.Bytes() == 0);
	};
	auto snapshot = image.Snapshot;
	snapshot.Eye.RequestId = 0;
	reject(snapshot);
	snapshot = image.Snapshot;
	snapshot.Producer.Session = 0;
	reject(snapshot);
	snapshot = image.Snapshot;
	snapshot.Producer.World.assign(257, 'x');
	reject(snapshot);
	snapshot = image.Snapshot;
	snapshot.Eye.PortalKey = std::string("bad\0key", 7);
	reject(snapshot);
	snapshot = image.Snapshot;
	snapshot.ExcludedPlayer = "031";
	reject(snapshot);
	snapshot = image.Snapshot;
	snapshot.SourceBounds[0] = 2;
	reject(snapshot);
	snapshot = image.Snapshot;
	snapshot.DomainBounds[0] = 0;
	reject(snapshot);
	snapshot = image.Snapshot;
	snapshot.LightViewProjection.fill(0);
	reject(snapshot);
	snapshot = image.Snapshot;
	snapshot.LightViewProjection[0] = std::numeric_limits<float>::infinity();
	reject(snapshot);
	snapshot = image.Snapshot;
	snapshot.SourceBounds[0] = std::numeric_limits<float>::quiet_NaN();
	reject(snapshot);
	std::vector<std::byte> wire{std::byte{42}};
	CHECK_FALSE(EncodePortalShadowTile(image, 16, wire, error));
	CHECK(wire == std::vector<std::byte>{std::byte{42}});
}

TEST_CASE(
	"empty shadow sources preserve non-origin domains and require clear depth", "[render][portal-shadow-wire]"
) {
	auto image = Pattern();
	image.Snapshot.SourceEmpty = true;
	image.Snapshot.SourceBounds.fill(0);
	image.Snapshot.DomainBounds = {10, 9, -20, 11, 10, -19};
	for (size_t offset = 0; offset < image.Depth.size(); offset += 4) {
		image.Depth[offset] = image.Depth[offset + 1] = std::byte{};
		image.Depth[offset + 2] = std::byte{0x80};
		image.Depth[offset + 3] = std::byte{0x3f};
	}
	image.Snapshot.DepthHash = assets::Hasher::Of(image.Depth);
	REQUIRE(ValidPortalShadowImage(image));
	auto snapshot = image.Snapshot;
	snapshot.SourceEmpty = false;
	CHECK_FALSE(ValidPortalShadowSnapshot(snapshot));
	snapshot = image.Snapshot;
	snapshot.SourceBounds[0] = -0.0f;
	CHECK_FALSE(ValidPortalShadowSnapshot(snapshot));
	snapshot.SourceBounds[0] = 1.0f;
	CHECK_FALSE(ValidPortalShadowSnapshot(snapshot));

	PortalShadowAssembly assembly;
	std::string error;
	REQUIRE(assembly.Begin(image.Snapshot, PORTAL_SHADOW_BYTES, error));
	auto forged = Tile(image, 0);
	CHECK(forged[6] == std::byte{1});
	forged[6] = std::byte{};
	CHECK_FALSE(assembly.Accept(forged, error));
	forged[6] = std::byte{1};
	// A finite, hash-valid half-depth is still impossible for an empty source.
	forged[forged.size() - PORTAL_SHADOW_TILE_BYTES + 2] = std::byte{};
	RehashTile(forged);
	CHECK_FALSE(assembly.Accept(forged, error));
	CHECK(assembly.CompletedTiles() == 0);
	for (uint8_t tile = 0; tile < PORTAL_SHADOW_TILE_COUNT; ++tile)
		REQUIRE(assembly.Accept(Tile(image, tile), error));
	const auto complete = assembly.Take();
	REQUIRE(complete);
	CHECK(*complete == image);

	image.Depth[2] = std::byte{};
	image.Snapshot.DepthHash = assets::Hasher::Of(image.Depth);
	CHECK_FALSE(ValidPortalShadowImage(image));
}

TEST_CASE(
	"shadow manifests share tile metadata and reject incomplete envelopes", "[render][portal-shadow-wire]"
) {
	const auto image = Pattern();
	std::vector<std::byte> wire;
	std::string error;
	REQUIRE(EncodePortalShadowManifest(image.Snapshot, wire, error));
	REQUIRE(wire.size() < MAX_PORTAL_EXCHANGE_BYTES);
	const auto tile = Tile(image, 0);
	CHECK(wire.size() == PrefixSize(tile));
	CHECK(std::equal(wire.begin(), wire.end(), tile.begin()));
	PortalShadowSnapshot decoded;
	REQUIRE(DecodePortalShadowManifest(wire, decoded, error));
	CHECK(decoded == image.Snapshot);
	for (size_t size = 0; size < wire.size(); ++size) {
		CHECK_FALSE(DecodePortalShadowManifest(std::span(wire).first(size), decoded, error));
		CHECK(decoded == image.Snapshot);
	}
	CHECK_FALSE(DecodePortalShadowManifest(tile, decoded, error));
	wire.push_back(std::byte{});
	CHECK_FALSE(DecodePortalShadowManifest(wire, decoded, error));
	wire.resize(MAX_PORTAL_EXCHANGE_BYTES + 1);
	CHECK_FALSE(DecodePortalShadowManifest(wire, decoded, error));
	CHECK(decoded == image.Snapshot);
}

TEST_CASE("shadow manifests reject hostile metadata before publishing", "[render][portal-shadow-wire]") {
	const auto image = Pattern();
	std::string error;
	std::vector<std::byte> valid;
	REQUIRE(EncodePortalShadowManifest(image.Snapshot, valid, error));
	PortalShadowSnapshot decoded = image.Snapshot;
	for (const size_t offset : {size_t(0), size_t(4), size_t(6)}) {
		auto wire = valid;
		wire[offset] = std::byte{0xff};
		CHECK_FALSE(DecodePortalShadowManifest(wire, decoded, error));
	}
	// A huge declared string length must fail before allocating owned metadata.
	auto wire = valid;
	std::fill_n(wire.begin() + 8, 4, std::byte{0xff});
	CHECK_FALSE(DecodePortalShadowManifest(wire, decoded, error));
	// Non-finite matrix coefficients are rejected even in otherwise complete metadata.
	wire = valid;
	const uint32_t infinity = 0x7f800000u;
	for (size_t byte = 0; byte < 4; ++byte)
		wire[wire.size() - 4 + byte] = std::byte((infinity >> (byte * 8)) & 0xffu);
	CHECK_FALSE(DecodePortalShadowManifest(wire, decoded, error));
	CHECK(decoded == image.Snapshot);
	auto invalid = image.Snapshot;
	invalid.Producer.World.assign(257, 'x');
	CHECK_FALSE(EncodePortalShadowManifest(invalid, valid, error));
	CHECK(valid.size() < MAX_PORTAL_EXCHANGE_BYTES);
}

TEST_CASE(
	"shadow manifests bind the pending eye and exact native light domain", "[render][portal-shadow-wire]"
) {
	auto snapshot = Pattern().Snapshot;
	PortalTreeShadowRequest request;
	request.Job = 1;
	request.Producer = snapshot.Producer;
	request.Eye = snapshot.Eye;
	request.CaptureTick = snapshot.CaptureTick;
	request.ContentRevision = snapshot.ContentRevision;
	request.LightingRevision = snapshot.LightingRevision;
	request.EyePixelHash = snapshot.EyePixelHash;
	request.ExcludedPlayer = snapshot.ExcludedPlayer;
	request.BodyBounds = core::AABB{{-4, -5, -6}, {4, 5, 6}};
	request.LightDirection = {1, -2, 3};
	const auto fit = [&](PortalShadowSnapshot &value, const core::AABB &domain) {
		value.DomainBounds = {
			domain.Minimum.X,
			domain.Minimum.Y,
			domain.Minimum.Z,
			domain.Maximum.X,
			domain.Maximum.Y,
			domain.Maximum.Z
		};
		const auto light =
			graph::FitDirectionalLight(domain, request.LightDirection / request.LightDirection.Magnitude());
		for (size_t column = 0; column < 4; ++column)
			for (size_t row = 0; row < 4; ++row)
				value.LightViewProjection[column * 4 + row] = light[column][row];
	};
	fit(snapshot, *request.BodyBounds);
	REQUIRE(MatchesPortalTreeShadowRequest(request, snapshot));
	std::vector<std::byte> wire;
	std::string error;
	REQUIRE(EncodePortalShadowManifest(snapshot, wire, error));
	PortalShadowSnapshot decoded;
	REQUIRE(DecodePortalShadowManifest(wire, decoded, error));
	REQUIRE(MatchesPortalTreeShadowRequest(request, decoded));
	// Each reply identity is independently bound, including old world/eye incarnations.
	for (size_t change = 0; change < 13; ++change) {
		auto wrong = decoded;
		switch (change) {
		case 0:
			wrong.Producer.World += "other";
			break;
		case 1:
			wrong.Producer.Channel += "other";
			break;
		case 2:
			++wrong.Producer.Session;
			break;
		case 3:
			++wrong.Producer.Generation;
			break;
		case 4:
			++wrong.Eye.RequestId;
			break;
		case 5:
			wrong.Eye.PortalKey += "other";
			break;
		case 6:
			++wrong.Eye.CameraRevision;
			break;
		case 7:
			++wrong.Eye.SeamRevision;
			break;
		case 8:
			++wrong.CaptureTick;
			break;
		case 9:
			++wrong.ContentRevision;
			break;
		case 10:
			++wrong.LightingRevision;
			break;
		case 11:
			wrong.EyePixelHash = assets::Hasher::Of(std::as_bytes(std::span("other", 5)));
			break;
		case 12:
			wrong.ExcludedPlayer = "32";
			break;
		}
		CAPTURE(change);
		REQUIRE(ValidPortalShadowSnapshot(wrong));
		CHECK_FALSE(MatchesPortalTreeShadowRequest(request, wrong));
	}
	auto wrong = decoded;
	wrong.DomainBounds[0] -= 1;
	CHECK_FALSE(MatchesPortalTreeShadowRequest(request, wrong));
	wrong = decoded;
	wrong.LightViewProjection[12] += .1f;
	CHECK_FALSE(MatchesPortalTreeShadowRequest(request, wrong));
	auto changedRequest = request;
	changedRequest.LightDirection = {-1, -2, 3};
	CHECK_FALSE(MatchesPortalTreeShadowRequest(changedRequest, decoded));
	changedRequest.LightDirection = {};
	CHECK_FALSE(MatchesPortalTreeShadowRequest(changedRequest, decoded));
	changedRequest = request;
	changedRequest.BodyBounds->Minimum.X = std::numeric_limits<float>::quiet_NaN();
	CHECK_FALSE(MatchesPortalTreeShadowRequest(changedRequest, decoded));
	changedRequest.BodyBounds = core::AABB{{5, 0, 0}, {4, 1, 1}};
	CHECK_FALSE(MatchesPortalTreeShadowRequest(changedRequest, decoded));
	changedRequest = request;
	changedRequest.BodyBounds->Maximum.X += 1;
	CHECK_FALSE(MatchesPortalTreeShadowRequest(changedRequest, decoded));

	// Empty sources contribute no phantom origin box to a non-origin body domain.
	snapshot.SourceEmpty = true;
	snapshot.SourceBounds.fill(0);
	request.BodyBounds = core::AABB{{10, 9, -20}, {11, 10, -19}};
	fit(snapshot, *request.BodyBounds);
	CHECK(MatchesPortalTreeShadowRequest(request, snapshot));
	request.BodyBounds.reset();
	CHECK_FALSE(MatchesPortalTreeShadowRequest(request, snapshot));
	fit(snapshot, graph::BoundsOfAll(std::span<const scene::DrawInstance>{}));
	CHECK(MatchesPortalTreeShadowRequest(request, snapshot));
}
