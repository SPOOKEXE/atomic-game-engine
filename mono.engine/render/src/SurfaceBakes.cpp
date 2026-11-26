#include "RenderTypes.hpp"
#include "RendererState.hpp"
#include "resources/MaterialSamplerAdmission.hpp"

#include <engine/assets/ContentHash.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/scene/content/TileSets.hpp>

#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <engine/core/Metrics.hpp>
#include <cmath>
#include <string>

namespace engine::render {
	namespace {
		// The five targets a bake writes; see `SURFACE_BAKE`.
		constexpr size_t BAKE_TARGETS = 5;
	}

	bool Renderer::Impl::AddBakeVariant(const core::Name &name, core::Name owner, std::span<const uint32_t> spirv) {
		if (!VariantsReady || BakeVertexShader == nullptr || spirv.empty()) return false;
		const auto hash = assets::Hasher::Of(std::as_bytes(spirv));
		auto &held = BakePipelines[ShaderVariantKey(name, owner)];
		if (held.Pipeline != nullptr && held.CodeHash == hash) return true;
		SDL_GPUShaderCreateInfo info{};
		info.code = reinterpret_cast<const Uint8 *>(spirv.data());
		info.code_size = spirv.size() * sizeof(uint32_t);
		info.entrypoint = Binary.EntryPoint;
		info.format = Binary.Format;
		info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
		info.num_samplers = SURFACE_FRAGMENT_SAMPLERS;
		info.num_uniform_buffers = 3;
		SDL_GPUShader *fragment = Binary.Form == resources::ShaderForm::Msl ? nullptr : SDL_CreateGPUShader(Device, &info);
		if (fragment == nullptr) {
			ENGINE_ERROR("bake '{}': {}", name.Text(), SDL_GetError());
			return false;
		}
		std::array<SDL_GPUColorTargetDescription, BAKE_TARGETS> targets{};
		for (auto &target : targets)
			target.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
		SDL_GPUGraphicsPipelineCreateInfo pipeline = VariantOpaqueInfo;
		pipeline.vertex_shader = BakeVertexShader;
		pipeline.fragment_shader = fragment;
		pipeline.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
		pipeline.depth_stencil_state = SDL_GPUDepthStencilState{};
		pipeline.target_info.has_depth_stencil_target = false;
		pipeline.target_info.color_target_descriptions = targets.data();
		pipeline.target_info.num_color_targets = static_cast<Uint32>(targets.size());
		SDL_GPUGraphicsPipeline *built = SDL_CreateGPUGraphicsPipeline(Device, &pipeline);
		SDL_ReleaseGPUShader(Device, fragment);
		if (built == nullptr) {
			ENGINE_ERROR("bake '{}' pipeline: {}", name.Text(), SDL_GetError());
			return false;
		}
		if (held.Pipeline != nullptr) SDL_ReleaseGPUGraphicsPipeline(Device, held.Pipeline);
		held = {hash, built, MaterialArraySlots(spirv)};
		return true;
	}

