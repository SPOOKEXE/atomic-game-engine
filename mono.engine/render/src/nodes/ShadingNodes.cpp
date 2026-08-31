// The screen-space chain over the material head: linear depth, the hierarchy
// built from it, ambient occlusion, the deferred lighting resolve, and the
// grade that turns the result into a display image.
//
// **All five are fullscreen triangles, and they all go through
// `ViewRecording::Fullscreen`.** One function opens the pass, binds the
// samplers, pushes the uniforms and sets the viewport, so a node added here
// cannot forget the scissor and scribble outside its own rectangle.

#include "ViewRecording.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>

#include <algorithm>
#include <array>
#include <tuple>
#include <type_traits>

namespace engine::render {

	void ViewRecording::RegisterShadingNodes(NodeTable &frameNodes) {
		frameNodes.Set(core::Name("last-frame"), [this](const graph::RunContext &context) {
			ViewRecording &recording = *this;
			const auto enterNamedPass = [&recording](
											core::Name name, SDL_GPUCommandBuffer *recordedCommand = nullptr
										) { recording.EnterNamedPass(name, recordedCommand); };

			// `output-image` copies the completed graph output into renderer-owned
			// history. This node is the dependency and profile boundary at which the
			// following frame may sample that image.
			enterNamedPass(context.Name);
			return true;
		});

		frameNodes.Set(core::Name("blit"), [this](const graph::RunContext &context) {
			if (context.Reads.size() != 1 || context.Writes.size() != 1) {
				return false;
			}
			const Impl::NamedTexture source = GraphTexture(context.Reads.front(), context, false);
			const Impl::NamedTexture target = GraphTexture(context.Writes.front(), context, true);
			if (!source.IsValid() || !target.IsValid()) {
				return false;
			}

			EnterNamedPass(context.Name);
			SDL_GPUBlitInfo blit{};
			blit.source.texture = source.Texture;
			blit.source.w = source.Width;
			blit.source.h = source.Height;
			blit.destination.texture = target.Texture;
			blit.destination.w = target.Width;
			blit.destination.h = target.Height;
			blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
			blit.filter = SDL_GPU_FILTER_LINEAR;
			blit.cycle = true;
			SDL_BlitGPUTexture(Command, &blit);
			return true;
		});

		frameNodes.Set(core::Name("depth-linearise"), [this](const graph::RunContext &context) {
			ViewRecording &recording = *this;
			Impl *const State = recording.State;
			const scene::Camera &drawCamera = recording.DrawCamera;
			const Impl::PbrDimensions &pbrDimensions = recording.PbrDimensions;
			Impl::PbrSlot &pbr = *recording.Pbr;
			PbrUniforms &uniforms = recording.Uniforms;
			const auto &depthBindings = recording.DepthBindings;
			const auto fullscreen = [&recording](
										core::Name name,
										SDL_GPUGraphicsPipeline *pipeline,
										SDL_GPUTexture *target,
										uint32_t passWidth,
										uint32_t passHeight,
										std::span<const SDL_GPUTextureSamplerBinding> bindings,
										const PbrUniforms *passUniforms,
										const LightUniforms *passLights,
										SDL_FColor clear
									) {
				recording.Fullscreen(
					name, pipeline, target, passWidth, passHeight, bindings, passUniforms, passLights, clear
				);
			};

			fullscreen(
				context.Name,
				State->DepthLinearPipeline,
				pbr.LinearDepth,
				pbrDimensions.LinearWidth,
				pbrDimensions.LinearHeight,
				depthBindings,
				&uniforms,
				nullptr,
				SDL_FColor{drawCamera.FarPlane, 0.0f, 0.0f, 0.0f}
			);
			return true;
		});

		// The authored depth hierarchy: the same pyramid the occlusion cull
		// seeds mid-gbuffer, rebuilt here over the *finished* depth so
		// screen-space consumers walk a pyramid that saw every opaque draw.
		// When the cull also ran this frame, this is a rebuild rather than a
		// duplicate resource - the levels are reused, and the cull already
		// consumed the version it made.
		frameNodes.Set(core::Name("hzb"), [this](const graph::RunContext &context) {
			ViewRecording &recording = *this;
			Impl *const State = recording.State;
			SDL_GPUCommandBuffer *const command = recording.Command;
			const uint32_t sceneWidth = recording.SceneWidth;
			const uint32_t sceneHeight = recording.SceneHeight;
			SDL_GPUDepthStencilTargetInfo &depthTarget = recording.DepthTarget;
			const auto enterNamedPass = [&recording](
											core::Name name, SDL_GPUCommandBuffer *recordedCommand = nullptr
										) { recording.EnterNamedPass(name, recordedCommand); };

			enterNamedPass(context.Name);
			if (State->Occlusion.Seed == nullptr || State->Occlusion.Reduce == nullptr) {
				// The compute shaders failed at startup; the log already
				// carries the reason, and a missing pyramid only disables what
				// reads it.
				return true;
			}
			if (!State->EnsurePyramid(sceneWidth, sceneHeight)) {
				return false;
			}
			State->BuildPyramid(command, depthTarget.texture);
			return true;
		});

		frameNodes.Set(core::Name("ssao"), [this](const graph::RunContext &context) {
			ViewRecording &recording = *this;
			Impl *const State = recording.State;
			const Impl::PbrDimensions &pbrDimensions = recording.PbrDimensions;
			Impl::PbrSlot &pbr = *recording.Pbr;
			PbrUniforms &uniforms = recording.Uniforms;
			SDL_GPUSampler *const sampler = recording.Sampler;
			const auto fullscreen = [&recording](
										core::Name name,
										SDL_GPUGraphicsPipeline *pipeline,
										SDL_GPUTexture *target,
										uint32_t passWidth,
										uint32_t passHeight,
										std::span<const SDL_GPUTextureSamplerBinding> bindings,
										const PbrUniforms *passUniforms,
										const LightUniforms *passLights,
										SDL_FColor clear
									) {
				recording.Fullscreen(
					name, pipeline, target, passWidth, passHeight, bindings, passUniforms, passLights, clear
				);
			};

			const std::array aoBindings = {
				SDL_GPUTextureSamplerBinding{pbr.LinearDepth, sampler},
				SDL_GPUTextureSamplerBinding{pbr.Normal, sampler},
			};
			fullscreen(
				context.Name,
				State->SsaoPipeline,
				pbr.Occlusion,
				pbrDimensions.OcclusionWidth,
				pbrDimensions.OcclusionHeight,
				aoBindings,
				&uniforms,
				nullptr,
				SDL_FColor{1.0f, 1.0f, 1.0f, 1.0f}
			);
			return true;
		});

		frameNodes.Set(core::Name("deferred-lighting"), [this](const graph::RunContext &context) {
			ViewRecording &recording = *this;
			Impl *const State = recording.State;
			const core::CFrame &cameraFrame = recording.Request.CameraFrame;
			Impl::SurfaceBank &bank = *recording.Bank;
			const LightUniforms &lightUniforms = recording.SceneLights;
			const Impl::PbrDimensions &pbrDimensions = recording.PbrDimensions;
			Impl::PbrSlot &pbr = *recording.Pbr;
			PbrUniforms &uniforms = recording.Uniforms;
			SDL_GPUSampler *const sampler = recording.Sampler;
			const auto &lightingBindings = recording.LightingBindings;
			const auto graphEnabled = [&recording](core::Name kind) { return recording.GraphEnabled(kind); };
			const auto clearOcclusion = [&recording] { recording.ClearOcclusion(); };
			const auto fullscreen = [&recording](
										core::Name name,
										SDL_GPUGraphicsPipeline *pipeline,
										SDL_GPUTexture *target,
										uint32_t passWidth,
										uint32_t passHeight,
										std::span<const SDL_GPUTextureSamplerBinding> bindings,
										const PbrUniforms *passUniforms,
										const LightUniforms *passLights,
										SDL_FColor clear
									) {
				recording.Fullscreen(
					name, pipeline, target, passWidth, passHeight, bindings, passUniforms, passLights, clear
				);
			};

			if (!graphEnabled(core::Name("ssao"))) {
				clearOcclusion();
			}

			// The seam light projectors, chosen and bound here rather than with
			// `lightingBindings`, because the capture textures are made inside
			// the portal-capture node this same frame - the graph's
			// portal-light edge is what guarantees that node has already run.
			// The nearest ready mouths win the two slots; empty slots stay
			// zeroed in `uniforms` and bind the fallback texel.
			// **The size comes from the type, not from `lightingBindings.size()`.**
			// That call is `constexpr` and never reads the object, so GCC and
			// Clang fold it - but the object itself cannot be `constexpr`, since
			// its elements are this frame's textures and samplers. MSVC requires
			// the object and refuses: `error C2975: '_Size': invalid template
			// argument for 'std::array', expected compile-time constant
			// expression`. `tuple_size_v` asks the type and never mentions the
			// object at all.
			constexpr size_t SPILL_BINDINGS =
				std::tuple_size_v<std::remove_cvref_t<decltype(lightingBindings)>> + MAX_SEAM_LIGHTS;
			std::array<SDL_GPUTextureSamplerBinding, SPILL_BINDINGS> spillBindings{};
			std::copy(lightingBindings.begin(), lightingBindings.end(), spillBindings.begin());

			std::array<const Impl::SeamLightTarget *, scene::MAX_SURFACES> ready{};
			size_t readyCount = 0;
			if (graphEnabled(core::Name("portal-capture"))) {
				for (const Impl::SeamLightTarget &seamLight : bank.SeamLights) {
					if (seamLight.Ready) {
						ready[readyCount++] = &seamLight;
					}
				}
			}
			const auto distanceTo = [&](const Impl::SeamLightTarget *candidate) {
				const core::Vector3 offset{
					candidate->Centre.x - cameraFrame.Position.X,
					candidate->Centre.y - cameraFrame.Position.Y,
					candidate->Centre.z - cameraFrame.Position.Z,
				};
				return offset.Dot(offset);
			};
			std::sort(
				ready.begin(),
				ready.begin() + static_cast<std::ptrdiff_t>(readyCount),
				[&](const Impl::SeamLightTarget *left, const Impl::SeamLightTarget *right) {
					return distanceTo(left) < distanceTo(right);
				}
			);

			std::array<const Impl::SeamLightTarget *, MAX_SEAM_LIGHTS> chosen{};
			for (size_t slot = 0; slot < chosen.size() && slot < readyCount; slot++) {
				chosen[slot] = ready[slot];
			}

			for (size_t slot = 0; slot < chosen.size(); slot++) {
				SDL_GPUTextureSamplerBinding &binding = spillBindings[lightingBindings.size() + slot];
				if (chosen[slot] != nullptr) {
					uniforms.SeamCentre[slot] = chosen[slot]->Centre;
					uniforms.SeamOutward[slot] = chosen[slot]->Outward;
					uniforms.SeamFirst[slot] = chosen[slot]->First;
					uniforms.SeamSecond[slot] = chosen[slot]->Second;
					binding = SDL_GPUTextureSamplerBinding{chosen[slot]->Colour, sampler};
				} else {
					uniforms.SeamCentre[slot] = glm::vec4{};
					uniforms.SeamOutward[slot] = glm::vec4{};
					uniforms.SeamFirst[slot] = glm::vec4{};
					uniforms.SeamSecond[slot] = glm::vec4{};
					binding = SDL_GPUTextureSamplerBinding{State->FallbackTexture, sampler};
				}
			}

			fullscreen(
				context.Name,
				State->DeferredLightingPipeline,
				pbr.Lit,
				pbrDimensions.LitWidth,
				pbrDimensions.LitHeight,
				spillBindings,
				&uniforms,
				&lightUniforms,
				SDL_FColor{State->FogColour.r, State->FogColour.g, State->FogColour.b, 1.0f}
			);
			return true;
		});

		frameNodes.Set(core::Name("sky"), [this](const graph::RunContext &context) {
			ViewRecording &recording = *this;
			Impl *const State = recording.State;
			Impl::PbrSlot &pbr = *recording.Pbr;
			PbrUniforms &uniforms = recording.Uniforms;
			SDL_GPUTexture *environment = State->EnsureEnvironment(
				recording.Request.World,
				recording.CurrentLighting.EnvironmentState,
				recording.Command,
				recording.Result.ComputeDispatches
			);
			uniforms.Fog.w = environment != nullptr ? 1.0f : 0.0f;
			const std::array bindings{
				SDL_GPUTextureSamplerBinding{pbr.Lit, recording.Sampler},
				SDL_GPUTextureSamplerBinding{recording.DepthTarget.texture, recording.Sampler},
				SDL_GPUTextureSamplerBinding{
					environment != nullptr ? environment : State->FallbackTexture, recording.Sampler
				},
			};
			recording.Fullscreen(
				context.Name,
				State->SkyPipeline,
				pbr.SkyLit,
				recording.PbrDimensions.LitWidth,
				recording.PbrDimensions.LitHeight,
				bindings,
				&uniforms,
				nullptr,
				SDL_FColor{}
			);
			return true;
		});

		frameNodes.Set(core::Name("tonemap"), [this](const graph::RunContext &context) {
			ViewRecording &recording = *this;
			Impl *const State = recording.State;
			const bool offscreen = recording.Offscreen;
			SDL_GPUColorTargetInfo &colourTarget = recording.ColourTarget;
			SDL_GPUDepthStencilTargetInfo &depthTarget = recording.DepthTarget;
			const auto &tonemapBindings = recording.TonemapBindings;
			const auto fullscreen = [&recording](
										core::Name name,
										SDL_GPUGraphicsPipeline *pipeline,
										SDL_GPUTexture *target,
										uint32_t passWidth,
										uint32_t passHeight,
										std::span<const SDL_GPUTextureSamplerBinding> bindings,
										const PbrUniforms *passUniforms,
										const LightUniforms *passLights,
										SDL_FColor clear
									) {
				recording.Fullscreen(
					name, pipeline, target, passWidth, passHeight, bindings, passUniforms, passLights, clear
				);
			};
			const auto graphTexture =
				[&recording](graph::ResourceId resource, const graph::RunContext &runContext, bool make) {
					return recording.GraphTexture(resource, runContext, make);
				};

			Impl::NamedTexture target;
			for (const graph::ResourceId resource : context.Writes) {
				target = graphTexture(resource, context, true);
				if (target.IsValid()) {
					break;
				}
			}
			// **The one place `PostProcessPipeline` is read.** The portal
			// preview above always draws with the engine's own tonemap - see
			// `Renderer::SetPostProcessShader`'s own header for why a custom
			// grade on the main view must not also recolour every mirror and
			// portal in it.
			fullscreen(
				context.Name,
				State->PostProcessPipeline != nullptr ? State->PostProcessPipeline : State->TonemapPipeline,
				target.Texture,
				target.Width,
				target.Height,
				tonemapBindings,
				nullptr,
				nullptr,
				colourTarget.clear_color
			);

			// The forward tail consumes both completed attachments.
			colourTarget.load_op = SDL_GPU_LOADOP_LOAD;
			depthTarget.load_op = SDL_GPU_LOADOP_LOAD;
			// An offscreen slot exposes this attachment through `ResourceTexture`
			// after the graph finishes, including the profiler's stage thumbnails.
			// Discarding it here returned a valid texture handle holding undefined
			// pixels. A window render has no later depth consumer and may still skip
			// the final store.
			depthTarget.store_op = offscreen ? SDL_GPU_STOREOP_STORE : SDL_GPU_STOREOP_DONT_CARE;
			// Cycling selects fresh backing storage, which cannot contain the depth
			// this pass explicitly loads from the G-buffer pass.
			depthTarget.cycle = false;
			return true;
		});
	}
}
