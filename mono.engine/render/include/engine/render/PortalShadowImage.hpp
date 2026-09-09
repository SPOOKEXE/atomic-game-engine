#pragma once

#include <engine/render/PortalCaptureTree.hpp>

namespace engine::render {
	inline constexpr uint32_t PORTAL_SHADOW_EXTENT = 2048;
	inline constexpr uint32_t PORTAL_SHADOW_TILE_EXTENT = 512;
	inline constexpr uint8_t PORTAL_SHADOW_TILE_COUNT = 16;
	inline constexpr size_t PORTAL_SHADOW_BYTES = size_t(PORTAL_SHADOW_EXTENT) * PORTAL_SHADOW_EXTENT * 4;
	inline constexpr size_t PORTAL_SHADOW_TILE_BYTES =
		size_t(PORTAL_SHADOW_TILE_EXTENT) * PORTAL_SHADOW_TILE_EXTENT * 4;

	struct PortalShadowSnapshot {
		PortalCaptureTreeEndpoint Producer;
		PortalExchangeKey Eye;
		uint64_t CaptureTick = 0, ContentRevision = 0, LightingRevision = 0;
		assets::ContentHash EyePixelHash, DepthHash;
		std::string ExcludedPlayer;
		// Empty sources have zero SourceBounds and a wholly clear depth map.
		bool SourceEmpty = false;
		// World-space minimum XYZ then maximum XYZ. DomainBounds encloses nonempty SourceBounds.
		std::array<float, 6> SourceBounds{}, DomainBounds{};
		// Column-major native zero-to-one depth projection in the producer's world.
		std::array<float, 16> LightViewProjection{};
		bool operator==(const PortalShadowSnapshot &) const = default;
	};
	struct PortalShadowImage {
		PortalShadowSnapshot Snapshot;
		// Exactly 2048 squared little-endian D32 samples, each finite and in [0,1].
		std::vector<std::byte> Depth;
		bool operator==(const PortalShadowImage &) const = default;
	};

	bool ValidPortalShadowSnapshot(const PortalShadowSnapshot &);
	bool ValidPortalShadowImage(const PortalShadowImage &);
	// A manifest announces the canonical snapshot without pixels. The caller must authenticate
	// its envelope and match a pending eye request before reserving an assembly from it.
	bool
	EncodePortalShadowManifest(const PortalShadowSnapshot &, std::vector<std::byte> &, std::string &error);
	bool DecodePortalShadowManifest(std::span<const std::byte>, PortalShadowSnapshot &, std::string &error);
	// Tiles are row-major 512 squared rectangles, each smaller than the existing wire bound.
	bool EncodePortalShadowTile(
		const PortalShadowImage &, uint8_t tile, std::vector<std::byte> &, std::string &error
	);
	// Validates a complete tile and its hash/samples without allocating an image assembly.
	bool DecodePortalShadowTileMetadata(
		std::span<const std::byte>, PortalShadowSnapshot &, uint8_t &tile, std::string &error
	);

	// The caller authenticates the presentation envelope and chooses the expected snapshot.
	// Packet metadata never establishes trust. One assembly reserves its whole image before
	// accepting tiles; owners charge Bytes() against their aggregate pending-image budget.
	class PortalShadowAssembly {
	  public:
		PortalShadowAssembly() = default;
		PortalShadowAssembly(const PortalShadowAssembly &) = delete;
		PortalShadowAssembly &operator=(const PortalShadowAssembly &) = delete;
		bool Begin(const PortalShadowSnapshot &expected, size_t byteBudget, std::string &error);
		bool Accept(std::span<const std::byte>, std::string &error);
		// Publishes only after all tiles and the full-image hash have passed validation.
		std::optional<PortalShadowImage> Take();
		void Cancel();
		size_t Bytes() const;
		size_t CompletedTiles() const;

	  private:
		std::optional<PortalShadowImage> Pending;
		uint16_t Received = 0;
		bool Complete = false;
	};
}
