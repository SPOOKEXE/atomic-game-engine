#pragma once

#include <engine/render/PortalCaptureTree.hpp>

namespace engine::render {
	// Side length in pixels of a complete portal shadow map.
	inline constexpr uint32_t PORTAL_SHADOW_EXTENT = 2048;
	// Side length in pixels of one transmitted shadow tile.
	inline constexpr uint32_t PORTAL_SHADOW_TILE_EXTENT = 512;
	// Number of row-major tiles composing a complete shadow map.
	inline constexpr uint8_t PORTAL_SHADOW_TILE_COUNT = 16;
	// Decoded byte size of one complete D32 shadow map.
	inline constexpr size_t PORTAL_SHADOW_BYTES = size_t(PORTAL_SHADOW_EXTENT) * PORTAL_SHADOW_EXTENT * 4;
	// Decoded byte size of one transmitted D32 tile.
	inline constexpr size_t PORTAL_SHADOW_TILE_BYTES =
		size_t(PORTAL_SHADOW_TILE_EXTENT) * PORTAL_SHADOW_TILE_EXTENT * 4;

	// Canonical identity and spatial domain of one portal shadow capture.
	struct PortalShadowSnapshot {
		// Endpoint that produced the capture and owns its producer-world coordinates.
		PortalCaptureTreeEndpoint Producer;
		// Requested eye key that the shadow image answers.
		PortalExchangeKey Eye;
		// Producer tick at which colour and depth were captured.
		uint64_t CaptureTick = 0;
		// Content revision represented by the capture.
		uint64_t ContentRevision = 0;
		// Lighting revision represented by the capture.
		uint64_t LightingRevision = 0;
		// Hash of eye pixels paired with this shadow capture.
		assets::ContentHash EyePixelHash;
		// Hash of the complete decoded depth image.
		assets::ContentHash DepthHash;
		// Player body intentionally omitted from the producer capture.
		std::string ExcludedPlayer;
		// Empty sources have zero SourceBounds and a wholly clear depth map.
		bool SourceEmpty = false;
		// World-space minimum XYZ then maximum XYZ. DomainBounds encloses nonempty SourceBounds.
		// Bounds of visible producer-world shadow casters.
		std::array<float, 6> SourceBounds{};
		// Producer-world domain enclosing SourceBounds and the shadow projection.
		std::array<float, 6> DomainBounds{};
		// Column-major native zero-to-one depth projection in the producer's world.
		std::array<float, 16> LightViewProjection{};
		// Value comparison for request matching and shadow cache reuse.
		bool operator==(const PortalShadowSnapshot &) const = default;
	};
	// Complete shadow snapshot with its decoded depth pixels.
	struct PortalShadowImage {
		// Identity and projection metadata for Depth.
		PortalShadowSnapshot Snapshot;
		// Exactly 2048 squared little-endian D32 samples, each finite and in [0,1].
		std::vector<std::byte> Depth;
		// Value comparison for assembled image validation and cache reuse.
		bool operator==(const PortalShadowImage &) const = default;
	};

	// Checks snapshot identity, bounds, hashes, and projection invariants.
	bool ValidPortalShadowSnapshot(const PortalShadowSnapshot &);
	// Checks a valid snapshot plus a complete finite D32 depth payload.
	bool ValidPortalShadowImage(const PortalShadowImage &);
	// A manifest announces the canonical snapshot without pixels. The caller must authenticate
	// its envelope and match a pending eye request before reserving an assembly from it.
	bool
	EncodePortalShadowManifest(const PortalShadowSnapshot &, std::vector<std::byte> &, std::string &error);
	// Decodes a manifest into a snapshot without allocating depth pixels.
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
		// Reserves one complete depth image only when expected fits the caller budget.
		bool Begin(const PortalShadowSnapshot &expected, size_t byteBudget, std::string &error);
		// Validates and copies one authenticated encoded tile into the reserved image.
		bool Accept(std::span<const std::byte>, std::string &error);
		// Publishes only after all tiles and the full-image hash have passed validation.
		std::optional<PortalShadowImage> Take();
		// Releases the incomplete reserved image and tile receipt state.
		void Cancel();
		// Reserved depth-pixel bytes charged while an assembly is pending.
		size_t Bytes() const;
		// Number of unique validated tiles copied into the pending image.
		size_t CompletedTiles() const;

	  private:
		std::optional<PortalShadowImage> Pending;
		uint16_t Received = 0;
		bool Complete = false;
	};
}
