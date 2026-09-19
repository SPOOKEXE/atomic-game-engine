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
	enum class ResourceImageDelivery : uint8_t { CopiedPixels, Resident };
	// Process-local request for one declared capture node. Tokens must be unique
	// until the result is taken or cancellation completes. No GPU handle escapes.
	struct ResourceImageRequest {
		uint64_t Token = 0;
		core::Name Pipeline{};
		core::Name Node{};
		size_t ViewSlot = 0;
		ResourceImageDelivery Delivery = ResourceImageDelivery::CopiedPixels;
		// Empty for ordinary renderer clients. Data captures use this as the
		// snapshot barrier checked before any GPU download is recorded.
		std::string ExpectedSnapshotId{};
	};

	enum class ResourceImageStatus : uint8_t { Ok, Unsupported, Failed };
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
		R32_UInt
	};
	// The state of the built-in SSAO image that produced an occlusion capture.
	// It describes estimator provenance, never physical ambient-occlusion truth.
	enum class AmbientOcclusionSourceState : uint8_t {
		Estimated,
		ClearedDisabled,
		ClearedNoPass,
		Unavailable
	};
	enum class AmbientOcclusionDenoiser : uint8_t { None };
	enum class AmbientOcclusionTemporalHistory : uint8_t { Disabled };
	enum class AmbientOcclusionBackgroundClassification : uint8_t { Unavailable };

	struct AmbientOcclusionProvenance {
		AmbientOcclusionSourceState SourceState = AmbientOcclusionSourceState::Unavailable;
		std::optional<uint64_t> ProducerFrame;
		std::optional<bool> Enabled;
		std::optional<uint32_t> SampleCount;
		std::optional<float> RadiusWorldUnits;
		std::optional<AmbientOcclusionDenoiser> Denoiser;
		std::optional<AmbientOcclusionTemporalHistory> TemporalHistory;
		std::optional<float> BackgroundValue;
		std::optional<AmbientOcclusionBackgroundClassification> BackgroundClassification;
	};
	// Transfer allocations retained by all capture slots share this bound.
	inline constexpr size_t MAX_RESOURCE_IMAGE_STAGING_BYTES = 32 * 1024 * 1024;
	struct ResourceShadowCapture {
		// Empty sources have canonical zero SourceBounds, not the native fitting fallback.
		bool SourceEmpty = false;
		core::AABB SourceBounds{};
		core::AABB DomainBounds{};
		// Exact column-major matrix used by the producing shadow pass.
		std::array<float, 16> LightViewProjection{};
	};

	struct ResourceImage {
		ResourceImageRequest Request;
		// Present for the built-in data-capture observation hook. The value is
		// copied before GPU work is submitted and survives asynchronous completion.
		std::optional<RenderObservationContext> Observation;
		core::Name Resource;
		ResourceImageStatus Status = ResourceImageStatus::Failed;
		ResourceImageKind Kind = ResourceImageKind::Colour;
		std::optional<ResourceShadowCapture> Shadow;
		// Renderer-local frame that actually executed the capture, zero if it did
		// not run. Compare within one live renderer only, never as a world tick.
		uint64_t CaptureFrame = 0;
		// Measured after the fence was observed complete, around mapping and
		// copying this image's transfer planes into owned CPU bytes.
		uint64_t ReadbackCpuNanoseconds = 0;
		// Filled at the capture node from the View that produced these bytes. It
		// is empty for ordinary renderer clients that do not establish a data
		// snapshot barrier.
		std::string SnapshotId;
		std::array<float, 16> CameraWorldFromCamera{};
		bool CameraProjectionAvailable = false;
		std::array<float, 16> CameraProjection{};
		float CameraFieldOfViewRadians = 0.0f;
		float CameraNearPlane = 0.0f;
		float CameraFarPlane = 0.0f;
		float CameraCropLeft = 0.0f;
		float CameraCropTop = 0.0f;
		float CameraCropWidth = 1.0f;
		float CameraCropHeight = 1.0f;
		uint32_t CaptureWidth = 0;
		uint32_t CaptureHeight = 0;
		uint32_t Width = 0;
		uint32_t Height = 0;
		uint32_t RowStride = 0;
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
		std::vector<std::byte> Depth;
		// Optional retained ambient inputs, present together with depth. Normal is
		// packed RGB10A2_UNORM; response and baseline are tight little-endian RGBA32F.
		core::Name NormalResource;
		core::Name AmbientResponseResource;
		core::Name LightingBaselineResource;
		core::Name DirectionalResponseResource;
		std::vector<std::byte> Normal;
		std::vector<std::byte> AmbientResponse;
		std::vector<std::byte> LightingBaseline;
		// Optional directional-light derivative and original shadow visibility.
		std::vector<std::byte> DirectionalResponse;
	};
}
