#pragma once

// Device residency derived from imagegraph documents. Authored values remain with
// their host; these copies and signatures exist only to reuse accepted GPU work.

#include "diagnostics/VulkanTimestamps.hpp"

#include <engine/imagegraph/Document.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/resources/TextureTable.hpp>

#include <SDL3/SDL_gpu.h>

#include <array>
#include <chrono>
#include <unordered_map>

namespace engine::render {
	struct ImageGraphGpuUniforms {
		std::array<uint32_t, 4> Control{};
		std::array<int32_t, 4> Extents{};
		std::array<int32_t, 4> Region{};
		std::array<float, 4> Colour{};
		std::array<float, 4> Affine{};
		std::array<float, 4> Parameters{};
		std::array<std::array<float, 4>, 2> Values{};
		std::array<std::array<float, 4>, 4> StopPositions{};
		std::array<std::array<float, 4>, 16> StopColours{};
	};
	static_assert(sizeof(ImageGraphGpuUniforms) == 448);

	struct ImageGraphGpuNode {
		SDL_GPUTexture *Texture = nullptr;
		// Wide row pass of a blur; null for every other kind.
		SDL_GPUTexture *Scratch = nullptr;
		imagegraph::ImageExtent Extent;
		uint64_t AuthoredSignature = 0;
		uint64_t AcceptedSignature = 0;
	};
	struct ImageGraphGpuPublication {
		core::Name Name;
		SDL_GPUTexture *Texture = nullptr; // Owned by TextureTable after commit.
		imagegraph::ImageExtent Extent;
		imagegraph::OutputSpace Space = imagegraph::OutputSpace::SRGB;
		imagegraph::OutputFormat Format = imagegraph::OutputFormat::RGBA8;
		uint64_t Signature = 0;
	};
	struct ImageGraphGpuDocument {
		core::Name Owner;
		imagegraph::Document Document;
		imagegraph::Plan Plan;
		std::vector<ImageGraphSourceBinding> Sources;
		std::vector<ImageGraphGpuNode> Nodes;
		std::vector<ImageGraphGpuPublication> Publications;
		uint64_t GeometrySignature = 0;
		std::vector<imagegraph::ExecutionPlan> Executions;
		std::vector<uint64_t> ExecutionSignatures;
		// Content-addressed source texture the last Pending evaluation waits for.
		core::Name NeededSource;
	};

	// One evaluation too large to keep resident, recorded in slices. Intermediates come
	// from a pool and return to it after their last reader; nothing survives the job.
	struct ImageGraphGpuJob {
		struct Pooled {
			SDL_GPUTexture *Texture = nullptr;
			imagegraph::ImageExtent Extent;
			SDL_GPUTextureFormat Format = SDL_GPU_TEXTUREFORMAT_INVALID;
			bool Free = true;
		};
		const ImageGraphGpuDocument *Graph = nullptr;
		core::Name Published;
		uint64_t Signature = 0;
		imagegraph::ExecutionPlan Execution;
		std::vector<uint32_t> Readers;
		// Pool index of each node's image, or -1.
		std::vector<int32_t> Images;
		std::vector<Pooled> Pool;
		int32_t Scratch = -1;
		// Next node in Execution.Order, its pass and its first unrecorded row.
		size_t Position = 0;
		uint32_t Pass = 0;
		uint32_t Row = 0;
		uint64_t Dispatches = 0;
		std::chrono::steady_clock::time_point Touched{};
	};

	// What one AdvanceImageGraphJob call evaluates and where it publishes.
	struct ImageGraphJobRequest {
		ImageGraphGpuDocument &Graph;
		core::Name Published;
		uint64_t Signature = 0;
		uint64_t PublicationSignature = 0;
		size_t Output = 0;
		size_t PublishSlot = 0;
	};

