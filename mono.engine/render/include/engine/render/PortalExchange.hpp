#pragma once

#include <engine/assets/ContentHash.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace engine::render {
	inline constexpr uint32_t MAX_PORTAL_IMAGE_EXTENT = 512;
	inline constexpr uint32_t MAX_PORTAL_IMAGE_PIXELS = 512 * 512;
	inline constexpr uint32_t MAX_PORTAL_IMAGE_RECURSION = 8;
	inline constexpr size_t MAX_PORTAL_EXCHANGE_BYTES = 4 * 1024 * 1024;

	// The outer presentation envelope carries world names and incarnation tokens.
	// Consumers must match that envelope as well as this key before accepting a reply.
	// The outer envelope Correlation must equal RequestId.
	struct PortalExchangeKey {
		uint64_t RequestId = 0;
		// Stable, nonempty key of at most 256 bytes, with no embedded NUL. Never interned by this codec.
		std::string PortalKey;
		// Changes whenever pose, lens, extent, content scope or capture profile changes.
		uint64_t CameraRevision = 0;
		uint64_t SeamRevision = 0;
		bool operator==(const PortalExchangeKey &) const = default;
	};

	// Partial capture is opt-in. CompleteWorld includes transparent content and spatial UI,
	// composed in linear HDR before shared tone mapping. Host/screen UI is excluded.
	enum class PortalImageScope : uint8_t { CompleteWorld, OpaqueLighting };

	struct PortalImageVersion {
		uint64_t ContentRevision = 0;
		uint64_t LightingRevision = 0;
		bool operator==(const PortalImageVersion &) const = default;
	};

	// The mapped entrance identifies one reciprocal mouth in the receiving world.
	// No source-world entity or renderer slot crosses the wire.
	struct PortalImageEntrance {
		std::string SourceWorld;
		std::array<float, 3> Centre{};
		std::array<float, 3> First{};
		std::array<float, 3> Second{};
		bool operator==(const PortalImageEntrance &) const = default;
	};

	enum class PortalImageProjection : uint8_t { Seam, Eye };

	struct PortalImageRequest {
		// Canonical decimal account identity for primary-eye body exclusion.
		// Applies to eye and seam captures; shadows and child views retain the body.
		// Empty selects no player. ECS handles never cross this boundary.
		std::string EyePlayer;
		PortalExchangeKey Key;
		PortalImageScope Scope = PortalImageScope::CompleteWorld;
		std::array<float, 3> Position{};
		// Unit quaternion, XYZW. The camera looks down local -Z with local +Y up.
		// Squared norm tolerance is 0.001, also for the clip normal; decoding never normalizes.
		std::array<float, 4> Orientation{0, 0, 0, 1};
		// Off-axis bounds at Near, then Near and Far, in camera coordinates.
		std::array<float, 6> Frustum{-1, 1, -1, 1, 1, 1000};
		// World-space normalized plane. The retained half-space has dot(XYZ,p)+W >= 0.
		std::array<float, 4> ClipPlane{0, 0, 1, 0};
		uint32_t Width = 0;
		uint32_t Height = 0;
		uint32_t RecursionDepth = 0;
		// Whole request budget, including recursive captures. The initial image must fit;
		// the scheduler also charges every additional capture against this total.
		uint32_t PixelBudget = 0;
		// Optional owned PortalGeometry payload, already mapped and clipped at the
		// source. Asset names and palette poses cross; source ECS handles do not.
		std::vector<std::byte> Geometry;
		// Set only while the source still owns the matching image.
		std::optional<PortalImageVersion> KnownImage;
		std::optional<PortalImageEntrance> Entrance;
		// Eye views use the ordinary frustum after crossing. Their clip plane
		// is canonically zero and no receiving surface is hidden.
		PortalImageProjection Projection = PortalImageProjection::Seam;
		// OpaqueLighting with two ordered transparent layers and an overflow probe.
		// All four captures count against PixelBudget. No recursive views or renewal.
		bool OrderedLayers = false;
		bool operator==(const PortalImageRequest &) const = default;
	};

	enum class PortalImageStatus : uint8_t { Ok, Unavailable, Stale, BudgetExceeded, Unsupported, Failed };

	inline constexpr size_t MAX_PORTAL_CAPTURE_LIGHTS = 16;
	struct PortalCaptureLight {
		std::array<float, 3> Position{};
		float Range = 8;
		std::array<float, 3> Colour{1, 1, 1};
		std::array<float, 3> Direction{0, -1, 0};
		float ConeCosine = -1;
		bool operator==(const PortalCaptureLight &) const = default;
	};
	// Evaluated shading inputs in capture-world units. Shadow maps, environment
	// textures and later radiance layers retain separate ownership requirements.
	struct PortalCaptureLighting {
		std::array<float, 3> Direction{0, -1, 0};
		std::array<float, 3> Ambient{}, OutdoorAmbient{}, Direct{};
		std::array<float, 3> FogColour{};
		float FogStart = 100000, FogEnd = 100001;
		uint8_t LightCount = 0;
		std::array<PortalCaptureLight, MAX_PORTAL_CAPTURE_LIGHTS> Lights{};
		bool operator==(const PortalCaptureLighting &) const = default;
	};

	// Shared validation for wire codecs and render-view preparation.
	bool ValidPortalCaptureLighting(const PortalCaptureLighting &lighting);

	struct PortalImageReply {
		PortalExchangeKey Key;
		PortalImageScope Scope = PortalImageScope::CompleteWorld;
		PortalImageStatus Status = PortalImageStatus::Unavailable;
		// At most 1024 bytes without NUL; required for every non-Ok status.
		std::string Diagnostic;
		uint64_t CaptureTick = 0;
		uint64_t ContentRevision = 0;
		uint64_t LightingRevision = 0;
		uint32_t Width = 0;
		uint32_t Height = 0;
		uint32_t RowStride = 0;
		assets::ContentHash PixelHash;
		// Owned top-left, tightly packed, linear RGBA16F. Each half is little-endian.
		// Successful replies require finite samples and a matching BLAKE3 digest.
		std::vector<std::byte> Pixels;
		// Optional depth paired with these pixels and their capture key. Top-left,
		// tightly packed little-endian float32 distance along the capture camera's
		// forward axis. Positive values are surfaces; positive zero is no surface.
		// Depth is in destination world units, before tone mapping or reprojection.
		assets::ContentHash DepthHash;
		std::vector<std::byte> Depth;
		std::optional<PortalCaptureLighting> CaptureLighting{};
		bool operator==(const PortalImageReply &) const = default;
	};

	inline constexpr size_t MAX_PORTAL_TRANSPARENT_LAYERS = 2;
	// One opaque image and ordered, premultiplied transparent images from the
	// same capture. Every image has paired forward depth. The producer must
	// refuse overflow before publishing; this schema cannot prove completeness.
	struct PortalImageLayerSet {
		PortalImageReply Opaque;
		std::vector<PortalImageReply> Transparent;
		bool operator==(const PortalImageLayerSet &) const = default;
	};

	// Validate owned members without serializing or compressing them.
	bool ValidPortalImageLayerSet(const PortalImageLayerSet &layers);

	// A single PIMG envelope. Total expanded pixels across all members must fit
	// MAX_PORTAL_IMAGE_PIXELS, and the wire stays within MAX_PORTAL_EXCHANGE_BYTES.
	// Outputs remain unchanged on failure. Each nested image retains its digest
	// and lossless encoding; the outer envelope authenticates no endpoint itself.
	bool EncodePortalImageLayerSet(
		const PortalImageLayerSet &layers, std::vector<std::byte> &out, std::string &error
	);
	bool
	DecodePortalImageLayerSet(std::span<const std::byte> bytes, PortalImageLayerSet &out, std::string &error);

	struct PortalImageReplyMatch {
		PortalImageStatus Status = PortalImageStatus::Failed;
		size_t DiagnosticBytes = 0;
		uint64_t CaptureTick = 0;
		size_t DepthBytes = 0;
	};

	// A successful same-render-owner completion. The bus carries no texture or
	// local slot ID. The receiver must resolve this exact receipt against its
	// host-owned resident capture table after authenticating both endpoints.
	struct PortalResidentReceipt {
		PortalExchangeKey Key;
		PortalImageScope Scope = PortalImageScope::CompleteWorld;
		uint64_t CaptureTick = 0;
		uint64_t ContentRevision = 0;
		uint64_t LightingRevision = 0;
		uint32_t Width = 0;
		uint32_t Height = 0;
		std::optional<PortalCaptureLighting> CaptureLighting{};
		bool operator==(const PortalResidentReceipt &) const = default;
	};
	bool EncodePortalResidentReceipt(
		const PortalResidentReceipt &receipt, std::vector<std::byte> &out, std::string &error
	);
	bool DecodePortalResidentReceipt(
		std::span<const std::byte> bytes, PortalResidentReceipt &out, std::string &error
	);

	// Renewal carries the same bounded metadata as a resident receipt, with a
	// distinct wire kind. It never transfers ownership of a new texture.
	bool EncodePortalImageRenewal(
		const PortalResidentReceipt &receipt, std::vector<std::byte> &out, std::string &error
	);
	bool DecodePortalImageRenewal(
		std::span<const std::byte> bytes, PortalResidentReceipt &out, std::string &error
	);

	// Borrows and compares the bounded reply prefix without allocating or interning text.
	// A match is only admission metadata; DecodePortalImageReply must still validate the full message.
	std::optional<PortalImageReplyMatch> MatchPortalImageReply(
		std::span<const std::byte> bytes,
		const PortalExchangeKey &expected,
		uint32_t width,
		uint32_t height,
		PortalImageScope scope = PortalImageScope::CompleteWorld
	);

	// Admission for the fixed two-transparent-layer request profile. Borrows every
	// member prefix without allocation; full decoding must still validate contents.
	// DepthBytes and DiagnosticBytes sum all members for transactional budget admission.
	std::optional<PortalImageReplyMatch> MatchPortalImageLayerSet(
		std::span<const std::byte> bytes, const PortalExchangeKey &expected, uint32_t width, uint32_t height
	);

	// Canonical little-endian versioned messages, bounded independently of the outer transport.
	// Output is unchanged on failure. Diagnostics are returned in error; success clears error.
	bool EncodePortalImageRequest(
		const PortalImageRequest &request, std::vector<std::byte> &out, std::string &error
	);
	bool
	DecodePortalImageRequest(std::span<const std::byte> bytes, PortalImageRequest &out, std::string &error);
	// Replies use lossless compression only when smaller. Decoding bounds expanded
	// storage by the validated image layout and checks the original HDR digest.
	bool
	EncodePortalImageReply(const PortalImageReply &reply, std::vector<std::byte> &out, std::string &error);
	bool DecodePortalImageReply(std::span<const std::byte> bytes, PortalImageReply &out, std::string &error);
}
