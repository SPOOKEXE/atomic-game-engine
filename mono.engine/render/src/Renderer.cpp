#include "DisplayColour.hpp"
#include "RenderTypes.hpp"
#include "RendererState.hpp"
#include "SurfaceScale.hpp"
#include "capture/ViewRecording.hpp"
#include "diagnostics/VulkanTimestamps.hpp"
#include "portal/PortalRendererTerminalObserver.hpp"
#include "presentation/FrameBatch.hpp"
#include "resources/MaterialSamplerAdmission.hpp"
#include "resources/ResourcePreview.hpp"
#include "resources/ShaderBinary.hpp"
#include "resources/StartupCompilation.hpp"

#include <engine/assets/Builtin.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/graph/Cull.hpp>
#include <engine/graph/EntityFlow.hpp>
#include <engine/graph/ExecutionPlan.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/graph/Schedule.hpp>
#include <engine/graph/Shadow.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/capture/DataFactoryHookBind.hpp>
#include <engine/render/pipeline/GraphRunner.hpp>
#include <engine/render/portal/PortalExchange.hpp>
#include <engine/render/resources/MeshTable.hpp>
#include <engine/render/resources/MissingTexture.hpp>
#include <engine/render/resources/ShaderCompiler.hpp>
#include <engine/render/resources/ShaderLibrary.hpp>
#include <engine/render/resources/TextureTable.hpp>
#include <engine/resources/Shaders.hpp>
#include <engine/scene/cameras/ActiveCamera.hpp>
#include <engine/scene/environment/Sunlight.hpp>

