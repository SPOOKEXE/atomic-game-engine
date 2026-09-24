#include "ImageGraphTransform3D.hpp"

#include "GpuHeap.hpp"
#include "ShaderBinary.hpp"

#include <engine/render/Renderer.hpp>
#include <engine/resources/Shaders.hpp>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstring>
#include <fstream>
#include <span>

namespace engine::render::imagegraph {
	namespace {
		struct Vertex {
			glm::vec3 Position;
			glm::vec2 Uv;
		};
		struct FragmentUniforms {
			glm::vec2 Tiling;
			glm::vec2 DepthRange;
			uint32_t UseBackSurface;
		};

		std::vector<uint8_t> ReadShader(const char *name, resources::ShaderForm form) {
			std::ifstream input(resources::Shader(name, form), std::ios::binary | std::ios::ate);
			if (!input) return {};
			const auto size = input.tellg();
			if (size <= 0) return {};
			std::vector<uint8_t> code(static_cast<size_t>(size));
			input.seekg(0);
			input.read(reinterpret_cast<char *>(code.data()), static_cast<std::streamsize>(code.size()));
			return input ? code : std::vector<uint8_t>{};
		}

		SDL_GPUTexture *
		Texture(SDL_GPUDevice *device, uint32_t width, uint32_t height, SDL_GPUTextureUsageFlags usage) {
			SDL_GPUTextureCreateInfo info{};
			info.type = SDL_GPU_TEXTURETYPE_2D;
			info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
			info.usage = usage;
			info.width = width;
			info.height = height;
			info.layer_count_or_depth = 1;
			info.num_levels = 1;
			info.sample_count = SDL_GPU_SAMPLECOUNT_1;
			return gpu::CreateTexture(device, &info);
		}

		SDL_GPUTransferBuffer *
		Transfer(SDL_GPUDevice *device, uint32_t bytes, SDL_GPUTransferBufferUsage usage) {
			SDL_GPUTransferBufferCreateInfo info{};
			info.usage = usage;
			info.size = bytes;
			return gpu::CreateTransferBuffer(device, &info);
		}

		glm::mat4 MatrixFor(const TransformImage3DRequest &request) {
			glm::mat4 model(1);
			model = glm::translate(
				model, glm::vec3(request.Position[0], request.Position[1], request.Position[2])
			);
			const double rotationLength = std::sqrt(
				double(request.Rotation[0]) * request.Rotation[0] +
				double(request.Rotation[1]) * request.Rotation[1] +
				double(request.Rotation[2]) * request.Rotation[2] +
				double(request.Rotation[3]) * request.Rotation[3]
			);
			model *= glm::mat4_cast(
				glm::quat(
					float(request.Rotation[3] / rotationLength),
					float(request.Rotation[0] / rotationLength),
					float(request.Rotation[1] / rotationLength),
					float(request.Rotation[2] / rotationLength)
				)
			);
			model = glm::scale(model, glm::vec3(request.Scale[0], request.Scale[1], request.Scale[2]));
			// Pixel Composer applies anchor before scale and rotation, then translates.
			model =
				glm::translate(model, glm::vec3(-request.Anchor[0], -request.Anchor[1], -request.Anchor[2]));
			if (request.Projection == TransformImage3DProjection::Orthographic)
				return glm::orthoRH_ZO(-1.f, 1.f, -1.f, 1.f, request.ViewRange[0], request.ViewRange[1]) *
					   model;
			const float aspect = static_cast<float>(request.Front.Width) / request.Front.Height;
			return glm::perspectiveRH_ZO(
					   glm::radians(request.FieldOfViewDegrees),
					   aspect,
					   request.ViewRange[0],
					   request.ViewRange[1]
				   ) *
				   model;
		}

		bool CopyToTransfer(
			SDL_GPUDevice *device, SDL_GPUTransferBuffer *transfer, std::span<const std::byte> source
		) {
			void *mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
			if (mapped == nullptr) return false;
			std::memcpy(mapped, source.data(), source.size());
			SDL_UnmapGPUTransferBuffer(device, transfer);
			return true;
		}