	struct ImageGraphGpuState {
		static constexpr size_t MAXIMUM_GRAPHS = 64;
		static constexpr size_t MAXIMUM_PENDING = 8;
		// Pixel work one job slice records; the next waits for its fence.
		static constexpr uint64_t JOB_SLICE_WORK = uint64_t(256) * 1024 * 1024;
		// Pixel work one resident evaluation may submit at once.
		static constexpr uint64_t RESIDENT_WORK = uint64_t(1024) * 1024 * 1024;
		// A job nobody asked for in this long is abandoned for the next graph.
		static constexpr std::chrono::seconds JOB_IDLE{2};
		// Each node may retire its texture and a blur scratch, plus one publication.
		static constexpr size_t MAXIMUM_RETIRED = imagegraph::Limits::MaximumNodes * 2 + 1;
		struct Pending {
			SDL_GPUFence *Fence = nullptr;
			std::array<SDL_GPUTexture *, MAXIMUM_RETIRED> Retired{};
			std::array<size_t, MAXIMUM_RETIRED> Bytes{};
			size_t Count = 0;
		};
		struct Reservation {
			uint64_t Key = 0;
			bool Inserted = false;
		};
		// Byte and wide intermediate pipelines; Wide is null when rgba16 storage is unsupported.
		SDL_GPUComputePipeline *Compute = nullptr;
		SDL_GPUComputePipeline *Wide = nullptr;
		// Publication passes by target: sRGB RGBA8, linear RGBA8, R16F and RGBA16F.
		std::array<SDL_GPUGraphicsPipeline *, 4> Publish{};
		std::unordered_map<uint64_t, ImageGraphGpuDocument> Documents;
		std::array<Pending, MAXIMUM_PENDING> PendingSubmissions{};
		ImageGraphGpuJob Job;
		SDL_GPUFence *JobFence = nullptr;
		// Set after a job's allocation fails, so the retry does not run every frame.
		std::chrono::steady_clock::time_point JobRetryAt{};
		VulkanTimestamps Timestamps;
		std::array<uint64_t, VulkanTimestamps::SLOTS> TimingSequences{};
		ImageGraphStatistics Profile;
		uint64_t Sequence = 0;
		bool Instrumented = false;

		// A table slot is reserved before recording. Commit cannot allocate and happens
		// only after submit succeeds; cancel removes only an empty newly reserved slot.
		bool Reserve(TextureTable &, core::Name owner, core::Name name, size_t bytes, Reservation &);
		void Cancel(TextureTable &, const Reservation &);
		SDL_GPUTexture *Commit(
			TextureTable &,
			const Reservation &,
			SDL_GPUTexture *,
			const imagegraph::ImageExtent &,
			imagegraph::OutputSpace,
			imagegraph::OutputFormat
		);
		void Touch(TextureTable &, core::Name owner, core::Name name);
		void Collect(SDL_GPUDevice *);
		void AbandonJob(SDL_GPUDevice *);
		void ReleaseDocument(SDL_GPUDevice *, TextureTable &, ImageGraphGpuDocument &);
		void Shutdown(SDL_GPUDevice *, TextureTable &);
	};

	// Native node family with up to four sampled inputs. Writes `rows` rows of
	// one bounded image from `firstRow`, a multiple of eight; zero rows means all of them.
	bool RecordImageGraphNode(
		SDL_GPUCommandBuffer *,
		SDL_GPUComputePipeline *,
		SDL_GPUSampler *,
		const std::array<SDL_GPUTexture *, imagegraph::Limits::MaximumInputs> &,
		SDL_GPUTexture *,
		const ImageGraphGpuUniforms &,
		uint32_t firstRow = 0,
		uint32_t rows = 0
	);
	// Draws an intermediate into a publication target; decode applies only to sRGB RGBA8.
	bool RecordImageGraphPublication(
		SDL_GPUCommandBuffer *,
		SDL_GPUGraphicsPipeline *,
		SDL_GPUSampler *,
		SDL_GPUTexture *,
		SDL_GPUTexture *,
		bool decode
	);
}
