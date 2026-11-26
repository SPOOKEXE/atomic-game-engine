#include "RenderTypes.hpp"
#include "RendererState.hpp"

#include <engine/assets/Mesh.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/render/raytrace/Bvh.hpp>

#include <SDL3/SDL_gpu.h>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstring>

namespace engine::render {
	namespace {
		// Most mesh trees one call builds: the update budget.
		constexpr uint32_t MESH_BUILD_BUDGET = 8;
		constexpr uint32_t MAXIMUM_LIGHTS = 8;

		struct TracedNode {
			float Low[3];
			uint32_t First;
			float High[3];
			uint32_t Count;
		};
		struct TracedInstance {
			float Rotation[4];
			float Position[3];
			uint32_t Root;
			float Scale[3];
			uint32_t Triangles;
			float Albedo[4];
			float Emission[4];
		};
		struct TraceUniforms {
			float Eye[4], Right[4], Up[4], Forward[4];
			float SunDirection[4], SunColour[4], Sky[4];
			float Settings[4];
			float LightPosition[MAXIMUM_LIGHTS][4];
			float LightColour[MAXIMUM_LIGHTS][4];
			uint32_t Size[4];
		};

		uint64_t Mix(uint64_t hash, uint64_t value) {
			hash ^= value + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2);
			return hash * 0xBF58476D1CE4E5B9ull;
		}
		uint64_t Bits(float value) {
			uint32_t bits = 0;
			std::memcpy(&bits, &value, sizeof(bits));
			return bits;
		}

