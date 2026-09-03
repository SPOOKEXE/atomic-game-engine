// Turning a draw order into draw calls, and the buffers they read.
//
// `DrawSlots` is the inner loop of every pass in the module: it walks a run of
// instance slots, splits it wherever the mesh, the textures or the shader
// change, and issues one call per run. `render/AGENTS.md` states the rule it
// must keep - the runs are split but never sorted, because the blended pass's
// order is back-to-front from the eye and reordering it is the transparency bug
// the sort exists to prevent.

#include "DisplayColour.hpp"
#include "RenderTypes.hpp"
#include "RendererState.hpp"
#include "VulkanTimestamps.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/render/MissingTexture.hpp>
#include <engine/scene/Tagging.hpp>

#include <SDL3/SDL_gpu.h>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace engine::render {
	void Renderer::Impl::BindInstanceBuffers(SDL_GPURenderPass *pass, SDL_GPUBuffer *indices) {
		const SDL_GPUBufferBinding vertices{Meshes.Vertices(), 0};
		SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);

		SDL_GPUBuffer *const storage[] = {
			InstanceBuffer,
			indices != nullptr ? indices : InstanceIndexBuffer,
			SkinOffsetBuffer,
			JointBuffer,
		};
		SDL_BindGPUVertexStorageBuffers(pass, 0, storage, 4);
	}

	uint32_t Renderer::Impl::DrawSlots(
		SDL_GPUCommandBuffer *command,
		SDL_GPURenderPass *pass,
		uint32_t first,
		uint32_t count,
		const LightingUniforms *lighting,
		SDL_GPUTexture *shadow,
		SDL_GPUSampler *shadowSampler,
		SDL_GPUTexture *surface,
		SDL_GPUSampler *surfaceSampler,
		uint32_t tagFilter,
		uint64_t &triangles,
		const IndirectPhase *indirect
	) {
		if (count == 0 || first + count > SlotMesh.size()) {
			return 0;
		}

		// **Nine call sites and three of them were outside every span.** This is
		// where a pass turns its slot range into draw calls, so it is the CPU
		// cost of recording geometry - the thing a frame graph is being read to
		// find. Named here rather than at each caller, which is what stops the
		// tenth caller from being the one that forgets.
		ENGINE_PROFILE_CAT("draw slots", core::ProfileCategory::Render);

		uint32_t calls = 0;

		// Which slot-run and which draw argument the walk is at, for the
		// indirect phases. Argument order **is** walk order - the builder in
		// the upload path walks the same slots with the same predicate, so an
		// index here names the entry it wrote there.
		uint32_t slotRun = 0;
		uint32_t argument = 0;

		// What the pass bound before this call, so it can be put back.
		//
		// **Restored rather than left**, because a caller issues several
		// `DrawSlots` in one pass and every one of them assumes the pass's own
		// pipeline is bound. A run that ended on a variant would hand the next
		// call somebody else's fragment shader.
		SDL_GPUGraphicsPipeline *const base = ActivePipeline;
		SDL_GPUGraphicsPipeline *bound = base;
		if (lighting == nullptr) {
			const SDL_GPUTextureSamplerBinding defaultSampler{
				Textures.Default() != nullptr ? Textures.Default() : FallbackTexture,
				Textures.Sampler(),
			};
			SDL_BindGPUFragmentSamplers(pass, 0, &defaultSampler, 1);
			const ShadowUniforms defaultUniforms;
			SDL_PushGPUFragmentUniformData(command, 0, &defaultUniforms, sizeof(defaultUniforms));
		}

		// One draw for one range of one mesh, over `run` consecutive instances.
		const auto emit = [&](const MeshRange &range,
							  const core::Name &texture,
							  const std::array<float, 4> &colour,
							  uint32_t slot,
							  uint32_t run,
							  bool simpleShadow) {
			if (range.IndexCount == 0) {
				return;
			}

			if (lighting != nullptr) {
				// **The default, not the fallback texel, and not "do not
				// sample".** A drawable naming no texture is not a drawable with
				// nothing to draw - it is one made of the engine's default
				// material, which `DefaultTexture.hpp` says is a real sheet of
				// white plastic. So the slot always has content and the shader
				// always samples it; what used to be the "no texture" branch is
				// now the case where `Material = None` means something.
				//
				// **And naming a texture that is not here is a third case, not
				// the first one again.** A part that asked for a sheet the table
				// does not hold gets the purple checkerboard, because "nobody
				// textured this" and "this asked for something that never
				// arrived" are different facts and only one of them is finished.
				// The same split `scene::KeepLoaded` makes for geometry.
				//
				// **A sheet still on its way is the first case, not the third**,
				// which is `D00107` closed: the marker now means *nothing is
				// coming*. `ChooseTexture` is the rule and carries the argument;
				// it is a free function so a suite can state it without a
				// device.
				SDL_GPUTexture *const found = Textures.Find(texture);
				const TextureChoice choice =
					ChooseTexture(found != nullptr, texture.IsValid(), Textures.Expecting(texture));

				// **Untextured draws the default and not the named image**, and
				// it is one substitution rather than a second pipeline family
				// because a texture is a *binding* and a fill mode is not -
				// `WireframeMode` had to become pipelines and this does not.
				// What it is for is the collider view: a collision shape drawn
				// over a textured scene is a wireframe over a photograph, and
				// nothing about the shape is legible in it.
				SDL_GPUTexture *const sampled = UntexturedMode					   ? Textures.Default()
												: choice == TextureChoice::Named   ? found
												: choice == TextureChoice::Missing ? Textures.Missing()
																				   : Textures.Default();
				const bool absent = !UntexturedMode && choice == TextureChoice::Missing;

				const auto dataMap = [&](core::Name name) {
					if (UntexturedMode) {
						// The data maps go with it. A normal map on a flat white
						// surface is the one that still reads, and reading it is
						// exactly what makes a shape hard to see.
						return static_cast<SDL_GPUTexture *>(nullptr);
					}
					SDL_GPUTexture *foundMap = Textures.Find(name);
					if (foundMap != nullptr) {
						return foundMap;
					}
					if (name.IsValid() && !Textures.Expecting(name)) {
						return Textures.Missing();
					}
					return static_cast<SDL_GPUTexture *>(nullptr);
				};

				SDL_GPUTexture *const normal = dataMap(SlotNormalMap[slot]);
				SDL_GPUTexture *const roughness = dataMap(SlotRoughnessMap[slot]);
				SDL_GPUTexture *const occlusion = dataMap(SlotOcclusionMap[slot]);
				SDL_GPUTexture *const height = dataMap(SlotHeightMap[slot]);
				SDL_GPUTexture *const metalness = dataMap(SlotMetalnessMap[slot]);
				SDL_GPUTexture *const emissive = dataMap(SlotEmissiveMap[slot]);
				SDL_GPUSampler *const materialSampler =
					SlotResample[slot] == scene::SurfaceResampleMode::Pixelated ? Textures.PixelSampler()
																				: Textures.Sampler();
				SDL_GPUSampler *const fallbackSampler =
					surfaceSampler != nullptr ? surfaceSampler : Textures.Sampler();
				SDL_GPUSampler *const sampledShadowSampler =
					shadowSampler != nullptr ? shadowSampler : fallbackSampler;
				SDL_GPUSampler *const sampledBeamSampler =
					ShadowSampler != nullptr ? ShadowSampler : sampledShadowSampler;

				// **The fallback texel is bound rather than the binding being
				// skipped** for data maps, because a sampler a pipeline
				// declares must have something in it - an unbound one is
				// undefined behaviour on some backends and a validation error on
				// others. Those two are genuinely absent features rather than
				// defaulted ones, and their uniform flags still say so.
				// **A fourth sampler for the beams**, bound from the member
				// rather than passed in: which holes transport a shadow is a
				// property of the frame and not of one draw, and threading it
				// through would put it in a signature five passes call.
				const SDL_GPUTextureSamplerBinding samplers[] = {
					{shadow != nullptr ? shadow : FallbackTexture, sampledShadowSampler},
					{surface != nullptr ? surface : FallbackTexture, fallbackSampler},
					{sampled != nullptr ? sampled : FallbackTexture, materialSampler},
					{BeamTexture != nullptr ? BeamTexture : FallbackTexture, sampledBeamSampler},
					{normal != nullptr ? normal : FallbackTexture, materialSampler},
					{roughness != nullptr ? roughness : FallbackTexture, materialSampler},
					{occlusion != nullptr ? occlusion : FallbackTexture, materialSampler},
					{emissive != nullptr ? emissive : FallbackTexture, materialSampler},
					{height != nullptr ? height : FallbackTexture, materialSampler},
					{metalness != nullptr ? metalness : FallbackTexture, materialSampler},
				};
				SDL_BindGPUFragmentSamplers(pass, 0, samplers, 10);

				LightingUniforms uniforms = *lighting;

				// **The marker arrives as itself, so the base colour stops
				// applying to it.** Every other texture here is modulated by the
				// material's colour, which is what makes one grey sheet serve a
				// whole palette - but a magenta check multiplied by a dark red
				// part is a dark pattern that reads as somebody's intent. A
				// marker that can be tinted into looking deliberate is not a
				// marker.
				const std::array<float, 4> tint =
					absent ? std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f} : colour;
				uniforms.BaseColour = glm::vec4{tint[0], tint[1], tint[2], tint[3]};
				uniforms.Surface = glm::vec4{
					sampled != nullptr ? 1.0f : 0.0f,
					0.0f,
					height != nullptr ? 1.0f : 0.0f,
					0.04f,
				};
				uniforms.Material = glm::vec4{
					normal != nullptr ? 1.0f : 0.0f,
					roughness != nullptr ? 1.0f : 0.0f,
					occlusion != nullptr ? 1.0f : 0.0f,
					emissive != nullptr ? 1.0f : 0.0f,
				};
				uniforms.MaterialExtra = glm::vec4{metalness != nullptr ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};

				// **The cell is per draw, not per instance**, which is the whole
				// simplification: a sheet plays on the clock rather than on
				// anything an entity carries, so every part showing one GIF shows
				// the same frame. A per-instance phase is a real feature and is a
				// different one - `effects::FlipbookLayout` already has it for
				// particles, where the cell is a function of a particle's age.
				const FlipbookCell cell = Textures.CellOf(texture, AnimationSeconds);
				uniforms.Flipbook = glm::vec4{cell.Scale, cell.OffsetU, cell.OffsetV, 0.0f};

				// **The clock is stamped here and the effect arrives from the
				// caller.** Which effect is a fact about the surface being
				// sampled; what time it is, is a fact about the frame, and this
				// is the one place that already holds it.
				//
				// Wrapped rather than passed whole: a `float` a session old has
				// lost the precision these effects step by, and a scan line that
				// coarsens over an afternoon is a bug nobody would reproduce.
				uniforms.Mirror.y = static_cast<float>(std::fmod(AnimationSeconds, 1000.0));

				// **The slot's own plane, and the run above guarantees they agree.**
				// A run is broken wherever the plane changes, so every instance in
				// this draw is cut the same way - which is what makes a per-instance
				// fact expressible in a per-draw uniform.
				uniforms.SeamPlane = SlotSeam[slot];

				// **The far half of a body in a hole is lit by the sun turned the way
				// the body was.** Its normals are the near half's rotated by the
				// seam, so the world's own direction would shade one body with two
				// suns a quarter apart. Set per draw for the same reason the plane is,
				// and only where the row asked for it - a zero vector is every
				// instance that is not half of something.
				const glm::vec3 mapped{SlotSeamLight[slot]};
				if (glm::dot(mapped, mapped) > 0.0f) {
					uniforms.Direction = glm::vec4{mapped, 0.0f};
				}

				SDL_PushGPUFragmentUniformData(command, 0, &uniforms, sizeof(uniforms));
			} else if (!simpleShadow) {
				// **Depth only, and it still has to know where the body is cut.** A
				// half drawn whole into the shadow map casts a whole body's shadow,
				// so the near half of somebody in a doorway would darken the floor as
				// if none of them had gone through. `shadow.frag` takes the plane and
				// discards on the same test `opaque.frag` makes. It also binds the
				// colour map so a clipped surface casts its authored silhouette, but
				// still needs only the compact shadow block rather than all lighting.
				SDL_GPUTexture *const found = Textures.Find(texture);
				const TextureChoice choice =
					ChooseTexture(found != nullptr, texture.IsValid(), Textures.Expecting(texture));
				SDL_GPUTexture *const sampled = choice == TextureChoice::Named	   ? found
												: choice == TextureChoice::Missing ? Textures.Missing()
																				   : Textures.Default();
				const SDL_GPUTextureSamplerBinding sampler{
					sampled != nullptr ? sampled : FallbackTexture,
					SlotResample[slot] == scene::SurfaceResampleMode::Pixelated ? Textures.PixelSampler()
																				: Textures.Sampler(),
				};
				SDL_BindGPUFragmentSamplers(pass, 0, &sampler, 1);

				ShadowUniforms uniforms;
				uniforms.Plane = SlotSeam[slot];
				uniforms.Material.x = colour[3];
				const FlipbookCell cell = Textures.CellOf(texture, AnimationSeconds);
				uniforms.Flipbook = glm::vec4{cell.Scale, cell.OffsetU, cell.OffsetV, 0.0f};
				SDL_PushGPUFragmentUniformData(command, 0, &uniforms, sizeof(uniforms));
			}

			if (indirect != nullptr) {
				// The counts live on the GPU. `run` here is the phase's upper
				// bound, so the triangle tally can only overcount what the
				// cull discarded - an estimate is honest for a number whose
				// exact value never comes back to the CPU.
				SDL_DrawGPUIndexedPrimitivesIndirect(
					pass,
					indirect->Arguments,
					(indirect->FirstArgument + argument) *
						static_cast<uint32_t>(sizeof(SDL_GPUIndexedIndirectDrawCommand)),
					1
				);
				argument++;
			} else {
				SDL_DrawGPUIndexedPrimitives(
					pass, range.IndexCount, run, range.FirstIndex, range.VertexOffset, slot
				);
			}
			calls++;
			triangles += static_cast<uint64_t>(range.IndexCount / 3) * run;
		};

		uint32_t slot = first;
		while (slot < first + count) {
			// **A filtered-out slot ends the run and is stepped over.** The draw
			// list is not re-ordered for it: the order is shared by every view
			// and a filter is per view, so partitioning for one surface would
			// partition for all of them and the screen pass would draw the group
			// instead of the world. The cost is a run break wherever an excluded
			// instance sits between two included ones, which is a draw call and
			// not a wrong picture.
			if (!scene::MatchesTags(SlotTags[slot], tagFilter)) {
				slot++;
				continue;
			}

			const MeshEntry *const mesh = SlotMesh[slot];
			const core::Name texture = SlotTexture[slot];

			// **The shader joins what ends a run, because it is a pipeline.** A
			// pipeline bind is per draw at best, so two instances drawn through
			// different fragment shaders cannot share one - the same reason the
			// seam plane breaks a run, one level up from a uniform. A world
			// where nothing selects a shader holds one invalid name throughout
			// and never breaks on it. `SlotsShareRun` is the whole rule: the
			// data maps, the shader, the seam plane and its light all join the
			// mesh and the texture in what ends a run.
			const core::Name shader = SlotShader[slot];

			uint32_t run = 1;
			bool simpleShadow = lighting == nullptr && SlotShadowDetail[slot] == 0;
			while (slot + run < first + count && SlotsShareRun(slot, slot + run) &&
				   scene::MatchesTags(SlotTags[slot + run], tagFilter)) {
				simpleShadow = simpleShadow && SlotShadowDetail[slot + run] == 0;
				run++;
			}

			// **An indirect phase with nothing in this run skips its binds but
			// not its argument entries.** The argument buffer holds every run's
			// draws in walk order, so a run stepped over still advances the
			// index by the draws it would have emitted.
			const uint32_t phaseInstances =
				indirect != nullptr
					? (slotRun < indirect->RunDraws->size() ? (*indirect->RunDraws)[slotRun] : 0u)
					: run;
			if (indirect != nullptr && phaseInstances == 0) {
				argument += DrawArgumentCount(*mesh);
				slotRun++;
				slot += run;
				continue;
			}

			// **Bound per run and only where it changes.** A scene with no
			// custom shaders never enters this branch, and one where every part
			// wears the same one binds twice: once here and once on the way out.
			SDL_GPUGraphicsPipeline *const wanted = VariantFor(shader);
			SDL_GPUGraphicsPipeline *const want = wanted != nullptr ? wanted : base;
			if (want != bound && want != nullptr) {
				SDL_BindGPUGraphicsPipeline(pass, want);
				bound = want;
			}

			if (mesh->Runs.empty()) {
				// A mesh with no materials of its own - every built-in.
				emit(mesh->Whole, texture, {1.0f, 1.0f, 1.0f, 1.0f}, slot, phaseInstances, simpleShadow);
			} else {
				for (size_t index = 0; index < mesh->Runs.size(); index++) {
					// **The instance's texture wins when it has one.** That is
					// what `MeshPart.TextureID` means: an imported mesh's own
					// per-material sheets are the default, and a part may
					// replace all of them at once.
					emit(
						mesh->Runs[index],
						texture.IsValid() ? texture : mesh->Textures[index],
						mesh->Colours[index],
						slot,
						phaseInstances,
						simpleShadow
					);
				}
			}

			slotRun++;
			slot += run;
		}

		// Put the pass's own pipeline back - see `base` above.
		if (bound != base && base != nullptr) {
			SDL_BindGPUGraphicsPipeline(pass, base);
		}

		return calls;
	}

	bool Renderer::Impl::CreateGeometry() {
		// The built-in meshes and the sampler every texture shares. What used
		// to be an unconditional upload of one cube is now a table that starts
		// with six shapes and grows as content arrives.
		if (!Meshes.Initialise(Device) || !Textures.Initialise(Device)) {
			return false;
		}

		// The sampler stand-in, created here because this function already owns
		// a command buffer and a copy pass to fill it. See `FallbackTexture`.
		{
			SDL_GPUTextureCreateInfo info{};
			info.type = SDL_GPU_TEXTURETYPE_2D;
			info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
			info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
			info.width = 1;
			info.height = 1;
			info.layer_count_or_depth = 1;
			info.num_levels = 1;
			info.sample_count = SDL_GPU_SAMPLECOUNT_1;

			FallbackTexture = gpu::CreateTexture(Device, &info);
			if (!FallbackTexture) {
				ENGINE_ERROR("fallback texture: {}", SDL_GetError());
				return false;
			}
		}

		// One texel, and the transfer buffer is temporary because nothing
		// rewrites it.
		SDL_GPUTransferBufferCreateInfo transferInfo{};
		transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		transferInfo.size = OverlayImage::BYTES_PER_PIXEL;

		SDL_GPUTransferBuffer *transfer = gpu::CreateTransferBuffer(Device, &transferInfo);
		if (!transfer) {
			ENGINE_ERROR("cube transfer buffer: {}", SDL_GetError());
			return false;
		}

		// Opaque white for the fallback texel. Nothing reads it - the uniform
		// flag sees to that - but a texture whose contents were never written
		// is uninitialised device memory, and "nothing reads it" is a claim
		// about the shaders of the day rather than a property of the resource.
		constexpr uint8_t FALLBACK_TEXEL[OverlayImage::BYTES_PER_PIXEL] = {255, 255, 255, 255};

		auto *mapped = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(Device, transfer, false));
		std::memcpy(mapped, FALLBACK_TEXEL, sizeof(FALLBACK_TEXEL));
		SDL_UnmapGPUTransferBuffer(Device, transfer);

		SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(Device);
		SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);

		SDL_GPUTextureTransferInfo texel{};
		texel.transfer_buffer = transfer;
		texel.offset = 0;
		texel.pixels_per_row = 1;
		texel.rows_per_layer = 1;

		SDL_GPUTextureRegion texelTarget{};
		texelTarget.texture = FallbackTexture;
		texelTarget.w = 1;
		texelTarget.h = 1;
		texelTarget.d = 1;
		SDL_UploadToGPUTexture(copy, &texel, &texelTarget, false);

		SDL_EndGPUCopyPass(copy);
		SDL_SubmitGPUCommandBuffer(command);
		gpu::ReleaseTransferBuffer(Device, transfer);

		return true;
	}

	bool Renderer::Impl::EnsureInstanceCapacity(
		uint32_t rows, uint32_t indices, bool &rowsReallocated, bool &indicesReallocated
	) {
		SceneSlot &slot = SlotAt(ActiveSlot);
		if (ActiveInstanceWorld == nullptr) {
			return false;
		}
		InstanceWorld &world = *ActiveInstanceWorld;
		rowsReallocated = false;
		indicesReallocated = false;

		const auto grown = [](uint32_t have, uint32_t need) {
			uint32_t capacity = have == 0 ? 256 : have;
			while (capacity < need) {
				capacity *= 2;
			}
			return capacity;
		};

		if (rows > world.Capacity || world.Buffer == nullptr || world.Transfer == nullptr) {
			const uint32_t capacity = grown(world.Capacity, rows);
			if (world.Buffer != nullptr) {
				gpu::ReleaseBuffer(Device, world.Buffer);
			}
			if (world.Transfer != nullptr) {
				gpu::ReleaseTransferBuffer(Device, world.Transfer);
			}

			const uint32_t bytes = capacity * static_cast<uint32_t>(sizeof(GpuInstance));
			SDL_GPUBufferCreateInfo bufferInfo{};
			bufferInfo.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
			bufferInfo.size = bytes;
			world.Buffer = gpu::CreateBuffer(Device, &bufferInfo);
			SDL_GPUTransferBufferCreateInfo transferInfo{};
			transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
			transferInfo.size = bytes;
			world.Transfer = gpu::CreateTransferBuffer(Device, &transferInfo);
			if (world.Buffer == nullptr || world.Transfer == nullptr) {
				ENGINE_ERROR("resident instance buffer of {} entries: {}", capacity, SDL_GetError());
				world.Capacity = 0;
				return false;
			}
			world.Capacity = capacity;
			rowsReallocated = true;
		}

		const uint32_t indexVersion = static_cast<uint32_t>(FrameCounter % IndexResidency::VERSIONS);
		SceneSlot::InstanceIndexVersion &residentIndices = slot.InstanceIndexVersions[indexVersion];
		if (indices > residentIndices.Capacity || residentIndices.Buffer == nullptr ||
			residentIndices.Transfer == nullptr) {
			const uint32_t capacity = grown(residentIndices.Capacity, indices);
			if (residentIndices.Buffer != nullptr) {
				gpu::ReleaseBuffer(Device, residentIndices.Buffer);
			}
			if (residentIndices.Transfer != nullptr) {
				gpu::ReleaseTransferBuffer(Device, residentIndices.Transfer);
			}

			const uint32_t bytes = capacity * static_cast<uint32_t>(sizeof(uint32_t));
			SDL_GPUBufferCreateInfo bufferInfo{};
			bufferInfo.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
			bufferInfo.size = bytes;
			residentIndices.Buffer = gpu::CreateBuffer(Device, &bufferInfo);
			SDL_GPUTransferBufferCreateInfo transferInfo{};
			transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
			transferInfo.size = bytes;
			residentIndices.Transfer = gpu::CreateTransferBuffer(Device, &transferInfo);
			if (residentIndices.Buffer == nullptr || residentIndices.Transfer == nullptr) {
				ENGINE_ERROR("instance index buffer of {} entries: {}", capacity, SDL_GetError());
				residentIndices.Capacity = 0;
				return false;
			}
			residentIndices.Capacity = capacity;
			indicesReallocated = true;
		}

		InstanceBuffer = world.Buffer;
		InstanceTransfer = world.Transfer;
		InstanceCapacity = world.Capacity;
		InstanceIndexBuffer = residentIndices.Buffer;
		InstanceIndexTransfer = residentIndices.Transfer;
		InstanceIndexCapacity = residentIndices.Capacity;
		return true;
	}
}
