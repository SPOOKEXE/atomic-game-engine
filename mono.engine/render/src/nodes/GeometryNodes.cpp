// The material head and the ordered tail: every instanced draw the eye's own
// camera makes.
//
// The blended tail shares the opaque depth attachment. Transparent-layer
// capture instead owns a nearest-fragment attachment and exports unblended
// radiance/depth for deferred composition. The
// particles and the ribbons ride in the transparent node for the same reason -
// see its own comment for why they are not a node of their own.

#include "Tessellation.hpp"
#include "TransparentLayerWork.hpp"
#include "ViewRecording.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/scene/ActiveCamera.hpp>

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace engine::render {

	void ViewRecording::RegisterGeometryNodes(NodeTable &frameNodes) {
		frameNodes.Set(core::Name("tessellate"), [this](const graph::RunContext &context) {
			if (context.Writes.size() != 3 || Command == nullptr || State->Meshes.Vertices() == nullptr ||
				State->Meshes.Indices() == nullptr || State->InstanceIndexBuffer == nullptr)
				return false;
			SDL_GPUBuffer *vertices = GraphBuffer(context.Writes[0], context, true);
			SDL_GPUBuffer *indices = GraphBuffer(context.Writes[1], context, true);
			SDL_GPUBuffer *commands = GraphBuffer(context.Writes[2], context, true);
			if (vertices == nullptr || indices == nullptr || commands == nullptr) return false;
			const auto *vertexDesc = Pipeline->Graph.FindResource(context.Writes[0]);
			const auto *indexDesc = Pipeline->Graph.FindResource(context.Writes[1]);
			const auto *commandDesc = Pipeline->Graph.FindResource(context.Writes[2]);
			if (vertexDesc == nullptr || indexDesc == nullptr || commandDesc == nullptr) return false;
			const TessellationCapacity capacity{
				static_cast<uint32_t>(
					vertexDesc->Bytes(SceneWidth, SceneHeight) / sizeof(GpuTessellationVertex)
				),
				static_cast<uint32_t>(indexDesc->Bytes(SceneWidth, SceneHeight) / sizeof(uint32_t)),
				static_cast<uint32_t>(
					commandDesc->Bytes(SceneWidth, SceneHeight) /
					(TESSELLATION_COMMAND_WORDS * sizeof(uint32_t))
				)
			};
			const graph::Node *node = Pipeline->Graph.Find(context.Node);
			const uint32_t maximum =
				node == nullptr ? 1u : std::clamp(node->Integer(core::Name("max-subdivisions"), 1), 1u, 8u);
			const float targetPixels =
				node == nullptr ? 12.0f : std::max(node->Number(core::Name("target-pixels"), 12.0f), 1.0f);
			const auto projectedEdge = [&](const scene::DrawInstance &instance) {
				const glm::vec3 centre{
					instance.Frame.Position.X, instance.Frame.Position.Y, instance.Frame.Position.Z
				};
				const float radius =
					std::max({instance.HalfExtent.X, instance.HalfExtent.Y, instance.HalfExtent.Z});
				const glm::vec4 clipCentre = Matrices.ViewProjection * glm::vec4{centre, 1.0f};
				if (!std::isfinite(clipCentre.w) || std::abs(clipCentre.w) < 1e-5f) return 0.0f;
				const glm::vec2 centreNdc = glm::vec2(clipCentre) / clipCentre.w;
				float pixels = 0.0f;
				for (const glm::vec3 axis :
					 {glm::vec3{radius, 0.0f, 0.0f},
					  glm::vec3{0.0f, radius, 0.0f},
					  glm::vec3{0.0f, 0.0f, radius}}) {
					const glm::vec4 clip = Matrices.ViewProjection * glm::vec4{centre + axis, 1.0f};
					if (std::isfinite(clip.w) && std::abs(clip.w) >= 1e-5f)
						pixels = std::max(
							pixels,
							glm::length(glm::vec2(clip) / clip.w - centreNdc) * 0.5f *
								std::max(SceneWidth, SceneHeight)
						);
				}
				return pixels * 2.0f;
			};
			std::vector<TessellationRequest> requests;
			bool fallback = false;
			for (uint32_t slot = SceneCount; slot < SceneCount + PlainOpaque && slot < State->SlotMesh.size();
				 ++slot) {
				const MeshEntry *mesh = State->SlotMesh[slot];
				if (mesh == nullptr) continue;
				// The compute input is the unpacked MeshVertex stream. Draw the entire
				// range coarsely when a source cannot use that layout.
				if (mesh->Packed) {
					fallback = true;
					break;
				}
				const uint32_t source = slot - SceneCount;
				if (source >= State->DrawOrder.size() ||
					State->DrawOrder[source] >= State->SceneInstances.size()) {
					fallback = true;
					break;
				}
				const scene::DrawInstance &instance = State->SceneInstances[State->DrawOrder[source]];
				const uint32_t factor = TessellationFactor(projectedEdge(instance), targetPixels, maximum);
				if (mesh->Runs.empty()) {
					requests.push_back({mesh->Whole, 0, slot, factor});
				} else
					for (uint32_t material = 0; material < mesh->Runs.size(); ++material)
						requests.push_back({mesh->Runs[material], material, slot, factor});
			}
			TessellationPlan plan;
			bool complete = false;
			for (uint32_t ceiling = maximum; ceiling > 0 && !complete && !fallback; --ceiling) {
				std::vector<TessellationRequest> reduced = requests;
				for (TessellationRequest &request : reduced)
					request.Factor = std::min(request.Factor, ceiling);
				complete = BuildCompleteTessellationPlan(reduced, capacity, plan);
			}
			State->Tessellation.Fallback = fallback || (!requests.empty() && !complete);
			State->Tessellation.Entries = plan.Entries;
			State->Tessellation.Count = static_cast<uint32_t>(plan.Entries.size());
			if (State->Tessellation.Fallback || plan.Entries.empty()) return true;
			const auto grow = [](uint32_t value) {
				uint32_t next = 64;
				while (next < value)
					next *= 2;
				return next;
			};
			if (State->Tessellation.Capacity < plan.Entries.size() || State->Tessellation.Plans == nullptr) {
				if (State->Tessellation.Plans != nullptr)
					gpu::ReleaseBuffer(State->Device, State->Tessellation.Plans);
				if (State->Tessellation.Transfer != nullptr)
					gpu::ReleaseTransferBuffer(State->Device, State->Tessellation.Transfer);
				State->Tessellation.Capacity = grow(static_cast<uint32_t>(plan.Entries.size()));
				SDL_GPUBufferCreateInfo buffer{};
				buffer.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
				buffer.size = State->Tessellation.Capacity * sizeof(GpuTessellationPlan);
				State->Tessellation.Plans = gpu::CreateBuffer(State->Device, &buffer);
				SDL_GPUTransferBufferCreateInfo transfer{};
				transfer.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
				transfer.size = buffer.size;
				State->Tessellation.Transfer = gpu::CreateTransferBuffer(State->Device, &transfer);
				if (State->Tessellation.Plans == nullptr || State->Tessellation.Transfer == nullptr)
					return false;
			}
			std::memcpy(
				SDL_MapGPUTransferBuffer(State->Device, State->Tessellation.Transfer, false),
				plan.Entries.data(),
				plan.Entries.size() * sizeof(GpuTessellationPlan)
			);
			SDL_UnmapGPUTransferBuffer(State->Device, State->Tessellation.Transfer);
			SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(Command);
			SDL_GPUTransferBufferLocation source{State->Tessellation.Transfer, 0};
			SDL_GPUBufferRegion destination{
				State->Tessellation.Plans,
				0,
				static_cast<uint32_t>(plan.Entries.size() * sizeof(GpuTessellationPlan))
			};
			SDL_UploadToGPUBuffer(copy, &source, &destination, false);
			SDL_EndGPUCopyPass(copy);
			if (State->Tessellation.Compute == nullptr)
				State->Tessellation.Compute =
					State->LoadComputePipeline("tessellate.comp", 0, 3, 0, 3, 64, 1);
			if (State->Tessellation.Compute == nullptr) return false;
			EnterNamedPass(context.Name);
			SDL_GPUStorageBufferReadWriteBinding writes[3]{};
			writes[0].buffer = vertices;
			writes[1].buffer = indices;
			writes[2].buffer = commands;
			SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(Command, nullptr, 0, writes, 3);
			if (pass == nullptr) return false;
			SDL_BindGPUComputePipeline(pass, State->Tessellation.Compute);
			SDL_GPUBuffer *reads[]{
				State->Meshes.Vertices(), State->Meshes.Indices(), State->Tessellation.Plans
			};
			SDL_BindGPUComputeStorageBuffers(pass, 0, reads, 3);
			const uint32_t counts[]{
				State->Tessellation.Count, capacity.Vertices, capacity.Indices, capacity.Commands
			};
			SDL_PushGPUComputeUniformData(Command, 0, counts, sizeof(counts));
			uint32_t maximumTriangles = 0;
			for (const GpuTessellationPlan &entry : plan.Entries)
				maximumTriangles = std::max(maximumTriangles, entry.SourceIndexCount / 3u);
			SDL_DispatchGPUCompute(pass, State->Tessellation.Count, (maximumTriangles + 63u) / 64u, 1);
			SDL_EndGPUComputePass(pass);
			Result.ComputeDispatches++;
			return true;
		});

		frameNodes.Set(core::Name("tessellated-draw"), [this](const graph::RunContext &context) {
			if (context.Reads.size() != 3 || context.Writes.size() != 2) return false;
			if (State->Tessellation.Count == 0 && !State->Tessellation.Fallback) return true;
			SDL_GPUBuffer *vertices =
				State->Tessellation.Fallback ? nullptr : GraphBuffer(context.Reads[0], context, false);
			SDL_GPUBuffer *indices =
				State->Tessellation.Fallback ? nullptr : GraphBuffer(context.Reads[1], context, false);
			SDL_GPUBuffer *commands =
				State->Tessellation.Fallback ? nullptr : GraphBuffer(context.Reads[2], context, false);
			auto colour = GraphTexture(context.Writes[0], context, true);
			auto depth = GraphTexture(context.Writes[1], context, true);
			if ((!State->Tessellation.Fallback &&
				 (vertices == nullptr || indices == nullptr || commands == nullptr)) ||
				!colour.IsValid() || !depth.IsValid())
				return false;
			EnterNamedPass(context.Name);
			SDL_GPUColorTargetInfo target{};
			target.texture = colour.Texture;
			target.clear_color = SDL_FColor{0, 0, 0, 1};
			target.load_op = SDL_GPU_LOADOP_CLEAR;
			target.store_op = SDL_GPU_STOREOP_STORE;
			SDL_GPUDepthStencilTargetInfo depthTarget{};
			depthTarget.texture = depth.Texture;
			depthTarget.clear_depth = 1.0f;
			depthTarget.load_op = SDL_GPU_LOADOP_CLEAR;
			depthTarget.store_op = SDL_GPU_STOREOP_STORE;
			SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(Command, &target, 1, &depthTarget);
			if (pass == nullptr) return false;
			if (State->HdrOpaquePipeline == nullptr) {
				SDL_EndGPURenderPass(pass);
				return false;
			}
			State->BindPipeline(pass, State->HdrOpaquePipeline, Impl::PipelineFamily::HdrOpaque);
			State->BindInstanceBuffers(pass, State->InstanceIndexBuffer);
			if (State->Tessellation.Fallback) {
				const SDL_GPUBufferBinding coarse{State->Meshes.Indices(), 0};
				SDL_BindGPUIndexBuffer(pass, &coarse, SDL_GPU_INDEXELEMENTSIZE_32BIT);
				SDL_SetGPUViewport(pass, &SceneViewport);
				SDL_SetGPUScissor(pass, &SceneScissor);
				SDL_PushGPUVertexUniformData(Command, 0, &Frame, sizeof(Frame));
				Result.DrawCalls += State->DrawSlots(
					Command,
					pass,
					SceneCount,
					PlainOpaque,
					&Lighting,
					State->ShadowTexture,
					State->ShadowSampler,
					nullptr,
					State->SurfaceSampler,
					0,
					Result.Triangles
				);
				SDL_EndGPURenderPass(pass);
				return true;
			}
			const SDL_GPUBufferBinding vertex{vertices, 0};
			SDL_BindGPUVertexBuffers(pass, 0, &vertex, 1);
			const SDL_GPUBufferBinding index{indices, 0};
			SDL_BindGPUIndexBuffer(pass, &index, SDL_GPU_INDEXELEMENTSIZE_32BIT);
			SDL_SetGPUViewport(pass, &SceneViewport);
			SDL_SetGPUScissor(pass, &SceneScissor);
			SDL_PushGPUVertexUniformData(Command, 0, &Frame, sizeof(Frame));
			for (uint32_t command = 0; command < State->Tessellation.Count; ++command) {
				const GpuTessellationPlan &entry = State->Tessellation.Entries[command];
				if (entry.Instance >= State->SlotMesh.size() || entry.Instance >= State->SlotTexture.size() ||
					entry.Instance >= State->SlotContentOwner.size() ||
					entry.Instance >= State->SlotResample.size() ||
					entry.Instance >= State->SlotNormalMap.size() ||
					entry.Instance >= State->SlotRoughnessMap.size() ||
					entry.Instance >= State->SlotOcclusionMap.size() ||
					entry.Instance >= State->SlotHeightMap.size() ||
					entry.Instance >= State->SlotMetalnessMap.size() ||
					entry.Instance >= State->SlotEmissiveMap.size() ||
					entry.Instance >= State->SlotSeam.size() || entry.Instance >= State->SlotSeamLight.size())
					continue;
				const MeshEntry *mesh = State->SlotMesh[entry.Instance];
				if (mesh == nullptr) continue;
				const TessellationMaterial source =
					TessellationMaterialFor(*mesh, entry.Material, State->SlotTexture[entry.Instance]);
				const core::Name texture = source.Texture;
				const core::Name owner =
					State->TextureContentOwner(texture, State->SlotContentOwner[entry.Instance]);
				SDL_GPUTexture *found = State->Textures.Find(texture, owner);
				const TextureChoice choice = ChooseTexture(
					found != nullptr, texture.IsValid(), State->Textures.Expecting(texture, owner)
				);
				SDL_GPUTexture *sampled = choice == TextureChoice::Named	 ? found
										  : choice == TextureChoice::Missing ? State->Textures.Missing()
																			 : State->Textures.Default();
				const bool absent = choice == TextureChoice::Missing;
				const auto dataMap = [&](core::Name name) {
					const core::Name mapOwner =
						State->TextureContentOwner(name, State->SlotContentOwner[entry.Instance]);
					SDL_GPUTexture *map = State->Textures.Find(name, mapOwner);
					if (map != nullptr) return map;
					if (name.IsValid() && !State->Textures.Expecting(name, mapOwner))
						return State->Textures.Missing();
					return static_cast<SDL_GPUTexture *>(nullptr);
				};
				SDL_GPUTexture *normal = dataMap(State->SlotNormalMap[entry.Instance]);
				SDL_GPUTexture *roughness = dataMap(State->SlotRoughnessMap[entry.Instance]);
				SDL_GPUTexture *occlusion = dataMap(State->SlotOcclusionMap[entry.Instance]);
				SDL_GPUTexture *height = dataMap(State->SlotHeightMap[entry.Instance]);
				SDL_GPUTexture *metalness = dataMap(State->SlotMetalnessMap[entry.Instance]);
				SDL_GPUTexture *emissive = dataMap(State->SlotEmissiveMap[entry.Instance]);
				SDL_GPUSampler *sampler =
					State->SlotResample[entry.Instance] == scene::SurfaceResampleMode::Pixelated
						? State->Textures.PixelSampler()
						: State->Textures.Sampler();
				SDL_GPUSampler *fallbackSampler =
					State->SurfaceSampler != nullptr ? State->SurfaceSampler : State->Textures.Sampler();
				SDL_GPUSampler *shadowSampler =
					State->ShadowSampler != nullptr ? State->ShadowSampler : fallbackSampler;
				SDL_GPUSampler *beamSampler =
					State->ShadowSampler != nullptr ? State->ShadowSampler : shadowSampler;
				const SDL_GPUTextureSamplerBinding bindings[]{
					{State->ShadowTexture != nullptr ? State->ShadowTexture : State->FallbackTexture,
					 shadowSampler},
					{State->FallbackTexture, fallbackSampler},
					{sampled != nullptr ? sampled : State->FallbackTexture, sampler},
					{State->BeamTexture != nullptr ? State->BeamTexture : State->FallbackTexture, beamSampler
					},
					{normal != nullptr ? normal : State->FallbackTexture, sampler},
					{roughness != nullptr ? roughness : State->FallbackTexture, sampler},
					{occlusion != nullptr ? occlusion : State->FallbackTexture, sampler},
					{emissive != nullptr ? emissive : State->FallbackTexture, sampler},
					{height != nullptr ? height : State->FallbackTexture, sampler},
					{metalness != nullptr ? metalness : State->FallbackTexture, sampler}
				};
				SDL_BindGPUFragmentSamplers(pass, 0, bindings, 10);
				LightingUniforms material = Lighting;
				const std::array<float, 4> tint = absent ? std::array<float, 4>{1, 1, 1, 1} : source.Colour;
				material.BaseColour = glm::vec4{tint[0], tint[1], tint[2], tint[3]};
				material.Surface =
					glm::vec4{sampled != nullptr ? 1.0f : 0.0f, 0.0f, height != nullptr ? 1.0f : 0.0f, 0.04f};
				material.Material = glm::vec4{
					normal != nullptr ? 1.0f : 0.0f,
					roughness != nullptr ? 1.0f : 0.0f,
					occlusion != nullptr ? 1.0f : 0.0f,
					emissive != nullptr ? 1.0f : 0.0f,
				};
				material.MaterialExtra = glm::vec4{metalness != nullptr ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
				const FlipbookCell cell = State->Textures.CellOf(texture, State->AnimationSeconds, owner);
				material.Flipbook = glm::vec4{cell.Scale, cell.OffsetU, cell.OffsetV, 0.0f};
				material.SeamPlane = State->SlotSeam[entry.Instance];
				const glm::vec3 seamLight{State->SlotSeamLight[entry.Instance]};
				if (glm::dot(seamLight, seamLight) > 0.0f) material.Direction = glm::vec4{seamLight, 0.0f};
				SDL_PushGPUFragmentUniformData(Command, 0, &material, sizeof(material));
				SDL_DrawGPUIndexedPrimitivesIndirect(
					pass, commands, command * TESSELLATION_COMMAND_WORDS * sizeof(uint32_t), 1
				);
			}
			SDL_EndGPURenderPass(pass);
			Result.DrawCalls += State->Tessellation.Count;
			return true;
		});

		frameNodes.Set(core::Name("forward"), [this](const graph::RunContext &context) {
			if (context.Writes.size() != 2) {
				return false;
			}
			Impl::NamedTexture colour = GraphTexture(context.Writes[0], context, true);
			Impl::NamedTexture depth = GraphTexture(context.Writes[1], context, true);
			if (!colour.IsValid() || !depth.IsValid()) {
				return false;
			}

			EnterNamedPass(context.Name);
			SDL_GPUColorTargetInfo colourTarget{};
			colourTarget.texture = colour.Texture;
			colourTarget.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
			colourTarget.load_op = SDL_GPU_LOADOP_CLEAR;
			colourTarget.store_op = SDL_GPU_STOREOP_STORE;
			colourTarget.cycle = true;
			SDL_GPUDepthStencilTargetInfo depthTarget{};
			depthTarget.texture = depth.Texture;
			depthTarget.clear_depth = 1.0f;
			depthTarget.load_op = SDL_GPU_LOADOP_CLEAR;
			depthTarget.store_op = SDL_GPU_STOREOP_STORE;
			depthTarget.stencil_load_op = SDL_GPU_LOADOP_CLEAR;
			depthTarget.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
			depthTarget.cycle = true;

			SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(Command, &colourTarget, 1, &depthTarget);
			if (pass == nullptr) {
				ENGINE_ERROR("forward: SDL_BeginGPURenderPass: {}", SDL_GetError());
				return false;
			}
			State->BindPipeline(pass, State->ForwardPipeline, Impl::PipelineFamily::Opaque);
			SDL_SetGPUViewport(pass, &SceneViewport);
			SDL_SetGPUScissor(pass, &SceneScissor);
			SDL_PushGPUVertexUniformData(Command, 0, &Frame, sizeof(Frame));
			if (HaveInstances && PlainOpaque > 0) {
				State->BindInstanceBuffers(pass, State->InstanceIndexBuffer);
				const SDL_GPUBufferBinding indexBinding{State->Meshes.Indices(), 0};
				SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
				Result.DrawCalls += State->DrawSlots(
					Command,
					pass,
					SceneCount,
					PlainOpaque,
					&Lighting,
					State->ShadowTexture,
					State->ShadowSampler,
					nullptr,
					State->SurfaceSampler,
					0,
					Result.Triangles,
					nullptr
				);
			}
			SDL_EndGPURenderPass(pass);
			return true;
		});

		frameNodes.Set(core::Name("gbuffer"), [this](const graph::RunContext &context) {
			ViewRecording &recording = *this;
			Impl *const State = recording.State;
			FrameResult &result = recording.Result;
			SDL_GPUCommandBuffer *const command = recording.Command;
			const std::span<const scene::DrawInstance> instances = recording.Instances;
			const uint32_t sceneWidth = recording.SceneWidth;
			const uint32_t sceneHeight = recording.SceneHeight;
			const bool haveInstances = recording.HaveInstances;
			const uint32_t plainOpaque = recording.PlainOpaque;
			const uint32_t sceneCount = recording.SceneCount;
			SDL_GPUDepthStencilTargetInfo &depthTarget = recording.DepthTarget;
			Impl::PbrSlot &pbr = *recording.Pbr;
			const FrameUniforms &frameUniforms = recording.Frame;
			const LightingUniforms &lighting = recording.Lighting;
			const SDL_GPUViewport &sceneViewport = recording.SceneViewport;
			const SDL_Rect &sceneScissor = recording.SceneScissor;
			const auto enterNamedPass = [&recording](
											core::Name name, SDL_GPUCommandBuffer *recordedCommand = nullptr
										) { recording.EnterNamedPass(name, recordedCommand); };

			enterNamedPass(context.Name);

			// Ordinary geometry uses one pass, or two around the occlusion cull. The second
			// begins where the first ended - every target loads - so the two
			// together paint exactly the frame one pass would have, minus the
			// pixels the cull proved covered.
			const bool occluded = State->OcclusionFrame.Active && haveInstances && plainOpaque > 0 &&
								  State->EnsurePyramid(sceneWidth, sceneHeight);

			const auto beginGBuffer = [&](bool clear) {
				SDL_GPUColorTargetInfo gbufferTargets[4]{};
				for (size_t target = 0; target < 4; target++) {
					gbufferTargets[target].clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f};
					gbufferTargets[target].load_op = clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
					gbufferTargets[target].store_op = SDL_GPU_STOREOP_STORE;
					// Cycling is for the frame's first touch; the late pass
					// must draw over the early pass's pixels, not fresh memory.
					gbufferTargets[target].cycle = clear;
				}
				gbufferTargets[0].texture = pbr.Albedo;
				gbufferTargets[1].texture = pbr.Normal;
				gbufferTargets[2].texture = pbr.Material;
				gbufferTargets[3].texture = pbr.Emissive;

				depthTarget.load_op = clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
				depthTarget.store_op = SDL_GPU_STOREOP_STORE;
				depthTarget.cycle = clear;

				SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, gbufferTargets, 4, &depthTarget);
				if (pass == nullptr) {
					return pass;
				}
				State->BindPipeline(pass, State->GBufferPipeline, Impl::PipelineFamily::GBuffer);
				SDL_SetGPUViewport(pass, &sceneViewport);
				SDL_SetGPUScissor(pass, &sceneScissor);
				SDL_PushGPUVertexUniformData(command, 0, &frameUniforms, sizeof(frameUniforms));
				return pass;
			};
			const auto drawOpaque =
				[&](SDL_GPURenderPass *pass, SDL_GPUBuffer *indices, const Impl::IndirectPhase *phase) {
					State->BindInstanceBuffers(pass, indices);
					const SDL_GPUBufferBinding indexBinding{State->Meshes.Indices(), 0};
					SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
					result.DrawCalls += State->DrawSlots(
						command,
						pass,
						sceneCount,
						plainOpaque,
						&lighting,
						State->ShadowTexture,
						State->ShadowSampler,
						nullptr,
						State->SurfaceSampler,
						0,
						result.Triangles,
						phase
					);
				};

			const auto authored = [&](uint32_t slot) {
				const auto found = State->ShaderVariants.find(
					Impl::ShaderVariantKey(State->SlotShader[slot], State->SlotContentOwner[slot])
				);
				return found != State->ShaderVariants.end() && found->second.HdrOpaque != nullptr;
			};
			bool customOpaque = false;
			if (haveInstances && !State->ShaderVariants.empty()) {
				for (uint32_t slot = sceneCount; slot < sceneCount + plainOpaque; ++slot)
					customOpaque = authored(slot) || customOpaque;
			}
			if (customOpaque) {
				// Authored colour occupies emissive while cleared normal alpha marks it
				// as already shaded. Drawing first preserves discard and nearest depth.
				auto *clear = beginGBuffer(true);
				if (clear == nullptr) return false;
				SDL_EndGPURenderPass(clear);
				SDL_GPUColorTargetInfo colour{};
				colour.texture = pbr.Emissive;
				colour.load_op = SDL_GPU_LOADOP_LOAD;
				colour.store_op = SDL_GPU_STOREOP_STORE;
				depthTarget.load_op = SDL_GPU_LOADOP_LOAD;
				depthTarget.cycle = false;
				auto *pass = SDL_BeginGPURenderPass(command, &colour, 1, &depthTarget);
				if (pass == nullptr) return false;
				State->BindPipeline(pass, State->HdrOpaquePipeline, Impl::PipelineFamily::HdrOpaque);
				State->BindInstanceBuffers(pass, State->InstanceIndexBuffer);
				const SDL_GPUBufferBinding indices{State->Meshes.Indices(), 0};
				SDL_BindGPUIndexBuffer(pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
				SDL_SetGPUViewport(pass, &sceneViewport);
				SDL_SetGPUScissor(pass, &sceneScissor);
				SDL_PushGPUVertexUniformData(command, 0, &frameUniforms, sizeof(frameUniforms));
				SDL_PushGPUFragmentUniformData(
					command, 1, &recording.SceneLights, sizeof(recording.SceneLights)
				);
				SDL_PushGPUFragmentUniformData(command, 2, &State->Beams, sizeof(State->Beams));
				for (uint32_t slot = sceneCount; slot < sceneCount + plainOpaque;) {
					if (!authored(slot)) {
						++slot;
						continue;
					}
					const auto first = slot++;
					while (slot < sceneCount + plainOpaque && authored(slot))
						++slot;
					result.DrawCalls += State->DrawSlots(
						command,
						pass,
						first,
						slot - first,
						&lighting,
						State->ShadowTexture,
						State->ShadowSampler,
						nullptr,
						State->SurfaceSampler,
						0,
						result.Triangles
					);
				}
				SDL_EndGPURenderPass(pass);
			}

			if (!occluded) {
				SDL_GPURenderPass *gbuffer = beginGBuffer(!customOpaque);
				if (gbuffer == nullptr) {
					ENGINE_ERROR("gbuffer: SDL_BeginGPURenderPass: {}", SDL_GetError());
					return false;
				}
				if (haveInstances && plainOpaque > 0) {
					drawOpaque(gbuffer, State->InstanceIndexBuffer, nullptr);
				}
				SDL_EndGPURenderPass(gbuffer);
				return true;
			}

			// Trace rather than a counter in `FrameResult`: the survivor count
			// lives on the GPU and never comes back, so the honest numbers are
			// the two the CPU decided.
			ENGINE_TRACE(
				"gbuffer: occlusion cull of {} candidate(s) behind {} occluder(s) in {} run(s)",
				State->OcclusionFrame.CandidateCount,
				State->OcclusionFrame.EarlyTotal,
				State->OcclusionFrame.RunCount
			);

			// Early phase: the CPU-picked occluders, by indirect arguments so
			// both phases drive their draws the same way.
			const Impl::IndirectPhase early{State->Occlusion.Arguments, 0, &State->OcclusionFrame.RunEarly};
			SDL_GPURenderPass *earlyPass = beginGBuffer(!customOpaque);
			if (earlyPass == nullptr) {
				ENGINE_ERROR("gbuffer early: SDL_BeginGPURenderPass: {}", SDL_GetError());
				return false;
			}
			drawOpaque(earlyPass, State->InstanceIndexBuffer, &early);
			SDL_EndGPURenderPass(earlyPass);

			// The pyramid over what the occluders wrote, then the cull that
			// compacts the survivors and fills the late arguments.
			State->BuildPyramid(command, depthTarget.texture);
			State->DispatchOcclusionCull(command, frameUniforms.ViewProjection);

			// Late phase: the survivors, loading everything the early phase
			// stored.
			const Impl::IndirectPhase late{
				State->Occlusion.Arguments,
				State->OcclusionFrame.ArgCount,
				&State->OcclusionFrame.RunCandidates
			};
			SDL_GPURenderPass *latePass = beginGBuffer(false);
			if (latePass == nullptr) {
				ENGINE_ERROR("gbuffer late: SDL_BeginGPURenderPass: {}", SDL_GetError());
				return false;
			}
			drawOpaque(latePass, State->Occlusion.LateIndices, &late);
			State->BindInstanceBuffers(latePass, State->InstanceIndexBuffer);
			result.DrawCalls += State->DrawSlots(
				command,
				latePass,
				sceneCount,
				plainOpaque,
				&lighting,
				State->ShadowTexture,
				State->ShadowSampler,
				nullptr,
				State->SurfaceSampler,
				0,
				result.Triangles,
				nullptr,
				Impl::SlotSelection::LodOnly
			);
			SDL_EndGPURenderPass(latePass);
			return true;
		});

		frameNodes.Set(core::Name("transparent-layer"), [this](const graph::RunContext &context) {
			const auto *node = Pipeline->Graph.Find(context.Node);
			if (!node || PlainTransparent != TransparentCount) return false;
			TransparentLayerWork work{
				.Meshes = PlainTransparent,
				.Particles = ParticleCount,
				.Ribbons = RibbonCount,
			};
			const bool particleLayers = work.Particles != 0;
			const bool ribbonLayers = work.Ribbons != 0;
			const uint32_t first = SceneCount + static_cast<uint32_t>(OpaqueCount);
			for (uint32_t slot = first; slot < first + PlainTransparent; ++slot)
				if (slot >= State->SlotShader.size() || State->SlotShader[slot].IsValid()) return false;
			const auto texture = [&](bool output, const char *port) {
				const auto &ports = output ? node->WritePorts : node->ReadPorts;
				const auto resources = output ? context.Writes : context.Reads;
				const auto found = std::find(ports.begin(), ports.end(), core::Name(port));
				if (found == ports.end()) return Impl::NamedTexture{};
				const size_t index = found - ports.begin();
				return index < resources.size() ? GraphTexture(resources[index], context, output)
												: Impl::NamedTexture{};
			};
			const auto opaque = texture(false, "opaque-z"), previous = texture(false, "previous-z");
			const bool hasPrevious =
				std::find(node->ReadPorts.begin(), node->ReadPorts.end(), core::Name("previous-z")) !=
				node->ReadPorts.end();
			if (hasPrevious && !previous.IsValid()) return false;
			const auto shadow = texture(false, "shadow");
			const auto colour = texture(true, "colour"), depth = texture(true, "depth"),
					   z = texture(true, "z");
			if (!opaque.IsValid() || !colour.IsValid() || !depth.IsValid() || !z.IsValid() ||
				opaque.Format != SDL_GPU_TEXTUREFORMAT_D32_FLOAT ||
				colour.Format != SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT ||
				depth.Format != SDL_GPU_TEXTUREFORMAT_R32_FLOAT ||
				z.Format != SDL_GPU_TEXTUREFORMAT_D32_FLOAT)
				return false;
			// The scene depth attachment can retain block-rounded capacity while
			// this view uses a smaller viewport starting at the same origin.
			if (opaque.Width < colour.Width || opaque.Height < colour.Height) return false;
			for (const auto &input : {depth, z, previous.IsValid() ? previous : z})
				if (input.Width != colour.Width || input.Height != colour.Height) return false;
			if ((previous.IsValid() && previous.Format != SDL_GPU_TEXTUREFORMAT_D32_FLOAT) ||
				!State->DepthLinearPipeline || !State->EnsureTransparentLayer())
				return false;
			auto *interface = DrawInterface ? Request.GameInterfaceHook : nullptr;
			const size_t interfaceBatches = interface ? interface->WorldBatchCount() : 0;
			work.InterfaceBatches = static_cast<uint32_t>(interfaceBatches);
			const auto scratchColour = texture(true, "interface-colour"),
					   scratchZ = texture(true, "interface-z");
			if (interfaceBatches &&
				(!interface->SupportsWorldLayers() ||
				 (interface->HasWorldOverlay() && !GraphEnabled(core::Name("spatial-overlay"))) ||
				 !scratchColour.IsValid() || !scratchZ.IsValid() ||
				 scratchColour.Format != SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT ||
				 scratchZ.Format != SDL_GPU_TEXTUREFORMAT_D32_FLOAT || scratchColour.Width != colour.Width ||
				 scratchColour.Height != colour.Height || scratchZ.Width != colour.Width ||
				 scratchZ.Height != colour.Height || !State->EnsureInterfaceLayer()))
				return false;
			EnterNamedPass(context.Name);
			uint32_t recordedPasses = 0;
			SDL_GPUColorTargetInfo targets[1]{};
			targets[0].texture = colour.Texture;
			for (auto &target : targets) {
				target.load_op = SDL_GPU_LOADOP_CLEAR;
				target.store_op = SDL_GPU_STOREOP_STORE;
				target.cycle = true;
			}
			SDL_GPUDepthStencilTargetInfo depthTarget{};
			depthTarget.texture = z.Texture;
			depthTarget.clear_depth = 1;
			depthTarget.load_op = SDL_GPU_LOADOP_CLEAR;
			depthTarget.store_op = SDL_GPU_STOREOP_STORE;
			depthTarget.cycle = true;
			for (uint32_t phase = 0; phase < work.PhaseCount(); ++phase) {
				targets[0].cycle = phase == 0;
				auto *pass = SDL_BeginGPURenderPass(Command, targets, 1, phase == 0 ? &depthTarget : nullptr);
				if (!pass) return false;
				++recordedPasses;
				const auto eye = Request.CameraFrame.Position;
				const auto forward = Request.CameraFrame.LookVector();
				const std::array<glm::vec4, 3> capture{
					glm::vec4{eye.X, eye.Y, eye.Z, 0},
					glm::vec4{forward.X, forward.Y, forward.Z, 0},
					glm::vec4{phase == 0 && previous.IsValid() ? 1.f : 0.f, phase == 1 ? 1.f : 0.f, 0, 0}
				};
				const SDL_GPUTextureSamplerBinding bounds[] = {
					{opaque.Texture, State->OverlaySampler},
					{phase == 1			  ? z.Texture
					 : previous.IsValid() ? previous.Texture
										  : opaque.Texture,
					 State->OverlaySampler}
				};
				const SDL_GPUViewport viewport{0, 0, float(colour.Width), float(colour.Height), 0, 1};
				const SDL_Rect scissor{0, 0, int(colour.Width), int(colour.Height)};
				SDL_SetGPUViewport(pass, &viewport);
				SDL_SetGPUScissor(pass, &scissor);
				if (PlainTransparent) {
					State->BindPipeline(
						pass,
						phase == 0 ? State->TransparentLayerPipeline : State->TransparentLayerColourPipeline,
						Impl::PipelineFamily::Other
					);
					SDL_BindGPUFragmentSamplers(pass, 10, bounds, 2);
					State->BindInstanceBuffers(pass);
					const SDL_GPUBufferBinding indexBinding{State->Meshes.Indices(), 0};
					SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
					const FrameUniforms frame{Matrices.ViewProjection, LightViewProjection, glm::mat4{1}};
					SDL_PushGPUVertexUniformData(Command, 0, &frame, sizeof(frame));
					SDL_PushGPUFragmentUniformData(Command, 1, &SceneLights, sizeof(SceneLights));
					SDL_PushGPUFragmentUniformData(Command, 2, &State->Beams, sizeof(State->Beams));
					SDL_PushGPUFragmentUniformData(Command, 3, capture.data(), sizeof(capture));
					auto lighting = LightingAt(eye, 0, 0);
					if (!shadow.IsValid()) lighting.Flags.x = 0;
					Result.DrawCalls += State->DrawSlots(
						Command,
						pass,
						first,
						PlainTransparent,
						&lighting,
						shadow.IsValid() ? shadow.Texture : State->FallbackTexture,
						State->ShadowSampler ? State->ShadowSampler : State->OverlaySampler,
						nullptr,
						State->OverlaySampler,
						0,
						Result.Triangles
					);
				}
				if (particleLayers || ribbonLayers) {
					auto *layerPipeline =
						particleLayers
							? (phase == 0 ? State->ParticleLayerPipeline : State->ParticleLayerColourPipeline)
							: (phase == 0 ? State->RibbonLayerPipeline : State->RibbonLayerColourPipeline);
					State->BindPipeline(pass, layerPipeline, Impl::PipelineFamily::Other);
					SDL_BindGPUFragmentSamplers(pass, 1, bounds, 2);
					SDL_PushGPUFragmentUniformData(Command, 1, capture.data(), sizeof(capture));
					const auto layer =
						phase == 0 ? TransparentLayerPhase::Nearest : TransparentLayerPhase::Colour;
					if (particleLayers)
						Result.DrawCalls += State->DrawParticles(
							Command,
							pass,
							Matrices.ViewProjection,
							Request.CameraFrame,
							Result.Triangles,
							Result.ParticlesDrawn,
							Result.Culled,
							WorldColourTarget::Hdr,
							layer
						);
					if (ribbonLayers)
						Result.DrawCalls += State->DrawRibbons(
							Command,
							pass,
							Matrices.ViewProjection,
							Request.CameraFrame,
							Request.RibbonRuns,
							Result.Triangles,
							WorldColourTarget::Hdr,
							layer
						);
				}
				SDL_EndGPURenderPass(pass);
				// Capture one authored batch at the same projection, then peel its
				// actual colour/depth. This preserves authored discard and alpha.
				for (size_t batch = 0; batch < interfaceBatches; ++batch) {
					SDL_GPUColorTargetInfo scratch{};
					scratch.texture = scratchColour.Texture;
					scratch.load_op = SDL_GPU_LOADOP_CLEAR;
					scratch.store_op = SDL_GPU_STOREOP_STORE;
					scratch.cycle = phase == 0 && batch == 0;
					SDL_GPUDepthStencilTargetInfo scratchDepth{};
					scratchDepth.texture = scratchZ.Texture;
					scratchDepth.clear_depth = 1;
					scratchDepth.load_op = SDL_GPU_LOADOP_CLEAR;
					scratchDepth.store_op = SDL_GPU_STOREOP_STORE;
					scratchDepth.cycle = scratch.cycle;
					auto *batchPass = SDL_BeginGPURenderPass(Command, &scratch, 1, &scratchDepth);
					if (!batchPass) return false;
					++recordedPasses;
					const SDL_GPUViewport viewport{0, 0, float(colour.Width), float(colour.Height), 0, 1};
					SDL_SetGPUViewport(batchPass, &viewport);
					const auto lighting = LightingAt(Request.CameraFrame.Position, 0, 0);
					WorldInterfaceCapture capture;
					capture.Command = Command;
					capture.Pass = batchPass;
					capture.ViewProjection = Matrices.ViewProjection;
					capture.Camera = Request.CameraFrame;
					capture.Ambient = {lighting.Ambient.x, lighting.Ambient.y, lighting.Ambient.z};
					capture.Sun = {lighting.Direction.x, lighting.Direction.y, lighting.Direction.z};
					capture.Width = colour.Width;
					capture.Height = colour.Height;
					const auto draws = interface->RecordWorldBatch(capture, batch);
					Result.DrawCalls += draws;
					SDL_EndGPURenderPass(batchPass);
					if (!draws) continue;
					SDL_GPUColorTargetInfo mergeTargets[1] = {targets[0]};
					for (auto &target : mergeTargets) {
						target.load_op = SDL_GPU_LOADOP_LOAD;
						target.cycle = false;
					}
					auto mergeDepth = depthTarget;
					mergeDepth.load_op = SDL_GPU_LOADOP_LOAD;
					mergeDepth.cycle = false;
					auto *merge =
						SDL_BeginGPURenderPass(Command, mergeTargets, 1, phase == 0 ? &mergeDepth : nullptr);
					if (!merge) return false;
					++recordedPasses;
					State->BindPipeline(
						merge,
						phase == 0 ? State->InterfaceLayerPipeline : State->InterfaceLayerColourPipeline,
						Impl::PipelineFamily::Other
					);
					SDL_SetGPUViewport(merge, &viewport);
					const SDL_Rect scissor{0, 0, int(colour.Width), int(colour.Height)};
					SDL_SetGPUScissor(merge, &scissor);
					const SDL_GPUTextureSamplerBinding samplers[] = {
						{scratchColour.Texture, State->OverlaySampler},
						{scratchZ.Texture, State->OverlaySampler},
						{opaque.Texture, State->OverlaySampler},
						{phase == 1			  ? z.Texture
						 : previous.IsValid() ? previous.Texture
											  : opaque.Texture,
						 State->OverlaySampler}
					};
					SDL_BindGPUFragmentSamplers(merge, 0, samplers, 4);
					const auto eye = Request.CameraFrame.Position, forward = Request.CameraFrame.LookVector();
					struct Uniforms {
						glm::mat4 Inverse;
						glm::vec4 CameraDepth, Flags;
					} uniforms{
						glm::inverse(Matrices.ViewProjection),
						{forward.X, forward.Y, forward.Z, -forward.Dot(eye)},
						{phase == 0 && previous.IsValid() ? 1.f : 0.f, phase == 1 ? 1.f : 0.f, 0, 0}
					};
					SDL_PushGPUFragmentUniformData(Command, 0, &uniforms, sizeof(uniforms));
					SDL_DrawGPUPrimitives(merge, 3, 1, 0, 0);
					++Result.DrawCalls;
					SDL_EndGPURenderPass(merge);
					core::Metrics::Count("render.transparent_layer.interface_batches", 1);
				}
			}

			// Body depth uses this same D32 reconstruction. Interpolated world depth
			// can put coincident glass in front of an opaque body after import.
			auto linearUniforms = Uniforms;
			linearUniforms.Target.z = linearUniforms.Target.w = 1;
			linearUniforms.Direction.w = 1;
			const std::array linearBindings{SDL_GPUTextureSamplerBinding{z.Texture, DepthBindings[0].sampler}
			};
			Fullscreen(
				context.Name,
				State->DepthLinearPipeline,
				depth.Texture,
				depth.Width,
				depth.Height,
				linearBindings,
				&linearUniforms,
				nullptr,
				SDL_FColor{}
			);
			++recordedPasses;
			core::Metrics::Count(
				"render.transparent_layer.depth_bytes", uint64_t(depth.Width) * depth.Height * 4
			);
			core::Metrics::Count("render.transparent_layer.passes", recordedPasses);
			core::Metrics::Count("render.transparent_layer.pixels", uint64_t(colour.Width) * colour.Height);
			return true;
		});

		frameNodes.Set(core::Name("transparent"), [this](const graph::RunContext &context) {
			ViewRecording &recording = *this;
			Impl *const State = recording.State;
			FrameResult &result = recording.Result;
			SDL_GPUCommandBuffer *const command = recording.Command;
			const core::CFrame &cameraFrame = recording.Request.CameraFrame;
			FrameOverlayHook *const gameInterfaceHook = recording.Request.GameInterfaceHook;
			const std::span<const effects::RibbonRun> ribbonRuns = recording.Request.RibbonRuns;
			const uint32_t sceneWidth = recording.SceneWidth;
			const uint32_t sceneHeight = recording.SceneHeight;
			const bool haveInstances = recording.HaveInstances;
			const size_t opaqueCount = recording.OpaqueCount;
			const glm::mat4 &lightViewProjection = recording.LightViewProjection;
			const uint32_t transparentCount = recording.TransparentCount;
			const uint32_t plainTransparent = recording.PlainTransparent;
			const uint32_t sceneCount = recording.SceneCount;
			const uint32_t instanceCount = recording.InstanceCount;
			const LightUniforms &lightUniforms = recording.SceneLights;
			const uint32_t particleCount = recording.ParticleCount;
			const uint32_t ribbonCount = recording.RibbonCount;
			const scene::CameraMatrices &matrices = recording.Matrices;
			const View::GroundGrid &groundGrid = recording.Request.Source->Grid;
			SDL_GPUColorTargetInfo &colourTarget = recording.ColourTarget;
			SDL_GPUDepthStencilTargetInfo &depthTarget = recording.DepthTarget;
			const bool drawInterface = recording.DrawInterface;
			const auto enterNamedPass = [&recording](
											core::Name name, SDL_GPUCommandBuffer *recordedCommand = nullptr
										) { recording.EnterNamedPass(name, recordedCommand); };
			const auto lightingAt = [&recording](
										const core::Vector3 &eye, float surfaceMode, float imageOpacity
									) { return recording.LightingAt(eye, surfaceMode, imageOpacity); };
			const auto graphTexture =
				[&recording](graph::ResourceId resource, const graph::RunContext &runContext, bool make) {
					return recording.GraphTexture(resource, runContext, make);
				};
			const auto drawImage = [&recording](
									   const Impl::NamedTexture &source,
									   const Impl::NamedTexture &target,
									   SDL_GPULoadOp load,
									   bool reverseSpectrum = false
								   ) { return recording.DrawImage(source, target, load, reverseSpectrum); };

			ENGINE_PROFILE_CAT("transparent pass", core::ProfileCategory::Render);
			// Entered unconditionally, and that is the honest reading rather
			// than a convenience: the stage clears colour and depth, so a frame
			// with nothing in it still ran this pass - the background is what it
			// drew. `Validate` sees the same thing, because the stage's writes
			// are marked `Clear`.
			enterNamedPass(context.Name);

			Impl::NamedTexture source;
			Impl::NamedTexture target;
			if (!context.Reads.empty()) {
				source = graphTexture(context.Reads.front(), context, false);
			}
			for (const graph::ResourceId resource : context.Writes) {
				target = graphTexture(resource, context, true);
				if (target.IsValid()) {
					break;
				}
			}
			if (!drawImage(source, target, SDL_GPU_LOADOP_CLEAR)) {
				ENGINE_WARN("'{}' needs a scene image and an output image", context.Name.Text());
				return true;
			}
			const bool hdr = target.Format == SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
			const auto worldTarget = hdr ? WorldColourTarget::Hdr : WorldColourTarget::Display;
			colourTarget.texture = target.Texture;
			colourTarget.load_op = SDL_GPU_LOADOP_LOAD;
			colourTarget.store_op = SDL_GPU_STOREOP_STORE;
			colourTarget.cycle = false;

			SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &colourTarget, 1, &depthTarget);
			// **The light set, pushed once for the whole pass.** Uniform state
			// on a command buffer persists until it is replaced, so one push
			// before the draws serves every one of them - which is the whole
			// reason this is a second buffer rather than fields on the
			// per-draw `LightingUniforms`.
			SDL_PushGPUFragmentUniformData(command, 1, &lightUniforms, sizeof(lightUniforms));

			// **The beams, beside the lights and for the same reason.**
			// Which holes carry a shadow is a fact about the frame, so it
			// is pushed once per pass rather than per draw - and it is
			// pushed even when there are none, because a stale block from
			// a previous frame would shadow through a hole that is no
			// longer there.
			SDL_PushGPUFragmentUniformData(command, 2, &State->Beams, sizeof(State->Beams));

			// **The world's rectangle inside an attachment that is larger than
			// it.** Without this the pass inherits a viewport covering the whole
			// texture, and a block-rounded target would draw the world into
			// 1600x960 while the panel shows the 1600x900 corner - the image
			// squashed by the rounding. Set once here and inherited by the
			// transparent draws in the same pass. See `SCENE_TARGET_BLOCK`.
			//
			// Correct on the window path too, where the two sizes are equal and
			// this restates the default rather than changing it.
			const SDL_GPUViewport view{
				0.0f, 0.0f, static_cast<float>(sceneWidth), static_cast<float>(sceneHeight), 0.0f, 1.0f
			};
			SDL_SetGPUViewport(pass, &view);

			// The scissor goes with it. A viewport shrinks what is drawn but
			// does not clip what a pipeline with no depth test could still
			// scribble outside it, and the border is memory nothing owns.
			const SDL_Rect scissor{0, 0, static_cast<int>(sceneWidth), static_cast<int>(sceneHeight)};
			SDL_SetGPUScissor(pass, &scissor);

			// **The ground grid, first in this pass and nowhere else.** It is
			// here rather than in a node of its own because a node would need
			// the depth as a *sampler* and this pass already has it as an
			// attachment - so the hardware does the occluding and the grid
			// costs one triangle. First, so a transparent pane blends over it
			// the way it blends over the floor.
			//
			// Off unless a view asked, which is the studio asking for an edited
			// world. A client pays one branch.
			if (groundGrid.Enabled && State->GridPipeline != nullptr) {
				ENGINE_PROFILE_CAT("ground grid", core::ProfileCategory::Render);

				GridUniforms gridUniforms;
				gridUniforms.ViewProjection = matrices.ViewProjection;
				gridUniforms.InverseViewProjection = glm::inverse(matrices.ViewProjection);
				gridUniforms.Eye =
					glm::vec4{cameraFrame.Position.X, cameraFrame.Position.Y, cameraFrame.Position.Z, 0.0f};
				gridUniforms.Params =
					glm::vec4{groundGrid.Step, groundGrid.Major, groundGrid.Reach, groundGrid.Strength};
				gridUniforms.Offset = glm::vec4{groundGrid.Offset.X, groundGrid.Offset.Z, 0.0f, 0.0f};
				gridUniforms.Colour = glm::vec4{
					groundGrid.Colour.R, groundGrid.Colour.G, groundGrid.Colour.B, groundGrid.Alpha
				};
				gridUniforms.AxisX = glm::vec4{
					groundGrid.AxisX.R, groundGrid.AxisX.G, groundGrid.AxisX.B, groundGrid.AxisAlpha
				};
				gridUniforms.AxisZ = glm::vec4{
					groundGrid.AxisZ.R, groundGrid.AxisZ.G, groundGrid.AxisZ.B, groundGrid.AxisAlpha
				};

				SDL_BindGPUGraphicsPipeline(pass, State->GridPipeline);
				SDL_PushGPUFragmentUniformData(command, 0, &gridUniforms, sizeof(gridUniforms));
				SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
				result.DrawCalls++;

				// **Bound directly rather than through `BindPipeline`, so the
				// tracked one is cleared by hand.** `DrawSlots` reads
				// `ActivePipeline` to know what to return to after a shader
				// variant, and leaving the grid there would send an instance
				// draw back to a fullscreen triangle's pipeline.
				State->ActivePipeline = nullptr;
			}

			if (haveInstances || drawInterface || particleCount > 0 || ribbonCount > 0) {
				State->BindPipeline(
					pass,
					hdr ? State->HdrOpaquePipeline : State->OpaquePipeline,
					hdr ? Impl::PipelineFamily::HdrOpaque : Impl::PipelineFamily::Opaque
				);

				if (haveInstances) {
					State->BindInstanceBuffers(pass);
					const SDL_GPUBufferBinding indexBinding{State->Meshes.Indices(), 0};
					SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
				}

				const glm::mat4 &viewProjection = matrices.ViewProjection;

				const FrameUniforms frameUniforms{
					viewProjection,
					lightViewProjection,
					glm::mat4{1.0f},
				};
				SDL_PushGPUVertexUniformData(command, 0, &frameUniforms, sizeof(frameUniforms));

				// **The surface flag is off for the opaque range and on for a
				// second draw over the instances that carry one.** Whether an
				// instance samples the surface is per instance and the uniform
				// is per draw, so the split is a third draw rather than a
				// per-fragment branch on data the shader does not have.
				const LightingUniforms lighting = lightingAt(cameraFrame.Position, 0.0f, 0.0f);
				SDL_PushGPUFragmentUniformData(command, 0, &lighting, sizeof(lighting));

				// Both samplers, every draw. A shadow map that was not rendered
				// binds another texture in its place rather than nothing: the
				// flag above is what stops it being read, and an unbound sampler
				// is undefined behaviour on several backends where a wrongly
				// bound one is merely ignored.
				//
				// **`FallbackTexture` rather than `OverlayTexture`**, which only
				// exists while a debug panel has something in it. A scene of
				// nothing but transparent geometry casts nothing, so the shadow
				// map is absent too - and with the panels closed both were null
				// and the guard below skipped the bind and drew anyway. See
				// `Impl::FallbackTexture`.
				SDL_GPUTexture *const shadow =
					State->ShadowTexture != nullptr ? State->ShadowTexture : State->FallbackTexture;
				SDL_GPUSampler *const shadowSampler =
					State->ShadowSampler != nullptr ? State->ShadowSampler : State->OverlaySampler;
				SDL_GPUSampler *const surfaceSampler =
					State->SurfaceSampler != nullptr ? State->SurfaceSampler : shadowSampler;

				if (drawInterface) {
					result.DrawCalls += gameInterfaceHook->RecordWorld(
						command,
						pass,
						viewProjection,
						cameraFrame,
						core::Color3{State->Ambient.x, State->Ambient.y, State->Ambient.z},
						core::Vector3{State->Sun.x, State->Sun.y, State->Sun.z},
						sceneWidth,
						sceneHeight,
						false,
						worldTarget
					);
				}

				if (transparentCount > 0) {
					// Same pass, same depth attachment, different pipeline -
					// blending on and depth writes off. A separate render pass
					// would have to reload the depth buffer, and the whole point
					// is that these fragments are tested against what the opaque
					// pass already wrote.
					//
					// Still its own stage, sharing a render pass. What the list
					// describes is what is drawn and in what order, not how many
					// times a target is bound.
					State->BindPipeline(
						pass,
						hdr ? State->HdrTransparentPipeline : State->TransparentPipeline,
						hdr ? Impl::PipelineFamily::HdrTransparent : Impl::PipelineFamily::Transparent
					);

					if (plainTransparent > 0) {
						result.DrawCalls += State->DrawSlots(
							command,
							pass,
							sceneCount + static_cast<uint32_t>(opaqueCount),
							plainTransparent,
							&lighting,
							shadow,
							shadowSampler,
							nullptr,
							surfaceSampler,
							0,
							result.Triangles
						);
					}
				}

				// --- particles ---------------------------------------------
				//
				// **After every blended run and inside the same pass**, which is
				// the arrangement the header states: a particle is depth-tested
				// against the world and drawn over the glass. Sorting half a
				// million particles into the geometry's own order would cost more
				// than the artefact of not doing it.
				//
				// **Not their own node**, deliberately. Particles share the ordered
				// transparent target and depth state, so the graph's `transparent`
				// node owns them along with blended geometry. Splitting that node
				// would require a resource edge and an independently executable
				// backend operation, not another fixed pass label in this body.
				if (particleCount > 0) {
					result.DrawCalls += State->DrawParticles(
						command,
						pass,
						frameUniforms.ViewProjection,
						cameraFrame,
						result.Triangles,
						result.ParticlesDrawn,
						result.Culled,
						worldTarget
					);
				}

				// The beams and trails, after the particles. See the header for
				// why the order is fixed rather than sorted.
				if (ribbonCount > 0) {
					result.DrawCalls += State->DrawRibbons(
						command,
						pass,
						frameUniforms.ViewProjection,
						cameraFrame,
						ribbonRuns,
						result.Triangles,
						worldTarget
					);
				}

				if (drawInterface) {
					result.DrawCalls += gameInterfaceHook->RecordWorld(
						command,
						pass,
						viewProjection,
						cameraFrame,
						core::Color3{State->Ambient.x, State->Ambient.y, State->Ambient.z},
						core::Vector3{State->Sun.x, State->Sun.y, State->Sun.z},
						sceneWidth,
						sceneHeight,
						true,
						worldTarget
					);
				}

				// **Counted as it is drawn rather than derived from the instance
				// count.** While everything was a cube, triangles were thirty-six
				// indices times however many instances; with a mesh per instance
				// there is no such multiplier, and the honest number is the one
				// `DrawSlots` accumulated. `instanceCount` is still what the
				// instance counter reports.
				(void)instanceCount;
			}

			SDL_EndGPURenderPass(pass);
			return true;
		});
	}
}
