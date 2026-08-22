// The depth pyramid and the GPU cull that reads it.
//
// The early phase draws the occluders the CPU picked, the pyramid is reduced
// over what they wrote, and the late phase draws whatever survives the test
// against it. Nothing here decides *whether* to cull: `gbuffer` does, from the
// plan `ViewRecording::Begin` built, and a frame with too few occluders or too
// few candidates falls back to the plain draw.

#include "DisplayColour.hpp"
#include "RenderTypes.hpp"
#include "RendererState.hpp"
#include "VulkanTimestamps.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>

#include <SDL3/SDL_gpu.h>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace engine::render {

	bool Renderer::Impl::EnsureOcclusionResources(
		uint32_t argCount, uint32_t candidateCount, uint32_t runCount, uint32_t lateCount
	) {
		// Powers of two, for `EnsureInstanceCapacity`'s reason. One helper
		// because six buffers grown six ways is six chances to grow five.
		const auto grown = [](uint32_t have, uint32_t need) {
			uint32_t capacity = have == 0 ? 64 : have;
			while (capacity < need) {
				capacity *= 2;
			}
			return capacity;
		};
		const auto ensure = [&](SDL_GPUBuffer *&buffer,
								uint32_t &capacity,
								uint32_t need,
								uint32_t stride,
								SDL_GPUBufferUsageFlags usage,
								const char *what) {
			if (need <= capacity && buffer != nullptr) {
				return true;
			}
			const uint32_t entries = grown(capacity, need);
			if (buffer != nullptr) {
				gpu::ReleaseBuffer(Device, buffer);
			}
			SDL_GPUBufferCreateInfo info{};
			info.usage = usage;
			info.size = entries * stride;
			buffer = gpu::CreateBuffer(Device, &info);
			if (buffer == nullptr) {
				ENGINE_ERROR("occlusion {} buffer of {} entries: {}", what, entries, SDL_GetError());
				capacity = 0;
				return false;
			}
			capacity = entries;
			return true;
		};

		constexpr uint32_t COMMAND_BYTES = sizeof(SDL_GPUIndexedIndirectDrawCommand);
		constexpr uint32_t CANDIDATE_BYTES = 32; // two vec4 - see occlusion-cull.comp

		// `ArgRuns` shares the argument capacity and `Counts` the run capacity,
		// because each pair grows for the same reason on the same frame. The
		// paired buffer is recreated whenever its partner was, which the null
		// check after a release makes true by construction.
		bool ready = true;
		if (argCount * 2 > Occlusion.ArgumentCapacity || Occlusion.Arguments == nullptr ||
			Occlusion.ArgRuns == nullptr) {
			if (Occlusion.ArgRuns != nullptr) {
				gpu::ReleaseBuffer(Device, Occlusion.ArgRuns);
				Occlusion.ArgRuns = nullptr;
			}
			ready = ensure(
						Occlusion.Arguments,
						Occlusion.ArgumentCapacity,
						argCount * 2,
						COMMAND_BYTES,
						SDL_GPU_BUFFERUSAGE_INDIRECT | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE,
						"argument"
					) &&
					ready;
			if (ready) {
				SDL_GPUBufferCreateInfo info{};
				info.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
				info.size = Occlusion.ArgumentCapacity * static_cast<uint32_t>(sizeof(uint32_t));
				Occlusion.ArgRuns = gpu::CreateBuffer(Device, &info);
				ready = Occlusion.ArgRuns != nullptr;
				if (!ready) {
					ENGINE_ERROR("occlusion argument-run buffer: {}", SDL_GetError());
				}
			}
		}

		if (runCount > Occlusion.RunCapacity || Occlusion.RunTable == nullptr ||
			Occlusion.Counts == nullptr) {
			if (Occlusion.Counts != nullptr) {
				gpu::ReleaseBuffer(Device, Occlusion.Counts);
				Occlusion.Counts = nullptr;
			}
			ready = ensure(
						Occlusion.RunTable,
						Occlusion.RunCapacity,
						runCount,
						sizeof(uint32_t),
						SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
						"run table"
					) &&
					ready;
			if (ready) {
				SDL_GPUBufferCreateInfo info{};
				info.usage =
					SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
				info.size = Occlusion.RunCapacity * static_cast<uint32_t>(sizeof(uint32_t));
				Occlusion.Counts = gpu::CreateBuffer(Device, &info);
				ready = Occlusion.Counts != nullptr;
				if (!ready) {
					ENGINE_ERROR("occlusion count buffer: {}", SDL_GetError());
				}
			}
		}

		ready = ensure(
					Occlusion.Candidates,
					Occlusion.CandidateCapacity,
					candidateCount,
					CANDIDATE_BYTES,
					SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
					"candidate"
				) &&
				ready;
		ready = ensure(
					Occlusion.LateIndices,
					Occlusion.LateCapacity,
					lateCount,
					sizeof(uint32_t),
					SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE,
					"late index"
				) &&
				ready;

		// One transfer stages everything the CPU writes, packed back to back in
		// the order `RecordUploads` copies it out: arguments, candidates, run
		// table, argument runs, count zeros.
		const uint32_t staged = argCount * 2 * COMMAND_BYTES + candidateCount * CANDIDATE_BYTES +
								runCount * static_cast<uint32_t>(sizeof(uint32_t)) +
								argCount * static_cast<uint32_t>(sizeof(uint32_t)) +
								runCount * static_cast<uint32_t>(sizeof(uint32_t));
		if (staged > Occlusion.TransferCapacity || Occlusion.Transfer == nullptr) {
			if (Occlusion.Transfer != nullptr) {
				gpu::ReleaseTransferBuffer(Device, Occlusion.Transfer);
			}
			const uint32_t bytes = grown(Occlusion.TransferCapacity, staged);
			SDL_GPUTransferBufferCreateInfo info{};
			info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
			info.size = bytes;
			Occlusion.Transfer = gpu::CreateTransferBuffer(Device, &info);
			if (Occlusion.Transfer == nullptr) {
				ENGINE_ERROR("occlusion transfer buffer of {} bytes: {}", bytes, SDL_GetError());
				Occlusion.TransferCapacity = 0;
				ready = false;
			} else {
				Occlusion.TransferCapacity = bytes;
			}
		}
		return ready;
	}

	bool Renderer::Impl::EnsurePyramid(uint32_t width, uint32_t height) {
		if (Occlusion.Width == width && Occlusion.Height == height && Occlusion.Levels[0] != nullptr) {
			return true;
		}
		for (SDL_GPUTexture *&level : Occlusion.Levels) {
			if (level != nullptr) {
				gpu::ReleaseTexture(Device, level);
				level = nullptr;
			}
		}
		Occlusion.Width = 0;
		Occlusion.Height = 0;
		Occlusion.LevelCount = 0;

		uint32_t levelWidth = width;
		uint32_t levelHeight = height;
		uint32_t count = 0;
		while (count < PYRAMID_LEVEL_LIMIT) {
			SDL_GPUTextureCreateInfo info{};
			info.type = SDL_GPU_TEXTURETYPE_2D;
			info.format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
			info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
			info.width = levelWidth;
			info.height = levelHeight;
			info.layer_count_or_depth = 1;
			info.num_levels = 1;
			info.sample_count = SDL_GPU_SAMPLECOUNT_1;
			Occlusion.Levels[count] = gpu::CreateTexture(Device, &info);
			if (Occlusion.Levels[count] == nullptr) {
				ENGINE_ERROR("depth pyramid level {}: {}", count, SDL_GetError());
				return false;
			}
			count++;
			if (levelWidth == 1 && levelHeight == 1) {
				break;
			}
			levelWidth = std::max(levelWidth / 2, 1u);
			levelHeight = std::max(levelHeight / 2, 1u);
		}

		Occlusion.Width = width;
		Occlusion.Height = height;
		Occlusion.LevelCount = count;
		return true;
	}

	void Renderer::Impl::BuildPyramid(SDL_GPUCommandBuffer *command, SDL_GPUTexture *depth) {
		ENGINE_PROFILE_CAT("depth pyramid", core::ProfileCategory::Render);

		// The shaders reproduce this halving, so the two must stay one rule:
		// level n is `max(size >> n, 1)` of level zero.
		uint32_t sourceWidth = Occlusion.Width;
		uint32_t sourceHeight = Occlusion.Height;
		for (uint32_t level = 0; level < Occlusion.LevelCount; level++) {
			const uint32_t destinationWidth = level == 0 ? sourceWidth : std::max(sourceWidth / 2, 1u);
			const uint32_t destinationHeight = level == 0 ? sourceHeight : std::max(sourceHeight / 2, 1u);

			SDL_GPUStorageTextureReadWriteBinding destination{};
			destination.texture = Occlusion.Levels[level];
			// A fresh version per view: another view's pyramid may still be in
			// flight, and its cull already bound the version it reads.
			destination.cycle = true;

			SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(command, &destination, 1, nullptr, 0);
			if (pass == nullptr) {
				ENGINE_ERROR("depth pyramid: SDL_BeginGPUComputePass: {}", SDL_GetError());
				return;
			}
			SDL_BindGPUComputePipeline(pass, level == 0 ? Occlusion.Seed : Occlusion.Reduce);

			const SDL_GPUTextureSamplerBinding source{
				level == 0 ? depth : Occlusion.Levels[level - 1], Textures.Sampler()
			};
			SDL_BindGPUComputeSamplers(pass, 0, &source, 1);

			// The seed reads xy as its own size; the reduce reads source then
			// destination.
			const int32_t sizes[4] = {
				static_cast<int32_t>(level == 0 ? destinationWidth : sourceWidth),
				static_cast<int32_t>(level == 0 ? destinationHeight : sourceHeight),
				static_cast<int32_t>(destinationWidth),
				static_cast<int32_t>(destinationHeight),
			};
			SDL_PushGPUComputeUniformData(command, 0, sizes, sizeof(sizes));
			SDL_DispatchGPUCompute(pass, (destinationWidth + 7) / 8, (destinationHeight + 7) / 8, 1);
			SDL_EndGPUComputePass(pass);

			sourceWidth = destinationWidth;
			sourceHeight = destinationHeight;
		}
	}

	void
	Renderer::Impl::DispatchOcclusionCull(SDL_GPUCommandBuffer *command, const glm::mat4 &viewProjection) {
		ENGINE_PROFILE_CAT("occlusion cull", core::ProfileCategory::Render);

		// Pass one: test every candidate and compact the survivors.
		{
			SDL_GPUStorageBufferReadWriteBinding outputs[2]{};
			// `Counts` is not cycled: the upload wrote this frame's zeros into
			// the version the atomics must land in. The late buffer took no
			// upload, so this write is its first touch and may cycle.
			outputs[0].buffer = Occlusion.Counts;
			outputs[1].buffer = Occlusion.LateIndices;
			outputs[1].cycle = true;

			SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(command, nullptr, 0, outputs, 2);
			if (pass == nullptr) {
				ENGINE_ERROR("occlusion cull: SDL_BeginGPUComputePass: {}", SDL_GetError());
				return;
			}
			SDL_BindGPUComputePipeline(pass, Occlusion.Cull);

			SDL_GPUTextureSamplerBinding levels[PYRAMID_LEVEL_LIMIT];
			for (uint32_t level = 0; level < PYRAMID_LEVEL_LIMIT; level++) {
				// The tail past `LevelCount` is never selected; level zero fills
				// it because a declared sampler must have something bound.
				levels[level] = SDL_GPUTextureSamplerBinding{
					Occlusion.Levels[std::min(level, Occlusion.LevelCount - 1)], Textures.Sampler()
				};
			}
			SDL_BindGPUComputeSamplers(pass, 0, levels, PYRAMID_LEVEL_LIMIT);

			SDL_GPUBuffer *const reads[2] = {Occlusion.Candidates, Occlusion.RunTable};
			SDL_BindGPUComputeStorageBuffers(pass, 0, reads, 2);

			struct CullUniform {
				glm::mat4 ViewProjection;
				uint32_t Counts[4];
				float Level0[4];
			} uniform{
				viewProjection,
				{OcclusionFrame.CandidateCount, Occlusion.LevelCount, 0, 0},
				{static_cast<float>(Occlusion.Width), static_cast<float>(Occlusion.Height), 0.0f, 0.0f},
			};
			SDL_PushGPUComputeUniformData(command, 0, &uniform, sizeof(uniform));
			SDL_DispatchGPUCompute(pass, (OcclusionFrame.CandidateCount + 63) / 64, 1, 1);
			SDL_EndGPUComputePass(pass);
		}

		// Pass two: copy each run's survivor count into every indirect draw
		// argument that run emits. Its own pass, so the counts are complete
		// before anything reads them.
		{
			SDL_GPUStorageBufferReadWriteBinding output{};
			output.buffer = Occlusion.Arguments;

			SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(command, nullptr, 0, &output, 1);
			if (pass == nullptr) {
				ENGINE_ERROR("occlusion arguments: SDL_BeginGPUComputePass: {}", SDL_GetError());
				return;
			}
			SDL_BindGPUComputePipeline(pass, Occlusion.Args);

			SDL_GPUBuffer *const reads[2] = {Occlusion.ArgRuns, Occlusion.Counts};
			SDL_BindGPUComputeStorageBuffers(pass, 0, reads, 2);

			const uint32_t range[4] = {OcclusionFrame.ArgCount, OcclusionFrame.ArgCount, 0, 0};
			SDL_PushGPUComputeUniformData(command, 0, range, sizeof(range));
			SDL_DispatchGPUCompute(pass, (OcclusionFrame.ArgCount + 63) / 64, 1, 1);
			SDL_EndGPUComputePass(pass);
		}
	}

	void Renderer::Impl::ReleaseOcclusion() {
		for (SDL_GPUTexture *&level : Occlusion.Levels) {
			if (level != nullptr) {
				gpu::ReleaseTexture(Device, level);
				level = nullptr;
			}
		}
		const auto releaseBuffer = [&](SDL_GPUBuffer *&buffer) {
			if (buffer != nullptr) {
				gpu::ReleaseBuffer(Device, buffer);
				buffer = nullptr;
			}
		};
		releaseBuffer(Occlusion.Arguments);
		releaseBuffer(Occlusion.Candidates);
		releaseBuffer(Occlusion.RunTable);
		releaseBuffer(Occlusion.ArgRuns);
		releaseBuffer(Occlusion.Counts);
		releaseBuffer(Occlusion.LateIndices);
		if (Occlusion.Transfer != nullptr) {
			gpu::ReleaseTransferBuffer(Device, Occlusion.Transfer);
			Occlusion.Transfer = nullptr;
		}
		const auto releasePipeline = [&](SDL_GPUComputePipeline *&pipeline) {
			if (pipeline != nullptr) {
				SDL_ReleaseGPUComputePipeline(Device, pipeline);
				pipeline = nullptr;
			}
		};
		releasePipeline(Occlusion.Seed);
		releasePipeline(Occlusion.Reduce);
		releasePipeline(Occlusion.Cull);
		releasePipeline(Occlusion.Args);
		Occlusion = OcclusionState{};
	}
}
