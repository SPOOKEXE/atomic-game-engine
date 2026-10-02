#pragma once
#include "GpuHeap.hpp"
#include "ShaderBinary.hpp"
#include "SourceCamera3D.hpp"
#include "TextureFormatSupport.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/imagegraph/Surface.hpp>
#include <engine/resources/Shaders.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
namespace engine::render::imagegraph::gpu_helpers {
	namespace source = engine::imagegraph;
	inline std::vector<uint8_t> ReadShader(const char *name, resources::ShaderForm form) {
		std::ifstream input(resources::Shader(name, form), std::ios::binary | std::ios::ate);
		if (!input) return {};
		const auto bytes = input.tellg();
		if (bytes <= 0) return {};
		std::vector<uint8_t> code(size_t(bytes), uint8_t{});
		input.seekg(0);
		input.read(reinterpret_cast<char *>(code.data()), code.size());
		return input ? code : std::vector<uint8_t>{};
	}
	inline SDL_GPUShader *Shader(
		SDL_GPUDevice *device,
		const char *name,
		SDL_GPUShaderStage stage,
		uint32_t samplers,
		SourceCamera3DResources &resources,
		uint32_t uniformBuffers = 1
	) {
		const auto binary = ShaderBinaryFor(device);
		const auto code = ReadShader(name, binary.Form);
		if (code.empty()) return nullptr;
		SDL_GPUShaderCreateInfo info{};
		info.code = code.data();
		info.code_size = code.size();
		info.entrypoint = binary.EntryPoint;
		info.format = binary.Format;
		info.stage = stage;
		info.num_samplers = samplers;
		info.num_uniform_buffers =
			stage == SDL_GPU_SHADERSTAGE_VERTEX && std::strstr(name, "post") ? 0 : uniformBuffers;
		auto *shader = SDL_CreateGPUShader(device, &info);
		if (shader) resources.Shaders.push_back(shader);
		return shader;
	}
	inline SDL_GPUTexture *Texture(
		SDL_GPUDevice *device,
		uint32_t width,
		uint32_t height,
		SDL_GPUTextureFormat format,
		SourceCamera3DResources &resources,
		uint32_t layers = 1,
		bool depth = false,
		bool sampleOnly = false
	) {
		SDL_GPUTextureCreateInfo info{};
		info.type = layers == 1 ? SDL_GPU_TEXTURETYPE_2D : SDL_GPU_TEXTURETYPE_2D_ARRAY;
		info.width = width;
		info.height = height;
		info.layer_count_or_depth = layers;
		info.num_levels = 1;
		info.sample_count = SDL_GPU_SAMPLECOUNT_1;
		info.format = format;
		info.usage = depth		  ? SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET
					 : sampleOnly ? SDL_GPU_TEXTUREUSAGE_SAMPLER
								  : SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
		auto *texture = gpu::CreateTexture(device, &info);
		if (texture) resources.Textures.push_back(texture);
		return texture;
	}
	inline SDL_GPUSampler *Sampler(SDL_GPUDevice *device, bool linear, SourceCamera3DResources &resources) {
		SDL_GPUSamplerCreateInfo info{};
		info.min_filter = info.mag_filter = linear ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
		info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
		info.address_mode_u = info.address_mode_v = info.address_mode_w =
			SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		auto *sampler = SDL_CreateGPUSampler(device, &info);
		if (sampler) resources.Samplers.push_back(sampler);
		return sampler;
	}
	inline SDL_GPUTransferBuffer *Transfer(
		SDL_GPUDevice *device,
		uint32_t bytes,
		SDL_GPUTransferBufferUsage usage,
		SourceCamera3DResources &resources
	) {
		SDL_GPUTransferBufferCreateInfo info{};
		info.usage = usage;
		info.size = bytes;
		auto *transfer = gpu::CreateTransferBuffer(device, &info);
		if (transfer) resources.Transfers.push_back(transfer);
		return transfer;
	}
	inline SDL_GPUTexture *Upload(
		SDL_GPUDevice *device,
		SDL_GPUCommandBuffer *command,
		const source::Image *image,
		const glm::vec4 &fallback,
		SourceCamera3DResources &resources
	) {
		const uint32_t width = image ? image->Width : 1, height = image ? image->Height : 1;
		assets::TextureFormat sourceFormat = assets::TextureFormat::RGBA8_LINEAR;
		if (image) switch (image->Format) {
			case source::SurfaceFormat::RGBA8Unorm:
				sourceFormat = assets::TextureFormat::RGBA8_LINEAR;
				break;
			case source::SurfaceFormat::RGBA4Unorm:
				sourceFormat = assets::TextureFormat::RGBA4_UNORM;
				break;
			case source::SurfaceFormat::RGBA16Float:
				sourceFormat = assets::TextureFormat::RGBA16_FLOAT;
				break;
			case source::SurfaceFormat::RGBA32Float:
				sourceFormat = assets::TextureFormat::RGBA32_FLOAT;
				break;
			case source::SurfaceFormat::R8Unorm:
				sourceFormat = assets::TextureFormat::R8;
				break;
			case source::SurfaceFormat::R16Float:
				sourceFormat = assets::TextureFormat::R16_FLOAT;
				break;
			case source::SurfaceFormat::R32Float:
				sourceFormat = assets::TextureFormat::R32_FLOAT;
				break;
			default:
				return nullptr;
			}
		const auto support = engine::render::detail::TextureFormatForUpload(sourceFormat);
		if (!support || !engine::render::detail::SupportsTextureFormat(device, sourceFormat)) return nullptr;
		const uint32_t bytes = uint64_t(width) * height * support->UploadBytesPerPixel;
		auto *texture = Texture(device, width, height, support->DeviceFormat, resources, 1, false, true);
		auto *transfer = Transfer(device, bytes, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, resources);
		if (!texture || !transfer) return nullptr;
		void *mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
		if (!mapped) return nullptr;
		if (image) {
			if (!engine::render::detail::CopyPixelsForUpload(
					sourceFormat,
					std::as_bytes(std::span(image->Pixels)),
					std::span(static_cast<std::byte *>(mapped), bytes)
				)) {
				SDL_UnmapGPUTransferBuffer(device, transfer);
				return nullptr;
			}
		} else
			for (size_t i = 0; i < 4; ++i)
				static_cast<uint8_t *>(mapped)[i] = uint8_t(std::clamp(fallback[i], 0.f, 1.f) * 255);
		SDL_UnmapGPUTransferBuffer(device, transfer);
		auto *copy = SDL_BeginGPUCopyPass(command);
		if (!copy) return nullptr;
		SDL_GPUTextureTransferInfo from{};
		from.transfer_buffer = transfer;
		from.pixels_per_row = width;
		from.rows_per_layer = height;
		SDL_GPUTextureRegion to{};
		to.texture = texture;
		to.w = width;
		to.h = height;
		to.d = 1;
		SDL_UploadToGPUTexture(copy, &from, &to, false);
		core::Metrics::Count("render.imagegraph.upload_bytes", bytes);
		SDL_EndGPUCopyPass(copy);
		resources.CommandReferenced = true;
		return texture;
	}

}
