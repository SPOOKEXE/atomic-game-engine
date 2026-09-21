#include "PortalImageSamples.hpp"
#include "PortalPlayerIdentity.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/render/PortalShadowImage.hpp>

#include <glm/mat4x4.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace engine::render {
	namespace {
		constexpr uint32_t SHADOW_IMAGE_MAGIC = 0x57444853;
		constexpr uint16_t SHADOW_IMAGE_VERSION = 1;
		constexpr size_t TILE_ROW_BYTES = PORTAL_SHADOW_TILE_EXTENT * 4;
		constexpr size_t IMAGE_ROW_BYTES = PORTAL_SHADOW_EXTENT * 4;
		constexpr uint16_t ALL_TILES = 0xffff;

		bool FailShadowImage(std::string &error, const char *message) {
			error = message;
			return false;
		}
		bool ShadowImageText(std::string_view value) {
			return !value.empty() && value.size() <= 256 && value.find('\0') == std::string_view::npos;
		}
		bool Bounds(const std::array<float, 6> &bounds) {
			for (float value : bounds)
				if (!std::isfinite(value)) return false;
			float squaredSize = 0;
			for (size_t axis = 0; axis < 3; ++axis) {
				const float size = bounds[axis + 3] - bounds[axis];
				if (bounds[axis] > bounds[axis + 3] || !std::isfinite(size) ||
					!std::isfinite(bounds[axis] + bounds[axis + 3]))
					return false;
				squaredSize += size * size;
			}
			return std::isfinite(squaredSize);
		}
		bool Samples(std::span<const std::byte> samples, bool sourceEmpty) {
			if (samples.size() % 4 != 0) return false;
			bool valid = true;
			for (size_t offset = 0; offset < samples.size(); offset += 4)
				valid &= sourceEmpty ? PortalSampleWord(samples.data() + offset) == 0x3f800000u
									 : PortalSampleWord(samples.data() + offset) <= 0x3f800000u;
			return valid;
		}
		void Hash(core::ByteWriter &writer, const assets::ContentHash &hash) {
			writer.WriteRaw(hash.Digest.data(), hash.Digest.size());
		}
		void Snapshot(core::ByteWriter &writer, const PortalShadowSnapshot &snapshot) {
			writer.WriteUInt32(SHADOW_IMAGE_MAGIC);
			writer.WriteUInt16(SHADOW_IMAGE_VERSION);
			writer.WriteUInt16(snapshot.SourceEmpty ? 1 : 0);
			writer.WriteString(snapshot.Producer.World);
			writer.WriteString(snapshot.Producer.Channel);
			writer.WriteUInt64(snapshot.Producer.Session);
			writer.WriteUInt64(snapshot.Producer.Generation);
			writer.WriteUInt64(snapshot.Eye.RequestId);
			writer.WriteString(snapshot.Eye.PortalKey);
			writer.WriteUInt64(snapshot.Eye.CameraRevision);
			writer.WriteUInt64(snapshot.Eye.SeamRevision);
			writer.WriteUInt64(snapshot.CaptureTick);
			writer.WriteUInt64(snapshot.ContentRevision);
			writer.WriteUInt64(snapshot.LightingRevision);
			Hash(writer, snapshot.EyePixelHash);
			Hash(writer, snapshot.DepthHash);
			writer.WriteString(snapshot.ExcludedPlayer);
			for (float value : snapshot.SourceBounds)
				writer.WriteFloat(value);
			for (float value : snapshot.DomainBounds)
				writer.WriteFloat(value);
			for (float value : snapshot.LightViewProjection)
				writer.WriteFloat(value);
		}
		bool Snapshot(core::ByteReader &reader, PortalShadowSnapshot &snapshot) {
			if (reader.ReadUInt32() != SHADOW_IMAGE_MAGIC || reader.ReadUInt16() != SHADOW_IMAGE_VERSION)
				return false;
			const auto flags = reader.ReadUInt16();
			if (flags > 1) return false;
			snapshot.SourceEmpty = flags != 0;
			const auto text = [&](std::string &out) {
				const auto value = reader.ReadString();
				if (reader.Failed() || !ShadowImageText(value)) return false;
				out = value;
				return true;
			};
			if (!text(snapshot.Producer.World) || !text(snapshot.Producer.Channel)) return false;
			snapshot.Producer.Session = reader.ReadUInt64();
			snapshot.Producer.Generation = reader.ReadUInt64();
			snapshot.Eye.RequestId = reader.ReadUInt64();
			if (!text(snapshot.Eye.PortalKey)) return false;
			snapshot.Eye.CameraRevision = reader.ReadUInt64();
			snapshot.Eye.SeamRevision = reader.ReadUInt64();
			snapshot.CaptureTick = reader.ReadUInt64();
			snapshot.ContentRevision = reader.ReadUInt64();
			snapshot.LightingRevision = reader.ReadUInt64();
			reader.ReadRaw(snapshot.EyePixelHash.Digest.data(), assets::ContentHash::BYTES);
			reader.ReadRaw(snapshot.DepthHash.Digest.data(), assets::ContentHash::BYTES);
			const auto excluded = reader.ReadString();
			if (reader.Failed() || !ValidPortalPlayerIdentity(excluded)) return false;
			snapshot.ExcludedPlayer = excluded;
			for (float &value : snapshot.SourceBounds)
				value = reader.ReadFloat();
			for (float &value : snapshot.DomainBounds)
				value = reader.ReadFloat();
			for (float &value : snapshot.LightViewProjection)
				value = reader.ReadFloat();
			return !reader.Failed() && ValidPortalShadowSnapshot(snapshot);
		}
		size_t TileOffset(uint8_t tile, size_t row) {
			return ((tile / 4) * PORTAL_SHADOW_TILE_EXTENT + row) * IMAGE_ROW_BYTES +
				   (tile % 4) * TILE_ROW_BYTES;
		}
	}

	bool ValidPortalShadowSnapshot(const PortalShadowSnapshot &snapshot) {
		if (!ShadowImageText(snapshot.Producer.World) || !ShadowImageText(snapshot.Producer.Channel) ||
			snapshot.Producer.Session == 0 || snapshot.Producer.Generation == 0 ||
			snapshot.Eye.RequestId == 0 || !ShadowImageText(snapshot.Eye.PortalKey) ||
			snapshot.EyePixelHash.IsZero() || snapshot.DepthHash.IsZero() ||
			!ValidPortalPlayerIdentity(snapshot.ExcludedPlayer) || !Bounds(snapshot.SourceBounds) ||
			!Bounds(snapshot.DomainBounds))
			return false;
		if (snapshot.SourceEmpty) {
			for (float value : snapshot.SourceBounds)
				if (std::bit_cast<uint32_t>(value) != 0) return false;
		} else {
			for (size_t axis = 0; axis < 3; ++axis)
				if (snapshot.DomainBounds[axis] > snapshot.SourceBounds[axis] ||
					snapshot.DomainBounds[axis + 3] < snapshot.SourceBounds[axis + 3])
					return false;
		}
		glm::mat4 matrix;
		for (size_t column = 0; column < 4; ++column)
			for (size_t row = 0; row < 4; ++row) {
				const float value = snapshot.LightViewProjection[column * 4 + row];
				if (!std::isfinite(value)) return false;
				matrix[column][row] = value;
			}
		const float determinant = glm::determinant(matrix);
		return std::isfinite(determinant) && determinant != 0;
	}

	bool ValidPortalShadowImage(const PortalShadowImage &image) {
		ENGINE_PROFILE("portal shadow image validation");
		return ValidPortalShadowSnapshot(image.Snapshot) && image.Depth.size() == PORTAL_SHADOW_BYTES &&
			   Samples(image.Depth, image.Snapshot.SourceEmpty) &&
			   assets::Hasher::Of(image.Depth) == image.Snapshot.DepthHash;
	}

	bool EncodePortalShadowManifest(
		const PortalShadowSnapshot &snapshot, std::vector<std::byte> &out, std::string &error
	) {
		if (!ValidPortalShadowSnapshot(snapshot))
			return FailShadowImage(error, "invalid portal shadow manifest");
		core::ByteWriter writer;
		Snapshot(writer, snapshot);
		if (writer.Size() > MAX_PORTAL_EXCHANGE_BYTES)
			return FailShadowImage(error, "portal shadow manifest exceeds wire budget");
		out.assign(writer.Bytes().begin(), writer.Bytes().end());
		error.clear();
		return true;
	}

	bool DecodePortalShadowManifest(
		std::span<const std::byte> packet, PortalShadowSnapshot &out, std::string &error
	) {
		if (packet.size() > MAX_PORTAL_EXCHANGE_BYTES)
			return FailShadowImage(error, "portal shadow manifest exceeds wire budget");
		core::ByteReader reader(packet);
		PortalShadowSnapshot snapshot;
		if (!Snapshot(reader, snapshot) || reader.Remaining() != 0)
			return FailShadowImage(error, "invalid portal shadow manifest");
		out = std::move(snapshot);
		error.clear();
		return true;
	}

	bool EncodePortalShadowTile(
		const PortalShadowImage &image, uint8_t tile, std::vector<std::byte> &out, std::string &error
	) {
		ENGINE_PROFILE("portal shadow tile encode");
		if (tile >= PORTAL_SHADOW_TILE_COUNT || !ValidPortalShadowImage(image))
			return FailShadowImage(error, "invalid portal shadow image or tile");
		core::ByteWriter writer;
		Snapshot(writer, image.Snapshot);
		writer.WriteUInt8(tile);
		assets::Hasher hasher;
		for (size_t row = 0; row < PORTAL_SHADOW_TILE_EXTENT; ++row)
			hasher.Update(std::span(image.Depth).subspan(TileOffset(tile, row), TILE_ROW_BYTES));
		Hash(writer, hasher.Finish());
		for (size_t row = 0; row < PORTAL_SHADOW_TILE_EXTENT; ++row)
			writer.WriteRaw(image.Depth.data() + TileOffset(tile, row), TILE_ROW_BYTES);
		if (writer.Size() > MAX_PORTAL_EXCHANGE_BYTES)
			return FailShadowImage(error, "portal shadow tile exceeds wire budget");
		out.assign(writer.Bytes().begin(), writer.Bytes().end());
		error.clear();
		return true;
	}

	bool DecodePortalShadowTileMetadata(
		std::span<const std::byte> packet, PortalShadowSnapshot &out, uint8_t &tile, std::string &error
	) {
		if (packet.size() > MAX_PORTAL_EXCHANGE_BYTES)
			return FailShadowImage(error, "portal shadow tile exceeds wire budget");
		core::ByteReader reader(packet);
		PortalShadowSnapshot snapshot;
		if (!Snapshot(reader, snapshot)) return FailShadowImage(error, "invalid portal shadow tile snapshot");
		const auto index = reader.ReadUInt8();
		assets::ContentHash hash;
		if (index >= PORTAL_SHADOW_TILE_COUNT ||
			!reader.ReadRaw(hash.Digest.data(), assets::ContentHash::BYTES) ||
			reader.Remaining() != PORTAL_SHADOW_TILE_BYTES)
			return FailShadowImage(error, "invalid portal shadow tile layout");
		const auto pixels = reader.ReadRawView(PORTAL_SHADOW_TILE_BYTES);
		if (reader.Failed() || assets::Hasher::Of(pixels) != hash || !Samples(pixels, snapshot.SourceEmpty))
			return FailShadowImage(error, "invalid portal shadow tile hash or depth");
		out = std::move(snapshot);
		tile = index;
		error.clear();
		return true;
	}

	bool
	PortalShadowAssembly::Begin(const PortalShadowSnapshot &expected, size_t byteBudget, std::string &error) {
		if (Pending) return FailShadowImage(error, "portal shadow assembly already active");
		if (byteBudget < PORTAL_SHADOW_BYTES || !ValidPortalShadowSnapshot(expected))
			return FailShadowImage(error, "invalid or over-budget portal shadow snapshot");
		PortalShadowImage image{expected, std::vector<std::byte>(PORTAL_SHADOW_BYTES)};
		if (image.Depth.capacity() > byteBudget)
			return FailShadowImage(error, "portal shadow allocation exceeds byte budget");
		Pending.emplace(std::move(image));
		Received = 0;
		Complete = false;
		error.clear();
		return true;
	}

	bool PortalShadowAssembly::Accept(std::span<const std::byte> packet, std::string &error) {
		ENGINE_PROFILE("portal shadow tile accept");
		if (!Pending || packet.size() > MAX_PORTAL_EXCHANGE_BYTES)
			return FailShadowImage(error, "inactive or over-budget portal shadow assembly");
		core::ByteWriter expected;
		Snapshot(expected, Pending->Snapshot);
		const size_t prefix = expected.Size();
		if (packet.size() != prefix + 1 + assets::ContentHash::BYTES + PORTAL_SHADOW_TILE_BYTES ||
			!std::equal(expected.Bytes().begin(), expected.Bytes().end(), packet.begin()))
			return FailShadowImage(error, "portal shadow tile does not match expected snapshot");
		const auto tile = std::to_integer<uint8_t>(packet[prefix]);
		if (tile >= PORTAL_SHADOW_TILE_COUNT)
			return FailShadowImage(error, "invalid portal shadow tile index");
		assets::ContentHash hash;
		std::memcpy(hash.Digest.data(), packet.data() + prefix + 1, hash.Digest.size());
		const auto pixels = packet.last(PORTAL_SHADOW_TILE_BYTES);
		if (assets::Hasher::Of(pixels) != hash || !Samples(pixels, Pending->Snapshot.SourceEmpty))
			return FailShadowImage(error, "invalid portal shadow tile hash or depth");
		const auto bit = uint16_t(1u << tile);
		if (Received & bit) {
			for (size_t row = 0; row < PORTAL_SHADOW_TILE_EXTENT; ++row)
				if (std::memcmp(
						Pending->Depth.data() + TileOffset(tile, row),
						pixels.data() + row * TILE_ROW_BYTES,
						TILE_ROW_BYTES
					) != 0)
					return FailShadowImage(error, "conflicting portal shadow tile");
			error.clear();
			return true;
		}
		for (size_t row = 0; row < PORTAL_SHADOW_TILE_EXTENT; ++row)
			std::memcpy(
				Pending->Depth.data() + TileOffset(tile, row),
				pixels.data() + row * TILE_ROW_BYTES,
				TILE_ROW_BYTES
			);
		Received |= bit;
		if (Received == ALL_TILES) {
			if (assets::Hasher::Of(Pending->Depth) != Pending->Snapshot.DepthHash) {
				Cancel();
				return FailShadowImage(error, "portal shadow image hash mismatch");
			}
			Complete = true;
		}
		error.clear();
		return true;
	}

	std::optional<PortalShadowImage> PortalShadowAssembly::Take() {
		if (!Complete) return std::nullopt;
		auto result = std::move(Pending);
		Cancel();
		return result;
	}
	void PortalShadowAssembly::Cancel() {
		Pending.reset();
		Received = 0;
		Complete = false;
	}
	size_t PortalShadowAssembly::Bytes() const {
		return Pending ? Pending->Depth.capacity() : 0;
	}
	size_t PortalShadowAssembly::CompletedTiles() const {
		return std::popcount(Received);
	}
}