	void Renderer::Impl::RunSurfaceBakes(std::span<const View> views) {
		for (const View &view : views) {
			for (SurfaceBake bake : view.Bakes) {
				// A bake names no owner of its own; the view's content is its.
				if (!bake.Owner.IsValid()) bake.Owner = view.ContentOwner;
				const uint64_t key = (uint64_t(bake.Owner.Id()) << 32) | bake.Output.Id();
				const auto done = BakedRevisions.find(key);
				if (done != BakedRevisions.end() && done->second == bake.Revision) continue;
				const auto variant = BakePipelines.find(ShaderVariantKey(bake.Shader, bake.Owner));
				if (variant == BakePipelines.end() || variant->second.Pipeline == nullptr) continue;
				if (!Meshes.Has(bake.Mesh, bake.Owner)) continue;
				const MeshEntry &mesh = Meshes.Resolve(bake.Mesh, bake.Owner);
				// The bake reads the full vertex format; a packed mesh is drawn live.
				if (mesh.Packed || mesh.Whole.IndexCount == 0) continue;
				ENGINE_PROFILE_CAT("surface bake", core::ProfileCategory::Render);

				// Its textures, resolved as a draw would; an array slot reads a layer set.
				const auto material = [&](const core::Name &name, bool array) -> SDL_GPUTexture * {
					if (!name.IsValid()) return nullptr;
					if (array)
						return scene::IsLayerSet(name.Text())
								   ? LayeredTexture(name, bake.Owner, [&](core::Name) { return bake.Owner; })
								   : nullptr;
					return scene::IsLayerSet(name.Text()) ? nullptr : Textures.Find(name, bake.Owner);
				};
				const uint8_t arrays = variant->second.ArraySlots;
				SDL_GPUTexture *material0 = material(bake.MaterialTexture0, (arrays & 1u) != 0);
				SDL_GPUTexture *material1 = material(bake.MaterialTexture1, (arrays & 2u) != 0);
				// A named texture that is not resident yet waits for the next frame.
				if ((bake.MaterialTexture0.IsValid() && material0 == nullptr) ||
					(bake.MaterialTexture1.IsValid() && material1 == nullptr))
					continue;

				const uint32_t size = std::clamp(bake.Size, 16u, 4096u);
				std::array<SDL_GPUTexture *, BAKE_TARGETS> outputs{};
				for (auto &output : outputs) {
					SDL_GPUTextureCreateInfo info{};
					info.type = SDL_GPU_TEXTURETYPE_2D;
					info.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
					info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
					info.width = info.height = size;
					info.layer_count_or_depth = 1;
					info.num_levels = 1;
					info.sample_count = SDL_GPU_SAMPLECOUNT_1;
					output = gpu::CreateTexture(Device, &info);
				}
				SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(Device);
				if (command == nullptr || std::find(outputs.begin(), outputs.end(), nullptr) != outputs.end()) {
					for (auto *output : outputs)
						if (output != nullptr) gpu::ReleaseTexture(Device, output);
					if (command != nullptr) SDL_CancelGPUCommandBuffer(command);
					continue;
				}
				std::array<SDL_GPUColorTargetInfo, BAKE_TARGETS> colour{};
				for (size_t index = 0; index < colour.size(); ++index) {
					colour[index].texture = outputs[index];
					colour[index].load_op = SDL_GPU_LOADOP_CLEAR;
					colour[index].store_op = SDL_GPU_STOREOP_STORE;
				}
				SDL_GPURenderPass *pass =
					SDL_BeginGPURenderPass(command, colour.data(), static_cast<Uint32>(colour.size()), nullptr);
				SDL_BindGPUGraphicsPipeline(pass, variant->second.Pipeline);
				BindInstanceBuffers(pass);
				const SDL_GPUBufferBinding indices{Meshes.Indices(), 0};
				SDL_BindGPUIndexBuffer(pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);

				std::array<SDL_GPUTextureSamplerBinding, SURFACE_FRAGMENT_SAMPLERS> samplers{};
				for (auto &binding : samplers)
					binding = {FallbackTexture, Textures.Sampler()};
				const auto standIn = [&](uint8_t bit) {
					SDL_GPUTexture *array = (arrays & bit) != 0 ? EnsureFallbackArrayTexture() : nullptr;
					return array != nullptr ? array : FallbackTexture;
				};
				samplers[11].texture = material0 != nullptr ? material0 : standIn(1);
				samplers[12].texture = material1 != nullptr ? material1 : standIn(2);
				SDL_BindGPUFragmentSamplers(pass, 0, samplers.data(), static_cast<Uint32>(samplers.size()));

				LightingUniforms uniforms{};
				std::memcpy(uniforms.Parameters, bake.Parameters.data(), sizeof(uniforms.Parameters));
				uniforms.ParameterState = glm::vec4{
					static_cast<float>(bake.ParameterMask),
					material0 != nullptr ? 1.0f : 0.0f,
					material1 != nullptr ? 1.0f : 0.0f,
					0.0f
				};
				uniforms.MaterialExtra.z = 1.0f;
				SDL_PushGPUFragmentUniformData(command, 0, &uniforms, sizeof(uniforms));
				const LightUniforms lights{};
				SDL_PushGPUFragmentUniformData(command, 1, &lights, sizeof(lights));
				const BeamUniforms beams{};
				SDL_PushGPUFragmentUniformData(command, 2, &beams, sizeof(beams));
				const auto draw = [&](const MeshRange &range) {
					if (range.IndexCount > 0)
						SDL_DrawGPUIndexedPrimitives(pass, range.IndexCount, 1, range.FirstIndex, range.VertexOffset, 0);
				};
				if (mesh.Runs.empty())
					draw(mesh.Whole);
				else
					for (const MeshRange &range : mesh.Runs)
						draw(range);
				SDL_EndGPURenderPass(pass);
				SDL_SubmitGPUCommandBuffer(command);

				// Published under the output's names, replacing an earlier bake.
				for (size_t index = 0; index < outputs.size(); ++index) {
					const core::Name name(std::string(bake.Output.Text()) + "/" + std::to_string(index));
					SDL_GPUTexture *retired = nullptr;
					const size_t bytes = size_t(size) * size * 8;
					const bool placed = Textures.Find(name, bake.Owner) != nullptr
											? Textures.ReplaceAdopt(
												  name, outputs[index], size, size, bytes, bake.Owner, retired,
												  assets::TextureFormat::RGBA16_FLOAT
											  )
											: Textures.Adopt(
												  name, outputs[index], size, size, bytes, bake.Owner,
												  assets::TextureFormat::RGBA16_FLOAT
											  );
					if (!placed) gpu::ReleaseTexture(Device, outputs[index]);
					// SDL keeps a released texture alive until the GPU is done with it.
					if (retired != nullptr) gpu::ReleaseTexture(Device, retired);
				}
				BakedRevisions[key] = bake.Revision;
				core::Metrics::Count("render.surface_bakes", 1);
			}
		}
	}

