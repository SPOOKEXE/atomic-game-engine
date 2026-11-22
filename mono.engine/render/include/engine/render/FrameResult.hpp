#pragma once

// What one Render call did: pass activity, frame results and GPU memory statistics.

#include <engine/core/Name.hpp>
#include <engine/render/capture/SceneCapture.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

namespace engine::render {
	// Effective inputs from the last scene recording for one view. Retained
	// frames keep these gates; command counts and FrameResult::Nodes describe work.
	struct PassActivity {
		// Whether a scene recording has been accepted; false before the first accepted record.
		bool Valid = false;
		// Whether sky inputs were active for the recorded view.
		bool Sky = false;
		// Whether fog inputs were active for the recorded view.
		bool Fog = false;
		// Whether shader lens inputs were active for the recorded view.
		bool ShaderLenses = false;
		// Whether depth of field inputs were active for the recorded view.
		bool DepthOfField = false;
		// Whether god ray inputs were active for the recorded view.
		bool GodRays = false;
		// Whether bloom inputs were active for the recorded view.
		bool Bloom = false;
		// Whether portal overlay inputs were active for the recorded view.
		bool PortalOverlay = false;
		// Whether mirror overlay inputs were active for the recorded view.
		bool MirrorOverlay = false;
		// Whether transparent inputs were active for the recorded view.
		bool Transparent = false;
		// Whether ground grid inputs were active for the recorded view.
		bool Grid = false;
		// Whether particle inputs were active for the recorded view.
		bool Particles = false;
		// Whether ribbon inputs were active for the recorded view.
		bool Ribbons = false;
	};

	// What one `Render` call submitted, and whether it reached the display.
	//
	// A default result means no frame was presented, including while the window
	// is minimised or resizing and when the renderer is unavailable.
	//
	// @since v0.1
	// @client
	struct FrameResult {
		// Whether the renderer submitted this frame's command buffer. Unlike
		// Presented, this stays true for a confirmed headless offscreen frame.
		bool Submitted = false;

		// Whether SDL accepted a command buffer for presentation.
		bool Presented = false;

		// Retained scene depth corresponding to this viewport frame, when available.
		SpatialDepthFrameKey SpatialDepth;
		// Projection copied with this color frame, independent of depth texture lifetime.
		std::optional<SpatialDepthFrameInfo> SpatialDepthProjection;
		// Whether this color frame has a proven matching GUI snapshot.
		SpatialSnapshotStatus SpatialSnapshot = SpatialSnapshotStatus::NotRequested;
		// Host-owned GUI snapshot sequence rendered with this depth frame.
		uint64_t SpatialSnapshotSequence = 0;

		// Number of opaque and overlay draw calls submitted for this frame.
		uint32_t DrawCalls = 0;

		// Number of opaque mesh triangles submitted for this frame.
		uint64_t Triangles = 0;

		// How many instances showed a surface texture.
		uint32_t SurfaceInstances = 0;

		// How many surface cameras re-rendered this frame.
		//
		// **Not how many exist**, which is the whole reason it is worth
		// reporting. A surface whose scene has not changed keeps the texture it
		// has and runs no pass, so a room with four mirrors in it costs four
		// passes on the frames something moves and none on the frames nothing
		// does. Those two frames are indistinguishable from the draw-call count
		// - a skipped surface pass and one that ran and changed nothing look
		// identical from there - and this is the number that tells them apart.
		//
		// Zero with mirrors on screen is the ordinary case for a still scene and
		// is not a failure. Equal to the mirror count on every frame means the
		// signature is never matching, which usually means something in the draw
		// list is moving that nobody thinks is moving.
		//
		// @since v0.8
		uint32_t SurfacePasses = 0;

		// How many portal sub-renders this frame ran.
		//
		// **Reported separately from `SurfacePasses` because it is the number
		// that goes up fastest.** A level is a whole scene rendered from a
		// camera that did not exist a moment ago, and the count is holes times
		// levels in the worst case - so this is where a corridor of portals
		// shows up, and where `graph::VisiblePane` doing its job shows up as the
		// same corridor costing two.
		//
		// @since v0.15
		uint32_t PortalPasses = 0;

		// A requested visible subcapture could not fit its depth or pixel limit.
		bool SurfaceBudgetExceeded = false;

		// Whether this frame rebuilt the retained plan for mirror and portal
		// surface captures. The cache diagnostics use this to distinguish a
		// stable camera hit from a descriptor or camera change.
		bool SurfaceCapturePlanWrite = false;

		// How many ribbon vertices were submitted this frame.
		//
		// Two per segment, so a beam is twenty-two and a trail is at most
		// thirty-two. Reported beside the particle count because the two are the
		// same question asked of the other half of the module.
		//
		// @since v0.10
		uint32_t RibbonVertices = 0;

		// How many particles were submitted this frame.
		//
		// **Reported because the number is the whole diagnosis for a scene that
		// is slow and looks fine.** An emitter whose rate ran away is invisible -
		// the particles are small and transparent - and shows up here as a count
		// an order of magnitude above what the scene should have.
		//
		// @since v0.10
		uint32_t Particles = 0;

		// How many particle quads reached a draw in any camera pass. This may exceed
		// Particles when portals draw the pool again. Zero means resident simulation
		// changed no scene pixel and the particle image can remain cached.
		uint32_t ParticlesDrawn = 0;

		// How many mesh instances or particle quads the frustum rejected.
		//
		// **Reported rather than inferred**, because the interesting number is
		// the ratio and the denominator is the caller's draw list - which the
		// caller has and this does not need to repeat. A camera framing its own
		// scene culls almost nothing and a camera inside a large world culls
		// almost everything; a reading that never moves means the frustum is
		// wrong, not that the scene is small.
		uint32_t Culled = 0;

