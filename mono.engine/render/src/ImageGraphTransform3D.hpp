#pragma once

// A small, synchronous SDL GPU pass for a Transform Image 3D request.
//
// This remains private to render until graph scheduling can own submissions.

#include "ImageGraphTransform3DRequest.hpp"

#include <SDL3/SDL_gpu.h>

namespace engine::render::imagegraph {
	// Device state for one live request. It has no download buffers because a live
	// result remains resident until the shared submission fence retires it.
	struct TransformImage3DLiveResources {
		SDL_GPUTexture *Front = nullptr;
		SDL_GPUTexture *Back = nullptr;
		SDL_GPUTexture *Rendered = nullptr;
		SDL_GPUTexture *EncodedDepth = nullptr;
		SDL_GPUTexture *Depth = nullptr;
		SDL_GPUTransferBuffer *FrontUpload = nullptr;
		SDL_GPUTransferBuffer *BackUpload = nullptr;
		SDL_GPUTransferBuffer *VertexUpload = nullptr;
		SDL_GPUBuffer *Vertices = nullptr;
		SDL_GPUSampler *Sampler = nullptr;
		SDL_GPUShader *VertexShader = nullptr;
		SDL_GPUShader *FragmentShader = nullptr;
		SDL_GPUGraphicsPipeline *Pipeline = nullptr;
		bool CommandReferenced = false;
	};

	uint64_t TransformImage3DLiveScratchBytes(const TransformImage3DRequest &request);
	bool RecordTransformImage3DLive(
		SDL_GPUDevice *device,
		SDL_GPUCommandBuffer *command,
		const TransformImage3DRequest &request,
		TransformImage3DLiveResources &resources
	);
	void ReleaseTransformImage3DLive(SDL_GPUDevice *device, TransformImage3DLiveResources &resources);

	class TransformImage3DGpuPass {
	  public:
		explicit TransformImage3DGpuPass(SDL_GPUDevice *device);
		~TransformImage3DGpuPass();

		TransformImage3DGpuPass(const TransformImage3DGpuPass &) = delete;
		TransformImage3DGpuPass &operator=(const TransformImage3DGpuPass &) = delete;

		// Uploads request surfaces, renders them, waits for completion, and reads both outputs.
		TransformImage3DStatus Run(const TransformImage3DRequest &request, TransformImage3DResult &result);

	  private:
		bool CreatePipeline();
		bool CreateResources(const TransformImage3DRequest &request);
		bool UploadAndRecord(const TransformImage3DRequest &request);
		bool Readback(const TransformImage3DRequest &request, TransformImage3DResult &result);
		void Release();

		SDL_GPUDevice *Device = nullptr;
		SDL_GPUTexture *Front = nullptr;
		SDL_GPUTexture *Back = nullptr;
		SDL_GPUTexture *Colour = nullptr;
		SDL_GPUTexture *EncodedDepth = nullptr;
		SDL_GPUTexture *Depth = nullptr;
		SDL_GPUBuffer *Vertices = nullptr;
		SDL_GPUTransferBuffer *FrontUpload = nullptr;
		SDL_GPUTransferBuffer *BackUpload = nullptr;
		SDL_GPUTransferBuffer *VertexUpload = nullptr;
		SDL_GPUTransferBuffer *ColourDownload = nullptr;
		SDL_GPUTransferBuffer *EncodedDepthDownload = nullptr;
		SDL_GPUTransferBuffer *DepthDownload = nullptr;
		SDL_GPUSampler *Sampler = nullptr;
		SDL_GPUShader *VertexShader = nullptr;
		SDL_GPUShader *FragmentShader = nullptr;
		SDL_GPUGraphicsPipeline *Pipeline = nullptr;
		SDL_GPUFence *Fence = nullptr;
	};
}
