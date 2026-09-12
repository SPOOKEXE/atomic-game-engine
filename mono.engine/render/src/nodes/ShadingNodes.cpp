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
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>

#include <algorithm>
#include <array>
#include <tuple>
#include <type_traits>

namespace engine::render {

	void ViewRecording::RegisterShadingNodes(NodeTable &frameNodes) {
		for (const auto kind :
			 {core::Name("ambient-response"), core::Name("ambient-merge"), core::Name("ambient-correct")}) {
			frameNodes.Set(kind, [this, kind](const graph::RunContext &context) {
				const auto *node = Pipeline->Graph.Find(context.Node);
				if (!node || !State->EnsureAmbientComposition()) return false;
				const bool response = kind == core::Name("ambient-response");
				const bool merge = kind == core::Name("ambient-merge");
				const bool directional = !response && !merge && context.Reads.size() > 3;
				if (directional && !State->EnsureDirectionalCorrection()) return false;
				const std::array<const char *, 7> ports =
					response
						? std::array<const char *, 7>{"albedo", "normal", "material", "depth", "occlusion"}
					: merge
						? std::
							  array<const char *, 7>{"body-depth", "body-normal", "room-depth", "room-normal"}
						: std::array<const char *, 7>{
							  "lighting-baseline",
							  "response",
							  "occlusion",
							  "directional-response",
							  "room-depth",
							  "room-normal",
							  "shadow"
						  };
				const size_t inputCount = response ? 5 : merge ? 4 : directional ? 7 : 3;
				if (context.Reads.size() != inputCount || context.Writes.size() != (merge ? 2u : 1u))
					return false;
				std::array<Impl::NamedTexture, 7> inputs;
				std::array<SDL_GPUTextureSamplerBinding, 7> bindings;
				for (size_t index = 0; index < inputCount; ++index) {
					const auto found =
						std::find(node->ReadPorts.begin(), node->ReadPorts.end(), core::Name(ports[index]));
					if (found == node->ReadPorts.end()) return false;
					inputs[index] =
						GraphTexture(context.Reads[found - node->ReadPorts.begin()], context, false);
					if (!inputs[index].IsValid()) return false;
					// AO uses the native linear sampler; matched geometry uses nearest surface selection.
					bindings[index] = {inputs[index].Texture, merge ? State->OverlaySampler : Sampler};
					if (directional && index == 6)
						bindings[index].sampler = State->ShadowSampler ? State->ShadowSampler : Sampler;
				}
				const auto output = [&](const char *port) {
					const auto found =
						std::find(node->WritePorts.begin(), node->WritePorts.end(), core::Name(port));
					return found == node->WritePorts.end()
							   ? Impl::NamedTexture{}
							   : GraphTexture(
									 context.Writes[found - node->WritePorts.begin()], context, true
								 );
				};
				const auto first = output(response ? "response" : merge ? "depth" : "colour");
				const auto second = merge ? output("normal") : Impl::NamedTexture{};
				const auto firstFormat = response ? SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT
										 : merge  ? SDL_GPU_TEXTUREFORMAT_R32_FLOAT
												  : SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
				if (!first.IsValid() || first.Format != firstFormat ||
					(merge &&
					 (!second.IsValid() || second.Format != SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM ||
					  first.Width != second.Width || first.Height != second.Height)))
					return false;
				if (merge && (inputs[0].Format != SDL_GPU_TEXTUREFORMAT_R32_FLOAT ||
							  inputs[1].Format != SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM ||
							  inputs[2].Format != SDL_GPU_TEXTUREFORMAT_R32_FLOAT ||
							  inputs[3].Format != SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM))
					return false;
				if (!response && !merge &&
					(inputs[0].Format != SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT ||
					 inputs[1].Format != SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT ||
					 inputs[2].Format != SDL_GPU_TEXTUREFORMAT_R8_UNORM))
					return false;
				if (response && ((inputs[0].Format != SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB &&
								  inputs[0].Format != SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM) ||
								 inputs[1].Format != SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM ||
								 inputs[2].Format != SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM ||
								 inputs[3].Format != SDL_GPU_TEXTUREFORMAT_R32_FLOAT ||
								 inputs[4].Format != SDL_GPU_TEXTUREFORMAT_R8_UNORM))
					return false;
				const auto matchesOutput = [&](const Impl::NamedTexture &input) {
					return input.Width == first.Width && input.Height == first.Height;
				};
				if (merge &&
					(!matchesOutput(inputs[0]) || !matchesOutput(inputs[2]) || !matchesOutput(inputs[3])))
					return false;
				if (!merge && !response && (!matchesOutput(inputs[0]) || !matchesOutput(inputs[1])))
					return false;
				if (directional && (inputs[3].Format != SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT ||
									inputs[4].Format != SDL_GPU_TEXTUREFORMAT_R32_FLOAT ||
									inputs[5].Format != SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM ||
									inputs[6].Format != SDL_GPU_TEXTUREFORMAT_D32_FLOAT ||
									inputs[6].Width != inputs[6].Height || !matchesOutput(inputs[3]) ||
									!matchesOutput(inputs[4]) || !matchesOutput(inputs[5])))
					return false;
				for (size_t index = 0; index < inputCount; ++index)
					if (inputs[index].Texture == first.Texture ||
						(merge && inputs[index].Texture == second.Texture))
						return false;
				if (response && !GraphEnabled(core::Name("ssao"))) ClearOcclusion();
				EnterNamedPass(context.Name);
				SDL_GPUColorTargetInfo targets[2]{};
				targets[0].texture = first.Texture;
				targets[1].texture = second.Texture;
				for (auto &target : targets) {
					target.load_op = SDL_GPU_LOADOP_DONT_CARE;
					target.store_op = SDL_GPU_STOREOP_STORE;
					target.cycle = true;
				}
				auto *pass = SDL_BeginGPURenderPass(Command, targets, merge ? 2 : 1, nullptr);
				if (!pass) return false;
				if (response) SDL_PushGPUFragmentUniformData(Command, 0, &Uniforms, sizeof(Uniforms));
				if (directional) {
					struct CorrectionUniforms {
						glm::mat4 InverseViewProjection, LightViewProjection;
						glm::vec4 CameraDepth, Direction, Shadow;
					} correction{
						Uniforms.InverseViewProjection,
						Uniforms.LightViewProjection,
						Uniforms.CameraDepth,
						Uniforms.Direction,
						glm::vec4{1, 1.0f / float(inputs[6].Width), 0, 0}
					};
					SDL_PushGPUFragmentUniformData(Command, 0, &correction, sizeof(correction));
				}
				if (merge) {
					const glm::vec4 parameters{Uniforms.Planes.y, Uniforms.Target.z, Uniforms.Target.w, 0};
					SDL_PushGPUFragmentUniformData(Command, 0, &parameters, sizeof(parameters));
				}
				SDL_BindGPUGraphicsPipeline(
					pass,
					response	  ? State->AmbientResponsePipeline
					: merge		  ? State->AmbientMergePipeline
					: directional ? State->DirectionalCorrectPipeline
								  : State->AmbientCorrectPipeline
				);
				SDL_BindGPUFragmentSamplers(pass, 0, bindings.data(), static_cast<uint32_t>(inputCount));
				const SDL_GPUViewport viewport{0, 0, float(first.Width), float(first.Height), 0, 1};
				const SDL_Rect scissor{0, 0, int(first.Width), int(first.Height)};
				SDL_SetGPUViewport(pass, &viewport);
				SDL_SetGPUScissor(pass, &scissor);
				SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
				SDL_EndGPURenderPass(pass);
				++Result.DrawCalls;
				core::Metrics::Count(
					"render.ambient.output_bytes", uint64_t(first.Width) * first.Height * (response ? 16 : 8)
				);
				return true;
			});
		}

		frameNodes.Set(core::Name("colour-compose"), [this](const graph::RunContext &context) {
			const auto *node = Pipeline->Graph.Find(context.Node);
			if (!node || context.Reads.size() != 2 || context.Writes.size() != 1) return false;
			std::array<Impl::NamedTexture, 2> inputs;
			const std::array ports{core::Name("foreground"), core::Name("background")};
			for (size_t i = 0; i < inputs.size(); ++i) {
				const auto found = std::find(node->ReadPorts.begin(), node->ReadPorts.end(), ports[i]);
				if (found == node->ReadPorts.end()) return false;
				inputs[i] = GraphTexture(context.Reads[found - node->ReadPorts.begin()], context, false);
				if (!inputs[i].IsValid() || inputs[i].Format != SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT)
					return false;
			}
			const auto output = GraphTexture(context.Writes[0], context, true);
			if (!output.IsValid() || output.Format != SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT ||
				!State->EnsureColourCompose())
				return false;
			EnterNamedPass(context.Name);
			SDL_GPUColorTargetInfo target{};
			target.texture = output.Texture;
			target.load_op = SDL_GPU_LOADOP_DONT_CARE;
			target.store_op = SDL_GPU_STOREOP_STORE;
			target.cycle = true;
			auto *pass = SDL_BeginGPURenderPass(Command, &target, 1, nullptr);
			if (!pass) return false;
			const SDL_GPUTextureSamplerBinding bindings[2]{
				{inputs[0].Texture, State->OverlaySampler}, {inputs[1].Texture, State->OverlaySampler}
			};
			SDL_BindGPUGraphicsPipeline(pass, State->ColourComposePipeline);
			SDL_BindGPUFragmentSamplers(pass, 0, bindings, 2);
			const SDL_GPUViewport viewport{0, 0, float(output.Width), float(output.Height), 0, 1};
			const SDL_Rect scissor{0, 0, int(output.Width), int(output.Height)};
			SDL_SetGPUViewport(pass, &viewport);
			SDL_SetGPUScissor(pass, &scissor);
			SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
			SDL_EndGPURenderPass(pass);
			++Result.DrawCalls;
			core::Metrics::Count(
				"render.colour_compose.output_bytes", uint64_t(output.Width) * output.Height * 8
			);
			return true;
		});

		frameNodes.Set(core::Name("depth-compose"), [this](const graph::RunContext &context) {
			const auto *node = Pipeline->Graph.Find(context.Node);
			if (node == nullptr || context.Reads.size() != 4 || context.Writes.size() != 2) return false;
			const auto resolve = [&](std::span<const graph::ResourceId> resources,
									 std::span<const core::Name> ports,
									 core::Name port,
									 size_t index,
									 bool create) {
				if (!ports.empty()) index = std::find(ports.begin(), ports.end(), port) - ports.begin();
				return index < resources.size() ? GraphTexture(resources[index], context, create)
												: Impl::NamedTexture{};
			};
			std::array<Impl::NamedTexture, 4> inputs;
			const std::array ports{"foreground", "foreground-depth", "background", "background-depth"};
			for (size_t i = 0; i < inputs.size(); ++i) {
				inputs[i] = resolve(context.Reads, node->ReadPorts, core::Name(ports[i]), i, false);
				const auto format =
					i % 2 ? SDL_GPU_TEXTUREFORMAT_R32_FLOAT : SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
				if (!inputs[i].IsValid() || inputs[i].Format != format) return false;
			}
			for (const size_t first : {size_t(0), size_t(2)}) {
				if (inputs[first].Width != inputs[first + 1].Width ||
					inputs[first].Height != inputs[first + 1].Height)
					return false;
			}
			const auto colour = resolve(context.Writes, node->WritePorts, core::Name("colour"), 0, true);
			const auto depth = resolve(context.Writes, node->WritePorts, core::Name("depth"), 1, true);
			if (!colour.IsValid() || !depth.IsValid() ||
				colour.Format != SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT ||
				depth.Format != SDL_GPU_TEXTUREFORMAT_R32_FLOAT || colour.Width != depth.Width ||
				colour.Height != depth.Height || !State->EnsureDepthCompose())
				return false;
			EnterNamedPass(context.Name);
			SDL_GPUColorTargetInfo targets[2]{};
			targets[0].texture = colour.Texture;
			targets[1].texture = depth.Texture;
			for (auto &target : targets) {
				target.load_op = SDL_GPU_LOADOP_DONT_CARE;
				target.store_op = SDL_GPU_STOREOP_STORE;
				target.cycle = true;
			}
			auto *pass = SDL_BeginGPURenderPass(Command, targets, 2, nullptr);
			if (pass == nullptr) return false;
			std::array<SDL_GPUTextureSamplerBinding, 4> bindings;
			for (size_t i = 0; i < inputs.size(); ++i)
				bindings[i] = {inputs[i].Texture, State->OverlaySampler};
			const auto *mode = node->Parameter(core::Name("mode"));
			const uint32_t foregroundMode = mode && *mode == "transparent"	   ? 1
											: mode && *mode == "premultiplied" ? 2
																			   : 0;
			SDL_PushGPUFragmentUniformData(Command, 0, &foregroundMode, sizeof(foregroundMode));
			SDL_BindGPUGraphicsPipeline(pass, State->DepthComposePipeline);
			SDL_BindGPUFragmentSamplers(pass, 0, bindings.data(), bindings.size());
			const SDL_GPUViewport viewport{0, 0, float(colour.Width), float(colour.Height), 0, 1};
			const SDL_Rect scissor{0, 0, int(colour.Width), int(colour.Height)};
			SDL_SetGPUViewport(pass, &viewport);
			SDL_SetGPUScissor(pass, &scissor);
			SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
			SDL_EndGPURenderPass(pass);
			++Result.DrawCalls;
			core::Metrics::Count(
				"render.depth_compose.output_bytes", uint64_t(colour.Width) * colour.Height * 12
			);
			return true;
		});

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
			if (context.Writes.size() != 1) return false;
			const auto target = recording.GraphTexture(context.Writes.front(), context, true);
			if (!target.IsValid() || target.Format != SDL_GPU_TEXTUREFORMAT_R32_FLOAT) return false;
			const auto *node = recording.Pipeline->Graph.Find(context.Node);
			const auto *background = node ? node->Parameter(core::Name("background")) : nullptr;
			const bool zeroBackground = background && *background == "zero";
			PbrUniforms uniforms = recording.Uniforms;
			uniforms.Direction.w = zeroBackground ? 1.f : 0.f;
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
				target.Texture,
				target.Width,
				target.Height,
				depthBindings,
				&uniforms,
				nullptr,
				SDL_FColor{zeroBackground ? 0.f : drawCamera.FarPlane, 0.0f, 0.0f, 0.0f}
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
			const uint32_t resolved = scene::ResolveRenderFeatures(
				scene::ALL_RENDER_FEATURES,
				recording.CurrentLighting.RenderFeatures,
				recording.DrawCamera.RenderFeatures,
				{},
				SupportedRenderFeatures(State->Caps)
			).Enabled;
			if ((resolved & scene::FeatureBit(scene::RenderFeature::AmbientOcclusion)) == 0u) {
				recording.ClearOcclusion();
				return true;
			}
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

			const auto *node = Pipeline->Graph.Find(context.Node);
			if (!node) return false;
			const auto read = [&](const char *port) {
				const auto found =
					std::find(node->ReadPorts.begin(), node->ReadPorts.end(), core::Name(port));
				return found == node->ReadPorts.end()
						   ? Impl::NamedTexture{}
						   : GraphTexture(context.Reads[found - node->ReadPorts.begin()], context, false);
			};
			const auto depth = read("depth"), normal = read("normal");
			if (!depth.IsValid() || !normal.IsValid()) return false;
			const std::array aoBindings = {
				SDL_GPUTextureSamplerBinding{depth.Texture, sampler},
				SDL_GPUTextureSamplerBinding{normal.Texture, sampler},
			};
			auto aoUniforms = uniforms;
			if (normal.Texture != pbr.Normal) {
				aoUniforms.Target.z = 1;
				aoUniforms.Target.w = 1;
			}

			fullscreen(
				context.Name,
				State->SsaoPipeline,
				pbr.Occlusion,
				pbrDimensions.OcclusionWidth,
				pbrDimensions.OcclusionHeight,
				aoBindings,
				&aoUniforms,
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

			const auto *node = Pipeline->Graph.Find(context.Node);
			if (!node) return false;
			const auto baselinePort =
				std::find(node->WritePorts.begin(), node->WritePorts.end(), core::Name("lighting-baseline"));
			const auto directionalPort = std::find(
				node->WritePorts.begin(), node->WritePorts.end(), core::Name("directional-response")
			);
			const bool directionalRequested = directionalPort != node->WritePorts.end();
			if (directionalRequested && baselinePort == node->WritePorts.end()) return false;
			if (baselinePort != node->WritePorts.end()) {
				const auto baseline =
					GraphTexture(context.Writes[baselinePort - node->WritePorts.begin()], context, true);
				if (!baseline.IsValid() || baseline.Format != SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT ||
					baseline.Width != pbrDimensions.LitWidth || baseline.Height != pbrDimensions.LitHeight ||
					!(directionalRequested ? State->EnsureDeferredLightingDirectional()
										   : State->EnsureDeferredLightingBaseline()))
					return false;
				SDL_GPUTexture *directionalTexture = nullptr;
				if (directionalRequested) {
					const auto directional = GraphTexture(
						context.Writes[directionalPort - node->WritePorts.begin()], context, true
					);
					if (!directional.IsValid() ||
						directional.Format != SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT ||
						directional.Width != pbrDimensions.LitWidth ||
						directional.Height != pbrDimensions.LitHeight)
						return false;
					directionalTexture = directional.Texture;
				}
				EnterNamedPass(context.Name);
				SDL_GPUColorTargetInfo targets[3]{};
				targets[0].texture = pbr.Lit;
				targets[1].texture = baseline.Texture;
				targets[2].texture = directionalTexture;
				for (auto &target : targets) {
					target.load_op = SDL_GPU_LOADOP_CLEAR;
					target.store_op = SDL_GPU_STOREOP_STORE;
					target.cycle = true;
				}
				auto *pass = SDL_BeginGPURenderPass(Command, targets, directionalRequested ? 3 : 2, nullptr);
				if (!pass) return false;
				SDL_BindGPUGraphicsPipeline(
					pass,
					directionalRequested ? State->DeferredLightingDirectionalPipeline
										 : State->DeferredLightingBaselinePipeline
				);
				SDL_BindGPUFragmentSamplers(
					pass, 0, spillBindings.data(), static_cast<uint32_t>(spillBindings.size())
				);
				SDL_PushGPUFragmentUniformData(Command, 0, &uniforms, sizeof(uniforms));
				SDL_PushGPUFragmentUniformData(Command, 1, &lightUniforms, sizeof(lightUniforms));
				const SDL_GPUViewport viewport{0, 0, float(baseline.Width), float(baseline.Height), 0, 1};
				const SDL_Rect scissor{0, 0, int(baseline.Width), int(baseline.Height)};
				SDL_SetGPUViewport(pass, &viewport);
				SDL_SetGPUScissor(pass, &scissor);
				SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
				SDL_EndGPURenderPass(pass);
				++Result.DrawCalls;
				core::Metrics::Count(
					"render.lighting_baseline.output_bytes", uint64_t(baseline.Width) * baseline.Height * 16
				);
				if (directionalRequested)
					core::Metrics::Count(
						"render.directional_response.output_bytes",
						uint64_t(baseline.Width) * baseline.Height * 16
					);
				return true;
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

		frameNodes.Set(core::Name("skybox-compute"), [this](const graph::RunContext &context) {
			// Tier B intentionally retains the sky draw but omits this compute-only
			// producer. Its sky handler binds the procedural fallback below.
			if (!State->Caps.HasCompute) return true;
			if (context.Reads.size() != 0 || context.Writes.size() != 1) return false;
			const Impl::NamedTexture target = GraphTexture(context.Writes.front(), context, true);
			if (!target.IsValid() || target.Format != SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT) return false;
			return State->RecordEnvironmentSkybox(
				CurrentLighting.EnvironmentState,
				Command,
				target.Texture,
				target.Width,
				target.Height,
				Result.ComputeDispatches
			);
		});

		frameNodes.Set(core::Name("clouds-compute"), [this](const graph::RunContext &context) {
			if (!State->Caps.HasCompute) return true;
			if (context.Reads.size() != 1 || context.Writes.size() != 1) return false;
			const Impl::NamedTexture source = GraphTexture(context.Reads.front(), context, false);
			const Impl::NamedTexture target = GraphTexture(context.Writes.front(), context, true);
			if (!source.IsValid() || !target.IsValid() || source.Format != SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT ||
				target.Format != source.Format || target.Width != source.Width || target.Height != source.Height) {
				return false;
			}
			return State->RecordEnvironmentClouds(
				CurrentLighting.EnvironmentState,
				Command,
				source.Texture,
				target.Texture,
				target.Width,
				target.Height,
				Result.ComputeDispatches
			);
		});

		frameNodes.Set(core::Name("sky"), [this](const graph::RunContext &context) {
			ViewRecording &recording = *this;
			Impl *const State = recording.State;
			Impl::PbrSlot &pbr = *recording.Pbr;
			PbrUniforms &uniforms = recording.Uniforms;
			if ((context.Reads.size() != 2 && context.Reads.size() != 3) || context.Writes.size() != 1) return false;
			const Impl::NamedTexture environment = context.Reads.size() == 3
				? recording.GraphTexture(context.Reads[2], context, false)
				: Impl::NamedTexture{};
			uniforms.Fog.w = State->Caps.HasCompute && environment.IsValid() ? 1.0f : 0.0f;
			const std::array bindings{
				SDL_GPUTextureSamplerBinding{pbr.Lit, recording.Sampler},
				SDL_GPUTextureSamplerBinding{recording.DepthTarget.texture, recording.Sampler},
				SDL_GPUTextureSamplerBinding{environment.IsValid() ? environment.Texture : State->FallbackTexture,
									 recording.Sampler},
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

		frameNodes.Set(core::Name("fog"), [this](const graph::RunContext &context) {
			ViewRecording &recording = *this;
			Impl::PbrSlot &pbr = *recording.Pbr;
			const std::array bindings{
				SDL_GPUTextureSamplerBinding{pbr.SkyLit, recording.Sampler},
				SDL_GPUTextureSamplerBinding{recording.DepthTarget.texture, recording.Sampler},
			};
			recording.Fullscreen(
				context.Name,
				recording.State->VolumePipeline,
				pbr.Lit,
				recording.PbrDimensions.LitWidth,
				recording.PbrDimensions.LitHeight,
				bindings,
				&recording.Uniforms,
				nullptr,
				SDL_FColor{}
			);
			return true;
		});

		frameNodes.Set(core::Name("shader-lenses"), [this](const graph::RunContext &context) {
			ViewRecording &recording = *this;
			Impl *const State = recording.State;
			const auto *node = Pipeline->Graph.Find(context.Node);
			if (!node || context.Reads.size() != 2 || context.Writes.size() != 2) return false;
			const auto resolve = [&](bool write, core::Name port) {
				const auto &ports = write ? node->WritePorts : node->ReadPorts;
				const auto resources = write ? context.Writes : context.Reads;
				const auto found = std::find(ports.begin(), ports.end(), port);
				const auto index = static_cast<size_t>(found - ports.begin());
				if (found == ports.end() || index >= resources.size()) return Impl::NamedTexture{};
				return GraphTexture(resources[index], context, write);
			};
			const auto input = resolve(false, core::Name("colour"));
			const auto depth = resolve(false, core::Name("depth"));
			const auto output = resolve(true, core::Name("colour"));
			const auto scratch = resolve(true, core::Name("scratch"));
			if (!input.IsValid() || !depth.IsValid() || !output.IsValid() || !scratch.IsValid() ||
				input.Format != SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT ||
				depth.Format != SDL_GPU_TEXTUREFORMAT_R32_FLOAT || output.Format != input.Format ||
				scratch.Format != input.Format || output.Width != scratch.Width ||
				output.Height != scratch.Height || input.Texture == output.Texture ||
				input.Texture == scratch.Texture || output.Texture == scratch.Texture)
				return false;
			SDL_GPUTexture *source = input.Texture;
			SDL_GPUTexture *target = scratch.Texture;

			for (size_t index = 0; index < recording.LensGroupCount; index++) {
				const ViewRecording::LensGroup &group = recording.LensGroups[index];
				SDL_GPUGraphicsPipeline *pipeline = nullptr;
				const auto &view = *recording.Request.Source;
				if (view.LensPrograms != 0) {
					const auto programs = std::find_if(
						State->PortalLensPrograms.begin(),
						State->PortalLensPrograms.end(),
						[&](const auto &candidate) { return candidate.Token == view.LensPrograms; }
					);
					if (programs == State->PortalLensPrograms.end()) return false;
					const auto binding = std::find_if(
						programs->Bindings.begin(), programs->Bindings.end(), [&](const auto &candidate) {
							return candidate.Shader == group.Shader;
						}
					);
					if (binding == programs->Bindings.end()) return false;
					pipeline = binding->Pipeline;
				} else {
					const auto authored = State->LensPipelines.find(
						Impl::ShaderVariantKey(
							group.Shader, view.LensContentOwner.value_or(view.ContentOwner)
						)
					);
					if (authored != State->LensPipelines.end()) pipeline = authored->second.Pipeline;
				}
				if (!pipeline || group.Count == 0) continue;
				LensPassUniforms &uniforms = recording.LensPassData;
				std::copy_n(recording.LensEntries.begin() + group.First, group.Count, uniforms.Lenses);
				uniforms.TimeCount.y = static_cast<float>(group.Count);
				const std::array bindings{
					SDL_GPUTextureSamplerBinding{source, recording.Sampler},
					SDL_GPUTextureSamplerBinding{depth.Texture, recording.Sampler},
				};
				recording.Fullscreen(
					context.Name,
					pipeline,
					target,
					output.Width,
					output.Height,
					bindings,
					nullptr,
					nullptr,
					SDL_FColor{},
					&uniforms,
					sizeof(uniforms)
				);
				source = target;
				target = target == scratch.Texture ? output.Texture : scratch.Texture;
			}

			if (source != output.Texture) {
				// Empty and odd chains still publish the declared final image.
				recording.EnterNamedPass(context.Name);
				SDL_GPUBlitInfo blit{};
				blit.source.texture = source;
				blit.source.w = source == input.Texture ? input.Width : output.Width;
				blit.source.h = source == input.Texture ? input.Height : output.Height;
				blit.destination.texture = output.Texture;
				blit.destination.w = output.Width;
				blit.destination.h = output.Height;
				blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
				blit.filter = SDL_GPU_FILTER_NEAREST;
				blit.cycle = true;
				SDL_BlitGPUTexture(recording.Command, &blit);
			}
			return true;
		});

		frameNodes.Set(core::Name("tonemap"), [this](const graph::RunContext &context) {
			ViewRecording &recording = *this;
			Impl *const State = recording.State;
			const bool offscreen = recording.Offscreen;
			SDL_GPUColorTargetInfo &colourTarget = recording.ColourTarget;
			SDL_GPUDepthStencilTargetInfo &depthTarget = recording.DepthTarget;
			if (context.Reads.empty()) return false;
			const auto source = recording.GraphTexture(context.Reads.front(), context, false);
			if (!source.IsValid()) return false;
			const std::array tonemapBindings{SDL_GPUTextureSamplerBinding{source.Texture, recording.Sampler}};
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
			// Portal previews retain their plain tonemap; this grade belongs to the view.
			const auto postprocess = State->PostProcessPipelines.find(Request.Source->ContentOwner.Id());
			fullscreen(
				context.Name,
				postprocess != State->PostProcessPipelines.end() ? postprocess->second.Pipeline
																 : State->TonemapPipeline,
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