		// How many chunks the resident instance pool spans, and how many contain
		// a row uploaded by this view.
		//
		// **Resident rows, not draw-order rows.** Visibility and ordering are a
		// separate uint index stream, so a camera turn or a frustum edge no longer
		// moves packed rows. A chunk is dirty only when a stable entity slot is
		// new or its packed transform, scale, rotation, or colour changed.
		//
		// The upload-order experiment that preceded residency rewrote 100% when a
		// cull or membership edit shifted rows. This pair measures the stable-slot
		// result that experiment selected.
		//
		// Equal counts every frame in a still scene means resident rows are still
		// changing and the delta is not paying.
		//
		// @since v0.19
		//@{
		uint32_t InstanceChunks = 0;
		uint32_t InstanceChunksDirty = 0;
		//@}

		// The same resident-pool measurement at row granularity. Chunks say how
		// scattered a copy is; rows say how many 36-byte payloads actually crossed.
		//@{
		uint32_t InstanceRows = 0;
		uint32_t InstanceRowsDirty = 0;
		//@}

		// Resource traffic declared by the graph after world and view scopes are
		// expanded for this frame. QueueTransferBytes is visibility traffic across
		// logical queues, not necessarily a physical memory copy.
		//@{
		uint64_t ScheduledReadBytes = 0;
		uint64_t ScheduledWriteBytes = 0;
		uint64_t QueueTransferBytes = 0;
		//@}

		// Bytes actually copied from CPU staging memory, and any dedicated SDL
		// copy command buffers that carried them. Unlike the scheduled figures
		// above, these are observed backend traffic for this frame. Ordinary frame
		// uploads are recorded into the main buffer, so a non-zero buffer count
		// identifies a transfer that could not join the batch.
		//@{
		uint64_t UploadedBytes = 0;
		uint32_t UploadCommandBuffers = 0;
		//@}

		// Compute work recorded this frame, and how often an async-eligible
		// dispatch was safe to submit on its own command buffer before any
		// dependency-bound work entered the main stream.
		//@{
		uint32_t ComputeDispatches = 0;
		uint32_t AsyncComputeCommandBuffers = 0;
		//@}

		// Later-transfer command buffers submitted after the main buffer:
		// resource previews and file captures download through them, so on SDL's
		// one unified queue they read the frame's finished images without a
		// copy pass inside the graphics stream. Owned HDR exports instead copy
		// at their graph read and share the scene fence to preserve alias lifetimes.
		//
		// @since v0.17
		uint32_t DownloadCommandBuffers = 0;

		// Command buffers the traffic plan splits the frame into, summed over
		// the pipelines this frame ran. SDL submits them serially on its one
		// unified queue; the count is the boundary a backend with independent
		// device queues would exploit, reported beside `ConcurrentWaves` for
		// the same reason.
		//
		// @since v0.17
		uint32_t TrafficCommandBuffers = 0;

		// Dependency waves containing independent work on more than one queue.
		// SDL records them serially, but the count remains useful to a backend that
		// exposes native async queues and to the Studio profiler.
		uint32_t ConcurrentWaves = 0;

		// Graph nodes that submitted work, in first-submission order.
		//
		// Names replace the old six-bit physical-pass mask. A world can author
		// more than six nodes and the result must report the graph it actually
		// ran rather than squeeze it back into the removed fixed renderer.
		std::vector<core::Name> Nodes;

		// Whether a named graph node submitted work this frame.
		bool Ran(core::Name node) const {
			return std::find(Nodes.begin(), Nodes.end(), node) != Nodes.end();
		}

		// Adds one view's counters while keeping node names unique and ordered.
		void Accumulate(const FrameResult &view);
	};

	// Logical payload held by the renderer's live GPU resources.
	//
	// SDL does not expose backend heap commitments portably, so these figures
	// count the bytes requested for buffers, transfer buffers, textures, mip
	// levels, and samples. Driver alignment, pipelines, samplers, shaders, and
	// swapchain images are deliberately outside the total.
	//
	// `AllocatedBytes`, `ReleasedBytes`, and the allocation counts are cumulative
	// for the device lifetime. Live bytes can stay flat while a target is rebuilt
	// every frame, so these figures distinguish residency from allocation churn.
	//
	// @since v0.19
	// @client
	struct GpuMemoryStatistics {
		// Current, peak, and cumulative logical payload bytes.
		//@{
		uint64_t LiveBytes = 0;
		uint64_t PeakBytes = 0;
		uint64_t AllocatedBytes = 0;
		uint64_t ReleasedBytes = 0;
		//@}

		// Current payload bytes by resource family.
		//@{
		uint64_t BufferBytes = 0;
		uint64_t TransferBufferBytes = 0;
		uint64_t TextureBytes = 0;
		//@}

		// Current live resources by family.
		//@{
		uint64_t Buffers = 0;
		uint64_t TransferBuffers = 0;
		uint64_t Textures = 0;
		//@}

		// Cumulative creations by resource family.
		//@{
		uint64_t BufferAllocations = 0;
		uint64_t TransferBufferAllocations = 0;
		uint64_t TextureAllocations = 0;
		//@}
	};

	// One content mesh's live instance rows and the latest staged GPU delta.
	// A row is one `GpuInstance`, so `StagedBytes` is exact transfer payload.
	// @client
	struct AssetResidencyStatistics {
		// Asset whose instance residency is being measured.
		core::Name Name;
		// Instance rows already resident on the device.
		uint32_t ResidentInstances = 0;
		// Instance rows included in the latest staged delta.
		uint32_t StagedInstances = 0;
		// Bytes in that staged instance delta.
		uint64_t StagedBytes = 0;
	};
}
