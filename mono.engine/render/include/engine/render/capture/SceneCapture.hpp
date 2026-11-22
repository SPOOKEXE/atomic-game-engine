#pragma once

// Scene image capture and spatial depth readback requests and results.

#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/render/resources/ResourceImage.hpp>
#include <engine/scene/Components.hpp>

#include <glm/mat4x4.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace engine::render {
	// An owned copy of the final scene image, before host chrome. Requests are
	// served by the next Render batch, including an explicit failure for a missing view.
	struct SceneCaptureRequest {
		// Target viewport, or Renderer::ANY_VIEWPORT for the next available view.
		size_t ViewSlot = static_cast<size_t>(-1);
		// Per-request pixel byte cap, also subject to the shared four-slot 64 MiB limit.
		size_t MaxBytes = 64u * 1024u * 1024u;
		// An engaged value requires the exact final native snapshot, including empty.
		std::optional<std::string> ExpectedSnapshotId{};
	};

	// Terminal outcome of one owned scene capture request.
	enum class SceneCaptureStatus : uint8_t {
		// The mapped readback has been copied into owned canonical RGBA bytes.
		Ready,
		// The requested view or its capture could not complete.
		Failed,
		// The image exceeds the request's or renderer's shared pixel byte budget.
		OverBudget
	};

	// Owned terminal result. Ready includes the final image and its actual producer provenance.
	struct SceneCaptureResult {
		// Process-unique ticket returned by RequestSceneCapture.
		uint64_t Token = 0;
		// Whether capture completed, failed or exceeded its byte budget.
		SceneCaptureStatus Status = SceneCaptureStatus::Failed;
		// Canonical, tightly packed RGBA bytes. Format states the actual transfer encoding.
		ResourceImageFormat Format = ResourceImageFormat::Unknown;
		// Captured image width in pixels.
		uint32_t Width = 0;
		// Captured image height in pixels.
		uint32_t Height = 0;
		// Bytes per packed row, equal to Width * 4.
		size_t RowStride = 0;
		// Owned tightly packed RGBA8 pixels, with encoding identified by Format.
		std::vector<uint8_t> Pixels;
		// Renderer frame counter that produced this image.
		uint64_t Frame = 0;
		// Process-local world key from the render request.
		uint64_t World = 0;
		// Source world's name copied from the rendered view.
		std::string WorldName;
		// Exact final source snapshot identity; empty is valid for ordinary native views.
		std::string SnapshotId;
		// Actual viewport slot that produced this image.
		size_t ViewSlot = 0;
		// Camera pose accepted by the render request.
		core::CFrame CameraFrame;
		// Effective raster lens, including any portal near-plane adjustment.
		// arch-waiver ecs-copy: owned diagnostic capture provenance, not world storage.
		scene::Camera Camera;
		// Resolved projection matrix used to rasterize this image.
		glm::mat4 Projection{1.0f};
		// Installed pipeline that rendered this image.
		core::Name Pipeline;
		// Revision of that installed pipeline at render time.
		uint64_t PipelineRevision = 0;
	};

	// Identifies depth retained for one rendered viewport and store incarnation.
	struct SpatialDepthFrameKey {
		// Renderer incarnation, store incarnation, viewport slot and frame sequence.
		//@{
		uint64_t Renderer = 0;
		uint64_t World = 0;
		uint64_t Viewport = 0;
		uint64_t Frame = 0;
		//@}
		// Compares every identity field.
		bool operator==(const SpatialDepthFrameKey &) const = default;
	};

	// Copied projection and extent of a retained spatial depth frame.
	struct SpatialDepthFrameInfo {
		// Frame identity and the host's corresponding GUI snapshot sequence.
		//@{
		SpatialDepthFrameKey Key;
		uint64_t SnapshotSequence = 0;
		//@}
		// Actual raster camera and projection, effective geometry eye and depth extent.
		//@{
		glm::mat4 ViewProjection{1.0f};
		core::CFrame CameraFrame;
		core::CFrame GeometryFrame;
		uint32_t Width = 0;
		uint32_t Height = 0;
		//@}
	};

	// Availability of a retained spatial depth frame.
	enum class SpatialDepthFrameStatus : uint8_t { Pending, Ready, Unknown };

	// Provenance of the GUI snapshot for this selected color frame.
	enum class SpatialSnapshotStatus : uint8_t {
		// No snapshot was requested.
		NotRequested,
		// No new image was produced; keep the previous displayed snapshot.
		Unchanged,
		// The output graph cannot prove the GUI and depth mapping.
		Unsupported,
		// A supported capture or retention failed.
		Unavailable,
		// The snapshot and any required depth belong to this frame.
		Ready
	};

	// State of one nonblocking spatial depth readback.
	enum class SpatialDepthReadbackStatus : uint8_t { Pending, Ready, Cancelled, Failed, Unknown };
	// Result of polling one retained spatial depth sample.
	struct SpatialDepthReadbackResult {
		// Completion status, source frame and normalized device depth.
		//@{
		SpatialDepthReadbackStatus Status = SpatialDepthReadbackStatus::Unknown;
		SpatialDepthFrameKey Frame;
		float Depth = 0.0f;
		//@}
	};
}
