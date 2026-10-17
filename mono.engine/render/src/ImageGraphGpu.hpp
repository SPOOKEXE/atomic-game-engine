#pragma once

// Device residency derived from imagegraph documents. Authored values remain with
// their host; these copies and signatures exist only to reuse accepted GPU work.

#include "VulkanTimestamps.hpp"

#include <engine/imagegraph/Document.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/TextureTable.hpp>

#include <SDL3/SDL_gpu.h>

#include <array>
#include <unordered_map>

namespace engine::render {
	struct ImageGraphGpuUniforms {
		std::array<uint32_t, 4> Control{};
		std::array<int32_t, 4> Extents{};
		std::array<int32_t, 4> Region{};
		std::array<float, 4> Colour{};
		std::array<float, 4> Affine{};
		std::array<float, 4> Parameters{};
	};
	static_assert(sizeof(ImageGraphGpuUniforms) == 96);

	struct ImageGraphGpuNode {
		SDL_GPUTexture *Texture = nullptr;
		imagegraph::ImageExtent Extent;
		uint64_t AuthoredSignature = 0;
		uint64_t AcceptedSignature = 0;
	};
	struct ImageGraphGpuPublication {
		core::Name Name;
		SDL_GPUTexture *Texture = nullptr; // Owned by TextureTable after commit.
		imagegraph::ImageExtent Extent;
		imagegraph::OutputSpace Space = imagegraph::OutputSpace::SRGB;
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
	};

	struct ImageGraphGpuState {
		static constexpr size_t MAXIMUM_GRAPHS = 64;
		static constexpr size_t MAXIMUM_PENDING = 8;
		static constexpr size_t MAXIMUM_RETIRED = imagegraph::Limits::MaximumNodes + 1;
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
		SDL_GPUComputePipeline *Compute = nullptr;
		SDL_GPUGraphicsPipeline *Publish = nullptr;
		std::unordered_map<uint64_t, ImageGraphGpuDocument> Documents;
		std::array<Pending, MAXIMUM_PENDING> PendingSubmissions{};
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
			imagegraph::OutputSpace
		);
		void Touch(TextureTable &, core::Name owner, core::Name name);
		void Collect(SDL_GPUDevice *);
		void ReleaseDocument(SDL_GPUDevice *, TextureTable &, ImageGraphGpuDocument &);
		void Shutdown(SDL_GPUDevice *, TextureTable &);
	};

	// Fixed seven-kind native node family. Every invocation writes one bounded image.
	bool RecordImageGraphNode(
		SDL_GPUCommandBuffer *,
		SDL_GPUComputePipeline *,
		SDL_GPUSampler *,
		SDL_GPUTexture *,
		SDL_GPUTexture *,
		SDL_GPUTexture *,
		const ImageGraphGpuUniforms &
	);
	bool RecordImageGraphPublication(
		SDL_GPUCommandBuffer *,
		SDL_GPUGraphicsPipeline *,
		SDL_GPUSampler *,
		SDL_GPUTexture *,
		SDL_GPUTexture *,
		imagegraph::OutputSpace
	);
}
