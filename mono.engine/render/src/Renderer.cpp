#include "DisplayColour.hpp"
#include "RenderTypes.hpp"
#include "RendererState.hpp"
#include "ResourcePreview.hpp"
#include "ShaderBinary.hpp"
#include "SurfaceScale.hpp"
#include "ViewRecording.hpp"
#include "VulkanTimestamps.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/graph/Cull.hpp>
#include <engine/graph/EntityFlow.hpp>
#include <engine/graph/ExecutionPlan.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/graph/Schedule.hpp>
#include <engine/graph/Shadow.hpp>
#include <engine/render/GraphRunner.hpp>
#include <engine/render/MeshTable.hpp>
#include <engine/render/MissingTexture.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/ShaderCompiler.hpp>
#include <engine/render/TextureTable.hpp>
#include <engine/resources/Shaders.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Sunlight.hpp>

// `scene::ReflectCamera` and `scene::SurfacePane`: where a mirror's camera goes
// for a viewer that is not the eye, which is what makes the levels below the
// first a recursion rather than a repeat.
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/scene/Tagging.hpp>

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_video.h>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
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
			caps.MaxColourTargets = 4;

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

			if (publish) {
				ResolvedTimingSequence = TimingSequence[slot];
			}
			PendingMarks[slot].clear();
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
		Presented = Presented || view.Presented;
		DrawCalls += view.DrawCalls;
		Triangles += view.Triangles;
		SurfaceInstances += view.SurfaceInstances;
		SurfacePasses += view.SurfacePasses;
		PortalPasses += view.PortalPasses;
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

	bool Renderer::WaitForFrame() {
		RequireOwningThread("WaitForFrame");

		if (State->Device == nullptr) {
			return false;
		}

		return State->BeginFrame();
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

	bool Renderer::Initialise(SDL_Window *window, uint32_t framesInFlight) {
		// **Re-bound here, and the constructor's claim is what makes the check
		// testable without a device.** A renderer is legitimately constructed by
		// whoever owns the object and initialised by whoever owns the window -
		// it is the device, not the C++ object, that the contract is about - so
		// this is the authoritative claim and the constructor's is a default
		// that costs nothing to be wrong about, because being wrong about it
		// means nothing has been created yet.
		Owner = std::this_thread::get_id();

		// **A null window is headless and is not an error.** A renderer with
		// nowhere to present still draws: into a `SceneTarget`, which is what a
		// capture, a CI comparison and an automated editor all read. Refusing it
		// was right while an offscreen target did not exist and stopped being
		// right at v0.7.
		State->Window = window;

		// One backend on every platform. Apple packages MoltenVK beside the
		// executable, so it follows this same Vulkan and SPIR-V path.
#if defined(__APPLE__)
		SDL_SetHint(SDL_HINT_VULKAN_LIBRARY, "@executable_path/libMoltenVK.dylib");
#endif
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

		if (window != nullptr && !SDL_SetGPUAllowedFramesInFlight(State->Device, queued)) {
			// Not fatal. The default is a working configuration and the only
			// thing lost is the latency this was trying to save.
			ENGINE_WARN("SDL_SetGPUAllowedFramesInFlight: {}", SDL_GetError());
		}

		const char *driver = SDL_GetGPUDeviceDriver(State->Device);
		State->Backend = driver ? driver : "unknown";
		const bool hasTimestamps = State->Timestamps.Probe(State->Device);
		State->Caps = ProbeCapabilities(State->Device, State->Binary, hasTimestamps);
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
		if (!InstallEngineDefault(defaultDocument)) {
			Shutdown();
			return false;
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

		if (!State->CreatePipelines() || !State->CreateGeometry()) {
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
		SDL_WaitForGPUIdle(device);
		for (Impl::PendingSceneSubmission &submission : State->PendingSceneSubmissions) {
			if (submission.Fence != nullptr) {
				SDL_ReleaseGPUFence(device, submission.Fence);
			}
		}
		State->PendingSceneSubmissions.clear();
		State->DropStagedSceneFrames();
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
		State->ReleaseOcclusion();
		State->ReleaseParticlePool();
		State->ReleaseEnvironments();

		if (State->OpaquePipeline) {
			SDL_ReleaseGPUGraphicsPipeline(device, State->OpaquePipeline);
		}
		if (State->ForwardPipeline) {
			SDL_ReleaseGPUGraphicsPipeline(device, State->ForwardPipeline);
		}
		if (State->WireframeOpaquePipeline) {
			SDL_ReleaseGPUGraphicsPipeline(device, State->WireframeOpaquePipeline);
		}
		if (State->WireframeTransparentPipeline) {
			SDL_ReleaseGPUGraphicsPipeline(device, State->WireframeTransparentPipeline);
		}
		for (SDL_GPUGraphicsPipeline *pipeline :
			 {State->GBufferPipeline,
			  State->DepthLinearPipeline,
			  State->SsaoPipeline,
			  State->DeferredLightingPipeline,
			  State->SkyPipeline,
			  State->TonemapPipeline,
			  State->PostProcessPipeline}) {
			if (pipeline != nullptr) {
				SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
			}
		}
		State->PostProcessPipeline = nullptr;
		State->PostProcessShaderName = core::Name{};
		if (State->TransparentPipeline) {
			SDL_ReleaseGPUGraphicsPipeline(device, State->TransparentPipeline);
		}
		if (State->EnvironmentCompute != nullptr) {
			SDL_ReleaseGPUComputePipeline(device, State->EnvironmentCompute);
			State->EnvironmentCompute = nullptr;
		}
		if (State->ShadowPipeline) {
			SDL_ReleaseGPUGraphicsPipeline(device, State->ShadowPipeline);
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
				if (target.Colour) {
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
		for (Impl::ResourcePreviewTarget &preview : State->ResourcePreviews) {
			for (SDL_GPUTexture *texture : preview.Textures) {
				if (texture != nullptr) {
					gpu::ReleaseTexture(device, texture);
				}
			}
		}
		State->ResourcePreviews.clear();
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
		if (State->OverlaySampler) {
			SDL_ReleaseGPUSampler(device, State->OverlaySampler);
		}

		if (State->Window) {
			SDL_ReleaseWindowFromGPUDevice(device, State->Window);
		}
		gpu::ForgetDevice(device);
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
		SDL_WaitForGPUIdle(State->Device);

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
				  &pool.Params,
				  &pool.Curves,
				  &pool.EmitterRuntime,
				  &pool.ParamUpdateBuffer,
				  &pool.CurveUpdateBuffer,
				  &pool.Seams}) {
				if (*buffer != nullptr) {
					gpu::ReleaseBuffer(State->Device, *buffer);
					*buffer = nullptr;
				}
			}
			for (SDL_GPUTransferBuffer **staging :
				 {&pool.StateStaging,
				  &pool.WorkStaging,
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

	bool Renderer::MeshExtentOf(const core::Name &name, core::Vector3 &out) const {
		if (State == nullptr || !State->Meshes.Has(name)) {
			return false;
		}
		out = State->Meshes.Resolve(name).Extent;
		return true;
	}

	bool Renderer::AddMesh(const core::Name &name, const assets::MeshData &mesh) {
		if (State == nullptr || State->Device == nullptr) {
			return false;
		}

		// **Accumulated here and uploaded by `Render`**, which is what makes a
		// burst of arrivals cost one transfer. It used to upload on the spot,
		// and because a copy pass cannot write part of a cycled buffer that
		// meant re-sending the whole table per mesh: admitting N meshes moved
		// O(N^2) bytes over the bus and froze the frame that a game finished
		// loading in. The entry is registered immediately either way, so
		// `MeshExtentOf` and the parts waiting to be sized by it are unaffected.
		//
		// A caller that needs the geometry resident before the next `Render` -
		// a readback, a preview taken outside the frame loop - calls
		// `FlushMeshes` itself.
		return State->Meshes.Add(name, mesh);
	}

	bool Renderer::FlushMeshes() {
		if (State == nullptr || State->Device == nullptr) {
			return false;
		}
		return State->Meshes.Flush();
	}

	bool Renderer::AddTexture(const core::Name &name, const assets::TextureData &image) {
		if (State == nullptr || State->Device == nullptr) {
			return false;
		}
		return State->Textures.Add(name, image);
	}

	void Renderer::ExpectTexture(const core::Name &name) {
		if (State != nullptr) {
			State->Textures.Expect(name);
		}
	}

	void Renderer::StopExpectingTexture(const core::Name &name) {
		if (State != nullptr) {
			State->Textures.StopExpecting(name);
		}
	}

	bool Renderer::ExpectingTexture(const core::Name &name) const {
		return State != nullptr && State->Textures.Expecting(name);
	}

	void Renderer::SetAnimationTime(double seconds) {
		if (State != nullptr) {
			State->AnimationSeconds = seconds;
		}
	}

	void Renderer::SetSurfaceBounces(uint32_t bounces) {
		if (State != nullptr) {
			// **Zero is kept rather than floored, and that is the change.** It
			// used to mean "no surface pass at all", which nobody wanted and
			// which has a clearer spelling; it is
			// `scene::AUTOMATIC_SURFACE_BOUNCES` now, and it is the default. A
			// stated number still floors at one for the original reason.
			//
			// **Capped**, which it did not need to be while this counted
			// iterations of one pass. It counts levels of a recursion - see
			// `MAX_SURFACE_DEPTH` for why that turns a large number from
			// wasteful into unfinishable.
			State->SurfaceBounces = bounces == 0 ? 0u : std::clamp<uint32_t>(bounces, 1u, MAX_SURFACE_DEPTH);
		}
	}

	uint32_t Renderer::SurfaceBounces() const {
		return State == nullptr ? 0u : State->SurfaceBounces;
	}

	void Renderer::SetSurfaceLimit(uint32_t panes) {
		if (State != nullptr) {
			// **Clamped here rather than at every use.** `scene::MAX_SURFACES`
			// is how far the per-viewport slot arrays reach, and a world may
			// state a larger number - `scene::SurfaceLimit` says in as many
			// words that the ceiling belongs to whoever draws. Zero is kept: a
			// world that wants its mirrors off has to be able to say so.
			State->SurfaceLimit = std::min<uint32_t>(panes, scene::MAX_SURFACES);
		}
	}

	uint32_t Renderer::SurfaceLimit() const {
		return State == nullptr ? static_cast<uint32_t>(scene::DEFAULT_SURFACE_LIMIT) : State->SurfaceLimit;
	}

	void Renderer::SetWireframe(bool enabled) {
		if (State != nullptr) {
			State->WireframeMode = enabled;
		}
	}

	bool Renderer::Wireframe() const {
		return State != nullptr && State->WireframeMode;
	}

	void Renderer::SetUntextured(bool enabled) {
		if (State != nullptr) {
			State->UntexturedMode = enabled;
		}
	}

	bool Renderer::Untextured() const {
		return State != nullptr && State->UntexturedMode;
	}

	void Renderer::SetPortalDepth(uint32_t depth) {
		if (State != nullptr) {
			// **Clamped rather than floored**, which is the opposite of the
			// bounce knob above and is right for the opposite reason: zero here
			// is a meaningful setting - every hole draws flat and nothing
			// recurses - while the ceiling exists because the pool is full-screen
			// targets per level per slot.
			State->PortalDepth = std::min(depth, MAX_PORTAL_DEPTH);
		}
	}

	void Renderer::SetSun(
		const core::Vector3 &direction, const core::Color3 &ambient, const core::Color3 &direct
	) {
		if (State == nullptr) {
			return;
		}

		// **Normalised by the caller and checked here.** `scene::SunOf` already
		// does it, and a direction of zero arriving from anywhere else would
		// shade every surface by `dot(n, 0)` - a world that goes flat grey with
		// nothing in the log to say why.
		const float length = direction.Magnitude();
		if (length > 0.0f) {
			State->Sun = glm::vec3{direction.X / length, direction.Y / length, direction.Z / length};
		}

		State->Ambient = glm::vec4{ambient.R, ambient.G, ambient.B, 1.0f};
		State->Direct = glm::vec4{direct.R, direct.G, direct.B, 1.0f};
	}

	void Renderer::SetLighting(const scene::WorldLighting &lighting) {
		if (State == nullptr) {
			return;
		}

		SetSun(lighting.Direction, lighting.Ambient, lighting.Direct);
		State->OutdoorAmbient = glm::vec4{
			lighting.OutdoorAmbient.R,
			lighting.OutdoorAmbient.G,
			lighting.OutdoorAmbient.B,
			1.0f,
		};
		State->FogColour = glm::vec4{lighting.FogColor.R, lighting.FogColor.G, lighting.FogColor.B, 1.0f};
		State->FogStart = std::max(lighting.FogStart, 0.0f);
		State->FogEnd = std::max(lighting.FogEnd, State->FogStart);
		State->EnvironmentState = lighting.EnvironmentState;
	}

	scene::WorldLighting Renderer::CurrentLighting() const {
		if (State == nullptr) {
			return {};
		}

		scene::WorldLighting lighting;
		lighting.Direction = core::Vector3{State->Sun.x, State->Sun.y, State->Sun.z};
		lighting.Ambient = core::Color3{State->Ambient.x, State->Ambient.y, State->Ambient.z};
		lighting.OutdoorAmbient = core::Color3{
			State->OutdoorAmbient.x,
			State->OutdoorAmbient.y,
			State->OutdoorAmbient.z,
		};
		lighting.Direct = core::Color3{State->Direct.x, State->Direct.y, State->Direct.z};
		lighting.FogColor = core::Color3{State->FogColour.x, State->FogColour.y, State->FogColour.z};
		lighting.FogStart = State->FogStart;
		lighting.FogEnd = State->FogEnd;
		lighting.EnvironmentState = State->EnvironmentState;
		return lighting;
	}

	core::Vector3 Renderer::SunDirection() const {
		return core::Vector3{State->Sun.x, State->Sun.y, State->Sun.z};
	}

	core::Color3 Renderer::SunAmbient() const {
		return core::Color3{State->Ambient.x, State->Ambient.y, State->Ambient.z};
	}

	core::Color3 Renderer::SunColor() const {
		return core::Color3{State->Direct.x, State->Direct.y, State->Direct.z};
	}

	uint32_t Renderer::PortalDepth() const {
		return State == nullptr ? 0u : State->PortalDepth;
	}

	FlipbookCell Renderer::TextureCell(const core::Name &name, double seconds) const {
		if (State == nullptr) {
			return {};
		}
		return State->Textures.CellOf(name, seconds);
	}

	uint64_t Renderer::TextureAnimationSignature(double seconds) const {
		return State == nullptr ? 0 : State->Textures.AnimationSignature(seconds);
	}

	void *Renderer::TextureHandle(const core::Name &name) const {
		if (State == nullptr) {
			return nullptr;
		}
		return State->Textures.Find(name);
	}

	bool Renderer::TextureSize(const core::Name &name, uint32_t &width, uint32_t &height) const {
		if (State == nullptr) {
			return false;
		}
		return State->Textures.SizeOf(name, width, height);
	}

	bool Renderer::DropTexture(const core::Name &name) {
		if (State == nullptr) {
			return false;
		}
		return State->Textures.Drop(name);
	}

	bool Renderer::AddShader(const core::Name &name, std::span<const uint32_t> spirv) {
		if (State == nullptr || State->Device == nullptr) {
			return false;
		}

		// **Built on the spot rather than at the next frame's barrier**, which
		// is `AddMesh`'s rule and its reason: a caller has already arranged for
		// this to be a moment it controls - a content pump or a
		// `ShaderLibrary::Refresh` - so deferring would add a second
		// synchronisation point for a caller that already had one.
		//
		// **A frame is waited for first, because replacing releases.** An author
		// editing a `ShaderScript` replaces a pipeline that the frame in flight
		// may still be drawing through, and releasing that is a use after free
		// inside the driver rather than an error here.
		if (State->ShaderVariants.contains(name.Id())) {
			(void)WaitForFrame();
		}
		return State->AddShaderVariant(name, spirv);
	}

	bool Renderer::DropShader(const core::Name &name) {
		if (State == nullptr || State->Device == nullptr) {
			return false;
		}
		if (!State->ShaderVariants.contains(name.Id())) {
			return false;
		}

		(void)WaitForFrame();
		State->DropShaderVariant(name);
		return true;
	}

	bool Renderer::HasShader(const core::Name &name) const {
		return State != nullptr && name.IsValid() && State->ShaderVariants.contains(name.Id());
	}

	bool Renderer::SetPostProcessShader(const core::Name &name, std::span<const uint32_t> spirv) {
		if (State == nullptr || State->Device == nullptr || spirv.empty()) {
			return false;
		}

		const bool toMsl = State->Binary.Form == resources::ShaderForm::Msl;
		std::string translated;
		if (toMsl) {
			msl::Translation result = msl::Translate(spirv);
			if (result.Failed) {
				ENGINE_ERROR(
					"postprocess shader '{}' cannot be translated to MSL: {}", name.Text(), result.Error
				);
				return false;
			}
			translated = std::move(result.Source);
		}

		SDL_GPUShaderCreateInfo fragmentInfo{};
		fragmentInfo.code = toMsl ? reinterpret_cast<const Uint8 *>(translated.data())
								  : reinterpret_cast<const Uint8 *>(spirv.data());
		fragmentInfo.code_size = toMsl ? translated.size() : spirv.size() * sizeof(uint32_t);
		fragmentInfo.entrypoint = State->Binary.EntryPoint;
		fragmentInfo.format = State->Binary.Format;
		fragmentInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;

		// **One sampler and no bound uniform buffer - `tonemap.frag`'s own
		// shape**, matching what `CreatePipelines` declares for it. See
		// `SetPostProcessShader`'s own header for the contract a
		// `ShaderScript` used here is written against.
		fragmentInfo.num_samplers = 1;
		fragmentInfo.num_uniform_buffers = 0;

		SDL_GPUShader *fragment = SDL_CreateGPUShader(State->Device, &fragmentInfo);
		if (fragment == nullptr) {
			ENGINE_ERROR("postprocess shader '{}': {}", name.Text(), SDL_GetError());
			return false;
		}

		// Reloaded rather than kept - SDL_GPU pipelines own what they need
		// from the shader objects that built them, `InterfacePass::
		// AddShaderVariant`'s own reason.
		SDL_GPUShader *vertex = State->LoadShader("overlay.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
		if (vertex == nullptr) {
			SDL_ReleaseGPUShader(State->Device, fragment);
			return false;
		}

		SDL_GPUColorTargetDescription target{};
		target.format = State->ColourFormat();

		SDL_GPUGraphicsPipelineCreateInfo info{};
		info.vertex_shader = vertex;
		info.fragment_shader = fragment;
		info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
		info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
		info.target_info.color_target_descriptions = &target;
		info.target_info.num_color_targets = 1;

		SDL_GPUGraphicsPipeline *pipeline = SDL_CreateGPUGraphicsPipeline(State->Device, &info);

		SDL_ReleaseGPUShader(State->Device, vertex);
		SDL_ReleaseGPUShader(State->Device, fragment);

		if (pipeline == nullptr) {
			ENGINE_ERROR("postprocess shader '{}' pipeline: {}", name.Text(), SDL_GetError());
			return false;
		}

		// **Waited for before replacing**, `AddShader`'s own reason: the
		// frame in flight may still be reading through whatever pipeline
		// this is about to release.
		if (State->PostProcessPipeline != nullptr) {
			(void)WaitForFrame();
			SDL_ReleaseGPUGraphicsPipeline(State->Device, State->PostProcessPipeline);
		}
		State->PostProcessPipeline = pipeline;
		State->PostProcessShaderName = name;
		return true;
	}

	void Renderer::ClearPostProcessShader() {
		if (State == nullptr || State->PostProcessPipeline == nullptr) {
			return;
		}
		if (State->Device != nullptr) {
			(void)WaitForFrame();
			SDL_ReleaseGPUGraphicsPipeline(State->Device, State->PostProcessPipeline);
		}
		State->PostProcessPipeline = nullptr;
		State->PostProcessShaderName = core::Name{};
	}

	core::Name Renderer::PostProcessShaderName() const {
		return State == nullptr ? core::Name{} : State->PostProcessShaderName;
	}

	bool Renderer::Impl::WriteCapture(
		SDL_GPUTransferBuffer *from,
		uint32_t width,
		uint32_t height,
		SDL_GPUTextureFormat format,
		const std::filesystem::path &path
	) const {
		void *mapped = SDL_MapGPUTransferBuffer(Device, from, false);
		if (mapped == nullptr) {
			ENGINE_ERROR("SDL_MapGPUTransferBuffer: {}", SDL_GetError());
			return false;
		}

		// The swapchain's format decides the channel order, and getting it
		// wrong writes a picture with red and blue swapped - which looks like a
		// shader bug rather than like a file-writing bug, so it is worth
		// asking rather than assuming.
		SDL_PixelFormat pixels = SDL_PIXELFORMAT_UNKNOWN;
		switch (format) {
		case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM:
		case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB:
			pixels = SDL_PIXELFORMAT_BGRA32;
			break;
		case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM:
		case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB:
			pixels = SDL_PIXELFORMAT_RGBA32;
			break;
		default:
			break;
		}

		if (pixels == SDL_PIXELFORMAT_UNKNOWN) {
			ENGINE_ERROR("capture: swapchain format {} has no BMP mapping", static_cast<int>(format));
			SDL_UnmapGPUTransferBuffer(Device, from);
			return false;
		}

		SDL_Surface *surface = SDL_CreateSurfaceFrom(
			static_cast<int>(width), static_cast<int>(height), pixels, mapped, static_cast<int>(width * 4)
		);

		bool wrote = false;
		if (surface != nullptr) {
			wrote = SDL_SaveBMP(surface, path.string().c_str());
			if (!wrote) {
				ENGINE_ERROR("SDL_SaveBMP: {}", SDL_GetError());
			}
			SDL_DestroySurface(surface);
		} else {
			// Every other failure here says so, and this one used to return
			// false in silence - a capture that produced no file and no reason
			// reads as the request having been ignored.
			ENGINE_ERROR("capture: SDL_CreateSurfaceFrom: {}", SDL_GetError());
		}

		SDL_UnmapGPUTransferBuffer(Device, from);
		return wrote;
	}

	void Renderer::RequestSceneCapture(std::filesystem::path path, size_t slot) {
		State->CapturePath = std::move(path);
		State->CaptureSlot = slot;
	}

	void Renderer::RequestWindowCapture(std::filesystem::path path) {
		State->WindowCapturePath = std::move(path);
	}

	bool Renderer::CapturePending() const {
		return State != nullptr && (!State->CapturePath.empty() || !State->WindowCapturePath.empty());
	}

	bool Renderer::IsHeadless() const {
		return State->Headless();
	}

	void *Renderer::SceneTexture(size_t slot) const {
		if (slot >= State->SceneSlots.size()) {
			return nullptr;
		}
		const Impl::SceneSlot &scene = State->SceneSlots[slot];
		if (scene.PublishedFrame == Impl::SceneSlot::NO_RETAINED_FRAME) {
			return nullptr;
		}
		return scene.Retained[scene.PublishedFrame].Texture;
	}

	FrameResult Renderer::SceneFrameResult(size_t slot) const {
		if (slot >= State->SceneSlots.size()) {
			return {};
		}
		const Impl::SceneSlot &scene = State->SceneSlots[slot];
		if (scene.PublishedFrame == Impl::SceneSlot::NO_RETAINED_FRAME) {
			return {};
		}
		return scene.Retained[scene.PublishedFrame].Result;
	}

	void *Renderer::ResourceTexture(core::Name resource, size_t slot) const {
		if (State == nullptr || !resource.IsValid()) {
			return nullptr;
		}
		const Impl::NamedPipeline *pipeline = State->PipelineFor(State->ActiveGraph);
		if (pipeline != nullptr) {
			for (const Impl::GraphTarget &target : State->GraphTargets) {
				if (target.Pipeline != pipeline->Name || target.Resource != resource ||
					target.Texture == nullptr) {
					continue;
				}
				if (target.Scope != graph::NodeScope::View || target.Owner == slot) {
					return target.Texture;
				}
			}
		}
		const Impl::ResourceRole role = State->RoleFor(resource);
		if (role == Impl::ResourceRole::PreviousFrame) {
			if (slot >= State->SceneSlots.size()) {
				return nullptr;
			}
			const Impl::SceneSlot &history = State->SceneSlots[slot];
			return history.HistoryReady ? history.History : nullptr;
		}
		if (role == Impl::ResourceRole::Scene) {
			return SceneTexture(slot);
		}
		if (role == Impl::ResourceRole::Depth) {
			return slot < State->SceneSlots.size() ? State->SceneSlots[slot].Depth : nullptr;
		}
		if (role == Impl::ResourceRole::Shadow) {
			return State->ShadowTexture;
		}
		if (role == Impl::ResourceRole::Surface && slot < State->SurfaceBanks.size()) {
			const Impl::SurfaceSlotState &surface = State->SurfaceBanks[slot].Surfaces[0];
			return surface.Ready ? surface.Texture[surface.Slot] : nullptr;
		}
		if ((role == Impl::ResourceRole::PortalImage || role == Impl::ResourceRole::PortalDisplay) &&
			slot < State->SurfaceBanks.size()) {
			const std::vector<Impl::PortalLevel> &levels = State->SurfaceBanks[slot].Portals;
			for (auto level = levels.rbegin(); level != levels.rend(); ++level) {
				for (const Impl::PortalTarget &portal : level->Targets) {
					SDL_GPUTexture *texture =
						role == Impl::ResourceRole::PortalImage ? portal.Colour : portal.Display;
					if (texture != nullptr) {
						return texture;
					}
				}
			}
			return nullptr;
		}
		if (role == Impl::ResourceRole::PortalLight && slot < State->SurfaceBanks.size()) {
			for (const Impl::SeamLightTarget &seamLight : State->SurfaceBanks[slot].SeamLights) {
				if (seamLight.Ready && seamLight.Colour != nullptr) {
					return seamLight.Colour;
				}
			}
			return nullptr;
		}
		if (slot >= State->PbrSlots.size()) {
			return nullptr;
		}

		const Impl::PbrSlot &pbr = State->PbrSlots[slot];
		if (role == Impl::ResourceRole::Albedo) {
			return pbr.Albedo;
		}
		if (role == Impl::ResourceRole::Normal) {
			return pbr.Normal;
		}
		if (role == Impl::ResourceRole::Material) {
			return pbr.Material;
		}
		if (role == Impl::ResourceRole::Emissive) {
			return pbr.Emissive;
		}
		if (role == Impl::ResourceRole::LinearDepth) {
			return pbr.LinearDepth;
		}
		if (role == Impl::ResourceRole::Occlusion) {
			return pbr.Occlusion;
		}
		if (role == Impl::ResourceRole::Lit) {
			return pbr.Lit;
		}
		if (role == Impl::ResourceRole::SkyLit) {
			return pbr.SkyLit;
		}
		return nullptr;
	}

	void Renderer::RefreshResourcePreview(
		core::Name pipeline, core::Name resource, size_t slot, bool reverseSpectrum
	) {
		if (State == nullptr || !pipeline.IsValid() || !resource.IsValid()) {
			return;
		}
		const ResourcePreviewRoute route{pipeline, resource, slot};
		for (Impl::ResourcePreviewTarget &preview : State->ResourcePreviews) {
			if (preview.Route == route) {
				preview.ReverseSpectrum = reverseSpectrum;
				preview.Refresh = true;
				return;
			}
		}
		Impl::ResourcePreviewTarget preview;
		preview.Route = route;
		preview.ReverseSpectrum = reverseSpectrum;
		State->ResourcePreviews.push_back(std::move(preview));
	}

	void *Renderer::ResourcePreviewTexture(core::Name pipeline, core::Name resource, size_t slot) const {
		if (State == nullptr || !pipeline.IsValid() || !resource.IsValid()) {
			return nullptr;
		}
		const ResourcePreviewRoute route{pipeline, resource, slot};
		for (const Impl::ResourcePreviewTarget &preview : State->ResourcePreviews) {
			if (preview.Route == route) {
				return preview.Slots.Ready ? preview.Textures[preview.Slots.Visible] : nullptr;
			}
		}
		return nullptr;
	}

	float Renderer::ResourcePreviewAspect(core::Name pipeline, core::Name resource, size_t slot) const {
		if (State == nullptr || !pipeline.IsValid() || !resource.IsValid()) {
			return 0.0f;
		}
		const ResourcePreviewRoute route{pipeline, resource, slot};
		for (const Impl::ResourcePreviewTarget &preview : State->ResourcePreviews) {
			if (preview.Route == route) {
				if (preview.Height == 0) {
					return 0.0f;
				}
				return static_cast<float>(preview.Width) / static_cast<float>(preview.Height);
			}
		}
		return 0.0f;
	}

	bool Renderer::CaptureSceneTexture(size_t slot, const core::Name &name) {
		if (State == nullptr || State->Device == nullptr || !name.IsValid()) {
			return false;
		}
		if (slot >= State->SceneSlots.size()) {
			return false;
		}

		const Impl::SceneSlot &source = State->SceneSlots[slot];
		if (source.Texture == nullptr || source.DrawnWidth == 0 || source.DrawnHeight == 0) {
			return false;
		}

		// **The drawn rectangle and not the whole target.** A scene target is
		// allocated in 64-pixel blocks with hysteresis, so most of it is border
		// the pass never wrote - `SceneTextureExtent` exists because of exactly
		// that. Copying the allocation would keep a picture with an unwritten
		// margin down two edges, and every consumer would then need the extent
		// as well as the handle, which is the coupling this call is meant to end.
		SDL_GPUTextureCreateInfo info{};
		info.type = SDL_GPU_TEXTURETYPE_2D;

		// The source's format, for `EnsureSceneTarget`'s reason one level up: a
		// blit between mismatched formats is a validation error on the backends
		// that check and a corrupt image on the ones that do not.
		info.format = State->ColourFormat();

		// Sampled, and a colour target because `SDL_BlitGPUTexture` renders into
		// its destination rather than copying into it.
		info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
		info.width = source.DrawnWidth;
		info.height = source.DrawnHeight;
		info.layer_count_or_depth = 1;
		info.num_levels = 1;

		SDL_GPUTexture *copy = gpu::CreateTexture(State->Device, &info);
		if (copy == nullptr) {
			ENGINE_ERROR("SDL_CreateGPUTexture (scene capture): {}", SDL_GetError());
			return false;
		}

		SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(State->Device);
		if (command == nullptr) {
			ENGINE_ERROR("SDL_AcquireGPUCommandBuffer (scene capture): {}", SDL_GetError());
			gpu::ReleaseTexture(State->Device, copy);
			return false;
		}

		// **A blit rather than a copy pass, because the source region is a
		// sub-rectangle.** `SDL_CopyGPUTextureToTexture` would work only if the
		// two agreed on size, which they deliberately do not.
		SDL_GPUBlitInfo blit{};
		blit.source.texture = source.Texture;
		blit.source.w = source.DrawnWidth;
		blit.source.h = source.DrawnHeight;
		blit.destination.texture = copy;
		blit.destination.w = source.DrawnWidth;
		blit.destination.h = source.DrawnHeight;

		// Nothing is preserved, because every texel of the destination is
		// written - saying so lets a tiler skip loading it.
		blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
		blit.filter = SDL_GPU_FILTER_NEAREST;

		SDL_BlitGPUTexture(command, &blit);
		SDL_SubmitGPUCommandBuffer(command);

		// **Four bytes a texel, which is the honest figure for every format this
		// swapchain uses.** Guessing low here would let the table's ceiling be
		// walked past by whoever captures the most.
		const size_t bytes = static_cast<size_t>(source.DrawnWidth) * source.DrawnHeight * 4;

		if (!State->Textures.Adopt(name, copy, source.DrawnWidth, source.DrawnHeight, bytes)) {
			// Refused, so the texture is still ours to release - `Adopt` says so.
			gpu::ReleaseTexture(State->Device, copy);
			return false;
		}

		return true;
	}

	SceneExtent Renderer::SceneTextureExtent(size_t slot) const {
		if (slot >= State->SceneSlots.size()) {
			return {};
		}

		const Impl::SceneSlot &target = State->SceneSlots[slot];
		if (target.PublishedFrame == Impl::SceneSlot::NO_RETAINED_FRAME) {
			return {};
		}
		const Impl::SceneSlot::RetainedFrame &frame = target.Retained[target.PublishedFrame];

		// **The whole texture when nothing has been drawn yet**, which is the
		// honest answer rather than a safe one: a caller sampling a texture no
		// pass has written is showing uninitialised memory whatever the
		// coordinates say, and a fraction of it is not better than all of it.
		if (frame.Width == 0 || frame.Height == 0) {
			return {};
		}

		return SceneExtent{
			static_cast<float>(frame.DrawnWidth) / static_cast<float>(frame.Width),
			static_cast<float>(frame.DrawnHeight) / static_cast<float>(frame.Height),
			frame.DrawnWidth,
			frame.DrawnHeight,
		};
	}

	SceneExtent Renderer::ResourceTextureExtent(core::Name resource, size_t slot) const {
		if (State == nullptr) {
			return {};
		}
		const Impl::NamedPipeline *pipeline = State->PipelineFor(State->ActiveGraph);
		if (pipeline != nullptr) {
			for (const Impl::GraphTarget &target : State->GraphTargets) {
				if (target.Pipeline != pipeline->Name || target.Resource != resource ||
					target.Texture == nullptr ||
					(target.Scope == graph::NodeScope::View && target.Owner != slot)) {
					continue;
				}
				return SceneExtent{1.0f, 1.0f};
			}
		}
		const Impl::ResourceRole role = State->RoleFor(resource);
		if (role == Impl::ResourceRole::PreviousFrame || role == Impl::ResourceRole::Shadow ||
			role == Impl::ResourceRole::Surface || role == Impl::ResourceRole::PortalImage ||
			role == Impl::ResourceRole::PortalDisplay) {
			return SceneExtent{1.0f, 1.0f};
		}
		if (role == Impl::ResourceRole::Scene || role == Impl::ResourceRole::Depth) {
			return SceneTextureExtent(slot);
		}
		if (slot >= State->PbrSlots.size()) {
			return {};
		}

		const Impl::PbrSlot &pbr = State->PbrSlots[slot];
		const Impl::PbrDimensions &dimensions = pbr.Dimensions;
		if (dimensions.TargetWidth == 0 || dimensions.TargetHeight == 0 || dimensions.ViewWidth == 0 ||
			dimensions.ViewHeight == 0) {
			return {};
		}

		if (role == Impl::ResourceRole::Albedo || role == Impl::ResourceRole::Normal ||
			role == Impl::ResourceRole::Material || role == Impl::ResourceRole::Emissive) {
			return SceneExtent{
				static_cast<float>(dimensions.ViewWidth) / static_cast<float>(dimensions.TargetWidth),
				static_cast<float>(dimensions.ViewHeight) / static_cast<float>(dimensions.TargetHeight),
			};
		}
		return SceneExtent{1.0f, 1.0f};
	}

	BackendHandles Renderer::Backend() const {
		BackendHandles handles;
		if (State->Device != nullptr) {
			handles.Device = State->Device;
			handles.ColourFormat = static_cast<uint32_t>(State->ColourFormat());
		}
		return handles;
	}

	FrameResult Renderer::Render(
		std::span<const View> views,
		OverlayImage &overlay,
		FrameOverlayHook *gameInterfaceHook,
		bool present,
		FrameOverlayHook *hostOverlayHook
	) {
		ENGINE_PROFILE_CAT("Renderer::Render views", core::ProfileCategory::Render);
		RequireOwningThread("Render views");

		FrameResult frame;
		if (State == nullptr || State->Device == nullptr || views.empty() || State->BatchActive) {
			return frame;
		}
		State->PollSceneFrames();

		// **Every mesh admitted since the last frame, in one transfer.** `AddMesh`
		// only accumulates, so this is the barrier the content pump used to pay
		// once per arriving mesh. Free when nothing arrived.
		State->Meshes.Flush();

		struct ViewGroup {
			uint64_t World = 0;
			core::Name Pipeline;
			std::vector<size_t> Views;
		};
		std::vector<ViewGroup> groups;
		groups.reserve(views.size());
		for (size_t index = 0; index < views.size(); index++) {
			const View &view = views[index];
			auto found = std::find_if(groups.begin(), groups.end(), [&](const ViewGroup &group) {
				return group.World == view.World && group.Pipeline == view.Pipeline;
			});
			if (found == groups.end()) {
				groups.push_back({view.World, view.Pipeline, {index}});
			} else {
				found->Views.push_back(index);
			}
		}

		// Presentation belongs to the caller's last view. Keep that group last so
		// another world's shared targets cannot overwrite it before its view runs.
		if (present) {
			const size_t finalView = views.size() - 1;
			const auto finalGroup = std::find_if(groups.begin(), groups.end(), [&](const ViewGroup &group) {
				return std::find(group.Views.begin(), group.Views.end(), finalView) != group.Views.end();
			});
			if (finalGroup != groups.end() && std::next(finalGroup) != groups.end()) {
				std::rotate(finalGroup, std::next(finalGroup), groups.end());
			}
		}

		std::vector<size_t> order;
		order.reserve(views.size());
		for (const ViewGroup &group : groups) {
			order.insert(order.end(), group.Views.begin(), group.Views.end());
		}
		for (size_t position = 0; position + 1 < order.size(); position++) {
			const SceneTarget *target = views[order[position]].Target;
			if (target == nullptr || !target->IsValid()) {
				ENGINE_ERROR("render batch view {} has no offscreen target", order[position]);
				return frame;
			}
		}

		SDL_GPUCommandBuffer *command = nullptr;
		SDL_GPUTexture *swapchain = nullptr;
		uint32_t width = 0;
		uint32_t height = 0;
		if (present) {
			if (!State->BeginFrame()) {
				return frame;
			}
			State->TakeFrame(command, swapchain, width, height);
		} else {
			command = SDL_AcquireGPUCommandBuffer(State->Device);
			if (command == nullptr) {
				ENGINE_ERROR("SDL_AcquireGPUCommandBuffer (view batch): {}", SDL_GetError());
				return frame;
			}
		}

		const SceneTarget *finalTarget = views[order.back()].Target;
		if (swapchain == nullptr && (finalTarget == nullptr || !finalTarget->IsValid())) {
			// A headless Studio has no swapchain and its viewport has no extent
			// until the first interface layout. That frame has nowhere to draw by
			// design. A windowed caller reaching the same state lost its target.
			if (!State->Headless()) {
				ENGINE_ERROR("render batch final view has neither a swapchain nor an offscreen target");
			}
			SDL_SubmitGPUCommandBuffer(command);
			return frame;
		}

		const scene::WorldLighting previousLighting = CurrentLighting();
		State->BatchActive = true;
		State->BatchFailed = false;
		State->BatchCommand = command;
		State->BatchSwapchain = swapchain;
		State->BatchWidth = width;
		State->BatchHeight = height;
		State->BatchTimingSlot = VulkanTimestamps::NO_SLOT;

		size_t position = 0;
		for (size_t groupIndex = 0; groupIndex < groups.size() && !State->BatchFailed; groupIndex++) {
			const ViewGroup &group = groups[groupIndex];
			for (size_t member = 0; member < group.Views.size(); member++, position++) {
				const size_t viewIndex = group.Views[member];
				const View &view = views[viewIndex];
				SetLighting(view.OverrideLighting ? view.Lighting : previousLighting);

				State->BatchFirst = position == 0;
				State->BatchFinal = position + 1 == order.size();
				State->BatchShared = member == 0;
				State->BatchViewIndex = viewIndex;
				State->BatchWorldIndex = groupIndex;

				frame.Accumulate(RenderView(
					view.CameraFrame,
					view.Camera,
					view.Instances,
					overlay,
					view.Surfaces,
					gameInterfaceHook,
					State->BatchFinal ? hostOverlayHook : nullptr,
					view.Target,
					view.Slot,
					view,
					view.Particles,
					view.RibbonVertices,
					view.RibbonRuns,
					view.Lights,
					view.Foreign,
					view.Portals,
					State->BatchFinal && present,
					view.Pipeline,
					view.World
				));
				if (State->BatchFailed) {
					break;
				}
			}
		}

		for (const ViewGroup &group : groups) {
			const Impl::NamedPipeline *named = State->PipelineFor(group.Pipeline);
			if (named == nullptr) {
				continue;
			}
			uint32_t planWidth = width;
			uint32_t planHeight = height;
			std::vector<uint64_t> worlds;
			worlds.reserve(group.Views.size());
			for (const size_t viewIndex : group.Views) {
				const View &view = views[viewIndex];
				worlds.push_back(view.World);
				if (view.Target != nullptr && view.Target->IsValid()) {
					planWidth = std::max(planWidth, view.Target->Width);
					planHeight = std::max(planHeight, view.Target->Height);
				}
			}
			planWidth = std::max(planWidth, 1u);
			planHeight = std::max(planHeight, 1u);

			graph::FrameExecutionPlan plan;
			core::Name offender;
			if (graph::PlanFrame(
					named->Graph, named->Schedule, worlds, planWidth, planHeight, plan, offender
				) == graph::ExecutionPlanStatus::Ok) {
				frame.ScheduledReadBytes += plan.ReadBytes;
				frame.ScheduledWriteBytes += plan.WriteBytes;
				frame.QueueTransferBytes += plan.QueueTransferBytes;
				frame.ConcurrentWaves += static_cast<uint32_t>(
					std::count_if(plan.Waves.begin(), plan.Waves.end(), [](const graph::PlannedWave &wave) {
						return wave.ConcurrentQueues;
					})
				);
				frame.TrafficCommandBuffers += static_cast<uint32_t>(named->Buffers.size());
			}
		}

		SetLighting(previousLighting);
		if (State->BatchCommand != nullptr) {
			State->Timestamps.Abandon(State->BatchTimingSlot);
			if (State->BatchTimingSlot < VulkanTimestamps::SLOTS) {
				State->PendingMarks[State->BatchTimingSlot].clear();
			}
			const bool submitted = State->SubmitSceneCommand(State->BatchCommand);
			if (!submitted) {
				ENGINE_ERROR("SDL_SubmitGPUCommandBuffer (failed view batch): {}", SDL_GetError());
			}
			State->CompleteResidentUploads(submitted);
			State->BatchCommand = nullptr;

			// A failed batch never reached the final view's submit, so any
			// downloads an earlier view recorded still hold their buffer.
			State->DropDownloads();
		}
		State->BatchActive = false;
		State->BatchFirst = false;
		State->BatchFinal = false;
		State->BatchShared = false;
		State->BatchFailed = false;
		State->BatchSwapchain = nullptr;
		State->BatchWidth = 0;
		State->BatchHeight = 0;
		State->BatchTimingSlot = VulkanTimestamps::NO_SLOT;
		return frame;
	}

	FrameResult Renderer::RenderView(
		const core::CFrame &cameraFrame,
		const scene::Camera &camera,
		std::span<const scene::DrawInstance> instances,
		OverlayImage &overlay,
		std::span<const SurfaceView> surfaces,
		FrameOverlayHook *gameInterfaceHook,
		FrameOverlayHook *hostOverlayHook,
		const SceneTarget *sceneTarget,
		size_t targetSlot,
		const View &source,
		std::span<const ParticleBatch> particles,
		std::span<const effects::RibbonVertex> ribbonVertices,
		std::span<const effects::RibbonRun> ribbonRuns,
		std::span<const SceneLight> lights,
		std::span<const scene::DrawInstance> foreign,
		std::span<const PortalView> portals,
		bool present,
		core::Name pipeline,
		uint64_t world
	) {
		ENGINE_PROFILE_CAT("Renderer::RenderView", core::ProfileCategory::Render);

		// **The single-threaded recording contract, checked rather than
		// described.** A studio draws one viewport after another; a second one
		// recording from another thread is the failure this refuses. See
		// `IsOnOwningThread` for why that is the design and not a limitation.
		RequireOwningThread("RenderView");

		FrameResult result;

		// **What this function used to be, in four steps.** Until v0.19 the
		// whole of it was here - 5,485 lines, two fifths of the module, holding
		// two dozen node handlers as lambdas over its own locals and opening
		// eighteen render passes inline. A pass opened inline is invisible to
		// the graph, and one translation unit that size cannot be split across
		// cores, so `docs/ARCH_REVIEW.md` C2 argued the two were one change.
		// This is that change: `ViewRecording` holds what the handlers closed
		// over, and each node family registers its own runners from its own
		// file. `D00016`.
		//
		// **The recording is declared before the table and outlives it**, which
		// the handlers depend on: each of them captures the recording and is
		// called while the graph runs.
		ViewRecording recording(*this, *State, result);

		// **Named rather than positional**, because nineteen arguments in a row
		// is nineteen chances to hand `foreign` to the parameter that wanted
		// `instances` - two spans of the same type, and the compiler would have
		// nothing to say about it.
		PresentationDamage damage = source.Damage;
		damage.Scene = damage.Scene || (damage.GameInterface && gameInterfaceHook != nullptr &&
										gameInterfaceHook->AffectsScene());
		const ViewRequest request{
			.CameraFrame = cameraFrame,
			.Camera = camera,
			.Instances = instances,
			.JointFrames = source.JointFrames,
			.Overlay = &overlay,
			.Surfaces = surfaces,
			.GameInterfaceHook = gameInterfaceHook,
			.HostOverlayHook = hostOverlayHook,
			.Target = sceneTarget,
			.TargetSlot = targetSlot,
			.Source = &source,
			.Particles = particles,
			.RibbonVertices = ribbonVertices,
			.RibbonRuns = ribbonRuns,
			.Lights = lights,
			.Foreign = foreign,
			.ForeignJointFrames = source.ForeignJointFrames,
			.Portals = portals,
			.Present = present,
			.Pipeline = pipeline,
			.World = world,
			.Damage = damage,
		};
		if (recording.Begin(request) != ViewStart::Recording) {
			return result;
		}

		// **Every kind the backend knows, defaulted to "did nothing".** A graph
		// naming a node this build has no runner for is refused by
		// `NodeTable::Missing` before recording starts rather than half way
		// through it, and a kind with a runner registered below replaces the
		// default.
		NodeTable frameNodes;
		{
			ENGINE_PROFILE_CAT("build node table", core::ProfileCategory::Render);
			frameNodes = BackendTable([](const graph::RunContext &) { return true; });
			for (const InstalledNodeHandler &installed : CustomNodeHandlers) {
				frameNodes.Set(installed.Kind, installed.Handler);
			}
			if (request.Damage.Scene) {
				recording.RegisterUploadNodes(frameNodes);
				recording.RegisterShadowNodes(frameNodes);
				recording.RegisterMirrorNodes(frameNodes);
				recording.RegisterPortalNodes(frameNodes);
				recording.RegisterGeometryNodes(frameNodes);
				recording.RegisterShadingNodes(frameNodes);
				recording.RegisterAuthoredNodes(frameNodes);
			}
			recording.RegisterOutputNodes(frameNodes);
		}

		recording.Finish(frameNodes);
		return result;
	}
}