		constexpr std::array<Vertex, 6> QUAD{{
			{{-1, -1, 0}, {0, 1}},
			{{1, -1, 0}, {1, 1}},
			{{1, 1, 0}, {1, 0}},
			{{-1, -1, 0}, {0, 1}},
			{{1, 1, 0}, {1, 0}},
			{{-1, 1, 0}, {0, 0}},
		}};
	}

	uint64_t TransformImage3DLiveScratchBytes(const TransformImage3DRequest &request) {
		const uint64_t pixels = uint64_t(request.Front.Width) * request.Front.Height;
		// Two sampled sources, two colour outputs, depth, and two upload copies.
		return pixels * 28 + sizeof(QUAD) * 2;
	}

	void ReleaseTransformImage3DLive(SDL_GPUDevice *device, TransformImage3DLiveResources &resources) {
		if (device == nullptr) return;
		if (resources.Pipeline) SDL_ReleaseGPUGraphicsPipeline(device, resources.Pipeline);
		if (resources.VertexShader) SDL_ReleaseGPUShader(device, resources.VertexShader);
		if (resources.FragmentShader) SDL_ReleaseGPUShader(device, resources.FragmentShader);
		if (resources.Sampler) SDL_ReleaseGPUSampler(device, resources.Sampler);
		gpu::ReleaseTransferBuffer(device, resources.VertexUpload);
		gpu::ReleaseTransferBuffer(device, resources.BackUpload);
		gpu::ReleaseTransferBuffer(device, resources.FrontUpload);
		gpu::ReleaseBuffer(device, resources.Vertices);
		gpu::ReleaseTexture(device, resources.Depth);
		gpu::ReleaseTexture(device, resources.EncodedDepth);
		gpu::ReleaseTexture(device, resources.Rendered);
		gpu::ReleaseTexture(device, resources.Back);
		gpu::ReleaseTexture(device, resources.Front);
		resources = {};
	}

