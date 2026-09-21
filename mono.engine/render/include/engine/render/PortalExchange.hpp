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
	// Largest width or height accepted for a portal image.
	inline constexpr uint32_t MAX_PORTAL_IMAGE_EXTENT = 512;
	// Total pixel ceiling for one expanded portal image set.
	inline constexpr uint32_t MAX_PORTAL_IMAGE_PIXELS = 512 * 512;
	// Maximum nested capture depth admitted from one request.
	inline constexpr uint32_t MAX_PORTAL_IMAGE_RECURSION = 8;
	// Maximum encoded size of one portal exchange message, in bytes.
	inline constexpr size_t MAX_PORTAL_EXCHANGE_BYTES = 4 * 1024 * 1024;

	// The outer presentation envelope carries world names and incarnation tokens.
	// Consumers must match that envelope as well as this key before accepting a reply.
	// The outer envelope Correlation must equal RequestId.
	struct PortalExchangeKey {
		// Correlation value that must match the outer envelope.
		uint64_t RequestId = 0;
		// Stable, nonempty key of at most 256 bytes, with no embedded NUL. Never interned by this codec.
		std::string PortalKey;
		// Changes whenever pose, lens, extent, content scope or capture profile changes.
		uint64_t CameraRevision = 0;
		// Changes when the source seam geometry changes.
		uint64_t SeamRevision = 0;
		// Compares all identity and revision fields.
		bool operator==(const PortalExchangeKey &) const = default;
	};

	// Partial capture is opt-in. CompleteWorld includes transparent content and spatial UI,
	// composed in linear HDR before shared tone mapping. Host/screen UI is excluded.
	enum class PortalImageScope : uint8_t { CompleteWorld, OpaqueLighting };

	// Source revisions represented by a captured portal image.
	struct PortalImageVersion {
		// Revision of captured world content.
		uint64_t ContentRevision = 0;
		// Revision of captured lighting inputs.
		uint64_t LightingRevision = 0;
		// Compares both revisions.
		bool operator==(const PortalImageVersion &) const = default;
	};

	// The mapped entrance identifies one reciprocal mouth in the receiving world.
	// No source-world entity or renderer slot crosses the wire.
	struct PortalImageEntrance {
		// Stable name of the world containing the source mouth.
		std::string SourceWorld;
		// Centre of the entrance in source-world coordinates.
		std::array<float, 3> Centre{};
		// First edge basis point in source-world coordinates.
		std::array<float, 3> First{};
		// Second edge basis point in source-world coordinates.
		std::array<float, 3> Second{};
		// Compares the world name and all three points.
		bool operator==(const PortalImageEntrance &) const = default;
	};

	// Whether a capture uses the seam camera or the crossed eye camera.
	enum class PortalImageProjection : uint8_t { Seam, Eye };

	// Bounded capture request sent to the world that owns the destination portal.
	struct PortalImageRequest {
		// Canonical decimal account identity for primary-eye body exclusion.
		// Applies to eye and seam captures; shadows and child views retain the body.
		// Empty selects no player. ECS handles never cross this boundary.
		std::string EyePlayer;
		// Explicit retained-body profile. The producer must authorize this account
		// for the exact requesting endpoint before removing its colour and shadow rows.
		// Empty preserves ordinary primary-eye visibility and shadow behavior.
		std::string RetainedBodyPlayer{};
		// Matches this request to its eventual reply and source revisions.
		PortalExchangeKey Key;
		// Scene layers the producer must include in the image.
		PortalImageScope Scope = PortalImageScope::CompleteWorld;
		// Capture-camera position in the receiving world.
		std::array<float, 3> Position{};
		// Unit quaternion, XYZW. The camera looks down local -Z with local +Y up.
		// Squared norm tolerance is 0.001, also for the clip normal; decoding never normalizes.
		std::array<float, 4> Orientation{0, 0, 0, 1};
		// Off-axis bounds at Near, then Near and Far, in camera coordinates.
		std::array<float, 6> Frustum{-1, 1, -1, 1, 1, 1000};
		// World-space normalized plane. The retained half-space has dot(XYZ,p)+W >= 0.
		std::array<float, 4> ClipPlane{0, 0, 1, 0};
		// Requested image width in pixels.
		uint32_t Width = 0;
		// Requested image height in pixels.
		uint32_t Height = 0;
		// Depth already traversed in the recursive capture tree.
		uint32_t RecursionDepth = 0;
		// Whole request budget, including recursive captures. The initial image must fit;
		// the scheduler also charges every additional capture against this total.
		uint32_t PixelBudget = 0;
		// Optional owned PortalGeometry payload, already mapped and clipped at the
		// source. Asset names and palette poses cross; source ECS handles do not.
		std::vector<std::byte> Geometry;
		// Set only while the source still owns the matching image.
		std::optional<PortalImageVersion> KnownImage;
		// Reciprocal mouth used to map this request into the receiving world.
		std::optional<PortalImageEntrance> Entrance;
		// Eye views use the ordinary frustum after crossing. Their clip plane
		// is canonically zero and no receiving surface is hidden.
		PortalImageProjection Projection = PortalImageProjection::Seam;
		// OpaqueLighting with two ordered transparent layers and an overflow probe.
		// The layers include complete-world surfaces, particles, ribbons and spatial UI.
		// All four captures count against PixelBudget. Recursive requests require a
		// bounded capture tree; renewal remains unavailable for this profile.
		bool OrderedLayers = false;
		// Compares the full request, including optional geometry and lens policy.
		bool operator==(const PortalImageRequest &) const = default;
	};

	// Terminal outcomes of producing or admitting a portal image.
	enum class PortalImageStatus : uint8_t { Ok, Unavailable, Stale, BudgetExceeded, Unsupported, Failed };

	// Maximum evaluated lights copied with one capture.
	inline constexpr size_t MAX_PORTAL_CAPTURE_LIGHTS = 16;
	// One light's evaluated shading values in capture-world units.
	struct PortalCaptureLight {
		// Light origin in capture-world coordinates.
		std::array<float, 3> Position{};
		// Distance over which this light contributes, in world units.
		float Range = 8;
		// Linear RGB light colour.
		std::array<float, 3> Colour{1, 1, 1};
		// Direction of a cone or directional light in world coordinates.
		std::array<float, 3> Direction{0, -1, 0};
		// Cosine of the cone's limiting angle; negative one is unrestricted.
		float ConeCosine = -1;
		// Compares the evaluated light values.
		bool operator==(const PortalCaptureLight &) const = default;
	};
	// Evaluated shading inputs in capture-world units. Shadow maps, environment
	// textures and later radiance layers retain separate ownership requirements.
	struct PortalCaptureLighting {
		// Direction of the primary light in capture-world coordinates.
		std::array<float, 3> Direction{0, -1, 0};
		// Linear RGB ambient light contribution.
		std::array<float, 3> Ambient{};
		// Linear RGB outdoor ambient contribution.
		std::array<float, 3> OutdoorAmbient{};
		// Linear RGB direct-light contribution.
		std::array<float, 3> Direct{};
		// Linear RGB fog colour.
		std::array<float, 3> FogColour{};
		// Distance where fog starts, in world units.
		float FogStart = 100000;
		// Distance where fog reaches its end, in world units.
		float FogEnd = 100001;
		// Number of valid entries in Lights.
		uint8_t LightCount = 0;
		// Bounded evaluated lights; only the first LightCount entries are used.
		std::array<PortalCaptureLight, MAX_PORTAL_CAPTURE_LIGHTS> Lights{};
		// Compares the lighting values and bounded light array.
		bool operator==(const PortalCaptureLighting &) const = default;
	};

	// Shared validation for wire codecs and render-view preparation.
	bool ValidPortalCaptureLighting(const PortalCaptureLighting &lighting);

	// Owned pixels and capture metadata returned by the producing world.
	struct PortalImageReply {
		// Correlation key copied from the request.
		PortalExchangeKey Key;
		// Scene layers included in the returned image.
		PortalImageScope Scope = PortalImageScope::CompleteWorld;
		// Outcome of capture or request admission.
		PortalImageStatus Status = PortalImageStatus::Unavailable;
		// At most 1024 bytes without NUL; required for every non-Ok status.
		std::string Diagnostic;
		// World tick from which the image was captured.
		uint64_t CaptureTick = 0;
		// Captured content revision used to detect a stale reply.
		uint64_t ContentRevision = 0;
		// Captured lighting revision used to detect a stale reply.
		uint64_t LightingRevision = 0;
		// Image width in pixels.
		uint32_t Width = 0;
		// Image height in pixels.
		uint32_t Height = 0;
		// Byte stride between successive pixel rows.
		uint32_t RowStride = 0;
		// BLAKE3 digest of the decoded pixel bytes.
		assets::ContentHash PixelHash;
		// Owned top-left, tightly packed, linear RGBA16F. Each half is little-endian.
		// Successful replies require finite samples and a matching BLAKE3 digest.
		std::vector<std::byte> Pixels;
		// Optional depth paired with these pixels and their capture key. Top-left,
		// tightly packed little-endian float32 distance along the capture camera's
		// forward axis. Positive values are surfaces; positive zero is no surface.
		// Depth is in destination world units, before tone mapping or reprojection.
		assets::ContentHash DepthHash;
		// Owned forward-depth samples paired with Pixels.
		std::vector<std::byte> Depth;
		// Optional three-plane ambient inputs for successful lit opaque captures with depth.
		// Normal is little-endian RGB10A2_UNORM. AmbientResponse is RGBA32F:
		// nonnegative RGB response including material AO/fog, and original SSAO in alpha.
		// LightingBaseline is finite little-endian RGBA32F before HDR half rounding.
		// Digest of the decoded normal plane.
		assets::ContentHash NormalHash;
		// Digest of the decoded ambient response plane.
		assets::ContentHash AmbientResponseHash;
		// Digest of the decoded lighting baseline plane.
		assets::ContentHash LightingBaselineHash;
		// Encoded normals of the first visible surface.
		std::vector<std::byte> Normal;
		// Owned ambient response samples before composition.
		std::vector<std::byte> AmbientResponse;
		// Owned lighting baseline samples before HDR rounding.
		std::vector<std::byte> LightingBaseline;
		// Optional RGBA32F directional-light response, requiring all three ambient planes.
		// RGB is finite and nonnegative; alpha is original shadow visibility in [0, 1].
		assets::ContentHash DirectionalResponseHash;
		// Owned directional-light response samples.
		std::vector<std::byte> DirectionalResponse;
		// Evaluated lights needed to compose the response planes.
		std::optional<PortalCaptureLighting> CaptureLighting{};
		// Compares capture metadata and all owned planes.
		bool operator==(const PortalImageReply &) const = default;
	};

	// Maximum lens records accepted with a captured portal image.
	inline constexpr size_t MAX_PORTAL_CAPTURE_LENSES = 16;
	// Maximum owned shader-program bytes in a lens set.
	inline constexpr size_t MAX_PORTAL_CAPTURE_LENS_PROGRAM_BYTES = 1024 * 1024;
	// A capture carries each accepted program once, identified by its content hash.
	struct PortalCaptureLensProgram {
		// Content address of the accepted shader program.
		assets::ContentHash Hash;
		// Shader program words in SPIR-V format.
		std::vector<uint32_t> SpirV;
		// Compares the content address and program words.
		bool operator==(const PortalCaptureLensProgram &) const = default;
	};
	// Captured order is authoritative, including ties. Local entity IDs never cross the wire.
	struct PortalCaptureLens {
		// Lens origin in capture-world coordinates.
		std::array<float, 3> Position{};
		// Unit quaternion describing lens orientation.
		std::array<float, 4> Orientation{0, 0, 0, 1};
		// Stable shader name selected for this lens.
		std::string Shader;
		// Content address of Shader's captured program.
		assets::ContentHash ProgramHash;
		// Outer influence radius in world units.
		float Radius = 16;
		// Fully affected inner radius in world units.
		float InnerRadius = 4;
		// Strength decay between inner and outer radii.
		float Falloff = .75f;
		// Lens contribution multiplier.
		float Strength = 1;
		// Rotation applied to the lens sampling pattern.
		float Spin = 0;
		// Ordering priority retained from the producer.
		int32_t Priority = 0;
		// Producer-selected lens footprint shape.
		uint8_t Shape = 0;
		// Compares the lens parameters and shader identity.
		bool operator==(const PortalCaptureLens &) const = default;
	};
	// Captured lens records and their deduplicated shader programs.
	struct PortalCaptureLenses {
		// Capture time supplied to animated lens programs, in seconds.
		float TimeSeconds = 0;
		// Lens records in producer-defined order.
		std::vector<PortalCaptureLens> Entries;
		// Shader programs addressed by lens ProgramHash values.
		std::vector<PortalCaptureLensProgram> Programs;
		// Compares capture time, ordered lenses and programs.
		bool operator==(const PortalCaptureLenses &) const = default;
	};
	// Validates lens limits, parameters and program references.
	bool ValidPortalCaptureLenses(const PortalCaptureLenses &lenses);
	// Owned records, shader text and program bytes charged before image decompression.
	size_t PortalCaptureLensBytes(const PortalCaptureLenses &lenses);

	// Maximum ordered transparent layers in one portal image set.
	inline constexpr size_t MAX_PORTAL_TRANSPARENT_LAYERS = 2;
	// One opaque image and ordered, premultiplied transparent images from the
	// same capture. Those images have paired forward depth. The producer must
	// refuse overflow before publishing; this schema cannot prove completeness.
	struct PortalImageLayerSet {
		// Opaque base image shared by all later layers.
		PortalImageReply Opaque;
		// Ordered premultiplied layers over the opaque image.
		std::vector<PortalImageReply> Transparent;
		// Premultiplied world-space always-on-top UI, applied after body composition.
		// Shares capture metadata but has no depth domain. Screen UI is excluded.
		std::optional<PortalImageReply> SpatialOverlay = std::nullopt;
		// Lens effects evaluated for this capture.
		PortalCaptureLenses Lenses{};
		// Compares all images and lens inputs.
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
	// Decodes a bounded PIMG envelope into owned images and layer metadata.
	bool
	DecodePortalImageLayerSet(std::span<const std::byte> bytes, PortalImageLayerSet &out, std::string &error);

	// Bounded reply metadata read before admitting the full image payload.
	struct PortalImageReplyMatch {
		// Reported producer outcome.
		PortalImageStatus Status = PortalImageStatus::Failed;
		// Encoded diagnostic bytes to reserve before decoding.
		size_t DiagnosticBytes = 0;
		// World tick reported by the capture.
		uint64_t CaptureTick = 0;
		// Expanded depth bytes to reserve before decoding.
		size_t DepthBytes = 0;
		// Expanded normal, ambient response, baseline and optional directional response bytes.
		size_t AmbientBytes = 0;
		// Number of images in this reply or layer set.
		size_t ImageCount = 1;
		// Encoded metadata bytes to reserve before decoding.
		size_t MetadataBytes = 0;
	};

	// A successful same-render-owner completion. The bus carries no texture or
	// local slot ID. The receiver must resolve this exact receipt against its
	// host-owned resident capture table after authenticating both endpoints.
	struct PortalResidentReceipt {
		// Request and source revisions matched by the resident table.
		PortalExchangeKey Key;
		// Scene layers represented by the resident image.
		PortalImageScope Scope = PortalImageScope::CompleteWorld;
		// World tick captured by the resident image.
		uint64_t CaptureTick = 0;
		// Content revision captured by the resident image.
		uint64_t ContentRevision = 0;
		// Lighting revision captured by the resident image.
		uint64_t LightingRevision = 0;
		// Resident image width in pixels.
		uint32_t Width = 0;
		// Resident image height in pixels.
		uint32_t Height = 0;
		// Evaluated light inputs retained beside the resident image.
		std::optional<PortalCaptureLighting> CaptureLighting{};
		// Compares identity, dimensions and optional lighting.
		bool operator==(const PortalResidentReceipt &) const = default;
	};
	// Encodes a same-render-owner receipt without transferring texture data.
	bool EncodePortalResidentReceipt(
		const PortalResidentReceipt &receipt, std::vector<std::byte> &out, std::string &error
	);
	// Decodes a receipt for host-owned resident-image lookup.
	bool DecodePortalResidentReceipt(
		std::span<const std::byte> bytes, PortalResidentReceipt &out, std::string &error
	);

	// Renewal carries the same bounded metadata as a resident receipt, with a
	// distinct wire kind. It never transfers ownership of a new texture.
	bool EncodePortalImageRenewal(
		const PortalResidentReceipt &receipt, std::vector<std::byte> &out, std::string &error
	);
	// Decodes a renewal without transferring ownership of another texture.
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
	// ImageCount, DepthBytes and DiagnosticBytes include the optional spatial overlay
	// for transactional budget admission.
	std::optional<PortalImageReplyMatch> MatchPortalImageLayerSet(
		std::span<const std::byte> bytes, const PortalExchangeKey &expected, uint32_t width, uint32_t height
	);

	// Canonical little-endian versioned messages, bounded independently of the outer transport.
	// Output is unchanged on failure. Diagnostics are returned in error; success clears error.
	bool EncodePortalImageRequest(
		const PortalImageRequest &request, std::vector<std::byte> &out, std::string &error
	);
	// Decodes a bounded image request while preserving out on failure.
	bool
	DecodePortalImageRequest(std::span<const std::byte> bytes, PortalImageRequest &out, std::string &error);
	// Replies use lossless compression only when smaller. Decoding bounds expanded
	// storage by the validated image layout and checks the original HDR digest.
	bool
	EncodePortalImageReply(const PortalImageReply &reply, std::vector<std::byte> &out, std::string &error);
	// Decodes a reply and validates all expanded plane sizes and digests.
	bool DecodePortalImageReply(std::span<const std::byte> bytes, PortalImageReply &out, std::string &error);
}