		// A storage buffer holding `bytes`, filled through one transfer.
		SDL_GPUBuffer *Storage(SDL_GPUDevice *device, SDL_GPUCopyPass *copy, std::vector<SDL_GPUTransferBuffer *> &transfers, const void *data, size_t size) {
			// Never empty: a binding needs a buffer even with nothing in it.
			const size_t bytes = std::max<size_t>(size, 16);
			SDL_GPUBufferCreateInfo info{};
			info.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
			info.size = static_cast<Uint32>(bytes);
			SDL_GPUBuffer *buffer = SDL_CreateGPUBuffer(device, &info);
			SDL_GPUTransferBufferCreateInfo transferInfo{};
			transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
			transferInfo.size = static_cast<Uint32>(bytes);
			SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(device, &transferInfo);
			if (buffer == nullptr || transfer == nullptr) return buffer;
			void *mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
			std::memset(mapped, 0, bytes);
			if (data != nullptr && size > 0) std::memcpy(mapped, data, size);
			SDL_UnmapGPUTransferBuffer(device, transfer);
			const SDL_GPUTransferBufferLocation source{transfer, 0};
			const SDL_GPUBufferRegion destination{buffer, 0, static_cast<Uint32>(bytes)};
			SDL_UploadToGPUBuffer(copy, &source, &destination, false);
			transfers.push_back(transfer);
			return buffer;
		}
	}

	uint32_t Renderer::ReferenceSamples() const {
		return State == nullptr ? 0 : State->Reference.Samples;
	}

	bool Renderer::TraceReference(const View &view, uint32_t samples, std::vector<float> &rgba, uint32_t bounces) {
		if (State == nullptr || State->Device == nullptr || view.Target == nullptr || view.Target->Width == 0 ||
			view.Target->Height == 0)
			return false;
		ENGINE_PROFILE_CAT("reference path trace", core::ProfileCategory::Render);
		auto &trace = State->Reference;
		SDL_GPUDevice *device = State->Device;
		if (trace.Pipeline == nullptr) {
			trace.Pipeline = State->LoadComputePipeline("path-trace.comp", 0, 3, 1, 0, 8, 8);
			if (trace.Pipeline == nullptr) return false;
		}
		const uint32_t width = view.Target->Width, height = view.Target->Height;

		// Mesh trees, within the build budget.
		uint32_t built = 0;
		std::vector<BvhInstance> placed;
		std::vector<const scene::DrawInstance *> sources;
		uint64_t signature = Mix(Mix(0, width), height);
		for (const scene::DrawInstance &instance : view.Instances) {
			if (!instance.Mesh.IsValid()) continue;
			const core::Name owner = view.ContentOwnerOf(instance.SourceWorld);
			const uint64_t key = (uint64_t(owner.Id()) << 32) | instance.Mesh.Id();
			const uint64_t revision = State->Meshes.RevisionOf(instance.Mesh, owner);
			auto &tree = trace.Meshes[key];
			if (tree.Tree == nullptr || tree.Revision != revision) {
				if (built >= MESH_BUILD_BUDGET) continue;
				assets::MeshData data;
				if (State->Meshes.Copy(instance.Mesh, data, 1u << 24, 1u << 26, owner) != MeshCopyStatus::Copied)
					continue;
				std::vector<float> positions;
				positions.reserve(data.Vertices.size() * 3);
				for (const auto &vertex : data.Vertices)
					positions.insert(positions.end(), {vertex.Position[0], vertex.Position[1], vertex.Position[2]});
				tree.Tree = std::make_unique<MeshBvh>();
				tree.Tree->Build(positions, data.Indices);
				tree.Revision = revision;
				++built;
			}
			// Unit meshes are scaled to the part's size.
			const glm::vec3 scale(instance.HalfExtent.X * 2.0f, instance.HalfExtent.Y * 2.0f, instance.HalfExtent.Z * 2.0f);
			placed.push_back({tree.Tree.get(), instance.Frame, scale});
			sources.push_back(&instance);
			for (const float value : {instance.Frame.Position.X, instance.Frame.Position.Y, instance.Frame.Position.Z, scale.x, scale.y, scale.z})
				signature = Mix(signature, Bits(value));
			signature = Mix(signature, key ^ revision);
		}
		SceneBvh scene;
		scene.Build(placed);

		// One node array: the instance tree, then each mesh's tree.
		std::vector<TracedNode> nodes;
		std::vector<float> corners;
		std::unordered_map<const MeshBvh *, std::pair<uint32_t, uint32_t>> offsets;
		const auto pushNodes = [&](const std::vector<BvhNode> &tree) {
			for (const BvhNode &node : tree)
				nodes.push_back({{node.Minimum.x, node.Minimum.y, node.Minimum.z}, node.First, {node.Maximum.x, node.Maximum.y, node.Maximum.z}, node.Count});
		};
		pushNodes(scene.Nodes());
		for (const BvhInstance &instance : placed) {
			if (offsets.contains(instance.Mesh)) continue;
			offsets[instance.Mesh] = {uint32_t(nodes.size()), uint32_t(corners.size() / 12)};
			pushNodes(instance.Mesh->Nodes());
			for (const glm::vec3 &corner : instance.Mesh->Corners())
				corners.insert(corners.end(), {corner.x, corner.y, corner.z, 0.0f});
		}
		// Instances in the instance tree's leaf order, so a leaf names them directly.
		std::vector<TracedInstance> instances;
		for (const uint32_t which : scene.Order()) {
			const scene::DrawInstance &source = *sources[which];
			const BvhInstance &instance = placed[which];
			const glm::quat rotation = instance.Frame.Rotation();
			const auto [root, triangles] = offsets[instance.Mesh];
			const bool mirror = source.Surface >= 0;
			instances.push_back({
				{rotation.x, rotation.y, rotation.z, rotation.w},
				{instance.Frame.Position.X, instance.Frame.Position.Y, instance.Frame.Position.Z},
				root,
				{instance.Scale.x, instance.Scale.y, instance.Scale.z},
				triangles,
				{source.Tint.R * source.SurfaceColour.R, source.Tint.G * source.SurfaceColour.G, source.Tint.B * source.SurfaceColour.B, mirror ? 1.0f : 0.0f},
				{source.EmissiveTint.R * source.EmissiveStrength, source.EmissiveTint.G * source.EmissiveStrength, source.EmissiveTint.B * source.EmissiveStrength, 0.0f},
			});
		}

		// The camera, the sun, the sky and the lights.
		TraceUniforms uniforms{};
		const core::CFrame &camera = view.CameraFrame;
		const core::Vector3 right = camera.RightVector(), up = camera.UpVector(), look = camera.LookVector();
		const float tanHalf = std::tan(view.Camera.FieldOfViewRadians * 0.5f);
		const float aspect = float(width) / float(height);
		const auto set = [](float *out, float x, float y, float z, float w = 0.0f) {
			out[0] = x, out[1] = y, out[2] = z, out[3] = w;
		};
		set(uniforms.Eye, camera.Position.X, camera.Position.Y, camera.Position.Z);
		set(uniforms.Right, right.X * tanHalf * aspect, right.Y * tanHalf * aspect, right.Z * tanHalf * aspect);
		set(uniforms.Up, up.X * tanHalf, up.Y * tanHalf, up.Z * tanHalf);
		set(uniforms.Forward, look.X, look.Y, look.Z);
		const auto &lighting = view.Lighting;
		const glm::vec3 sun = glm::normalize(-glm::vec3(lighting.Direction.X, lighting.Direction.Y, lighting.Direction.Z));
		set(uniforms.SunDirection, sun.x, sun.y, sun.z);
		set(uniforms.SunColour, lighting.Direct.R, lighting.Direct.G, lighting.Direct.B);
		set(uniforms.Sky, lighting.Ambient.R, lighting.Ambient.G, lighting.Ambient.B);
		const uint32_t lightCount = std::min<uint32_t>(uint32_t(view.Lights.size()), MAXIMUM_LIGHTS);
		for (uint32_t index = 0; index < lightCount; ++index) {
			const SceneLight &light = view.Lights[index];
			set(uniforms.LightPosition[index], light.Position.X, light.Position.Y, light.Position.Z, light.Range);
			set(uniforms.LightColour[index], light.Colour.R, light.Colour.G, light.Colour.B);
			signature = Mix(signature, Bits(light.Position.X) ^ Bits(light.Position.Y) << 1 ^ Bits(light.Position.Z) << 2);
		}
		for (const float value : {camera.Position.X, camera.Position.Y, camera.Position.Z, look.X, look.Y, look.Z, sun.x, sun.y, sun.z})
			signature = Mix(signature, Bits(value));

		// The running average restarts with a new picture.
		const bool restart = trace.Accumulation == nullptr || trace.Width != width || trace.Height != height || trace.Signature != signature;
		if (trace.Accumulation == nullptr || trace.Width != width || trace.Height != height) {
			if (trace.Accumulation != nullptr) gpu::ReleaseTexture(device, trace.Accumulation);
			SDL_GPUTextureCreateInfo info{};
			info.type = SDL_GPU_TEXTURETYPE_2D;
			info.format = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
			info.usage = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE |
						 SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
			info.width = width;
			info.height = height;
			info.layer_count_or_depth = 1;
			info.num_levels = 1;
			info.sample_count = SDL_GPU_SAMPLECOUNT_1;
			trace.Accumulation = gpu::CreateTexture(device, &info);
			trace.Width = width;
			trace.Height = height;
			if (trace.Accumulation == nullptr) return false;
		}
		if (restart) trace.Samples = 0;
		trace.Signature = signature;
		set(uniforms.Settings, float(trace.Frame++), float(samples), float(bounces), float(lightCount));
		uniforms.Size[0] = width;
		uniforms.Size[1] = height;

		SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(device);
		if (command == nullptr) return false;
		if (restart) {
			SDL_GPUColorTargetInfo clear{};
			clear.texture = trace.Accumulation;
			clear.load_op = SDL_GPU_LOADOP_CLEAR;
			clear.store_op = SDL_GPU_STOREOP_STORE;
			SDL_EndGPURenderPass(SDL_BeginGPURenderPass(command, &clear, 1, nullptr));
		}
		std::vector<SDL_GPUTransferBuffer *> transfers;
		SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);
		SDL_GPUBuffer *buffers[3] = {
			Storage(device, copy, transfers, nodes.data(), nodes.size() * sizeof(TracedNode)),
			Storage(device, copy, transfers, corners.data(), corners.size() * sizeof(float)),
			Storage(device, copy, transfers, instances.data(), instances.size() * sizeof(TracedInstance)),
		};
		SDL_EndGPUCopyPass(copy);
		const SDL_GPUStorageTextureReadWriteBinding target{.texture = trace.Accumulation};
		SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(command, &target, 1, nullptr, 0);
		SDL_BindGPUComputePipeline(pass, trace.Pipeline);
		SDL_BindGPUComputeStorageBuffers(pass, 0, buffers, 3);
		SDL_PushGPUComputeUniformData(command, 0, &uniforms, sizeof(uniforms));
		if (!instances.empty()) SDL_DispatchGPUCompute(pass, (width + 7) / 8, (height + 7) / 8, 1);
		SDL_EndGPUComputePass(pass);

		// Read the average back.
		const size_t bytes = size_t(width) * height * 16;
		SDL_GPUTransferBufferCreateInfo download{};
		download.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
		download.size = static_cast<Uint32>(bytes);
		SDL_GPUTransferBuffer *readback = SDL_CreateGPUTransferBuffer(device, &download);
		SDL_GPUCopyPass *back = SDL_BeginGPUCopyPass(command);
		SDL_GPUTextureRegion region{};
		region.texture = trace.Accumulation;
		region.w = width;
		region.h = height;
		region.d = 1;
		SDL_GPUTextureTransferInfo destination{};
		destination.transfer_buffer = readback;
		destination.pixels_per_row = width;
		destination.rows_per_layer = height;
		SDL_DownloadFromGPUTexture(back, &region, &destination);
		SDL_EndGPUCopyPass(back);
		SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
		if (fence != nullptr) {
			SDL_WaitForGPUFences(device, true, &fence, 1);
			SDL_ReleaseGPUFence(device, fence);
		}
		for (SDL_GPUTransferBuffer *transfer : transfers)
			SDL_ReleaseGPUTransferBuffer(device, transfer);
		for (SDL_GPUBuffer *buffer : buffers)
			if (buffer != nullptr) SDL_ReleaseGPUBuffer(device, buffer);
		if (!instances.empty()) trace.Samples += samples;
		rgba.assign(size_t(width) * height * 4, 0.0f);
		if (const auto *pixels = static_cast<const float *>(SDL_MapGPUTransferBuffer(device, readback, false))) {
			for (size_t index = 0; index < size_t(width) * height; ++index) {
				const float count = std::max(pixels[index * 4 + 3], 1.0f);
				for (int channel = 0; channel < 3; ++channel)
					rgba[index * 4 + channel] = pixels[index * 4 + channel] / count;
				rgba[index * 4 + 3] = 1.0f;
			}
			SDL_UnmapGPUTransferBuffer(device, readback);
		}
		SDL_ReleaseGPUTransferBuffer(device, readback);
		return true;
	}
}