	void Renderer::Impl::RequestTextureDetail(const View &view) {
		if (view.Target == nullptr || view.Target->Height == 0 || view.Instances.empty()) return;
		// Pixels across per unit of size at distance one.
		const float focal = float(view.Target->Height) / (2.0f * std::tan(view.Camera.FieldOfViewRadians * 0.5f));
		std::vector<core::Vector3> eyes{view.CameraFrame.Position};
		for (const SurfaceView &surface : view.Surfaces)
			eyes.push_back(surface.Frame.Position);
		for (const PortalView &portal : view.Portals)
			eyes.push_back(portal.Warp.Frame.Position);
		for (const scene::DrawInstance &instance : view.Instances) {
			const core::Vector3 &half = instance.HalfExtent;
			const float radius = std::sqrt(half.X * half.X + half.Y * half.Y + half.Z * half.Z);
			float nearest = 1e30f;
			for (const core::Vector3 &eye : eyes) {
				const core::Vector3 offset = instance.Frame.Position - eye;
				nearest = std::min(nearest, std::sqrt(offset.X * offset.X + offset.Y * offset.Y + offset.Z * offset.Z));
			}
			const float projected = 2.0f * radius * focal / std::max(nearest - radius, 0.05f);
			const core::Name owner = view.ContentOwnerOf(instance.SourceWorld);
			const auto ask = [&](const core::Name &name) {
				if (!name.IsValid() || scene::IsLayerSet(name.Text())) return;
				const auto one = [&](const core::Name &texture) {
					uint32_t width = 0, height = 0;
					if (!Textures.SizeOf(texture, width, height, owner)) return;
					const float ratio = float(std::max(width, height)) / std::max(projected, 1.0f);
					const uint32_t level = ratio <= 1.0f ? 0u : static_cast<uint32_t>(std::floor(std::log2(ratio)));
					Textures.RequestDetail(texture, owner, level, projected);
				};
				if (scene::IsTileSet(name.Text())) {
					for (const auto &tile : scene::ExpandTileSet(name.Text()))
						one(tile.Texture);
				} else {
					one(name);
				}
			};
			for (const core::Name *name :
				 {&instance.Texture,
				  &instance.NormalMap,
				  &instance.RoughnessMap,
				  &instance.OcclusionMap,
				  &instance.HeightMap,
				  &instance.MetalnessMap,
				  &instance.EmissiveMap,
				  &instance.PackedPbrMap,
				  &instance.MaterialTexture0,
				  &instance.MaterialTexture1})
				ask(*name);
		}
	}
}
