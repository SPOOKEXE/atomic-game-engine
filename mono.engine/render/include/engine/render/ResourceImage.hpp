#pragma once

#include <engine/core/Name.hpp>
#include <engine/core/types/AABB.hpp>
#include <engine/render/RenderObservation.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace engine::render {
	// Whether a capture returns owned pixels or a renderer-resident image.
	enum class ResourceImageDelivery : uint8_t { CopiedPixels, Resident };
	// Process-local request for one declared capture node. Tokens must be unique
	// until the result is taken or cancellation completes. No GPU handle escapes.
	struct ResourceImageRequest {
		// Process-local ticket used to match asynchronous completion.
		uint64_t Token = 0;
		// Pipeline containing the requested capture node.
		core::Name Pipeline{};
		// Node whose declared image resource will be copied.
		core::Name Node{};
		// View slot to capture.
		size_t ViewSlot = 0;
		// Ownership mode requested for the completed image.
		ResourceImageDelivery Delivery = ResourceImageDelivery::CopiedPixels;
		// Empty for ordinary renderer clients. Data captures use this as the
		// snapshot barrier checked before any GPU download is recorded.
		std::string ExpectedSnapshotId{};
	};

	// Completion outcomes for a requested resource image.
	enum class ResourceImageStatus : uint8_t { Ok, Unsupported, Failed };
	// Native image interpretation selected by the capture node.
	enum class ResourceImageKind : uint8_t { Colour, DirectionalShadow };
	// Native texture storage copied for a colour capture. Consumers must inspect
	// this before assigning scalar or colour-space semantics to Pixels.
	enum class ResourceImageFormat : uint8_t {
		Unknown,
		R8_UNorm,
		RGBA8_UNorm,
		RGBA8_SRGB,
		RG16_Float,
		RGBA16_Float,
		R32_UInt,
		RGBA32_Float
	};
	// The state of the built-in SSAO image that produced an occlusion capture.
	// It describes estimator provenance, never physical ambient-occlusion truth.
	enum class AmbientOcclusionSourceState : uint8_t {
		Estimated,
		ClearedDisabled,
		ClearedNoPass,
		Unavailable
	};
	// Denoising policy recorded for the built-in occlusion estimator.
	enum class AmbientOcclusionDenoiser : uint8_t { None };
	// Temporal policy recorded for the built-in occlusion estimator.
	enum class AmbientOcclusionTemporalHistory : uint8_t { Disabled };
	// Classification recorded when the background cannot be identified.
	enum class AmbientOcclusionBackgroundClassification : uint8_t { Unavailable };

	// Settings and state of the estimator that produced an occlusion plane.
	struct AmbientOcclusionProvenance {
		// Whether occlusion was estimated, cleared or unavailable.
		AmbientOcclusionSourceState SourceState = AmbientOcclusionSourceState::Unavailable;
		// Renderer frame that produced the estimate, when known.
		std::optional<uint64_t> ProducerFrame;
		// Whether the estimator was enabled for this capture.
		std::optional<bool> Enabled;
		// Number of samples used by the estimator, when known.
		std::optional<uint32_t> SampleCount;
		// Estimator sampling radius in world units, when known.
		std::optional<float> RadiusWorldUnits;
		// Denoising policy applied to the estimate, when known.
		std::optional<AmbientOcclusionDenoiser> Denoiser;
		// Temporal history policy used by the estimate, when known.
		std::optional<AmbientOcclusionTemporalHistory> TemporalHistory;
		// Occlusion value written where no scene surface was found.
		std::optional<float> BackgroundValue;
		// How background pixels were identified, when known.
		std::optional<AmbientOcclusionBackgroundClassification> BackgroundClassification;
	};
	// Transfer allocations retained by all capture slots share this bound. It
	// matches one retained data-factory capture at its native 1280x720 extent.
	inline constexpr size_t MAX_RESOURCE_IMAGE_STAGING_BYTES = 64 * 1024 * 1024;
	// Shadow-map geometry and matrix returned with a directional capture.
	struct ResourceShadowCapture {
		// Empty sources have canonical zero SourceBounds, not the native fitting fallback.
		bool SourceEmpty = false;
		// Bounds of source geometry that contributed to the map.
		core::AABB SourceBounds{};
		// Bounds fitted into the shadow projection.
		core::AABB DomainBounds{};
		// Exact column-major matrix used by the producing shadow pass.
		std::array<float, 16> LightViewProjection{};
	};

	// Owned readback and capture metadata from one renderer resource.
	struct ResourceImage {
		// Ticket and node selection that initiated this readback.
		ResourceImageRequest Request;
		// Timestamp query identifier shared with a data-capture ticket.
		uint64_t DataCaptureTimingId = 0;
		// Present for the built-in data-capture observation hook. The value is
		// copied before GPU work is submitted and survives asynchronous completion.
		std::optional<RenderObservationContext> Observation;
		// Graph resource actually copied by the capture node.
		core::Name Resource;
		// Outcome of the resource copy.
		ResourceImageStatus Status = ResourceImageStatus::Failed;
		// Colour or directional-shadow interpretation of owned planes.
		ResourceImageKind Kind = ResourceImageKind::Colour;
		// Shadow fitting metadata for a directional-shadow image.
		std::optional<ResourceShadowCapture> Shadow;
		// Renderer-local frame that actually executed the capture, zero if it did
		// not run. Compare within one live renderer only, never as a world tick.
		uint64_t CaptureFrame = 0;
		// Measured after the fence was observed complete, around mapping and
		// copying this image's transfer planes into owned CPU bytes.
		uint64_t ReadbackCpuNanoseconds = 0;
		// Sum of the six readback vectors' element capacities once collection has
		// completed. This is ticket-scoped owned vector storage, not a process heap
		// counter or an allocator peak.
		uint64_t ReadbackHostReservedCapacityBytes = 0;
		// Capacity of the tracked GPU transfer buffer assigned to this copied image
		// while it was recorded. Reused buffers report their reserved capacity but
		// do not claim that this image created an allocation.
		uint64_t ReadbackDeviceStagingReservedCapacityBytes = 0;
		// Filled at the capture node from the View that produced these bytes. It
		// is empty for ordinary renderer clients that do not establish a data
		// snapshot barrier.
		std::string SnapshotId;
		// Column-major transform from the captured camera to its world.
		std::array<float, 16> CameraWorldFromCamera{};
		// False when no camera projection was captured.
		bool CameraProjectionAvailable = false;
		// Column-major projection matrix when available.
		std::array<float, 16> CameraProjection{};
		// Vertical field of view in radians.
		float CameraFieldOfViewRadians = 0.0f;
		// Near clipping distance in capture-world units.
		float CameraNearPlane = 0.0f;
		// Far clipping distance in capture-world units.
		float CameraFarPlane = 0.0f;
		// Left edge of the normalized projection crop.
		float CameraCropLeft = 0.0f;
		// Top edge of the normalized projection crop.
		float CameraCropTop = 0.0f;
		// Width of the normalized projection crop.
		float CameraCropWidth = 1.0f;
		// Height of the normalized projection crop.
		float CameraCropHeight = 1.0f;
		// Full capture target width in pixels before resource sizing.
		uint32_t CaptureWidth = 0;
		// Full capture target height in pixels before resource sizing.
		uint32_t CaptureHeight = 0;
		// Copied resource width in pixels.
		uint32_t Width = 0;
		// Copied resource height in pixels.
		uint32_t Height = 0;
		// Byte stride between successive resource rows.
		uint32_t RowStride = 0;
		// Native texture format of Pixels.
		ResourceImageFormat Format = ResourceImageFormat::Unknown;
		// Present for an R8 ambient-occlusion capture. Custom R8 resources carry
		// the explicit Unavailable state rather than borrowed built-in settings.
		std::optional<AmbientOcclusionProvenance> AmbientOcclusion;
		// Set for the built-in second-surface pair. The string names the source
		// depth format and its exact equality-bias rule.
		std::string Provenance;
		// The renderer frame that supplied the preceding verified camera for a
		// camera-motion capture. Empty means this image carries no temporal fact.
		std::optional<uint64_t> PreviousCameraMotionFrame;
		// Owned top-left rows in the native Format, in little-endian order.
		std::vector<std::byte> Pixels;
		// Optional declared R32F capture input, copied in the same submission.
		// DirectionalShadow instead owns only this plane: exact D32F device depth,
		// with RowStride == Width * 4 and matching Shadow metadata.
		// Tight top-left little-endian float32 rows; sample semantics belong to
		// the named resource. The built-in linear-depth resource clears to FarPlane.
		core::Name DepthResource;
		// Owned depth samples from DepthResource.
		std::vector<std::byte> Depth;
		// Optional retained ambient inputs, present together with depth. Normal is
		// packed RGB10A2_UNORM; response and baseline are tight little-endian RGBA32F.
		core::Name NormalResource;
		// Graph resource that supplied ambient-response samples.
		core::Name AmbientResponseResource;
		// Graph resource that supplied pre-rounded lighting samples.
		core::Name LightingBaselineResource;
		// Graph resource that supplied the directional-light response.
		core::Name DirectionalResponseResource;
		// Owned first-surface normal samples.
		std::vector<std::byte> Normal;
		// Owned ambient-response samples.
		std::vector<std::byte> AmbientResponse;
		// Owned lighting-baseline samples.
		std::vector<std::byte> LightingBaseline;
		// Optional directional-light derivative and original shadow visibility.
		std::vector<std::byte> DirectionalResponse;
	};
}