	bool RecordTransformImage3DLive(
		SDL_GPUDevice *device,
		SDL_GPUCommandBuffer *command,
		const TransformImage3DRequest &request,
		TransformImage3DLiveResources &resources
	) {
		if (device == nullptr || command == nullptr ||
			ValidateTransformImage3D(request) != TransformImage3DStatus::Ok) {
			return false;
		}
		ReleaseTransformImage3DLive(device, resources);
		const ShaderBinary binary = ShaderBinaryFor(device);
		const std::vector<uint8_t> vertexCode = ReadShader("imagegraph-transform-3d.vert", binary.Form);
		const std::vector<uint8_t> fragmentCode = ReadShader("imagegraph-transform-3d.frag", binary.Form);
		if (vertexCode.empty() || fragmentCode.empty()) return false;

		SDL_GPUShaderCreateInfo vertexInfo{};
		vertexInfo.code = vertexCode.data();
		vertexInfo.code_size = vertexCode.size();
		vertexInfo.entrypoint = binary.EntryPoint;
		vertexInfo.format = binary.Format;
		vertexInfo.stage = SDL_GPU_SHADERSTAGE_VERTEX;
		vertexInfo.num_uniform_buffers = 1;
		resources.VertexShader = SDL_CreateGPUShader(device, &vertexInfo);
		SDL_GPUShaderCreateInfo fragmentInfo{};
		fragmentInfo.code = fragmentCode.data();
		fragmentInfo.code_size = fragmentCode.size();
		fragmentInfo.entrypoint = binary.EntryPoint;
		fragmentInfo.format = binary.Format;
		fragmentInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
		fragmentInfo.num_samplers = 2;
		fragmentInfo.num_uniform_buffers = 1;
		resources.FragmentShader = SDL_CreateGPUShader(device, &fragmentInfo);
		SDL_GPUVertexBufferDescription buffer{};
		buffer.slot = 0;
		buffer.pitch = sizeof(Vertex);
		buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
		SDL_GPUVertexAttribute attributes[2]{
			{0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(Vertex, Position)},
			{1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(Vertex, Uv)},
		};
		SDL_GPUColorTargetDescription targets[2]{};
		targets[0].format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		targets[1].format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		SDL_GPUGraphicsPipelineCreateInfo pipelineInfo{};
		pipelineInfo.vertex_shader = resources.VertexShader;
		pipelineInfo.fragment_shader = resources.FragmentShader;
		pipelineInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		pipelineInfo.vertex_input_state.vertex_buffer_descriptions = &buffer;
		pipelineInfo.vertex_input_state.num_vertex_buffers = 1;
		pipelineInfo.vertex_input_state.vertex_attributes = attributes;
		pipelineInfo.vertex_input_state.num_vertex_attributes = 2;
		pipelineInfo.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
		pipelineInfo.depth_stencil_state.enable_depth_test = true;
		pipelineInfo.depth_stencil_state.enable_depth_write = true;
		pipelineInfo.target_info.color_target_descriptions = targets;
		pipelineInfo.target_info.num_color_targets = 2;
		pipelineInfo.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
		pipelineInfo.target_info.has_depth_stencil_target = true;
		resources.Pipeline = SDL_CreateGPUGraphicsPipeline(device, &pipelineInfo);

		const uint32_t bytes = request.Front.Width * request.Front.Height * 4;
		resources.Front =
			Texture(device, request.Front.Width, request.Front.Height, SDL_GPU_TEXTUREUSAGE_SAMPLER);
		resources.Back =
			Texture(device, request.Front.Width, request.Front.Height, SDL_GPU_TEXTUREUSAGE_SAMPLER);
		// The completed output is adopted by TextureTable after its submission
		// fence. It must remain both a render target here and sampleable there.
		resources.Rendered = Texture(
			device,
			request.Front.Width,
			request.Front.Height,
			SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER
		);
		resources.EncodedDepth =
			Texture(device, request.Front.Width, request.Front.Height, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
		SDL_GPUTextureCreateInfo depth{};
		depth.type = SDL_GPU_TEXTURETYPE_2D;
		depth.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
		depth.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
		depth.width = request.Front.Width;
		depth.height = request.Front.Height;
		depth.layer_count_or_depth = 1;
		depth.num_levels = 1;
		depth.sample_count = SDL_GPU_SAMPLECOUNT_1;
		resources.Depth = gpu::CreateTexture(device, &depth);
		resources.FrontUpload = Transfer(device, bytes, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD);
		resources.BackUpload = Transfer(device, bytes, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD);
		SDL_GPUBufferCreateInfo vertexInfoBuffer{};
		vertexInfoBuffer.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
		vertexInfoBuffer.size = sizeof(QUAD);
		resources.Vertices = gpu::CreateBuffer(device, &vertexInfoBuffer);
		resources.VertexUpload = Transfer(device, sizeof(QUAD), SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD);
		SDL_GPUSamplerCreateInfo sampler{};
		sampler.min_filter = SDL_GPU_FILTER_NEAREST;
		sampler.mag_filter = SDL_GPU_FILTER_NEAREST;
		sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
		sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		resources.Sampler = SDL_CreateGPUSampler(device, &sampler);
		if (!resources.VertexShader || !resources.FragmentShader || !resources.Pipeline || !resources.Front ||
			!resources.Back || !resources.Rendered || !resources.EncodedDepth || !resources.Depth ||
			!resources.FrontUpload || !resources.BackUpload || !resources.Vertices ||
			!resources.VertexUpload || !resources.Sampler ||
			!CopyToTransfer(device, resources.FrontUpload, request.Front.Rgba8) ||
			!CopyToTransfer(
				device,
				resources.BackUpload,
				request.Back.Rgba8.empty() ? std::span(request.Front.Rgba8) : std::span(request.Back.Rgba8)
			) ||
			!CopyToTransfer(device, resources.VertexUpload, std::as_bytes(std::span(QUAD)))) {
			ReleaseTransformImage3DLive(device, resources);
			return false;
		}

		SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);
		if (copy == nullptr) {
			ReleaseTransformImage3DLive(device, resources);
			return false;
		}
		const auto upload = [copy, &request](SDL_GPUTransferBuffer *source, SDL_GPUTexture *destination) {
			SDL_GPUTextureTransferInfo from{};
			from.transfer_buffer = source;
			from.pixels_per_row = request.Front.Width;
			from.rows_per_layer = request.Front.Height;
			SDL_GPUTextureRegion to{};
			to.texture = destination;
			to.w = request.Front.Width;
			to.h = request.Front.Height;
			to.d = 1;
			SDL_UploadToGPUTexture(copy, &from, &to, false);
		};
		upload(resources.FrontUpload, resources.Front);
		upload(resources.BackUpload, resources.Back);
		SDL_GPUTransferBufferLocation vertexSource{};
		vertexSource.transfer_buffer = resources.VertexUpload;
		SDL_GPUBufferRegion vertexTarget{};
		vertexTarget.buffer = resources.Vertices;
		vertexTarget.size = sizeof(QUAD);
		SDL_UploadToGPUBuffer(copy, &vertexSource, &vertexTarget, false);
		SDL_EndGPUCopyPass(copy);
		resources.CommandReferenced = true;
		SDL_GPUColorTargetInfo colourTargets[2]{};
		for (SDL_GPUColorTargetInfo &target : colourTargets) {
			target.load_op = SDL_GPU_LOADOP_CLEAR;
			target.store_op = SDL_GPU_STOREOP_STORE;
		}
		colourTargets[0].texture = resources.Rendered;
		colourTargets[1].texture = resources.EncodedDepth;
		SDL_GPUDepthStencilTargetInfo depthTarget{};
		depthTarget.texture = resources.Depth;
		depthTarget.clear_depth = 1;
		depthTarget.load_op = SDL_GPU_LOADOP_CLEAR;
		depthTarget.store_op = SDL_GPU_STOREOP_STORE;
		depthTarget.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
		depthTarget.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
		SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, colourTargets, 2, &depthTarget);
		if (pass == nullptr) return false;
		SDL_BindGPUGraphicsPipeline(pass, resources.Pipeline);
		SDL_GPUBufferBinding vertexBinding{};
		vertexBinding.buffer = resources.Vertices;
		SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
		const SDL_GPUTextureSamplerBinding samplers[]{
			{resources.Front, resources.Sampler}, {resources.Back, resources.Sampler}
		};
		SDL_BindGPUFragmentSamplers(pass, 0, samplers, 2);
		const glm::mat4 matrix = MatrixFor(request);
		const FragmentUniforms uniforms{
			{request.TextureTiling[0], request.TextureTiling[1]},
			{request.DepthRange[0], request.DepthRange[1]},
			request.Back.Rgba8.empty() ? 0u : 1u
		};
		SDL_PushGPUVertexUniformData(command, 0, &matrix, sizeof(matrix));
		SDL_PushGPUFragmentUniformData(command, 0, &uniforms, sizeof(uniforms));
		SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
		SDL_EndGPURenderPass(pass);
		return true;
	}

	TransformImage3DGpuPass::TransformImage3DGpuPass(SDL_GPUDevice *device) : Device(device) {}
	TransformImage3DGpuPass::~TransformImage3DGpuPass() {
		Release();
	}

	bool TransformImage3DGpuPass::CreatePipeline() {
		const ShaderBinary binary = ShaderBinaryFor(Device);
		const std::vector<uint8_t> vertexCode = ReadShader("imagegraph-transform-3d.vert", binary.Form);
		const std::vector<uint8_t> fragmentCode = ReadShader("imagegraph-transform-3d.frag", binary.Form);
		if (vertexCode.empty() || fragmentCode.empty()) return false;
		SDL_GPUShaderCreateInfo vertexInfo{};
		vertexInfo.code = vertexCode.data();
		vertexInfo.code_size = vertexCode.size();
		vertexInfo.entrypoint = binary.EntryPoint;
		vertexInfo.format = binary.Format;
		vertexInfo.stage = SDL_GPU_SHADERSTAGE_VERTEX;
		vertexInfo.num_uniform_buffers = 1;
		VertexShader = SDL_CreateGPUShader(Device, &vertexInfo);
		SDL_GPUShaderCreateInfo fragmentInfo{};
		fragmentInfo.code = fragmentCode.data();
		fragmentInfo.code_size = fragmentCode.size();
		fragmentInfo.entrypoint = binary.EntryPoint;
		fragmentInfo.format = binary.Format;
		fragmentInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
		fragmentInfo.num_samplers = 2;
		fragmentInfo.num_uniform_buffers = 1;
		FragmentShader = SDL_CreateGPUShader(Device, &fragmentInfo);
		if (VertexShader == nullptr || FragmentShader == nullptr) return false;
		SDL_GPUVertexBufferDescription buffer{};
		buffer.slot = 0;
		buffer.pitch = sizeof(Vertex);
		buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
		SDL_GPUVertexAttribute attributes[2]{};
		attributes[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(Vertex, Position)};
		attributes[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(Vertex, Uv)};
		SDL_GPUColorTargetDescription targets[2]{};
		targets[0].format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		targets[1].format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		SDL_GPUGraphicsPipelineCreateInfo info{};
		info.vertex_shader = VertexShader;
		info.fragment_shader = FragmentShader;
		info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		info.vertex_input_state.vertex_buffer_descriptions = &buffer;
		info.vertex_input_state.num_vertex_buffers = 1;
		info.vertex_input_state.vertex_attributes = attributes;
		info.vertex_input_state.num_vertex_attributes = 2;
		info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
		info.depth_stencil_state.enable_depth_test = true;
		info.depth_stencil_state.enable_depth_write = true;
		info.target_info.color_target_descriptions = targets;
		info.target_info.num_color_targets = 2;
		info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
		info.target_info.has_depth_stencil_target = true;
		Pipeline = SDL_CreateGPUGraphicsPipeline(Device, &info);
		return Pipeline != nullptr;
	}

	bool TransformImage3DGpuPass::CreateResources(const TransformImage3DRequest &request) {
		const uint32_t bytes = request.Front.Width * request.Front.Height * 4;
		Front = Texture(Device, request.Front.Width, request.Front.Height, SDL_GPU_TEXTUREUSAGE_SAMPLER);
		Back = Texture(Device, request.Front.Width, request.Front.Height, SDL_GPU_TEXTUREUSAGE_SAMPLER);
		Colour =
			Texture(Device, request.Front.Width, request.Front.Height, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
		EncodedDepth =
			Texture(Device, request.Front.Width, request.Front.Height, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
		SDL_GPUTextureCreateInfo depth{};
		depth.type = SDL_GPU_TEXTURETYPE_2D;
		depth.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
		depth.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
		depth.width = request.Front.Width;
		depth.height = request.Front.Height;
		depth.layer_count_or_depth = 1;
		depth.num_levels = 1;
		depth.sample_count = SDL_GPU_SAMPLECOUNT_1;
		Depth = gpu::CreateTexture(Device, &depth);
		FrontUpload = Transfer(Device, bytes, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD);
		BackUpload = Transfer(Device, bytes, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD);
		SDL_GPUBufferCreateInfo vertexInfo{};
		vertexInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
		vertexInfo.size = sizeof(QUAD);
		Vertices = gpu::CreateBuffer(Device, &vertexInfo);
		VertexUpload = Transfer(Device, sizeof(QUAD), SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD);
		ColourDownload = Transfer(Device, bytes, SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD);
		EncodedDepthDownload = Transfer(Device, bytes, SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD);
		DepthDownload = Transfer(Device, bytes, SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD);
		SDL_GPUSamplerCreateInfo sampler{};
		sampler.min_filter = SDL_GPU_FILTER_NEAREST;
		sampler.mag_filter = SDL_GPU_FILTER_NEAREST;
		sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
		sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		Sampler = SDL_CreateGPUSampler(Device, &sampler);
		return Front && Back && Colour && EncodedDepth && Depth && FrontUpload && BackUpload && Vertices &&
			   VertexUpload && ColourDownload && EncodedDepthDownload && DepthDownload && Sampler;
	}

	bool TransformImage3DGpuPass::UploadAndRecord(const TransformImage3DRequest &request) {
		if (!CopyToTransfer(Device, FrontUpload, request.Front.Rgba8)) return false;
		if (!CopyToTransfer(
				Device,
				BackUpload,
				request.Back.Rgba8.empty() ? std::span(request.Front.Rgba8) : std::span(request.Back.Rgba8)
			))
			return false;
		if (!CopyToTransfer(Device, VertexUpload, std::as_bytes(std::span(QUAD)))) return false;
		SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(Device);
		if (command == nullptr) return false;
		SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);
		if (copy == nullptr) {
			SDL_CancelGPUCommandBuffer(command);
			return false;
		}
		const auto upload = [copy, &request](SDL_GPUTransferBuffer *source, SDL_GPUTexture *destination) {
			SDL_GPUTextureTransferInfo from{};
			from.transfer_buffer = source;
			from.pixels_per_row = request.Front.Width;
			from.rows_per_layer = request.Front.Height;
			SDL_GPUTextureRegion to{};
			to.texture = destination;
			to.w = request.Front.Width;
			to.h = request.Front.Height;
			to.d = 1;
			SDL_UploadToGPUTexture(copy, &from, &to, false);
		};
		upload(FrontUpload, Front);
		upload(BackUpload, Back);
		SDL_GPUTransferBufferLocation vertexSource{};
		vertexSource.transfer_buffer = VertexUpload;
		SDL_GPUBufferRegion vertexTarget{};
		vertexTarget.buffer = Vertices;
		vertexTarget.size = sizeof(QUAD);
		SDL_UploadToGPUBuffer(copy, &vertexSource, &vertexTarget, false);
		SDL_EndGPUCopyPass(copy);
		SDL_GPUColorTargetInfo colourTargets[2]{};
		for (SDL_GPUColorTargetInfo &target : colourTargets) {
			target.load_op = SDL_GPU_LOADOP_CLEAR;
			target.store_op = SDL_GPU_STOREOP_STORE;
		}
		colourTargets[0].texture = Colour;
		colourTargets[1].texture = EncodedDepth;
		SDL_GPUDepthStencilTargetInfo depthTarget{};
		depthTarget.texture = Depth;
		depthTarget.clear_depth = 1;
		depthTarget.load_op = SDL_GPU_LOADOP_CLEAR;
		depthTarget.store_op = SDL_GPU_STOREOP_STORE;
		depthTarget.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
		depthTarget.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
		SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, colourTargets, 2, &depthTarget);
		if (pass == nullptr) {
			SDL_CancelGPUCommandBuffer(command);
			return false;
		}
		SDL_BindGPUGraphicsPipeline(pass, Pipeline);
		SDL_GPUBufferBinding vertexBinding{};
		vertexBinding.buffer = Vertices;
		SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
		const SDL_GPUTextureSamplerBinding samplers[]{{Front, Sampler}, {Back, Sampler}};
		SDL_BindGPUFragmentSamplers(pass, 0, samplers, 2);
		const glm::mat4 matrix = MatrixFor(request);
		const FragmentUniforms uniforms{
			{request.TextureTiling[0], request.TextureTiling[1]},
			{request.DepthRange[0], request.DepthRange[1]},
			request.Back.Rgba8.empty() ? 0u : 1u
		};
		SDL_PushGPUVertexUniformData(command, 0, &matrix, sizeof(matrix));
		SDL_PushGPUFragmentUniformData(command, 0, &uniforms, sizeof(uniforms));
		SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
		SDL_EndGPURenderPass(pass);
		copy = SDL_BeginGPUCopyPass(command);
		if (copy == nullptr) {
			SDL_CancelGPUCommandBuffer(command);
			return false;
		}
		const auto download = [copy, &request](SDL_GPUTexture *source, SDL_GPUTransferBuffer *destination) {
			SDL_GPUTextureRegion from{};
			from.texture = source;
			from.w = request.Front.Width;
			from.h = request.Front.Height;
			from.d = 1;
			SDL_GPUTextureTransferInfo to{};
			to.transfer_buffer = destination;
			to.pixels_per_row = request.Front.Width;
			to.rows_per_layer = request.Front.Height;
			SDL_DownloadFromGPUTexture(copy, &from, &to);
		};
		download(Colour, ColourDownload);
		download(EncodedDepth, EncodedDepthDownload);
		download(Depth, DepthDownload);
		SDL_EndGPUCopyPass(copy);
		Fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
		return Fence != nullptr;
	}

	bool TransformImage3DGpuPass::Readback(
		const TransformImage3DRequest &request, TransformImage3DResult &result
	) {
		if (!SDL_WaitForGPUFences(Device, true, &Fence, 1)) return false;
		const size_t bytes = static_cast<size_t>(request.Front.Width) * request.Front.Height * 4;
		const auto read = [this,
						   bytes](SDL_GPUTransferBuffer *transfer, std::vector<std::byte> &destination) {
			const void *mapped = SDL_MapGPUTransferBuffer(Device, transfer, false);
			if (mapped == nullptr) return false;
			destination.resize(bytes);
			std::memcpy(destination.data(), mapped, bytes);
			SDL_UnmapGPUTransferBuffer(Device, transfer);
			return true;
		};
		if (!read(ColourDownload, result.RenderedRgba8) || !read(EncodedDepthDownload, result.DepthRgba8))
			return false;
		const void *mapped = SDL_MapGPUTransferBuffer(Device, DepthDownload, false);
		if (mapped == nullptr) return false;
		result.Depth.resize(bytes / sizeof(float));
		std::memcpy(result.Depth.data(), mapped, bytes);
		SDL_UnmapGPUTransferBuffer(Device, DepthDownload);
		result.Width = request.Front.Width;
		result.Height = request.Front.Height;
		return true;
	}

	TransformImage3DStatus
	TransformImage3DGpuPass::Run(const TransformImage3DRequest &request, TransformImage3DResult &result) {
		result = {};
		const TransformImage3DStatus valid = ValidateTransformImage3D(request);
		if (valid != TransformImage3DStatus::Ok) return valid;
		Release();
		if (Device == nullptr || !CreatePipeline() || !CreateResources(request) ||
			!UploadAndRecord(request) || !Readback(request, result)) {
			Release();
			return TransformImage3DStatus::GpuUnavailable;
		}
		return TransformImage3DStatus::Ok;
	}

	TransformImage3DStatus ExecuteTransformImage3D(
		Renderer &renderer, const TransformImage3DRequest &request, TransformImage3DResult &result
	) {
		const auto backend = renderer.Backend();
		auto *device = static_cast<SDL_GPUDevice *>(backend.Device);
		if (device == nullptr) return TransformImage3DStatus::GpuUnavailable;
		TransformImage3DGpuPass pass(device);
		const TransformImage3DStatus status = pass.Run(request, result);
		if (status != TransformImage3DStatus::Ok) return status;
		result.Mesh.Positions = {-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, -1, 0, 1, 1, 0, -1, 1, 0};
		result.Mesh.TextureCoordinates = {0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0};
		return status;
	}

	void TransformImage3DGpuPass::Release() {
		if (Device == nullptr) return;
		if (Fence) SDL_ReleaseGPUFence(Device, Fence);
		if (Pipeline) SDL_ReleaseGPUGraphicsPipeline(Device, Pipeline);
		if (VertexShader) SDL_ReleaseGPUShader(Device, VertexShader);
		if (FragmentShader) SDL_ReleaseGPUShader(Device, FragmentShader);
		if (Sampler) SDL_ReleaseGPUSampler(Device, Sampler);
		gpu::ReleaseTransferBuffer(Device, DepthDownload);
		gpu::ReleaseTransferBuffer(Device, EncodedDepthDownload);
		gpu::ReleaseTransferBuffer(Device, ColourDownload);
		gpu::ReleaseTransferBuffer(Device, VertexUpload);
		gpu::ReleaseTransferBuffer(Device, BackUpload);
		gpu::ReleaseTransferBuffer(Device, FrontUpload);
		gpu::ReleaseBuffer(Device, Vertices);
		gpu::ReleaseTexture(Device, Depth);
		gpu::ReleaseTexture(Device, EncodedDepth);
		gpu::ReleaseTexture(Device, Colour);
		gpu::ReleaseTexture(Device, Back);
		gpu::ReleaseTexture(Device, Front);
		Front = Back = Colour = EncodedDepth = Depth = nullptr;
		Vertices = nullptr;
		FrontUpload = BackUpload = VertexUpload = ColourDownload = EncodedDepthDownload = DepthDownload =
			nullptr;
		Sampler = nullptr;
		VertexShader = FragmentShader = nullptr;
		Pipeline = nullptr;
		Fence = nullptr;
	}
}