// `scene::ReflectCamera` and `scene::SurfacePane`: where a mirror's camera goes
// for a viewer that is not the eye, which is what makes the levels below the
// first a recursion rather than a repeat.
#include <engine/scene/Tagging.hpp>
#include <engine/scene/cameras/SurfaceCameras.hpp>

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_video.h>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <spirv-tools/libspirv.hpp>
#include <spirv_cross.hpp>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace engine::render {
	namespace {
		bool IsDepthFormat(graph::ResourceFormat format) {
			return format == graph::ResourceFormat::D24S8 || format == graph::ResourceFormat::D32F;
		}

		bool IsCompressedFormat(graph::ResourceFormat format) {
			return format == graph::ResourceFormat::BC1_SRGB || format == graph::ResourceFormat::BC3 ||
				   format == graph::ResourceFormat::BC5 || format == graph::ResourceFormat::BC7_SRGB;
		}

		DeviceCaps ProbeCapabilities(SDL_GPUDevice *device, const ShaderBinary &binary, bool hasTimestamps) {
			DeviceCaps caps;
			if (device == nullptr) {
				return caps;
			}

			// Compute and indexed indirect commands are baseline operations of an
			// SDL GPU device. Texture formats remain device-specific and are
			// queried below instead of inferred from the backend name.
			caps.HasCompute = true;
			caps.HasIndirectDraws = true;
			caps.HasTimestamps = hasTimestamps;
			caps.UnifiedQueue = true;
			caps.PrefersMSL = binary.Form == resources::ShaderForm::Msl;
			caps.MaxSamplersPerDraw = 10;
			// SDL exposes no portable maximum-colour-target query. The renderer
			// fills this only after its real eight-target G-buffer pipeline builds.
			// A format query alone does not prove simultaneous attachment support.

			const SDL_GPUTextureUsageFlags storageUsage = SDL_GPU_TEXTUREUSAGE_SAMPLER |
														  SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ |
														  SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
			caps.HasStorageTextures = SDL_GPUTextureSupportsFormat(
				device, DeviceFormat(graph::ResourceFormat::R32F), SDL_GPU_TEXTURETYPE_2D, storageUsage
			);

			for (uint8_t value = static_cast<uint8_t>(graph::ResourceFormat::R8);
				 value <= static_cast<uint8_t>(graph::ResourceFormat::BC7_SRGB);
				 value++) {
				const auto format = static_cast<graph::ResourceFormat>(value);
				SDL_GPUTextureUsageFlags usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
				if (IsDepthFormat(format)) {
					usage |= SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
				} else if (!IsCompressedFormat(format)) {
					usage |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
				}
				if (SDL_GPUTextureSupportsFormat(
						device, DeviceFormat(format), SDL_GPU_TEXTURETYPE_2D, usage
					)) {
					caps.Formats.push_back(format);
				}
			}
			return caps;
		}
	}

	void Renderer::Impl::CollectTimings() {
		for (uint32_t slot = 0; slot < VulkanTimestamps::SLOTS; slot++) {
			if (!Timestamps.Pending(slot)) {
				continue;
			}
			double times[VulkanTimestamps::MARKS]{};
			uint32_t count = 0;
			if (!Timestamps.Collect(slot, times, count)) {
				continue;
			}

			// **Published only when this slot is not older than what is already
			// on show.** Slots resolve out of order, and a stale one overwriting
			// a fresh one would make the panel walk backwards in time.
			const bool publish = TimingSequence[slot] >= ResolvedTimingSequence;
			if (publish) {
				GpuTimings.clear();
			}

			for (const PassMarks &marks : PendingMarks[slot]) {
				if (marks.Opened >= count || marks.Closed >= count) {
					continue;
				}
				// **Microseconds, which is what `PassTimings` documents itself to
				// report.** `VulkanTimestamps::Between` returns nanoseconds -
				// ticks already multiplied by the device's `timestampPeriod` -
				// so this divide is the one that fixes the unit, and the
				// frame graph below has to divide again because it takes
				// milliseconds. Getting that second divide wrong reads as a
				// renderer spending three seconds of GPU time in a third of a
				// second of wall clock, which is how it was caught.
				const double microseconds =
					VulkanTimestamps::Between(times, marks.Opened, marks.Closed) / 1000.0;

				if (publish) {
					GpuTimings[marks.Name.Id()] += microseconds;
				}

				// **Every slot, not just the published one, and that is the
				// whole accuracy argument.** `GpuTimings` is a snapshot for a
				// panel, so showing the newest and dropping a straggler is
				// right for it. A flamegraph is a *total* over a run, so a
				// dropped straggler is GPU time that happened and that the
				// graph will never account for - and a slot reported twice is
				// time that did not happen. This loop runs once per submitted
				// slot and each slot's marks are cleared below, so every
				// measurement reaches the graph exactly once.
				//
				// **The frame it lands in is not the frame it happened in.** A
				// query pool resolves a few frames late, so this is a duration
				// in a tree rather than a position on a timeline - which is
				// exactly what `FrameGraph::Report` documents a reported span to
				// be, and why the GPU has a category of its own instead of
				// inflating `Render`.
				//
				// **`gpu ssao` and not `ssao`.** `GraphRunner` already opens a
				// CPU scope named after the node, so an undecorated name would
				// put two spans with one label in every frame and the per-span
				// table would sum them - the exact CPU-versus-GPU confusion this
				// category exists to end, arriving through the label instead of
				// through the colour. `GpuSpanName` owns the decorated text, so
				// the span may borrow it and the overlay may read it after the
				// frame has ended.
				core::FrameGraph::Report(
					GpuSpanName(marks.Name),
					core::ProfileCategory::Gpu,
					static_cast<float>(microseconds / 1000.0)
				);
			}
			for (const CaptureTimingMarks &marks : PendingCaptureTimings[slot]) {
				auto found = CaptureTimings.find(marks.Id);
				if (found == CaptureTimings.end()) continue;
				CaptureTiming &capture = found->second;
				if (capture.State == CaptureTimingState::Unavailable) continue;
				if (marks.Opened >= count || marks.Closed >= count || marks.Closed <= marks.Opened) {
					capture.State = CaptureTimingState::Unavailable;
					capture.Reason = "unavailable/invalid_capture_gpu_timestamp";
					continue;
				}
				const double nanoseconds = VulkanTimestamps::Between(times, marks.Opened, marks.Closed);
				if (nanoseconds < 0.0 ||
					nanoseconds > static_cast<double>(UINT64_MAX) - capture.Nanoseconds) {
					capture.State = CaptureTimingState::Unavailable;
					capture.Reason = "unavailable/capture_gpu_timestamp_overflow";
					continue;
				}
				capture.Nanoseconds += static_cast<uint64_t>(nanoseconds);
				capture.State = CaptureTimingState::Ready;
				capture.Reason.clear();
			}

			if (publish) {
				ResolvedTimingSequence = TimingSequence[slot];
			}
			PendingMarks[slot].clear();
			PendingCaptureTimings[slot].clear();
			TimingSequence[slot] = 0;
		}
	}

	void Renderer::Impl::CollectPreview() {
		void *mapped = SDL_MapGPUTransferBuffer(Device, Preview.Transfer, false);
		if (mapped == nullptr) {
			ENGINE_ERROR("resource preview: SDL_MapGPUTransferBuffer: {}", SDL_GetError());
			return;
		}

		const size_t count = static_cast<size_t>(Preview.Width) * Preview.Height;
		const auto *bytes = static_cast<const uint8_t *>(mapped);
		Preview.Pixels.resize(count);
		if (Preview.BytesPerPixel == 4) {
			std::memcpy(Preview.Pixels.data(), bytes, count * sizeof(uint32_t));
		} else if (Preview.BytesPerPixel == 2) {
			for (size_t index = 0; index < count; index++) {
				Preview.Pixels[index] = static_cast<uint32_t>(bytes[index * 2]) |
										(static_cast<uint32_t>(bytes[index * 2 + 1]) << 8) | 0xFF000000u;
			}
		} else {
			for (size_t index = 0; index < count; index++) {
				const uint32_t value = bytes[index];
				Preview.Pixels[index] = value | (value << 8) | (value << 16) | 0xFF000000u;
			}
		}
		SDL_UnmapGPUTransferBuffer(Device, Preview.Transfer);
		Preview.Histogram =
			Preview.Rgba ? render::HistogramRgba(Preview.Pixels) : render::Histogram(Preview.Pixels);
	}

	void Renderer::Impl::PollPreview() {
		if (Preview.Fence == nullptr || !SDL_QueryGPUFence(Device, Preview.Fence)) {
			return;
		}
		SDL_ReleaseGPUFence(Device, Preview.Fence);
		Preview.Fence = nullptr;
		if (Preview.Pending.Poll(true)) {
			CollectPreview();
		}
	}

	bool Renderer::Impl::RequestPreview(
		SDL_GPUCommandBuffer *command,
		SDL_GPUTexture *texture,
		uint32_t width,
		uint32_t height,
		core::Name source,
		size_t slot,
		SDL_GPUTextureFormat format
	) {
		if (!Preview.Pending.CanRequest() || command == nullptr || texture == nullptr || width == 0 ||
			height == 0 || !source.IsValid()) {
			return false;
		}

		uint32_t bytesPerPixel = 0;
		bool rgba = true;
		switch (format) {
		case SDL_GPU_TEXTUREFORMAT_R8_UNORM:
			bytesPerPixel = 1;
			break;
		case SDL_GPU_TEXTUREFORMAT_R8G8_UNORM:
			bytesPerPixel = 2;
			break;
		case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM:
		case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB:
			bytesPerPixel = 4;
			break;
		case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM:
		case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB:
			bytesPerPixel = 4;
			rgba = false;
			break;
		default:
			return false;
		}

		const uint64_t bytes = static_cast<uint64_t>(width) * height * bytesPerPixel;
		if (bytes > std::numeric_limits<uint32_t>::max()) {
			ENGINE_ERROR("resource preview: {}x{} is too large for one transfer", width, height);
			return false;
		}
		if (Preview.Capacity < bytes) {
			if (Preview.Transfer != nullptr) {
				gpu::ReleaseTransferBuffer(Device, Preview.Transfer);
				Preview.Transfer = nullptr;
			}

			SDL_GPUTransferBufferCreateInfo info{};
			info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
			info.size = static_cast<uint32_t>(bytes);
			Preview.Transfer = gpu::CreateTransferBuffer(Device, &info);
			if (Preview.Transfer == nullptr) {
				ENGINE_ERROR("resource preview: SDL_CreateGPUTransferBuffer: {}", SDL_GetError());
				Preview.Capacity = 0;
				return false;
			}
			Preview.Capacity = static_cast<uint32_t>(bytes);
		}

		SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);
		if (copy == nullptr) {
			ENGINE_ERROR("resource preview: SDL_BeginGPUCopyPass: {}", SDL_GetError());
			return false;
		}
		SDL_GPUTextureRegion region{};
		region.texture = texture;
		region.w = width;
		region.h = height;
		region.d = 1;
		SDL_GPUTextureTransferInfo destination{};
		destination.transfer_buffer = Preview.Transfer;
		destination.pixels_per_row = width;
		destination.rows_per_layer = height;
		SDL_DownloadFromGPUTexture(copy, &region, &destination);
		SDL_EndGPUCopyPass(copy);

		Preview.Source = source;
		Preview.Slot = slot;
		Preview.Width = width;
		Preview.Height = height;
		Preview.BytesPerPixel = bytesPerPixel;
		Preview.Rgba = rgba;
		Preview.Pending.Submitted(FrameCounter);
		PreviewSubmitted = true;
		return true;
	}

	// -----------------------------------------------------------------------

	void FrameResult::Accumulate(const FrameResult &view) {
		Submitted = Submitted || view.Submitted;
		Presented = Presented || view.Presented;
		if (view.Presented) {
			SpatialDepth = view.SpatialDepth;
			SpatialDepthProjection = view.SpatialDepthProjection;
			SpatialSnapshotSequence = view.SpatialSnapshotSequence;
			SpatialSnapshot = view.SpatialSnapshot;
		}
		DrawCalls += view.DrawCalls;
		Triangles += view.Triangles;
		SurfaceInstances += view.SurfaceInstances;
		SurfacePasses += view.SurfacePasses;
		PortalPasses += view.PortalPasses;
		SurfaceBudgetExceeded = SurfaceBudgetExceeded || view.SurfaceBudgetExceeded;
		RibbonVertices += view.RibbonVertices;
		InstanceChunks += view.InstanceChunks;
		InstanceChunksDirty += view.InstanceChunksDirty;
		InstanceRows += view.InstanceRows;
		InstanceRowsDirty += view.InstanceRowsDirty;
		Particles += view.Particles;
		ParticlesDrawn += view.ParticlesDrawn;
		Culled += view.Culled;
		ScheduledReadBytes += view.ScheduledReadBytes;
		ScheduledWriteBytes += view.ScheduledWriteBytes;
		QueueTransferBytes += view.QueueTransferBytes;
		UploadedBytes += view.UploadedBytes;
		UploadCommandBuffers += view.UploadCommandBuffers;
		ComputeDispatches += view.ComputeDispatches;
		AsyncComputeCommandBuffers += view.AsyncComputeCommandBuffers;
		DownloadCommandBuffers += view.DownloadCommandBuffers;
		TrafficCommandBuffers += view.TrafficCommandBuffers;
		ConcurrentWaves += view.ConcurrentWaves;
		for (const core::Name node : view.Nodes) {
			if (!Ran(node)) {
				Nodes.push_back(node);
			}
		}
	}

	void Renderer::Inspect(core::Name resource, size_t slot) {
		RequireOwningThread("Inspect");
		State->Inspected = resource;
		State->InspectedSlot = slot;
	}

	core::Name Renderer::Inspecting() const {
		return State == nullptr ? core::Name{} : State->Inspected;
	}

	void Renderer::ObservePipeline(core::Name pipeline) {
		RequireOwningThread("ObservePipeline");
		if (State == nullptr || State->ObservedPipeline == pipeline) return;
		State->ObservedPipeline = pipeline;
		State->ObservedScratch = {};
		State->ObservedRun = {};
		State->Observer = {};
		if (!pipeline.IsValid()) return;
		const auto now = [] {
			return std::chrono::duration_cast<std::chrono::nanoseconds>(
					   std::chrono::steady_clock::now().time_since_epoch()
			)
				.count();
		};
		Impl *state = State.get();
		state->Observer.Hooks.PreNode = [state, now](const ang::NodeInfo &) {
			state->ObservedNodeStart = now();
		};
		state->Observer.Hooks.PostNode = [state, now](const ang::NodeInfo &info, ang::NodeStatus status) {
			// The engine executor names nodes `render.<authored name>`.
			constexpr std::string_view PREFIX = "render.";
			std::string_view name = info.Id;
			if (name.starts_with(PREFIX)) name.remove_prefix(PREFIX.size());
			const int64_t end = now();
			state->ObservedScratch.Nodes.push_back({
				.Node = core::Name(name),
				.Status = status,
				.BeginMilliseconds =
					static_cast<double>(state->ObservedNodeStart - state->ObservedStart) / 1.0e6,
				.Milliseconds = static_cast<double>(end - state->ObservedNodeStart) / 1.0e6,
			});
			state->ObservedScratch.Milliseconds = static_cast<double>(end - state->ObservedStart) / 1.0e6;
		};
	}

	const Renderer::PipelineRun &Renderer::ObservedRun() const {
		static const PipelineRun NONE;
		return State != nullptr ? State->ObservedRun : NONE;
	}

	graph::EngineObserver *Renderer::Impl::ObserverFor(core::Name pipeline) {
		if (!ObservedPipeline.IsValid() || pipeline != ObservedPipeline) return nullptr;
		if (ObservedFrame != FrameCounter || ObservedScratch.Nodes.empty()) {
			if (!ObservedScratch.Nodes.empty()) ObservedRun = std::move(ObservedScratch);
			ObservedScratch = {};
			ObservedScratch.Pipeline = pipeline;
			ObservedFrame = FrameCounter;
			ObservedStart = std::chrono::duration_cast<std::chrono::nanoseconds>(
								std::chrono::steady_clock::now().time_since_epoch()
			)
								.count();
		}
		return &Observer;
	}

	Renderer::ReadbackImage Renderer::Readback() const {
		ReadbackImage image;
		if (State == nullptr || !State->Preview.Pending.HasImage()) {
			return image;
		}
		image.Source = State->Preview.Source;
		image.Slot = State->Preview.Slot;
		image.Width = State->Preview.Width;
		image.Height = State->Preview.Height;
		image.Pixels = State->Preview.Pixels;
		image.Histogram = State->Preview.Histogram;
		image.Age = State->Preview.Pending.Age(State->FrameCounter);
		return image;
	}

	const std::unordered_map<uint32_t, double> &Renderer::PassTimings() const {
		return State->GpuTimings;
	}

	uint64_t Renderer::PassTimingSequence() const {
		return State->ResolvedTimingSequence;
	}

	const std::unordered_map<uint32_t, double> &Renderer::PassWallTimes() const {
		return State->WallTimings;
	}

	bool Renderer::Timed() const {
		return State != nullptr && State->Timestamps.Ready();
	}

	void Renderer::SetProfiling(ProfilingTier tier) {
		RequireOwningThread("SetProfiling");
		if (State->ProfileTier == tier) {
			return;
		}
		if (tier != ProfilingTier::Full) {
			for (uint32_t slot = 0; slot < VulkanTimestamps::SLOTS; slot++) {
				State->Timestamps.Abandon(slot);
			}
		}
		State->ProfileTier = tier;
		State->DroppedProfileMarks = 0;
	}

	ProfilingTier Renderer::Profiling() const {
		return State != nullptr ? State->ProfileTier : ProfilingTier::Off;
	}

	size_t Renderer::DroppedProfileMarks() const {
		return State != nullptr ? State->DroppedProfileMarks : 0;
	}

	const DeviceCaps &Renderer::Capabilities() const {
		static const DeviceCaps unavailable;
		return State != nullptr ? State->Caps : unavailable;
	}

	std::string_view Renderer::BackendName() const {
		return State->Backend;
	}

	GpuMemoryStatistics Renderer::MemoryStatistics() const {
		return State != nullptr ? gpu::MemoryStatistics(State->Device) : GpuMemoryStatistics{};
	}

	std::vector<AssetResidencyStatistics> Renderer::AssetResidencies() const {
		std::unordered_map<uint32_t, AssetResidencyStatistics> merged;
		if (State == nullptr) {
			return {};
		}

		for (const Impl::InstanceWorld &world : State->InstanceWorlds) {
			for (const AssetInstanceRows &rows : world.Instances.AssetRows()) {
				auto [found, inserted] = merged.try_emplace(rows.Mesh.Id());
				if (inserted) {
					found->second.Name = rows.Mesh;
				}
				found->second.ResidentInstances += rows.Resident;
				found->second.StagedInstances += rows.Staged;
			}
		}

		std::vector<AssetResidencyStatistics> result;
		result.reserve(merged.size());
		for (auto &[id, row] : merged) {
			(void)id;
			row.StagedBytes = static_cast<uint64_t>(row.StagedInstances) * sizeof(GpuInstance);
			result.push_back(row);
		}
		std::sort(
			result.begin(),
			result.end(),
			[](const AssetResidencyStatistics &left, const AssetResidencyStatistics &right) {
				return left.Name.Text() < right.Name.Text();
			}
		);
		return result;
	}

	bool Renderer::AppendMemoryReport(const std::filesystem::path &path) const {
		std::ofstream file(path, std::ios::app);
		if (!file) {
			return false;
		}

		const GpuMemoryStatistics memory = MemoryStatistics();
		file << "\ngpu logical heap\n";
		file << "  live      " << memory.LiveBytes << " bytes\n";
		file << "  peak      " << memory.PeakBytes << " bytes\n";
		file << "  allocated " << memory.AllocatedBytes << " bytes\n";
		file << "  released  " << memory.ReleasedBytes << " bytes\n";
		file << "  buffers   " << memory.BufferBytes << " bytes in " << memory.Buffers << " resources, "
			 << memory.BufferAllocations << " created\n";
		file << "  transfers " << memory.TransferBufferBytes << " bytes in " << memory.TransferBuffers
			 << " resources, " << memory.TransferBufferAllocations << " created\n";
		file << "  textures  " << memory.TextureBytes << " bytes in " << memory.Textures << " resources, "
			 << memory.TextureAllocations << " created\n";
		file
			<< "  coverage  requested payload; driver alignment, pipelines, samplers, shaders, and swapchain "
			   "images excluded\n";
		return file.good();
	}

	bool Renderer::SetVerticalSync(bool enabled) {
		if (!State->Device) {
			return false;
		}

		const SDL_GPUPresentMode mode = enabled ? SDL_GPU_PRESENTMODE_VSYNC : SDL_GPU_PRESENTMODE_IMMEDIATE;

		// VSYNC is the only mode required to exist. Asking for IMMEDIATE on a
		// backend without it fails rather than silently staying synchronised,
		// so check before asking for it - an unsupported mode would otherwise
		// leave the swapchain in whatever state the failed call left it.
		//
		// **The query is the half that is safe to do now**, which is what lets
		// this keep answering the caller straight away. It reads the surface's
		// capabilities and touches nothing; setting the mode is what recreates
		// the swapchain, and that is deferred.
		if (!SDL_WindowSupportsGPUPresentMode(State->Device, State->Window, mode)) {
			ENGINE_WARN("present mode unsupported on {}", State->Backend);
			return false;
		}

		// **Queued rather than set, and see `Impl::PendingPresentMode` for why.**
		// Setting it destroys and rebuilds every swapchain image, and this is
		// called from the middle of a frame that is already holding one of them.
		State->PendingPresentMode = mode;
		State->PresentModePending = true;
		return true;
	}

	uint64_t Renderer::Impl::GraphBufferBytes() const {
		uint64_t total = 0;
		for (const auto &buffer : GraphResources.Buffers)
			total += buffer.Bytes;
		return total;
	}
	uint64_t Renderer::Impl::ComputeBufferBytes() const {
		return (ComputeRegistry ? ComputeRegistry->ReservedDeviceBytes() : 0) +
			   GpuQueues.RetainedDeviceBytes();
	}
	script::ComputeBridgeFactory Renderer::ComputeForWorld(std::string world, uint32_t view) {
		RequireOwningThread("ComputeForWorld");
		if (!State->ComputeRegistry)
			State->ComputeRegistry = std::make_shared<ScriptComputeBridge>(State->Device);
		return State->ComputeRegistry->ForWorld(std::move(world), view);
	}
	void Renderer::PumpCompute() {
		RequireOwningThread("PumpCompute");
		if (State->Device) State->PollSceneFrames();
		if (State->ComputeRegistry)
			State->ComputeRegistry->Pump(State->GraphBufferBytes() + State->GpuQueues.RetainedDeviceBytes());
	}
	bool Renderer::WaitForFrame() {
		RequireOwningThread("WaitForFrame");
		PumpCompute();
		if (State->Device == nullptr) return false;
		return State->BeginFrame();
	}

	bool Renderer::PollForFrame() {
		RequireOwningThread("PollForFrame");
		PumpCompute();
		if (State->Device == nullptr) return false;
		State->PollAcquire = true;
		const bool claimed = State->BeginFrame();
		State->PollAcquire = false;
		return claimed;
	}

	bool Renderer::SetDecoupledDisplay(bool enabled) {
		if (!State->Device) return false;
		// Mailbox replaces a queued image instead of blocking, which is the
		// display taking only the newest frame. Vertical sync is the fallback
		// every backend has.
		SDL_GPUPresentMode mode = SDL_GPU_PRESENTMODE_VSYNC;
		if (enabled &&
			SDL_WindowSupportsGPUPresentMode(State->Device, State->Window, SDL_GPU_PRESENTMODE_MAILBOX))
			mode = SDL_GPU_PRESENTMODE_MAILBOX;
		State->PendingPresentMode = mode;
		State->PresentModePending = true;
		return true;
	}

	bool Renderer::PresentsLatestFrame() const {
		return State->Device != nullptr && State->PendingPresentMode == SDL_GPU_PRESENTMODE_MAILBOX;
	}

	bool Renderer::IsOnOwningThread() const {
		return Owner == std::this_thread::get_id();
	}

	void Renderer::RequireOwningThread(const char *what) const {
		if (IsOnOwningThread()) {
			return;
		}

		// Abort rather than return, for `ecs::Store::RequireOwningThread`'s
		// reason and one of this module's own. By the time a second thread is
		// inside here it has already recorded into a command buffer another
		// thread is filling, so there is nothing left to decline - and the
		// symptom on the far side is a driver validation error or a frame of
		// somebody else's geometry, neither of which points back here. The stack
		// at the violation is the whole value.
		ENGINE_ERROR(
			"renderer: {} called from a thread that does not own it. "
			"Passes share one command buffer and one device, so a frame is "
			"recorded by the thread that initialised the renderer and by no "
			"other. Draw viewports one after another.",
			what
		);
		std::abort();
	}

	bool Renderer::Initialise(
		SDL_Window *window,
		uint32_t framesInFlight,
		bool retainSourceTextures,
		size_t textureBudgetBytes,
		const StartupProgressCallback &progress
	) {
		ENGINE_PROFILE_CAT("renderer.initialise", engine::core::ProfileCategory::Render);
		// **Re-bound here, and the constructor's claim is what makes the check
		// testable without a device.** A renderer is legitimately constructed by
		// whoever owns the object and initialised by whoever owns the window -
		// it is the device, not the C++ object, that the contract is about - so
		// this is the authoritative claim and the constructor's is a default
		// that costs nothing to be wrong about, because being wrong about it
		// means nothing has been created yet.
		Owner = std::this_thread::get_id();
		static std::atomic<uint64_t> nextSpatialRendererIdentity{1};
		State->SpatialRendererIdentity = nextSpatialRendererIdentity.fetch_add(1, std::memory_order_relaxed);
		if (State->SpatialRendererIdentity == 0) std::abort();
		State->NextSpatialFrame = 1;
		const auto reportSetup = [&](std::string_view name) {
			if (!progress) return true;
			try {
				return progress({0, 0, name});
			} catch (...) {
				// Setup callbacks have the same cleanup guarantee as the worker pump.
				Shutdown();
				throw;
			}
		};
		if (!reportSetup("GPU device")) return false;

		// **A null window is headless and is not an error.** A renderer with
		// nowhere to present still draws: into a `SceneTarget`, which is what a
		// capture, a CI comparison and an automated editor all read. Refusing it
		// was right while an offscreen target did not exist and stopped being
		// right at v0.7.
		State->Window = window;
		State->RetainSourceTextures = retainSourceTextures;
		State->TextureBudgetBytes =
			textureBudgetBytes == 0 ? TextureTable::MAXIMUM_BYTES : textureBudgetBytes;
		State->StageProbe.Configure();

		// One backend on every platform. Apple packages MoltenVK beside the
		// executable, so it follows this same Vulkan and SPIR-V path.
#if defined(__APPLE__)
		SDL_SetHint(SDL_HINT_VULKAN_LIBRARY, "@executable_path/libMoltenVK.dylib");
#endif
		if (GpuQueueBatch::Requested()) {
			const auto properties = SDL_CreateProperties();
			SDL_SetStringProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING, "vulkan");
			SDL_SetBooleanProperty(
				properties,
				SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN,
				(SUPPORTED_SHADER_FORMATS & SDL_GPU_SHADERFORMAT_SPIRV) != 0
			);
			SDL_SetBooleanProperty(
				properties,
				SDL_PROP_GPU_DEVICE_CREATE_SHADERS_MSL_BOOLEAN,
				(SUPPORTED_SHADER_FORMATS & SDL_GPU_SHADERFORMAT_MSL) != 0
			);
			SDL_SetBooleanProperty(properties, ATOMIC_GPU_QUEUES_CREATE_PROPERTY, true);
			State->Device = SDL_CreateGPUDeviceWithProperties(properties);
			SDL_DestroyProperties(properties);
		} else
			State->Device = SDL_CreateGPUDevice(SUPPORTED_SHADER_FORMATS, false, "vulkan");
		if (!State->Device) {
			ENGINE_ERROR("SDL_CreateGPUDevice: {}", SDL_GetError());
			return false;
		}

		// Asked once, here, rather than assumed at each of the three places a
		// shader is created. `docs/DEFERRED.md` D00001's guess at what would
		// break first on a Mac was this line naming SPIR-V and nothing else.
		State->Binary = ShaderBinaryFor(State->Device);

		if (window != nullptr && !SDL_ClaimWindowForGPUDevice(State->Device, window)) {
			ENGINE_ERROR("SDL_ClaimWindowForGPUDevice: {}", SDL_GetError());
			Shutdown();
			return false;
		}

		// **One frame queued rather than SDL's two, and the frame rate is not
		// what this buys.** The default lets the CPU submit a second frame
		// before the GPU has finished the first, so what a display is showing is
		// up to two frames behind the input that produced it - 33 ms at 60 Hz
		// before the compositor takes its turn. SDL's own wording is that higher
		// values "increase throughput at the expense of visual latency", and an
		// editor is the case where that trade is backwards: nobody drags a
		// splitter to reach a frame rate, and every millisecond between the
		// mouse and the picture is felt by the hand holding it.
		//
		// What it costs is the throughput it was buying. A frame that would have
		// overlapped now waits, so a GPU-bound scene loses some of its rate -
		// which is why the measurement that matters here is the one taken by
		// hand, not the one the profiler reports.
		// **Clamped rather than trusted.** SDL takes 1 to 3 and answers false
		// for anything else, which would leave the device at its default with
		// only a warning to say so - a number nobody chose deciding the feel of
		// the editor.
		const uint32_t queued = std::clamp<uint32_t>(framesInFlight, 1, 3);
		State->AllowedFramesInFlight = queued;

		if (window != nullptr && !SDL_SetGPUAllowedFramesInFlight(State->Device, queued)) {
			// Not fatal. The default is a working configuration and the only
			// thing lost is the latency this was trying to save.
			ENGINE_WARN("SDL_SetGPUAllowedFramesInFlight: {}", SDL_GetError());
		}

		const char *driver = SDL_GetGPUDeviceDriver(State->Device);
		State->Backend = driver ? driver : "unknown";
		const bool hasTimestamps = State->Timestamps.Probe(State->Device);
		State->Caps = ProbeCapabilities(State->Device, State->Binary, hasTimestamps);
		if (State->GpuQueues.Initialise(State->Device)) {
			const auto *queues = State->GpuQueues.Api();
			const char *mode = std::getenv("MONO_GPU_QUEUES");
			State->Caps.UnifiedQueue =
				mode == nullptr || std::string_view(mode) != "parallel" ||
				(queues->Queue[ATOMIC_GPU_QUEUE_COMPUTE] == queues->Queue[ATOMIC_GPU_QUEUE_GRAPHICS] &&
				 queues->Queue[ATOMIC_GPU_QUEUE_TRANSFER] == queues->Queue[ATOMIC_GPU_QUEUE_GRAPHICS]);
		}
		for (InstalledNodeHandler &installed : CustomNodeHandlers) {
			if (installed.Lifecycle.Reinstall && !installed.Lifecycle.Reinstall(Backend())) {
				ENGINE_ERROR("custom render node '{}' failed to install", installed.Kind.Text());
				Shutdown();
				return false;
			}
			installed.Live = true;
		}

		// **Before the pipelines, because they name the format too.** A pipeline
		// built against one depth format and bound beside a texture in another
		// is a validation error at bind time. See `Impl::DepthFormat`.
		{
			// **Both usages, because one format serves both kinds of depth
			// texture.** The viewport's buffer is only ever a target, but the
			// shadow map is sampled as well - and a second format for the shadow
			// map would be a second thing that has to agree with the shadow
			// pipeline. Asking for the intersection once is cheaper than keeping
			// two in step.
			const auto supports = [&](SDL_GPUTextureFormat format) {
				return SDL_GPUTextureSupportsFormat(
					State->Device,
					format,
					SDL_GPU_TEXTURETYPE_2D,
					SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER
				);
			};

			// Most precision first. The shadow pass compares depths across a
			// whole scene, so the extra bits are worth asking for - and
			// D16_UNORM is the fallback rather than the preference because a
			// sixteen-bit shadow map stair-steps on a large world.
			if (supports(SDL_GPU_TEXTUREFORMAT_D32_FLOAT)) {
				State->DepthFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
			} else if (supports(SDL_GPU_TEXTUREFORMAT_D24_UNORM)) {
				State->DepthFormat = SDL_GPU_TEXTUREFORMAT_D24_UNORM;
			} else {
				// Guaranteed by SDL, so there is no third case to handle.
				State->DepthFormat = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
			}
		}

		SDL_GPUSamplerCreateInfo sampler{};
		// Nearest, because the overlay is pixel art at exactly one texel per
		// pixel. Linear would blur the 3x5 font into illegibility.
		sampler.min_filter = SDL_GPU_FILTER_NEAREST;
		sampler.mag_filter = SDL_GPU_FILTER_NEAREST;
		sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		State->OverlaySampler = SDL_CreateGPUSampler(State->Device, &sampler);

		bool pipelinesReady = false;
		try {
			StartupCompilation compilation;
			State->Compilation = progress ? &compilation : nullptr;
			// SDL GPU resource creation has no window affinity. All swapchain,
			// geometry recording and device teardown stay on the owner thread.
			const auto swapchainFormat = State->ColourFormat();
			pipelinesReady =
				compilation.Run([&] { return State->CreatePipelines(swapchainFormat); }, progress);
			State->Compilation = nullptr;
		} catch (...) {
			// Both worker and UI exceptions reach here only after the worker joined.
			State->Compilation = nullptr;
			Shutdown();
			throw;
		}
		if (!pipelinesReady || !reportSetup("GPU geometry") || !State->CreateGeometry()) {
			Shutdown();
			return false;
		}

		// The native G-buffer pipeline is the proof that this device accepts all
		// eight attachments at once. Choose after that probe so a backend that
		// rejects it starts with the forward graph instead of aborting startup.
		const PipelineTierDecision defaultTier = ChooseDefaultPipeline(State->Caps);
		for (const PipelineTierRejection &rejected : defaultTier.Fallthrough) {
			ENGINE_INFO(
				"{} engine default skipped: {}{}{}",
				Describe(rejected.Tier),
				Describe(rejected.Cause.Status),
				rejected.Cause.Status == CapabilityStatus::MissingFormat ? ": " : "",
				rejected.Cause.Status == CapabilityStatus::MissingFormat
					? graph::Describe(rejected.Cause.Format)
					: ""
			);
		}
		graph::PipelineDocument defaultDocument;
		switch (defaultTier.Tier) {
		case DefaultPipelineTier::A:
			defaultDocument = graph::DefaultPbrDocument();
			break;
		case DefaultPipelineTier::B:
			defaultDocument = graph::DefaultPbrTierBDocument();
			break;
		case DefaultPipelineTier::C:
			defaultDocument = graph::DefaultForwardTierCDocument();
			break;
		case DefaultPipelineTier::Unavailable:
			ENGINE_ERROR("renderer has no supported default pipeline tier");
			Shutdown();
			return false;
		}
		if (!reportSetup("Render graph") || !InstallEngineDefault(defaultDocument)) {
			Shutdown();
			return false;
		}

		ENGINE_INFO("renderer ready on {}", State->Backend);
		return true;
	}

	void Renderer::Shutdown() {
		// Checked here as well as in `Render`, because releasing a device while
		// another thread holds a command buffer against it is the same violation
		// arriving at the end of the frame instead of the middle.
		RequireOwningThread("Shutdown");
		if (State->ComputeRegistry) {
			State->ComputeRegistry->Shutdown();
			State->ComputeRegistry.reset();
		}
		State->VisibilityWorking.Invalidate();
		State->VisibilityCompleted = {};
		HookBind->Shutdown();

		// **A frame waited for and never drawn, which is what quitting during
		// the event pump produces.** The loop's usual shape makes this
		// unreachable - pump, simulate, present, and only then test whether to
		// stop - but "usual" is not a guarantee, and a swapchain image held past
		// the device's destruction is a crash inside the backend rather than an
		// error here. See `Impl::AbandonFrame` for why it submits.
		State->AbandonFrame();

		auto *device = State->Device;
		if (!device) {
			return;
		}
		State->StageProbe.Clear(device);
		const BackendHandles handles = Backend();
		for (auto installed = CustomNodeHandlers.rbegin(); installed != CustomNodeHandlers.rend();
			 installed++) {
			if (installed->Live && installed->Lifecycle.Release) {
				installed->Lifecycle.Release(handles);
			}
			installed->Live = false;
		}

		// Everything below is still referenced by frames that may not have
		// finished. Waiting once here is simpler and no slower than tracking
		// per-resource fences for a shutdown path.
		const bool idle = SDL_WaitForGPUIdle(device);
		if (idle) {
			for (auto &slot : State->SceneSlots) {
				if (!slot.SpatialDepth) continue;
				(void)slot.SpatialDepth->CancelAll();
				slot.SpatialDepth->Reap(device);
				(void)slot.SpatialDepth->AbortAll(device);
				(void)slot.SpatialDepth->Shutdown(device);
			}
			(void)State->SpatialDepthReads.Shutdown(device);
		}
		for (auto &capture : State->SceneCaptures) {
			if (capture.Transfer != nullptr) gpu::ReleaseTransferBuffer(device, capture.Transfer);
			capture = {};
		}

		State->GpuQueues.Shutdown();
		State->ImageGraphs.Shutdown(device, State->Textures);
		if (State->PortalTreeJob.Fence) {
			SDL_ReleaseGPUFence(device, State->PortalTreeJob.Fence);
			State->PortalTreeJob.Fence = nullptr;
		}
		State->PortalTreeJob.Submitted = false;
		CancelPortalCaptureTreeComposition(State->PortalTreeJob.Token);

		for (Impl::PendingSceneSubmission &submission : State->GraphResources.PendingSceneSubmissions) {
			if (submission.Fence != nullptr) {
				SDL_ReleaseGPUFence(device, submission.Fence);
			}
		}
		State->GraphResources.PendingSceneSubmissions.clear();
		State->DropStagedSceneFrames();
		for (auto &tree : State->ImportedPortalTrees)
			DropPortalCaptureTree(tree.Token);
		for (Impl::ResourceImageSlot &slot : State->GraphResources.Images) {
			State->ReleaseResidentImage(slot);
			gpu::ReleaseTransferBuffer(device, slot.Transfer);
			slot = {};
		}
		for (auto &image : State->ImportedPortals) {
#if ENGINE_ASSERTS_ENABLED
			const uint64_t released = image.Handle;
#endif
			State->ReleasePortalImport(image);
#if ENGINE_ASSERTS_ENABLED
			if (released != 0)
				test_support::ObservePortalRendererTerminal(
					{this, test_support::PortalRendererTerminalKind::ReleasePortalImport, released, true}
				);
#endif
		}
		for (auto &image : State->ImportedPortalShadows) {
			State->ReleasePortalShadow(image);
		}
		State->ReleaseResidentImageCache();
		gpu::ReleaseTransferBuffer(device, State->PortalImportStaging);
		State->PortalImportStaging = nullptr;
		State->PortalImportStagingBytes = 0;
		State->PortalImportUsage.StagingBytes = 0;
		State->ReportPortalImportUsage();
		State->Timestamps.Shutdown();
		if (State->Preview.Fence != nullptr) {
			SDL_ReleaseGPUFence(device, State->Preview.Fence);
			State->Preview.Fence = nullptr;
		}
		if (State->Preview.Transfer != nullptr) {
			gpu::ReleaseTransferBuffer(device, State->Preview.Transfer);
			State->Preview.Transfer = nullptr;
		}

		// **Before the pipelines they were derived from**, which costs nothing
		// and reads in the order the objects were built. `SDL_WaitForGPUIdle`
		// above is what makes releasing any of this safe.
		State->ReleaseShaderVariants();
		State->ReleaseAllGraphState();
		State->ReleaseLod();
		State->ReleaseOcclusion();
		State->ReleaseGpuParticleField();
		State->ReleaseWeatherPipelines();
		State->ReleaseParticlePool();
		State->ReleaseEnvironments();

		if (State->OpaquePipeline) {
			SDL_ReleaseGPUGraphicsPipeline(device, State->OpaquePipeline);
		}
		for (const auto &pending : State->DeferredPipelines)
			if (pending.TwoSided && pending.Pipeline)
				SDL_ReleaseGPUGraphicsPipeline(device, pending.Pipeline);
		State->DeferredPipelines.clear();
		if (State->ForwardPipeline) {
			SDL_ReleaseGPUGraphicsPipeline(device, State->ForwardPipeline);
		}
		if (State->WireframeOpaquePipeline) {
			SDL_ReleaseGPUGraphicsPipeline(device, State->WireframeOpaquePipeline);
		}
		if (State->WireframeTransparentPipeline) {
			SDL_ReleaseGPUGraphicsPipeline(device, State->WireframeTransparentPipeline);
		}
		if (State->WireframeGBufferPipeline) {
			SDL_ReleaseGPUGraphicsPipeline(device, State->WireframeGBufferPipeline);
		}
		for (SDL_GPUGraphicsPipeline *pipeline :
			 {State->PackedOpaquePipeline,
			  State->PackedForwardPipeline,
			  State->PackedTransparentPipeline,
			  State->PackedWireframeOpaquePipeline,
			  State->PackedWireframeTransparentPipeline,
			  State->PackedHdrOpaquePipeline,
			  State->PackedHdrTransparentPipeline,
			  State->PackedHdrWireframeOpaquePipeline,
			  State->PackedHdrWireframeTransparentPipeline,
			  State->PackedGBufferPipeline,
			  State->PackedGBufferTransparentPipeline,
			  State->PackedWireframeGBufferPipeline,
			  State->PackedDepthPeelPipeline,
			  State->PackedMeshShadowPipeline,
			  State->PackedTransparentLayerPipeline,
			  State->PackedTransparentLayerColourPipeline,
			  State->ParticleLayerPipeline,
			  State->ParticleLayerColourPipeline,
			  State->AdditiveParticleLayerColourPipeline,
			  State->RibbonLayerPipeline,
			  State->RibbonLayerColourPipeline,
			  State->AdditiveRibbonLayerColourPipeline,
			  State->ParticlePipeline,
			  State->AdditiveParticlePipeline,
			  State->RibbonPipeline,
			  State->AdditiveRibbonPipeline,
			  State->HdrParticlePipeline,
			  State->HdrAdditiveParticlePipeline,
			  State->HdrRibbonPipeline,
			  State->HdrAdditiveRibbonPipeline,
			  State->HdrOpaquePipeline,
			  State->HdrTransparentPipeline,
			  State->GridPipeline,
			  State->HdrGridPipeline,
			  State->HdrGridCompositePipeline,
			  State->HdrWireframeOpaquePipeline,
			  State->HdrWireframeTransparentPipeline,
			  State->GBufferPipeline,
			  State->GBufferTransparentPipeline,
			  State->DepthPeelPipeline,
			  State->DepthLinearPipeline,
			  State->DepthValidityPipeline,
			  State->CameraMotionPipeline,
			  State->DepthComposePipeline,
			  State->ColourComposePipeline,
			  State->AmbientResponsePipeline,
			  State->DeferredLightingBaselinePipeline,
			  State->DeferredLightingDirectionalPipeline,
			  State->DeferredLocalLightAccumulationPipeline,
			  State->AmbientMergePipeline,
			  State->AmbientCorrectPipeline,
			  State->DirectionalCorrectPipeline,
			  State->TransparentLayerPipeline,
			  State->TransparentLayerColourPipeline,
			  State->InterfaceLayerPipeline,
			  State->InterfaceLayerColourPipeline,
			  State->SsaoPipeline,
			  State->SsaoUpsamplePipeline,
			  State->PackChannelsPipeline,
			  State->DeferredLightingPipeline,
			  State->SkyPipeline,
			  State->WeatherFallbackPipeline,
			  State->VolumePipeline,
			  State->BloomPipeline,
			  State->DepthOfFieldPipeline,
			  State->GodRaysPipeline,
			  State->TonemapPipeline}) {
			if (pipeline != nullptr) {
				SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
			}
		}
		State->ParticlePipeline = nullptr;
		State->AdditiveParticlePipeline = nullptr;
		State->RibbonPipeline = nullptr;
		State->AdditiveRibbonPipeline = nullptr;
		for (const auto &[owner, variant] : State->PostProcessPipelines) {
			(void)owner;
			SDL_ReleaseGPUGraphicsPipeline(device, variant.Pipeline);
		}
		State->PostProcessPipelines.clear();
		for (const auto &[name, variant] : State->LensPipelines) {
			(void)name;
			if (variant.Pipeline != nullptr) {
				SDL_ReleaseGPUGraphicsPipeline(device, variant.Pipeline);
			}
		}
		State->LensPipelines.clear();
		for (const auto &group : State->PortalLensPrograms)
			for (const auto &binding : group.Bindings)
				SDL_ReleaseGPUGraphicsPipeline(State->Device, binding.Pipeline);
		State->PortalLensPrograms = {};
		for (auto &refusals : State->ShaderRefusals)
			refusals.clear();
		if (State->TransparentPipeline) {
			SDL_ReleaseGPUGraphicsPipeline(device, State->TransparentPipeline);
		}
		if (State->EnvironmentSkyCompute != nullptr) {
			SDL_ReleaseGPUComputePipeline(device, State->EnvironmentSkyCompute);
			State->EnvironmentSkyCompute = nullptr;
		}
		if (State->EnvironmentCloudCompute != nullptr) {
			SDL_ReleaseGPUComputePipeline(device, State->EnvironmentCloudCompute);
			State->EnvironmentCloudCompute = nullptr;
		}
		if (State->ShadowPipeline) {
			SDL_ReleaseGPUGraphicsPipeline(device, State->ShadowPipeline);
		}
		if (State->PackedShadowPipeline) {
			SDL_ReleaseGPUGraphicsPipeline(device, State->PackedShadowPipeline);
		}
		if (State->ShadowTexture) {
			gpu::ReleaseTexture(device, State->ShadowTexture);
		}
		if (State->BeamTexture) {
			gpu::ReleaseTexture(device, State->BeamTexture);
		}
		if (State->ShadowSampler) {
			SDL_ReleaseGPUSampler(device, State->ShadowSampler);
		}
		for (Impl::SurfaceBank &bank : State->SurfaceBanks) {
			ReleaseWeatherCapture(device, bank.WeatherCapture);
			for (Impl::SurfaceSlotState &surface : bank.Surfaces) {
				for (SDL_GPUTexture *texture : surface.Texture) {
					if (texture) {
						gpu::ReleaseTexture(device, texture);
					}
				}
				if (surface.Depth) {
					gpu::ReleaseTexture(device, surface.Depth);
				}
			}

			for (Impl::PortalLevel &level : bank.Portals) {
				for (Impl::PortalTarget &target : level.Targets) {
					if (target.Colour) {
						gpu::ReleaseTexture(device, target.Colour);
					}
					if (target.Display) {
						gpu::ReleaseTexture(device, target.Display);
					}
					if (target.Depth) {
						gpu::ReleaseTexture(device, target.Depth);
					}
				}
			}

			for (Impl::MirrorLevel &level : bank.Mirrors) {
				for (Impl::MirrorTarget &target : level.Targets) {
					if (target.Colour) {
						gpu::ReleaseTexture(device, target.Colour);
					}
					if (target.Depth) {
						gpu::ReleaseTexture(device, target.Depth);
					}
				}
			}

			for (Impl::SeamLightTarget &target : bank.SeamLights) {
				if (target.Colour && !target.Borrowed) {
					gpu::ReleaseTexture(device, target.Colour);
				}
				if (target.Depth) {
					gpu::ReleaseTexture(device, target.Depth);
				}
			}
		}
		State->SurfaceBanks.clear();
		if (State->SurfaceSampler) {
			SDL_ReleaseGPUSampler(device, State->SurfaceSampler);
		}
		for (Impl::SceneSlot &slot : State->SceneSlots) {
			for (Impl::SceneSlot::RetainedFrame &frame : slot.Retained) {
				if (frame.Texture != nullptr) {
					gpu::ReleaseTexture(device, frame.Texture);
					frame.Texture = nullptr;
				}
			}
			if (slot.Texture) {
				gpu::ReleaseTexture(device, slot.Texture);
				slot.Texture = nullptr;
			}
			if (slot.Depth) {
				gpu::ReleaseTexture(device, slot.Depth);
				slot.Depth = nullptr;
			}
			if (slot.History) {
				gpu::ReleaseTexture(device, slot.History);
				slot.History = nullptr;
			}
			for (Impl::SceneSlot::InstanceIndexVersion &indices : slot.InstanceIndexVersions) {
				if (indices.Buffer != nullptr) {
					gpu::ReleaseBuffer(device, indices.Buffer);
				}
				if (indices.Transfer != nullptr) {
					gpu::ReleaseTransferBuffer(device, indices.Transfer);
				}
			}
			for (SDL_GPUBuffer *buffer : {slot.SkinOffsetBuffer, slot.JointBuffer}) {
				if (buffer != nullptr) {
					gpu::ReleaseBuffer(device, buffer);
				}
			}
			for (SDL_GPUTransferBuffer *transfer : {slot.SkinOffsetTransfer, slot.JointTransfer}) {
				if (transfer != nullptr) {
					gpu::ReleaseTransferBuffer(device, transfer);
				}
			}
		}
		if (State->InstancePrefixBuffer != nullptr) {
			gpu::ReleaseBuffer(device, State->InstancePrefixBuffer);
		}
		if (State->EmptyStorageBuffer != nullptr) {
			gpu::ReleaseBuffer(device, State->EmptyStorageBuffer);
			State->EmptyStorageBuffer = nullptr;
		}
		for (Impl::InstanceWorld &world : State->InstanceWorlds) {
			if (world.Buffer != nullptr) {
				gpu::ReleaseBuffer(device, world.Buffer);
			}
			if (world.Transfer != nullptr) {
				gpu::ReleaseTransferBuffer(device, world.Transfer);
			}
		}
		for (Impl::PbrSlot &slot : State->PbrSlots) {
			State->ReleasePbr(slot);
		}
		State->PbrSlots.clear();
		for (Impl::ResourcePreviewTarget &preview : State->GraphResources.Previews) {
			for (SDL_GPUTexture *texture : preview.Textures) {
				if (texture != nullptr) {
					gpu::ReleaseTexture(device, texture);
				}
			}
		}
		State->GraphResources.Previews.clear();
		if (State->WindowCaptureTexture != nullptr) {
			gpu::ReleaseTexture(device, State->WindowCaptureTexture);
			State->WindowCaptureTexture = nullptr;
		}

		// Anything a resize retired and no frame came along to free. Shutting
		// down is the one path where the next frame never arrives, so leaving
		// this to `DrainRetiredScenes` would leak a texture per resize on exit.
		State->DrainRetiredScenes();

		if (State->ImagePipeline) {
			SDL_ReleaseGPUGraphicsPipeline(device, State->ImagePipeline);
		}
		if (State->OverlayPipeline) {
			SDL_ReleaseGPUGraphicsPipeline(device, State->OverlayPipeline);
		}
		State->Meshes.Shutdown();
		State->Textures.Shutdown();
		if (State->DepthTexture) {
			gpu::ReleaseTexture(device, State->DepthTexture);
		}
		if (State->FallbackTexture) {
			gpu::ReleaseTexture(device, State->FallbackTexture);
		}
		if (State->OverlayTexture) {
			gpu::ReleaseTexture(device, State->OverlayTexture);
		}
		if (State->OverlayTransfer) {
			gpu::ReleaseTransferBuffer(device, State->OverlayTransfer);
		}
		if (State->CloudDensityBuffer) {
			gpu::ReleaseBuffer(device, State->CloudDensityBuffer);
		}
		if (State->CloudDensityTransfer) {
			gpu::ReleaseTransferBuffer(device, State->CloudDensityTransfer);
		}
		if (State->OverlaySampler) {
			SDL_ReleaseGPUSampler(device, State->OverlaySampler);
		}

		if (State->Window) {
			SDL_ReleaseWindowFromGPUDevice(device, State->Window);
		}
		State->ReleaseHeadlessFrames();
		State->GpuQueues.ReleaseForDeviceDestruction();
		gpu::ForgetDevice(device);
		for (auto *shader : State->PipelineShaders)
			SDL_ReleaseGPUShader(device, shader);
		State->PipelineShaders.clear();
		SDL_DestroyGPUDevice(device);

		// **Rebuilt in place rather than assigned from a fresh one.** The two
		// tables own device resources and are deliberately not copyable, so
		// `*State = Impl{}` no longer compiles - and it should not: an
		// assignment would have released their buffers a second time, after
		// `Shutdown` above already did.
		const ProfilingTier profileTier = State->ProfileTier;
		const uint32_t profileSampleRate = State->ProfileSampleRate;
		State = std::make_unique<Impl>();
		State->ProfileTier = profileTier;
		State->ProfileSampleRate = profileSampleRate;
	}

	void Renderer::ForgetWorld(uint64_t world, core::Name name) {
		if (State == nullptr || State->Device == nullptr) {
			return;
		}
		RequireOwningThread("ForgetWorld");
		for (auto &slot : State->SceneSlots) {
			if (slot.ForwardWorking.World == world) slot.ForwardWorking = {};
			if (slot.ForwardWorld != world) continue;
			slot.ForwardedTextures.clear();
			slot.ForwardPipeline = {};
			slot.Activity = {};
		}
		SDL_WaitForGPUIdle(State->Device);
		State->ForgetWeatherWorld(world);
		for (auto &tree : State->ImportedPortalTrees)
			if (!tree.Nodes.empty() && tree.Nodes.front().Binding.World == world &&
				tree.Nodes.front().Binding.WorldName == name)
				DropPortalCaptureTree(tree.Token);
		for (auto &image : State->ImportedPortals) {
			if (image.Binding.World == world && image.Binding.WorldName == name) {
#if ENGINE_ASSERTS_ENABLED
				const uint64_t released = image.Handle;
#endif
				State->ReleasePortalImport(image);
#if ENGINE_ASSERTS_ENABLED
				if (released != 0)
					test_support::ObservePortalRendererTerminal(
						{this, test_support::PortalRendererTerminalKind::ReleasePortalImport, released, true}
					);
#endif
			}
		}
		for (auto &image : State->ImportedPortalShadows) {
			if (image.Binding.World == world && image.Binding.WorldName == name) {
				State->ReleasePortalShadow(image);
			}
		}
		// World-scoped graph resources use the world id as their owner. Retire
		// them at the same idle boundary as resident instance and particle rows;
		// otherwise each discarded world leaves its history images on the device.
		const auto graphWorld = State->GraphResources.WorldNames.find(world);
		if (graphWorld != State->GraphResources.WorldNames.end() && graphWorld->second == name) {
			std::erase_if(State->GraphResources.PendingHistoryWrites, [world](const auto &write) {
				return write.Scope == graph::NodeScope::World && write.Owner == world;
			});
			std::erase_if(State->GraphResources.Targets, [this, world](const auto &target) {
				if (target.Scope != graph::NodeScope::World || target.Owner != world) return false;
				if (target.Texture != nullptr) gpu::ReleaseTexture(State->Device, target.Texture);
				return true;
			});
			std::erase_if(State->GraphResources.Buffers, [this, world](const auto &buffer) {
				if (buffer.Scope != graph::NodeScope::World || buffer.Owner != world) return false;
				if (buffer.Buffer != nullptr) gpu::ReleaseBuffer(State->Device, buffer.Buffer);
				return true;
			});
			State->GraphResources.WorldNames.erase(graphWorld);
		}

		const auto sameWorld = [world, name](const auto &resident) {
			return resident.Id == world && resident.Name == name;
		};
		for (Impl::InstanceWorld &resident : State->InstanceWorlds) {
			if (!sameWorld(resident)) {
				continue;
			}
			if (resident.Buffer != nullptr) {
				gpu::ReleaseBuffer(State->Device, resident.Buffer);
				resident.Buffer = nullptr;
			}
			if (resident.Transfer != nullptr) {
				gpu::ReleaseTransferBuffer(State->Device, resident.Transfer);
				resident.Transfer = nullptr;
			}
		}
		for (Impl::ParticleWorld &resident : State->ParticleWorlds) {
			if (!sameWorld(resident)) {
				continue;
			}
			Impl::ParticlePool &pool = resident.Pool;
			for (SDL_GPUBuffer **buffer :
				 {&resident.Buffer,
				  &pool.States,
				  &pool.Work,
				  &pool.EmitWork,
				  &pool.Params,
				  &pool.Curves,
				  &pool.EmitterRuntime,
				  &pool.ParamUpdateBuffer,
				  &pool.CurveUpdateBuffer,
				  &pool.Seams,
				  &pool.Timeline}) {
				if (*buffer != nullptr) {
					gpu::ReleaseBuffer(State->Device, *buffer);
					*buffer = nullptr;
				}
			}
			for (SDL_GPUTransferBuffer **staging :
				 {&pool.StateStaging,
				  &pool.WorkStaging,
				  &pool.EmitWorkStaging,
				  &pool.ParamStaging,
				  &pool.CurveStaging,
				  &pool.SeamStaging,
				  &pool.EmitterRuntimeStaging}) {
				if (*staging != nullptr) {
					gpu::ReleaseTransferBuffer(State->Device, *staging);
					*staging = nullptr;
				}
			}
		}
		for (Impl::GpuParticleFieldWorld &resident : State->GpuParticleFieldWorlds) {
			if (!sameWorld(resident)) continue;
			if (resident.States != nullptr) gpu::ReleaseBuffer(State->Device, resident.States);
			if (resident.Samples != nullptr) gpu::ReleaseBuffer(State->Device, resident.Samples);
			if (resident.SampleTransfer != nullptr)
				gpu::ReleaseTransferBuffer(State->Device, resident.SampleTransfer);
		}
		State->GpuParticleFieldWorlds.erase(
			std::remove_if(
				State->GpuParticleFieldWorlds.begin(), State->GpuParticleFieldWorlds.end(), sameWorld
			),
			State->GpuParticleFieldWorlds.end()
		);
		State->ActiveGpuParticleFieldWorld = nullptr;
		State->PendingInstanceUploads.erase(
			std::remove_if(
				State->PendingInstanceUploads.begin(), State->PendingInstanceUploads.end(), sameWorld
			),
			State->PendingInstanceUploads.end()
		);
		State->InstanceWorlds.erase(
			std::remove_if(State->InstanceWorlds.begin(), State->InstanceWorlds.end(), sameWorld),
			State->InstanceWorlds.end()
		);
		State->ParticleWorlds.erase(
			std::remove_if(State->ParticleWorlds.begin(), State->ParticleWorlds.end(), sameWorld),
			State->ParticleWorlds.end()
		);
		State->ActiveInstanceWorld = nullptr;
		State->ActiveParticleWorld = nullptr;
	}
}
