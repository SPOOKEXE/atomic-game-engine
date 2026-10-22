// The screen-space chain over the material head: linear depth, the hierarchy
// built from it, ambient occlusion, the deferred lighting resolve, and the
// grade that turns the result into a display image.
//
// Fullscreen effects share pass setup through `ViewRecording::Fullscreen`.
// Inactive effects publish their outputs with copies or clears, preserving
// graph resources without running a shader over every pixel.

#include "../EnvironmentModes.hpp"
#include "../GpuCommandIdentity.hpp"
#include "../SeamLightSelection.hpp"
#include "../SsaoSettings.hpp"
#include "../WeatherFallback.hpp"
#include "ViewRecording.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/weather/Clock.hpp>
#include <engine/weather/Runtime.hpp>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <tuple>
#include <type_traits>

namespace engine::render {

	void ViewRecording::RegisterShadingNodes(NodeTable &frameNodes) {
		RegisterWeatherShadowNodes(frameNodes);
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
				if (response && !GraphEnabled(core::Name("ssao"))) {
					const uint32_t resolved = scene::ResolveRenderFeatures(
												  scene::ALL_RENDER_FEATURES,
												  CurrentLighting.RenderFeatures,
												  DrawCamera.RenderFeatures,
												  {},
												  SupportedRenderFeatures(State->Caps)
					)
												  .Enabled;
					ClearOcclusion(
						AmbientOcclusionSourceState::ClearedNoPass,
						(resolved & scene::FeatureBit(scene::RenderFeature::AmbientOcclusion)) != 0u
					);
				}
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

		frameNodes.Set(core::Name("pack-channels"), [this](const graph::RunContext &context) {
			if (context.Reads.size() != 4 || context.Writes.size() != 1) return false;
			const auto *node = Pipeline->Graph.Find(context.Node);
			if (node == nullptr) return false;
			const std::array ports{"r", "g", "b", "a"};
			std::array<Impl::NamedTexture, 4> inputs;
			std::array<uint32_t, 4> components{};
			for (size_t index = 0; index < ports.size(); ++index) {
				const auto found =
					std::find(node->ReadPorts.begin(), node->ReadPorts.end(), core::Name(ports[index]));
				if (found == node->ReadPorts.end()) return false;
				inputs[index] = GraphTexture(context.Reads[found - node->ReadPorts.begin()], context, false);
				const std::string *value =
					node->Parameter(core::Name(std::string(ports[index]) + "-component"));
				if (value != nullptr && (value->size() != 1 || (*value)[0] < '0' || (*value)[0] > '3'))
					return false;
				components[index] = value == nullptr ? 0 : uint32_t((*value)[0] - '0');
				if (!inputs[index].IsValid()) return false;
			}
			const auto output = GraphTexture(context.Writes.front(), context, true);
			if (!output.IsValid() || output.Format != SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT ||
				output.Width != inputs.front().Width || output.Height != inputs.front().Height ||
				State->PackChannelsPipeline == nullptr)
				return false;
			for (const Impl::NamedTexture &input : inputs)
				if (input.Texture == output.Texture) return false;
			std::array<SDL_GPUTextureSamplerBinding, 4> bindings;
			for (size_t index = 0; index < inputs.size(); ++index)
				bindings[index] = {inputs[index].Texture, Sampler};
			Fullscreen(
				context.Name,
				State->PackChannelsPipeline,
				output.Texture,
				output.Width,
				output.Height,
				bindings,
				nullptr,
				nullptr,
				SDL_FColor{0, 0, 0, 0},
				components.data(),
				sizeof(components)
			);
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
			uniforms.Target.x = static_cast<float>(target.Width);
			uniforms.Target.y = static_cast<float>(target.Height);
			uniforms.Direction.w = zeroBackground ? 1.f : 0.f;
			const auto &depthBindings = recording.DepthBindings;
			if (recording.EmptyHardwareDepth != nullptr &&
				depthBindings.front().texture == recording.EmptyHardwareDepth) {
				if (!recording.ClearImage(
						context.Name,
						target,
						SDL_FColor{zeroBackground ? 0.f : drawCamera.FarPlane, 0.f, 0.f, 0.f}
					))
					return false;
				if (!zeroBackground) recording.FarLinearDepth = target.Texture;
				core::Metrics::Count("render.empty_depth.clears", 1);
				return true;
			}
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

		frameNodes.Set(core::Name("depth-validity"), [this](const graph::RunContext &context) {
			ViewRecording &recording = *this;
			Impl *const State = recording.State;
			if (context.Reads.size() != 1 || context.Writes.size() != 1) return false;
			const auto depth = recording.GraphTexture(context.Reads.front(), context, false);
			const auto target = recording.GraphTexture(context.Writes.front(), context, true);
			if (!depth.IsValid() || !target.IsValid() || depth.Format != State->DepthFormat ||
				target.Format != SDL_GPU_TEXTUREFORMAT_R8_UNORM || depth.Width != target.Width ||
				depth.Height != target.Height || State->DepthValidityPipeline == nullptr)
				return false;

			PbrUniforms uniforms = recording.Uniforms;
			// The shared depth shader writes binary R8 when this mode is selected.
			// G-buffer depth omits blended draws, so the plane remains honest about its scope.
			uniforms.Target.x = static_cast<float>(target.Width);
			uniforms.Target.y = static_cast<float>(target.Height);
			uniforms.Direction.w = 2.0f;
			recording.Fullscreen(
				context.Name,
				State->DepthValidityPipeline,
				target.Texture,
				target.Width,
				target.Height,
				recording.DepthBindings,
				&uniforms,
				nullptr,
				SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f}
			);
			return true;
		});

		frameNodes.Set(core::Name("camera-motion"), [this](const graph::RunContext &context) {
			if (context.Reads.size() != 1 || context.Writes.size() != 1 || Pbr == nullptr) return false;
			const auto depth = GraphTexture(context.Reads.front(), context, false);
			const auto target = GraphTexture(context.Writes.front(), context, true);
			if (!depth.IsValid() || !target.IsValid() || depth.Format != State->DepthFormat ||
				target.Format != SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT || depth.Width != target.Width ||
				depth.Height != target.Height)
				return false;
			Impl::PbrSlot &pbr = *Pbr;
			const bool validIdentity = Request.Source != nullptr &&
									   !Request.Source->CameraTemporalId.empty() &&
									   Request.Source->CameraTemporalId.size() <= 256 &&
									   Request.Source->CameraTemporalId.find('\0') == std::string::npos &&
									   Request.Source->CameraTemporalSequence != 0;
			const bool history =
				validIdentity && !Request.Source->CameraCut && pbr.CameraMotionHistory &&
				pbr.PreviousCameraMotionWorld == Request.Source->World &&
				pbr.PreviousCameraMotionWorldName == Request.Source->WorldName &&
				pbr.PreviousCameraTemporalId == Request.Source->CameraTemporalId &&
				pbr.PreviousCameraTemporalSequence + 1 == Request.Source->CameraTemporalSequence;
			CameraMotionUniforms uniforms{
				Uniforms.InverseViewProjection,
				history ? pbr.PreviousCameraMotionViewProjection : Matrices.ViewProjection,
				glm::vec4{
					float(target.Width),
					float(target.Height),
					float(SceneWidth) / float(depth.Width),
					float(SceneHeight) / float(depth.Height)
				}
			};
			const SDL_GPUTextureSamplerBinding binding{depth.Texture, Sampler};
			Fullscreen(
				context.Name,
				State->CameraMotionPipeline,
				target.Texture,
				target.Width,
				target.Height,
				std::span(&binding, 1),
				nullptr,
				nullptr,
				SDL_FColor{0, 0, 0, 0},
				&uniforms,
				sizeof(uniforms)
			);
			pbr.CameraMotionProduced = history;
			pbr.CameraMotionSourceFrame = history ? pbr.PreviousCameraMotionFrame : 0;
			pbr.PreviousCameraMotionViewProjection = Matrices.ViewProjection;
			pbr.PreviousCameraMotionFrame = State->FrameCounter;
			pbr.PreviousCameraMotionWorld = Request.Source != nullptr ? Request.Source->World : 0;
			pbr.PreviousCameraMotionWorldName =
				Request.Source != nullptr ? Request.Source->WorldName : core::Name{};
			pbr.PreviousCameraTemporalId = validIdentity ? Request.Source->CameraTemporalId : "";
			pbr.PreviousCameraTemporalSequence = validIdentity ? Request.Source->CameraTemporalSequence : 0;
			pbr.CameraMotionHistory = true;
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
			)
										  .Enabled;
			if ((resolved & scene::FeatureBit(scene::RenderFeature::AmbientOcclusion)) == 0u) {
				recording.ClearOcclusion(AmbientOcclusionSourceState::ClearedDisabled, false);
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
										SDL_FColor clear,
										const BeamUniforms *passBeams = nullptr
									) {
				recording.Fullscreen(
					name,
					pipeline,
					target,
					passWidth,
					passHeight,
					bindings,
					passUniforms,
					passLights,
					clear,
					nullptr,
					0,
					passBeams
				);
			};

			const auto *node = Pipeline->Graph.Find(context.Node);
			if (!node) {
				ENGINE_ERROR("deferred lighting '{}' has no graph node", context.Name.Text());
				return false;
			}
			const auto read = [&](const char *port) {
				const auto found =
					std::find(node->ReadPorts.begin(), node->ReadPorts.end(), core::Name(port));
				return found == node->ReadPorts.end()
						   ? Impl::NamedTexture{}
						   : GraphTexture(context.Reads[found - node->ReadPorts.begin()], context, false);
			};
			const auto depth = read("depth"), normal = read("normal");
			if (!depth.IsValid() || !normal.IsValid()) return false;
			if (recording.EmptyGBufferNormals && normal.Texture == pbr.Normal) {
				recording.EnterNamedPass(context.Name);
				recording.ClearOcclusion(AmbientOcclusionSourceState::ClearedNoPass, true);
				return true;
			}
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
			pbr.OcclusionProvenance = {
				.SourceState = AmbientOcclusionSourceState::Estimated,
				.ProducerFrame = State->FrameCounter,
				.Enabled = true,
				.SampleCount = SSAO_SAMPLE_COUNT,
				.RadiusWorldUnits = SSAO_RADIUS_WORLD_UNITS,
				.Denoiser = AmbientOcclusionDenoiser::None,
				.TemporalHistory = AmbientOcclusionTemporalHistory::Disabled,
				.BackgroundValue = 1.0f,
				.BackgroundClassification = AmbientOcclusionBackgroundClassification::Unavailable
			};
			return true;
		});

		frameNodes.Set(core::Name("deferred-lighting"), [this](const graph::RunContext &context) {
			ViewRecording &recording = *this;
			Impl *const State = recording.State;
			const core::Vector3 &behaviourEye = recording.Request.VisibilityCameraFrame.Position;
			Impl::SurfaceBank &bank = *recording.Bank;
			const LightUniforms &lightUniforms = recording.SceneLights;
			const Impl::PbrDimensions &pbrDimensions = recording.PbrDimensions;
			Impl::PbrSlot &pbr = *recording.Pbr;
			PbrUniforms &uniforms = recording.Uniforms;
			SDL_GPUSampler *const sampler = recording.Sampler;
			const auto &lightingBindings = recording.LightingBindings;
			WeatherShadowLightingUniforms weatherShadowLighting;
			const auto graphEnabled = [&recording](core::Name kind) { return recording.GraphEnabled(kind); };
			const auto clearOcclusion = [&recording, State] {
				const uint32_t resolved = scene::ResolveRenderFeatures(
											  scene::ALL_RENDER_FEATURES,
											  recording.CurrentLighting.RenderFeatures,
											  recording.DrawCamera.RenderFeatures,
											  {},
											  SupportedRenderFeatures(State->Caps)
				)
											  .Enabled;
				recording.ClearOcclusion(
					AmbientOcclusionSourceState::ClearedNoPass,
					(resolved & scene::FeatureBit(scene::RenderFeature::AmbientOcclusion)) != 0u
				);
			};
			const auto fullscreen = [&recording, &weatherShadowLighting](
										core::Name name,
										SDL_GPUGraphicsPipeline *pipeline,
										SDL_GPUTexture *target,
										uint32_t passWidth,
										uint32_t passHeight,
										std::span<const SDL_GPUTextureSamplerBinding> bindings,
										const PbrUniforms *passUniforms,
										const LightUniforms *passLights,
										SDL_FColor clear,
										const BeamUniforms *passBeams = nullptr
									) {
				recording.Fullscreen(
					name,
					pipeline,
					target,
					passWidth,
					passHeight,
					bindings,
					passUniforms,
					passLights,
					clear,
					nullptr,
					0,
					passBeams,
					nullptr,
					&weatherShadowLighting
				);
			};

			if (!graphEnabled(core::Name("ssao"))) {
				clearOcclusion();
			}

			// The seam light projectors, chosen and bound here rather than with
			// `lightingBindings`, because the capture textures are made inside
			// the portal-capture node this same frame - the graph's
			// portal-light edge is what guarantees that node has already run.
			// Ready spill fields reaching visible receivers win the two slots, nearest
			// receiver first. Empty slots stay zeroed in `uniforms` and bind the fallback texel.
			// **The size comes from the type, not from `lightingBindings.size()`.**
			// That call is `constexpr` and never reads the object, so GCC and
			// Clang fold it - but the object itself cannot be `constexpr`, since
			// its elements are this frame's textures and samplers. MSVC requires
			// the object and refuses: `error C2975: '_Size': invalid template
			// argument for 'std::array', expected compile-time constant
			// expression`. `tuple_size_v` asks the type and never mentions the
			// object at all.
			constexpr size_t SPILL_BINDINGS =
				std::tuple_size_v<std::remove_cvref_t<decltype(lightingBindings)>> + MAX_SEAM_LIGHTS + 2;
			std::array<SDL_GPUTextureSamplerBinding, SPILL_BINDINGS> spillBindings{};
			std::copy(lightingBindings.begin(), lightingBindings.end(), spillBindings.begin());

			std::array<const Impl::SeamLightTarget *, MAX_SEAM_LIGHT_TARGETS> ready{};
			size_t readyCount = 0;
			const graph::Frustum receiverFrustum =
				graph::Frustum::FromViewProjection(recording.Matrices.ViewProjection);
			const auto projectorOf = [&](const Impl::SeamLightTarget *candidate) {
				return SeamLightProjector{
					.Centre = {candidate->Centre.x, candidate->Centre.y, candidate->Centre.z},
					.Outward = {candidate->Outward.x, candidate->Outward.y, candidate->Outward.z},
					.First = {candidate->First.x, candidate->First.y, candidate->First.z},
					.Second = {candidate->Second.x, candidate->Second.y, candidate->Second.z},
					.Range = candidate->Outward.w,
					.Index = static_cast<size_t>(candidate - bank.SeamLights),
				};
			};
			if (graphEnabled(core::Name("portal-capture"))) {
				for (const Impl::SeamLightTarget &seamLight : bank.SeamLights) {
					if (seamLight.Ready && SeamLightMayAffect(projectorOf(&seamLight), receiverFrustum)) {
						ready[readyCount++] = &seamLight;
					}
				}
			}
			std::sort(
				ready.begin(),
				ready.begin() + static_cast<std::ptrdiff_t>(readyCount),
				[&](const Impl::SeamLightTarget *left, const Impl::SeamLightTarget *right) {
					const SeamLightProjector leftProjector = projectorOf(left);
					const SeamLightProjector rightProjector = projectorOf(right);
					const float leftDistance = SeamLightInfluenceDistanceSquared(
						leftProjector, State->VisibleInstances, State->DrawOrder, behaviourEye
					);
					const float rightDistance = SeamLightInfluenceDistanceSquared(
						rightProjector, State->VisibleInstances, State->DrawOrder, behaviourEye
					);
					if (leftDistance != rightDistance) {
						return leftDistance < rightDistance;
					}
					const float leftCoverage =
						SeamLightScreenCoverage(leftProjector, recording.Matrices.ViewProjection);
					const float rightCoverage =
						SeamLightScreenCoverage(rightProjector, recording.Matrices.ViewProjection);
					return leftCoverage == rightCoverage ? leftProjector.Index < rightProjector.Index
														 : leftCoverage > rightCoverage;
				}
			);

			std::array<const Impl::SeamLightTarget *, MAX_SEAM_LIGHTS> chosen{};
			for (size_t slot = 0; slot < chosen.size() && slot < readyCount; slot++) {
				chosen[slot] = ready[slot];
			}

			for (size_t slot = 0; slot < chosen.size(); slot++) {
				SDL_GPUTextureSamplerBinding &binding = spillBindings[7 + slot];
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
			spillBindings[9] = SDL_GPUTextureSamplerBinding{
				State->BeamTexture != nullptr ? State->BeamTexture : State->FallbackTexture,
				State->ShadowSampler != nullptr ? State->ShadowSampler : sampler,
			};

			const auto *node = Pipeline->Graph.Find(context.Node);
			if (!node) return false;
			spillBindings[10] = {State->FallbackTexture, sampler};
			const auto cloudShadowPort =
				std::find(node->ReadPorts.begin(), node->ReadPorts.end(), core::Name("weather-shadow"));
			const auto cloudGeneration = std::find_if(
				State->WeatherGenerations.begin(),
				State->WeatherGenerations.end(),
				[this](const WeatherGpuGeneration &generation) {
					return generation.World == Request.World && generation.Pipeline == Pipeline->Name;
				}
			);
			const auto &weatherEnvironment = CurrentLighting.EnvironmentState;
			if (cloudShadowPort != node->ReadPorts.end() && weatherEnvironment.HasWeatherCompute &&
				weatherEnvironment.Weather.Enabled && weatherEnvironment.Weather.ShadowsEnabled &&
				cloudGeneration != State->WeatherGenerations.end()) {
				const auto *shadowLighting = cloudGeneration->Shadows.LightingFor(Command);
				const auto shadowMap =
					GraphTexture(context.Reads[cloudShadowPort - node->ReadPorts.begin()], context, false);
				if (shadowLighting && shadowMap.IsValid() &&
					shadowMap.Texture == cloudGeneration->Shadows.Texture) {
					weatherShadowLighting = *shadowLighting;
					spillBindings[10] = {shadowMap.Texture, State->OverlaySampler};
				}
			}
			const auto baselinePort =
				std::find(node->WritePorts.begin(), node->WritePorts.end(), core::Name("lighting-baseline"));
			const auto directionalPort = std::find(
				node->WritePorts.begin(), node->WritePorts.end(), core::Name("directional-response")
			);
			const bool directionalRequested = directionalPort != node->WritePorts.end();
			const bool emptyLighting = recording.FarLinearDepth != nullptr &&
									   lightingBindings[4].texture == recording.FarLinearDepth;
			const bool blackSkyLit = [&] {
				if (!emptyLighting || recording.EmptyHardwareDepth == nullptr ||
					recording.EmptyHardwareDepth != recording.DepthTarget.texture ||
					!recording.Request.Damage.Scene || !State->Caps.HasCompute ||
					!recording.GraphEnabled(core::Name("sky")) || State->Inspected.IsValid() ||
					State->StageProbe.Enabled(State->FrameCounter, recording.Request.TargetSlot))
					return false;
				const auto colourPort =
					std::find(node->WritePorts.begin(), node->WritePorts.end(), core::Name("colour"));
				if (colourPort == node->WritePorts.end() ||
					static_cast<size_t>(colourPort - node->WritePorts.begin()) >= context.Writes.size())
					return false;
				const graph::ResourceId lit = context.Writes[colourPort - node->WritePorts.begin()];
				const auto *litDesc = Pipeline->Graph.FindResourceDesc(lit);
				if (litDesc == nullptr ||
					recording.FixedTexture(litDesc->Name, recording.Request.TargetSlot).Texture != pbr.Lit)
					return false;
				auto next = std::find(
					Pipeline->Compiled.PerView.begin(), Pipeline->Compiled.PerView.end(), context.Node
				);
				if (next == Pipeline->Compiled.PerView.end() || ++next == Pipeline->Compiled.PerView.end())
					return false;
				const auto *sky = Pipeline->Graph.Find(*next);
				if (sky == nullptr || !sky->Enabled || sky->Kind != core::Name("sky") ||
					sky->Reads.size() != 3 || sky->Reads[0] != lit || sky->Writes.size() != 1)
					return false;
				const auto depth =
					recording.ResourceTexture(sky->Reads[1], recording.Request.TargetSlot, false);
				const auto environment =
					recording.ResourceTexture(sky->Reads[2], recording.Request.TargetSlot, false);
				if (depth.Texture != recording.EmptyHardwareDepth ||
					!State->IsBlackEnvironment(
						recording.CurrentLighting.EnvironmentState, Command, environment.Texture
					))
					return false;
				const auto observesLit = [&](graph::NodeId id) {
					const auto *later = Pipeline->Graph.Find(id);
					if (later == nullptr) return false;
					return std::any_of(later->Reads.begin(), later->Reads.end(), [&](graph::ResourceId read) {
						const auto *desc = Pipeline->Graph.FindResourceDesc(read);
						return read == lit ||
							   (desc != nullptr &&
								recording.FixedTexture(desc->Name, recording.Request.TargetSlot).Texture ==
									pbr.Lit);
					});
				};
				for (++next; next != Pipeline->Compiled.PerView.end(); ++next) {
					if (observesLit(*next)) return false;
					const auto *later = Pipeline->Graph.Find(*next);
					if (later == nullptr || !later->Enabled) continue;
					const bool republishesLit =
						std::any_of(later->Writes.begin(), later->Writes.end(), [&](graph::ResourceId write) {
							const auto *desc = Pipeline->Graph.FindResourceDesc(write);
							return desc != nullptr &&
								   recording.FixedTexture(desc->Name, recording.Request.TargetSlot).Texture ==
									   pbr.Lit;
						});
					if (republishesLit) return true;
				}
				return std::none_of(
					Pipeline->Compiled.Final.begin(), Pipeline->Compiled.Final.end(), observesLit
				);
			}();
			if (directionalRequested && baselinePort == node->WritePorts.end()) {
				ENGINE_ERROR(
					"deferred lighting '{}' requires lighting-baseline when directional-response is "
					"requested",
					context.Name.Text()
				);
				return false;
			}
			const auto applyLocalShadowCorrections = [&]() {
				bool anyShadow = false;
				for (uint32_t index = 0; index < static_cast<uint32_t>(lightUniforms.Count.x); ++index)
					anyShadow = anyShadow || recording.SceneLightShadows[index];
				if (!anyShadow) return true;
				if (!State->EnsureDeferredLocalLightAccumulation()) return false;
				const SDL_GPUViewport viewport{
					0, 0, float(pbrDimensions.LitWidth), float(pbrDimensions.LitHeight), 0, 1
				};
				const SDL_Rect scissor{0, 0, int(pbrDimensions.LitWidth), int(pbrDimensions.LitHeight)};
				const std::array<glm::vec3, 6> directions{{
					{1, 0, 0},
					{-1, 0, 0},
					{0, 1, 0},
					{0, -1, 0},
					{0, 0, 1},
					{0, 0, -1},
				}};
				const std::array<glm::vec3, 6> ups{{
					{0, -1, 0},
					{0, -1, 0},
					{0, 0, 1},
					{0, 0, -1},
					{0, -1, 0},
					{0, -1, 0},
				}};
				for (uint32_t lightRow = 0; lightRow < static_cast<uint32_t>(lightUniforms.Count.x);
					 ++lightRow) {
					if (!recording.SceneLightShadows[lightRow]) continue;
					LightUniforms selected{};
					selected.Position[0] = lightUniforms.Position[lightRow];
					selected.Colour[0] = lightUniforms.Colour[lightRow];
					selected.Direction[0] = lightUniforms.Direction[lightRow];
					selected.Count.x = 1.0f;
					const bool point = selected.Direction[0].w <= -1.0f;
					const uint32_t faceCount = point ? 6u : 1u;
					const glm::vec3 position = glm::vec3(selected.Position[0]);
					for (uint32_t face = 0; face < faceCount; ++face) {
						glm::mat4 projection;
						glm::mat4 view;
						if (point) {
							projection = glm::perspectiveRH_ZO(
								glm::radians(90.0f), 1.0f, 0.05f, selected.Position[0].w
							);
							view = glm::lookAtRH(position, position + directions[face], ups[face]);
						} else {
							const float angle =
								2.0f * acos(glm::clamp(selected.Direction[0].w, -0.999f, 0.999f));
							projection = glm::perspectiveRH_ZO(angle, 1.0f, 0.05f, selected.Position[0].w);
							const glm::vec3 direction = glm::normalize(glm::vec3(selected.Direction[0]));
							const glm::vec3 up =
								std::abs(direction.y) > 0.99f ? glm::vec3{0, 0, 1} : glm::vec3{0, 1, 0};
							view = glm::lookAtRH(position, position + direction, up);
						}
						if (!recording.RecordLocalLightShadow(projection * view)) return false;
						PbrUniforms localUniforms = uniforms;
						localUniforms.LightViewProjection = projection * view;
						localUniforms.Shadow = glm::vec4{
							1.0f, 1.0f / float(SHADOW_RESOLUTION), point ? float(face) : -1.0f, 0.0f
						};
						spillBindings[6] =
							SDL_GPUTextureSamplerBinding{State->ShadowTexture, State->ShadowSampler};
						SDL_GPUColorTargetInfo target{};
						target.texture = pbr.Lit;
						target.load_op = SDL_GPU_LOADOP_LOAD;
						target.store_op = SDL_GPU_STOREOP_STORE;
						auto *pass = SDL_BeginGPURenderPass(Command, &target, 1, nullptr);
						if (!pass) return false;
						SDL_BindGPUGraphicsPipeline(pass, State->DeferredLocalLightAccumulationPipeline);
						SDL_BindGPUFragmentSamplers(
							pass, 0, spillBindings.data(), static_cast<uint32_t>(spillBindings.size())
						);
						SDL_PushGPUFragmentUniformData(Command, 0, &localUniforms, sizeof(localUniforms));
						SDL_PushGPUFragmentUniformData(Command, 1, &selected, sizeof(selected));
						SDL_PushGPUFragmentUniformData(Command, 2, &State->Beams, sizeof(State->Beams));
						SDL_SetGPUViewport(pass, &viewport);
						SDL_SetGPUScissor(pass, &scissor);
						SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
						SDL_EndGPURenderPass(pass);
						++Result.DrawCalls;
					}
				}
				return true;
			};

			if (baselinePort != node->WritePorts.end()) {
				const auto baseline =
					GraphTexture(context.Writes[baselinePort - node->WritePorts.begin()], context, true);
				if (!baseline.IsValid()) {
					ENGINE_ERROR(
						"deferred lighting '{}' could not allocate its lighting-baseline target",
						context.Name.Text()
					);
					return false;
				}
				if (baseline.Format != SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT) {
					ENGINE_ERROR(
						"deferred lighting '{}' requires an RGBA32F lighting-baseline target",
						context.Name.Text()
					);
					return false;
				}
				if (baseline.Width != pbrDimensions.LitWidth || baseline.Height != pbrDimensions.LitHeight) {
					ENGINE_ERROR(
						"deferred lighting '{}' requires lighting-baseline extent {}x{}, got {}x{}",
						context.Name.Text(),
						pbrDimensions.LitWidth,
						pbrDimensions.LitHeight,
						baseline.Width,
						baseline.Height
					);
					return false;
				}
				if (!(directionalRequested ? State->EnsureDeferredLightingDirectional()
										   : State->EnsureDeferredLightingBaseline())) {
					ENGINE_ERROR(
						"deferred lighting '{}' could not create its response pipeline", context.Name.Text()
					);
					return false;
				}
				SDL_GPUTexture *directionalTexture = nullptr;
				if (directionalRequested) {
					const auto directional = GraphTexture(
						context.Writes[directionalPort - node->WritePorts.begin()], context, true
					);
					if (!directional.IsValid()) {
						ENGINE_ERROR(
							"deferred lighting '{}' could not allocate its directional-response target",
							context.Name.Text()
						);
						return false;
					}
					if (directional.Format != SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT) {
						ENGINE_ERROR(
							"deferred lighting '{}' requires an RGBA32F directional-response target",
							context.Name.Text()
						);
						return false;
					}
					if (directional.Width != pbrDimensions.LitWidth ||
						directional.Height != pbrDimensions.LitHeight) {
						ENGINE_ERROR(
							"deferred lighting '{}' requires directional-response extent {}x{}, got {}x{}",
							context.Name.Text(),
							pbrDimensions.LitWidth,
							pbrDimensions.LitHeight,
							directional.Width,
							directional.Height
						);
						return false;
					}
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
				if (emptyLighting) {
					const SDL_FColor background{
						uniforms.FogColour.r, uniforms.FogColour.g, uniforms.FogColour.b, 1.f
					};
					targets[0].clear_color = blackSkyLit ? SDL_FColor{0.f, 0.f, 0.f, 1.f} : background;
					targets[1].clear_color = background;
					targets[2].clear_color = SDL_FColor{0.f, 0.f, 0.f, 1.f};
				}
				auto *pass = SDL_BeginGPURenderPass(Command, targets, directionalRequested ? 3 : 2, nullptr);
				if (!pass) return false;
				const SDL_GPUViewport viewport{0, 0, float(baseline.Width), float(baseline.Height), 0, 1};
				const SDL_Rect scissor{0, 0, int(baseline.Width), int(baseline.Height)};
				if (emptyLighting) {
					SDL_EndGPURenderPass(pass);
					core::Metrics::Count("render.empty_lighting.clears", 1);
					core::Metrics::Count("render.colour_clear.commands", 1);
					core::Metrics::Count(
						"render.colour_clear.bytes",
						uint64_t(baseline.Width) * baseline.Height * (directionalRequested ? 40 : 24)
					);
				} else {
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
					SDL_PushGPUFragmentUniformData(Command, 2, &State->Beams, sizeof(State->Beams));
					SDL_PushGPUFragmentUniformData(
						Command, 3, &weatherShadowLighting, sizeof(weatherShadowLighting)
					);
					SDL_SetGPUViewport(pass, &viewport);
					SDL_SetGPUScissor(pass, &scissor);
					SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
					SDL_EndGPURenderPass(pass);
					++Result.DrawCalls;
				}
				core::Metrics::Count(
					"render.lighting_baseline.output_bytes", uint64_t(baseline.Width) * baseline.Height * 16
				);
				if (directionalRequested)
					core::Metrics::Count(
						"render.directional_response.output_bytes",
						uint64_t(baseline.Width) * baseline.Height * 16
					);
				// Each capture target is rendered with exactly one source row. It is
				// not a copy of the summed local-light result.
				for (uint32_t index = 0; index < 4; ++index) {
					const core::Name requested = recording.LocalLightCaptureIds[index];
					if (!requested.IsValid()) continue;
					const auto activeLightEnd =
						recording.SceneLightIds.begin() + static_cast<size_t>(lightUniforms.Count.x);
					const auto row = std::find(recording.SceneLightIds.begin(), activeLightEnd, requested);
					recording.LocalLightCaptureMatched[index] = row != activeLightEnd;
					recording.LocalLightShadowCaptureAvailable[index] =
						recording.LocalLightCaptureMatched[index] &&
						recording
							.SceneLightShadows[static_cast<size_t>(row - recording.SceneLightIds.begin())];
					const core::Name port(std::string("local-light-response-") + std::to_string(index));
					auto found = std::find(node->WritePorts.begin(), node->WritePorts.end(), port);
					if (found == node->WritePorts.end()) continue;
					const auto target =
						GraphTexture(context.Writes[found - node->WritePorts.begin()], context, true);
					if (!target.IsValid() || target.Format != SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT) {
						ENGINE_ERROR(
							"deferred local light capture target {} invalid={} format={} extent={}x{}",
							index,
							!target.IsValid(),
							static_cast<int>(target.Format),
							target.Width,
							target.Height
						);
						return false;
					}
					const core::Name visibilityPort(
						std::string("local-light-shadow-visibility-") + std::to_string(index)
					);
					auto visibilityFound =
						std::find(node->WritePorts.begin(), node->WritePorts.end(), visibilityPort);
					if (visibilityFound == node->WritePorts.end()) return false;
					const auto visibilityTarget = GraphTexture(
						context.Writes[visibilityFound - node->WritePorts.begin()], context, true
					);
					if (!visibilityTarget.IsValid() ||
						visibilityTarget.Format != SDL_GPU_TEXTUREFORMAT_R8_UNORM ||
						visibilityTarget.Width != target.Width || visibilityTarget.Height != target.Height)
						return false;
					if (emptyLighting || !recording.LocalLightCaptureMatched[index]) {
						SDL_GPUColorTargetInfo emptyTargets[2]{};
						emptyTargets[0].texture = target.Texture;
						emptyTargets[1].texture = visibilityTarget.Texture;
						for (auto &emptyTarget : emptyTargets) {
							emptyTarget.load_op = SDL_GPU_LOADOP_CLEAR;
							emptyTarget.store_op = SDL_GPU_STOREOP_STORE;
							emptyTarget.cycle = true;
						}
						auto *emptyPass = SDL_BeginGPURenderPass(Command, emptyTargets, 2, nullptr);
						if (!emptyPass) return false;
						SDL_EndGPURenderPass(emptyPass);
						core::Metrics::Count("render.colour_clear.commands", 1);
						core::Metrics::Count(
							"render.colour_clear.bytes", uint64_t(target.Width) * target.Height * 17
						);
						continue;
					}
					const uint32_t lightRow = static_cast<uint32_t>(row - recording.SceneLightIds.begin());
					if (!State->EnsureDeferredLocalLight()) {
						ENGINE_ERROR("deferred local light capture pipeline is unavailable");
						return false;
					}
					LightUniforms selected{};
					selected.Position[0] = lightUniforms.Position[lightRow];
					selected.Colour[0] = lightUniforms.Colour[lightRow];
					selected.Direction[0] = lightUniforms.Direction[lightRow];
					selected.Count.x = 1.0f;
					const bool shadows = recording.SceneLightShadows[lightRow];
					const bool point = selected.Direction[0].w <= -1.0f;
					const uint32_t faceCount = shadows && point ? 6u : 1u;
					const glm::vec3 position = glm::vec3(selected.Position[0]);
					const std::array<glm::vec3, 6> directions{{
						{1, 0, 0},
						{-1, 0, 0},
						{0, 1, 0},
						{0, -1, 0},
						{0, 0, 1},
						{0, 0, -1},
					}};
					const std::array<glm::vec3, 6> ups{{
						{0, -1, 0},
						{0, -1, 0},
						{0, 0, 1},
						{0, 0, -1},
						{0, -1, 0},
						{0, -1, 0},
					}};
					for (uint32_t face = 0; face < faceCount; ++face) {
						PbrUniforms localUniforms = uniforms;
						localUniforms.Shadow = glm::vec4{0.0f, 0.0f, -1.0f, 0.0f};
						if (shadows) {
							glm::mat4 projection;
							glm::mat4 view;
							if (point) {
								projection = glm::perspectiveRH_ZO(
									glm::radians(90.0f), 1.0f, 0.05f, selected.Position[0].w
								);
								view = glm::lookAtRH(position, position + directions[face], ups[face]);
								localUniforms.Shadow.z = static_cast<float>(face);
							} else {
								const float angle =
									2.0f * acos(glm::clamp(selected.Direction[0].w, -0.999f, 0.999f));
								projection =
									glm::perspectiveRH_ZO(angle, 1.0f, 0.05f, selected.Position[0].w);
								const glm::vec3 direction = glm::normalize(glm::vec3(selected.Direction[0]));
								const glm::vec3 up =
									std::abs(direction.y) > 0.99f ? glm::vec3{0, 0, 1} : glm::vec3{0, 1, 0};
								view = glm::lookAtRH(position, position + direction, up);
							}
							if (!recording.RecordLocalLightShadow(projection * view)) return false;
							localUniforms.LightViewProjection = projection * view;
							localUniforms.Shadow.x = 1.0f;
							localUniforms.Shadow.y = 1.0f / float(SHADOW_RESOLUTION);
							spillBindings[6] =
								SDL_GPUTextureSamplerBinding{State->ShadowTexture, State->ShadowSampler};
						}
						SDL_GPUColorTargetInfo targetInfos[2]{};
						targetInfos[0].texture = target.Texture;
						targetInfos[1].texture = visibilityTarget.Texture;
						for (auto &targetInfo : targetInfos) {
							targetInfo.load_op = face == 0 ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
							targetInfo.store_op = SDL_GPU_STOREOP_STORE;
							targetInfo.cycle = face == 0;
						}
						auto *localPass = SDL_BeginGPURenderPass(Command, targetInfos, 2, nullptr);
						if (!localPass) return false;
						SDL_BindGPUGraphicsPipeline(localPass, State->DeferredLocalLightPipeline);
						SDL_BindGPUFragmentSamplers(
							localPass, 0, spillBindings.data(), static_cast<uint32_t>(spillBindings.size())
						);
						SDL_PushGPUFragmentUniformData(Command, 0, &localUniforms, sizeof(localUniforms));
						SDL_PushGPUFragmentUniformData(Command, 1, &selected, sizeof(selected));
						SDL_PushGPUFragmentUniformData(Command, 2, &State->Beams, sizeof(State->Beams));
						SDL_SetGPUViewport(localPass, &viewport);
						SDL_SetGPUScissor(localPass, &scissor);
						SDL_DrawGPUPrimitives(localPass, 3, 1, 0, 0);
						SDL_EndGPURenderPass(localPass);
					}
				}
				if (!emptyLighting && !applyLocalShadowCorrections()) return false;
				if (emptyLighting && blackSkyLit) recording.BlackSkyLit = pbr.Lit;
				return true;
			}

			if (emptyLighting) {
				if (!recording.ClearImage(
						context.Name,
						{pbr.Lit,
						 pbrDimensions.LitWidth,
						 pbrDimensions.LitHeight,
						 SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT},
						blackSkyLit
							? SDL_FColor{0.f, 0.f, 0.f, 1.f}
							: SDL_FColor{
								  uniforms.FogColour.r, uniforms.FogColour.g, uniforms.FogColour.b, 1.f
							  }
					))
					return false;
				core::Metrics::Count("render.empty_lighting.clears", 1);
				if (blackSkyLit) recording.BlackSkyLit = pbr.Lit;
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
				SDL_FColor{State->FogColour.r, State->FogColour.g, State->FogColour.b, 1.0f},
				&State->Beams
			);
			return applyLocalShadowCorrections();
		});

		const auto refuseWeather = [this]() {
			for (auto &generation : State->WeatherGenerations)
				if (generation.World == Request.World && generation.Pipeline == Pipeline->Name)
					RefuseWeatherPublication(generation);
			core::Metrics::Count("render.weather.product_refusals", 1);
			return true;
		};
		const auto weatherInputs = [this]() -> std::optional<WeatherGpuInputs> {
			if (!State->Caps.HasCompute || !State->WeatherClockCompute || !State->WeatherFieldCompute ||
				!State->WeatherSummaryCompute)
				return std::nullopt;
			const auto &environment = CurrentLighting.EnvironmentState;
			if (environment.CloudLayer.Enabled &&
				(!State->WeatherRaysCompute || !State->WeatherTemporalCompute ||
				 !State->WeatherReconstructCompute))
				return std::nullopt;
			const bool visibleMedium =
				environment.CloudLayer.Cover > 0 && environment.CloudLayer.Density > 0 &&
				(environment.CloudLayer.Enabled ||
				 (environment.Weather.ShadowsEnabled && environment.Weather.ShadowStrength > 0 &&
				  environment.Weather.ShadowDistanceMetres > 0));
			if (!environment.HasWeatherCompute || !environment.Weather.Enabled || !environment.HasClouds ||
				!visibleMedium)
				return std::nullopt;
			const bool stateful = environment.Weather.Mode == scene::WeatherMode::Stateful;
			if (stateful && !State->WeatherEvolutionCompute) return std::nullopt;
			const auto cached = std::find_if(
				State->WeatherGenerations.begin(),
				State->WeatherGenerations.end(),
				[this](const WeatherGpuGeneration &generation) {
					return generation.World == Request.World && generation.Pipeline == Pipeline->Name;
				}
			);
			std::optional<weather::CompiledWeather> compiled;
			const auto grid = stateful
								  ? (cached != State->WeatherGenerations.end() && cached->PhysicalOwner &&
											 cached->PhysicalOwner->ExpectedDefinition &&
											 cached->PhysicalOwner->RecipeSignature ==
												 WeatherPhysicalSignature(
													 environment,
													 Request.Source->WeatherSimulationLayer,
													 Request.Source->WeatherTerrainRevision
												 )
										 ? WeatherSimulationGridOf(*cached->PhysicalOwner->ExpectedDefinition)
										 : WeatherSimulationGridOf(environment.Weather))
								  : WeatherGridOf(environment.Weather);
			if (!grid) return std::nullopt;
			if (stateful) {
				compiled.emplace();
				compiled->Definition.Seed = environment.Weather.Seed;
				compiled->Definition.LayerKey =
					weather::WeatherLayerKey(environment.Weather.LayerName.Text());
				compiled->Definition.Revision = environment.Weather.DefinitionRevision;
				compiled->Definition.BaseCell = grid->Origin.Y;
				compiled->Definition.ThicknessCells = grid->Dimensions.y;
				compiled->Profile = WeatherEncodingOf(environment.Weather);
			} else {
				compiled = weather::Compile(environment.Weather);
			}
			if (!compiled) return std::nullopt;
			const auto time = environment.HasWeatherClock
								  ? weather::WeatherTimeOf(environment.Clock)
								  : weather::ResolveTime(weather::InitialAnchor(compiled->Definition), 0);
			if (!time) return std::nullopt;
			WeatherGpuInputs inputs{
				WeatherUniformsOf(compiled->Definition, *time, *grid), *compiled, *time, stateful
			};
			if (!stateful && cached != State->WeatherGenerations.end() &&
				!cached->Topology.PageKeys.empty()) {
				const uint32_t count = static_cast<uint32_t>(cached->Topology.PageKeys.size());
				inputs.Uniforms.Grid = {8, 8, 8, count * 512};
				inputs.Uniforms.Origin.w = 0;
				inputs.Uniforms.Bricks = {0, 0, 0, count};
				inputs.Uniforms.Phase.w = static_cast<uint32_t>(cached->Topology.PageKeys.back().w);
			}
			if (stateful) {
				inputs.Uniforms.Time = {};
				const bool published =
					cached != State->WeatherGenerations.end() && cached->PhysicalOwner &&
					cached->PhysicalOwner->Physical.Published &&
					cached->PhysicalOwner->PhysicalRecipeSignature ==
						WeatherPhysicalSignature(
							environment,
							Request.Source->WeatherSimulationLayer,
							Request.Source->WeatherTerrainRevision
						) &&
					cached->PhysicalOwner->Physical.Tick <= inputs.Time.PhaseTick &&
					cached->PhysicalOwner->Physical.Definition.Grid.CellSizeMetres ==
						environment.Weather.CellSizeMetres &&
					cached->PhysicalOwner->Physical.Definition.TickRate == environment.Weather.TickRate &&
					(!environment.HasWeatherClock ||
					 cached->PhysicalOwner->Physical.Definition.WorldEpoch == environment.Clock.Epoch) &&
					cached->PhysicalOwner->Physical.Definition.Grid.Cells ==
						std::array<uint32_t, 3>{grid->Dimensions.x, grid->Dimensions.y, grid->Dimensions.z} &&
					cached->PhysicalOwner->Physical.Definition.Grid.Origin ==
						std::array<int32_t, 3>{grid->Origin.X, grid->Origin.Y, grid->Origin.Z} &&
					cached->PhysicalOwner->EncodingSignature ==
						WeatherEncodingSignature(inputs.Compiled.Profile);
				inputs.Uniforms.Phase = published
					? glm::uvec4{uint32_t(cached->PhysicalOwner->Physical.Tick), uint32_t(cached->PhysicalOwner->Physical.Tick >> 32), 0, 0}
					: glm::uvec4{0, 0, UINT32_MAX, 0};
				inputs.Time.DisplacementQ8 = {};
				if (published) {
					inputs.Time.PhaseTick = cached->PhysicalOwner->Physical.Tick;
					inputs.Compiled.Definition.Revision = cached->PhysicalOwner->Physical.Definition.Revision;
					inputs.Uniforms.Identity.z = cached->PhysicalOwner->Physical.Definition.Revision;
				}
			}
			return inputs;
		};

		frameNodes.Set(
			core::Name("weather-residency"),
			[this, weatherInputs, refuseWeather](const graph::RunContext &context) {
				if (context.Writes.size() != 2 || !context.Reads.empty()) return false;
				const auto inputs = weatherInputs();
				if (!State->Caps.HasCompute || !State->WeatherFieldCompute || !inputs) return true;
				auto *pages = GraphBuffer(context.Writes[0], context, true);
				auto *previousPages = GraphBuffer(context.Writes[1], context, true);
				if (!pages || !previousPages) return refuseWeather();
				auto found = std::find_if(
					State->WeatherGenerations.begin(),
					State->WeatherGenerations.end(),
					[this, pages](const WeatherGpuGeneration &generation) {
						return generation.World == Request.World && generation.Pages == pages;
					}
				);
				if (found == State->WeatherGenerations.end()) {
					State->WeatherGenerations.emplace_back();
					found = std::prev(State->WeatherGenerations.end());
					found->World = Request.World;
					found->Pipeline = Pipeline->Name;
					found->Pages = pages;
				}
				found->PreviousPages = previousPages;
				if (inputs->Stateful) {
					const glm::ivec4 key{
						inputs->Uniforms.Origin.x, inputs->Uniforms.Origin.y, inputs->Uniforms.Origin.z, 0
					};
					const uint64_t signature = WeatherSignature(inputs->Uniforms);
					if (found->Residency.Matches(signature, Command)) return true;
					EnterNamedPass(context.Name);
					if (!RecordWeatherPages(State->Device, Command, pages, std::span(&key, 1)))
						return refuseWeather();
					if (!RecordWeatherPages(State->Device, Command, previousPages, std::span(&key, 1)))
						return refuseWeather();
					found->Residency.Stage(signature, Command);
					StageHistoryWrites(context, Command);
					return true;
				}
				std::array<WeatherResidencyView, weather::MAXIMUM_RESIDENCY_VIEWS> views{};
				size_t viewCount = 0;
				for (const auto &view : State->WeatherViews)
					if (view.World == Request.World) {
						if (viewCount < views.size())
							views[viewCount++] = view.Demand;
						else
							core::Metrics::Count("render.weather.refused_view_demands", 1);
					}
				WeatherResidencyView currentView;
				currentView.View.Key = scene::MixSignature(
					Request.TargetSlot, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(Request.Target))
				);
				currentView.View.CameraPositionMetres = Request.CameraFrame.Position;
				currentView.View.PortalRevision =
					Request.Source->CameraCut ? Request.Source->CameraTemporalSequence : 0;
				currentView.ProjectionPixelsPerRadian =
					SceneHeight / std::max(DrawCamera.FieldOfViewRadians, .01f);
				currentView.MaximumDistanceMetres = DrawCamera.FarPlane;
				const auto sameCamera = std::find_if(
					views.begin(),
					views.begin() + static_cast<ptrdiff_t>(viewCount),
					[&](const WeatherResidencyView &view) {
						return view.View.CameraPositionMetres == currentView.View.CameraPositionMetres &&
							   view.ProjectionPixelsPerRadian == currentView.ProjectionPixelsPerRadian;
					}
				);
				if (sameCamera == views.begin() + static_cast<ptrdiff_t>(viewCount)) {
					if (viewCount < views.size())
						views[viewCount++] = currentView;
					else {
						// Every refused refinement still samples the guaranteed whole-world coarse field.
						views[viewCount - 1] = currentView;
						core::Metrics::Count("render.weather.refused_view_demands", 1);
					}
				}
				const auto shadowPlan = BuildWeatherShadowPlan(
					CurrentLighting.EnvironmentState.Weather,
					*inputs,
					{State->Sun.x, State->Sun.y, State->Sun.z},
					CurrentLighting.EnvironmentState.CloudLayer.Density *
						std::clamp(CurrentLighting.EnvironmentState.CloudLayer.Cover, 0.0f, 1.0f),
					scene::MixSignature(
						CurrentLighting.EnvironmentState.Clock.Epoch,
						CurrentLighting.EnvironmentState.WeatherHistoryRevision
					),
					std::span(views.data(), viewCount)
				);
				viewCount = AppendWeatherShadowDemand(
					views, viewCount, shadowPlan, inputs->Compiled.Definition.LayerKey
				);
				const auto plan = BuildWeatherResidency(
					found->Topology,
					CurrentLighting.EnvironmentState.Weather,
					inputs->Compiled,
					inputs->Time,
					scene::MixSignature(
						CurrentLighting.EnvironmentState.Clock.Epoch,
						CurrentLighting.EnvironmentState.WeatherHistoryRevision
					),
					std::span(views.data(), viewCount),
					State->FrameCounter + 1,
					State->WeatherPresentationSeconds
						? *State->WeatherPresentationSeconds
						: std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
							  .count()
				);
				if (!plan) return refuseWeather();
				found->FieldWords = plan->FieldWords;
				found->TransitionAlpha = plan->TransitionAlpha;
				found->PreviousPageCount = static_cast<uint32_t>(plan->PreviousPages.size());
				if (found->Residency.Matches(plan->Signature, Command)) {
					core::Metrics::Count("render.weather.residency_cache_hits", 1);
					StageHistoryWrites(context, Command);
					return true;
				}
				EnterNamedPass(context.Name);
				std::array<glm::ivec4, WEATHER_MAX_BRICKS> descriptors{};
				uint32_t offsetWords = 0;
				for (size_t index = 0; index < plan->Pages.size(); ++index) {
					descriptors[index] = plan->Pages[index];
					const uint32_t kind = plan->PageKinds[index];
					descriptors[index].w = (plan->Pages[index].w & 31) | static_cast<int32_t>(kind << 5) |
										   static_cast<int32_t>(offsetWords << 7);
					offsetWords += kind == 2 ? 0 : kind == 1 ? 512 : 1024;
				}
				if (!RecordWeatherPages(
						State->Device, Command, pages, std::span(descriptors.data(), plan->Pages.size())
					))
					return refuseWeather();
				const glm::ivec4 emptyKey{};
				if (!RecordWeatherPages(
						State->Device,
						Command,
						previousPages,
						plan->PreviousPages.empty() ? std::span(&emptyKey, 1) : plan->PreviousPages
					))
					return refuseWeather();
				found->Residency.Stage(plan->Signature, Command);
				core::Metrics::Count("render.weather.refused_refinements", plan->RefusedRefinements);
				core::Metrics::SetGauge("render.weather.maintenance_age_ticks", plan->MaintenanceAgeTicks);
				core::Metrics::SetGauge("render.weather.coarse_pages", plan->CoarsePages);
				StageHistoryWrites(context, Command);
				return true;
			}
		);

		frameNodes.Set(
			core::Name("weather-field"),
			[this, weatherInputs, refuseWeather](const graph::RunContext &context) {
				if (context.Writes.size() != 1 || context.Reads.size() != 1) return false;
				auto inputs = weatherInputs();
				if (!State->Caps.HasCompute || !State->WeatherFieldCompute || !inputs) {
					State->ForgetWeatherWorld(Request.World);
					return true;
				}
				auto *pages = GraphBuffer(context.Reads[0], context, false);
				if (!pages) return refuseWeather();
				auto found = std::find_if(
					State->WeatherGenerations.begin(),
					State->WeatherGenerations.end(),
					[this, pages](const WeatherGpuGeneration &generation) {
						return generation.World == Request.World && generation.Pages == pages;
					}
				);
				if (found == State->WeatherGenerations.end()) {
					State->WeatherGenerations.emplace_back();
					found = std::prev(State->WeatherGenerations.end());
					found->World = Request.World;
					found->Pipeline = Pipeline->Name;
					found->Pages = pages;
				}
				if (found->Residency.SignatureFor(Command) == 0) return refuseWeather();
				const uint64_t physicalBytes = inputs->Stateful ? uint64_t(inputs->Uniforms.Grid.w) * 8
																: uint64_t(found->FieldWords) * 4;
				auto *cells =
					GraphBuffer(context.Writes[0], context, true, std::max(uint64_t{8}, physicalBytes));
				if (!cells) return refuseWeather();
				if (found->Cells != cells) {
					found->Field = {};
					found->Summary = {};
				}
				found->Cells = cells;
				found->PhysicalOwner = inputs->Stateful ? &State->WeatherPhysicalFor(
															  Request.World,
															  WeatherPhysicalSignature(
																  CurrentLighting.EnvironmentState,
																  Request.Source->WeatherSimulationLayer,
																  Request.Source->WeatherTerrainRevision
															  )
														  )
														: nullptr;
				if (inputs->Stateful) {
					const auto &source = *Request.Source;
					const bool hadFence = found->PhysicalOwner->Physical.Fence != nullptr;
					const bool accepted =
						PollWeatherSimulationGpu(State->Device, found->PhysicalOwner->Physical);
					if (accepted) {
						found->PhysicalOwner->CheckpointSignature =
							found->PhysicalOwner->PendingCheckpointSignature;
						found->PhysicalOwner->EncodingSignature =
							found->PhysicalOwner->PendingEncodingSignature;
						found->PhysicalOwner->PhysicalRecipeSignature =
							found->PhysicalOwner->PendingPhysicalRecipeSignature;
					} else if (hadFence && !found->PhysicalOwner->Physical.Fence) {
						found->PhysicalOwner->RefusedStageSignature =
							found->PhysicalOwner->PendingStageSignature;
						found->PhysicalOwner->CatchupPending = false;
					}
					uint64_t checkpointSignature = 0;
					if (!source.WeatherCheckpoint.empty()) {
						checkpointSignature = scene::MixSignature(
							source.WeatherCheckpointVersion, source.WeatherCheckpointTick
						);
						checkpointSignature =
							scene::MixSignature(checkpointSignature, source.WeatherCheckpointEpoch);
						checkpointSignature =
							scene::MixSignature(checkpointSignature, source.WeatherCheckpointRevision);
						if (!source.WeatherCheckpointVersion)
							for (std::byte byte : source.WeatherCheckpoint)
								checkpointSignature =
									scene::MixSignature(checkpointSignature, std::to_integer<uint8_t>(byte));
					}
					if (!found->PhysicalOwner->Physical.Fence) {
						auto &definition = found->PhysicalOwner->ExpectedDefinition;
						if (!definition) {
							auto physicalControls = CurrentLighting.EnvironmentState.Weather;
							physicalControls.Paused = false;
							physicalControls.TimeScale = 1;
							definition = weather::MakeSimulationDefinition(
								physicalControls,
								source.WeatherSimulationLayer.IsValid()
									? std::string(source.WeatherSimulationLayer.Text())
									: "weather"
							);
							if (definition) {
								definition->WorldEpoch = CurrentLighting.EnvironmentState.Clock.Epoch;
								definition->TerrainRevision = source.WeatherTerrainRevision;
							}
						}
						// Revisions carry forcing edits too. Retain the compatible resident model while
						// requiring the new checkpoint revision before another physical step.
						if (definition)
							definition->Revision =
								CurrentLighting.EnvironmentState.Weather.DefinitionRevision;
						if (definition && found->PhysicalOwner->Physical.CellCount &&
							definition->Grid != found->PhysicalOwner->Physical.Definition.Grid) {
							ReleaseWeatherSimulationGpu(State->Device, found->PhysicalOwner->Physical);
							found->PhysicalOwner->CheckpointSignature = 0;
							found->PhysicalOwner->EncodingSignature = 0;
						}
						const auto checkpoint =
							checkpointSignature != found->PhysicalOwner->CheckpointSignature
								? source.WeatherCheckpoint
								: std::span<const std::byte>{};
						if (found->PhysicalOwner->Physical.Published || !checkpoint.empty()) {
							const auto targetTime =
								weather::WeatherTimeOf(CurrentLighting.EnvironmentState.Clock);
							const uint64_t stageSignature = scene::MixSignature(
								scene::MixSignature(
									scene::MixSignature(
										checkpointSignature, targetTime ? targetTime->PhaseTick : 0
									),
									CurrentLighting.EnvironmentState.Weather.DefinitionRevision
								),
								WeatherEncodingSignature(inputs->Compiled.Profile)
							);
							const bool refused =
								found->PhysicalOwner->RefusedStageSignature == stageSignature;
							const bool staged = !refused && targetTime && definition &&
												StageWeatherSimulationGpu(
													State->Device,
													State->WeatherEvolutionCompute,
													found->PhysicalOwner->Physical,
													targetTime->PhaseTick,
													inputs->Compiled.Profile,
													checkpoint,
													128,
													{},
													&*definition,
													true
												);
							found->PhysicalOwner->CatchupPending =
								staged && targetTime &&
								(found->PhysicalOwner->Physical.Fence
									 ? found->PhysicalOwner->Physical.PendingTick
									 : found->PhysicalOwner->Physical.Tick) < targetTime->PhaseTick;
							if (staged && found->PhysicalOwner->Physical.Fence) {
								found->PhysicalOwner->PendingStageSignature = stageSignature;
								found->PhysicalOwner->PendingCheckpointSignature =
									checkpointSignature ? checkpointSignature
														: found->PhysicalOwner->CheckpointSignature;
								found->PhysicalOwner->PendingEncodingSignature =
									WeatherEncodingSignature(inputs->Compiled.Profile);
								found->PhysicalOwner->PendingPhysicalRecipeSignature =
									WeatherPhysicalSignature(
										CurrentLighting.EnvironmentState,
										Request.Source->WeatherSimulationLayer,
										Request.Source->WeatherTerrainRevision
									);
								Result.ComputeDispatches +=
									found->PhysicalOwner->Physical.LastSubmittedDispatches;
								++Result.AsyncComputeCommandBuffers;
							} else if (!staged && !refused) {
								found->PhysicalOwner->RefusedStageSignature = stageSignature;
								core::Metrics::Count("render.weather.stateful.fallbacks", 1);
							}
						}
					}
					inputs = weatherInputs();
					if (!inputs) return refuseWeather();
				}
				const auto &environment = CurrentLighting.EnvironmentState;
				const WeatherGpuClockSample clockSample{
					.Time = inputs->Time,
					.SourceTick =
						environment.HasWeatherClock ? environment.Clock.Tick : inputs->Time.PhaseTick,
					.TickRate = environment.Weather.TickRate,
					.Identity = scene::MixSignature(
						scene::MixSignature(environment.CloudProvider.Id, environment.Clock.Epoch),
						environment.WeatherHistoryRevision
					)
				};
				// The shared field stage advances once per logical sample, including batches with many views.
				const auto clockPlan = PlanWeatherGpuClock(found->Clock, clockSample, Command);
				if (!clockPlan) return refuseWeather();
				if (clockPlan->Action != WeatherGpuClockAction::Reuse)
					EnterNamedPass(core::Name("weather-clock"));
				if (!RecordWeatherGpuClock(
						State->Device, Command, State->WeatherClockCompute, found->Clock, clockSample
					))
					return refuseWeather();
				if (clockPlan->Action != WeatherGpuClockAction::Reuse) {
					++Result.ComputeDispatches;
					ClosePass();
				}
				auto fieldIdentity = inputs->Uniforms;
				if (!inputs->Stateful && physicalBytes == 0) {
					fieldIdentity.Time = {};
					fieldIdentity.Phase.x = 0;
					fieldIdentity.Phase.y = 0;
					fieldIdentity.Phase.z = 0;
				}
				const uint64_t signature = scene::MixSignature(
					scene::MixSignature(
						WeatherSignature(fieldIdentity), found->Residency.SignatureFor(Command)
					),
					CurrentLighting.EnvironmentState.CloudProvider.Id
				);
				if (found->Field.Matches(signature, Command)) {
					if (found->Field.Pending && found->Field.PendingCommand == GpuCommandIdentity(Command))
						found->PendingRenderInputs = *inputs;
					else
						found->RenderInputs = *inputs;
					core::Metrics::Count("render.weather.field_cache_hits", 1);
					StageHistoryWrites(context, Command);
					return true;
				}
				EnterNamedPass(context.Name);
				if (!inputs->Stateful && physicalBytes == 0) {
					if (!found->Field.Ready && !RecordWeatherEmptyField(State->Device, Command, cells))
						return refuseWeather();
					core::Metrics::Count("render.weather.certified_empty_field_skips", 1);
				} else if (inputs->Stateful && inputs->Uniforms.Phase.z != UINT32_MAX) {
					auto *copy = SDL_BeginGPUCopyPass(Command);
					if (!copy) return refuseWeather();
					const SDL_GPUBufferLocation from{found->PhysicalOwner->Physical.PublishedPacked, 0};
					const SDL_GPUBufferLocation to{cells, 0};
					SDL_CopyGPUBufferToBuffer(
						copy, &from, &to, found->PhysicalOwner->Physical.CellCount * 8, true
					);
					SDL_EndGPUCopyPass(copy);
					core::Metrics::Count(
						"render.weather.stateful.packed_copy_bytes",
						found->PhysicalOwner->Physical.CellCount * 8
					);
				} else {
					if (!RecordWeatherField(
							Command,
							State->WeatherFieldCompute,
							cells,
							pages,
							inputs->Uniforms,
							physicalBytes,
							found->Clock.Buffer
						))
						return refuseWeather();
					++Result.ComputeDispatches;
				}
				found->Field.Stage(signature, Command);
				found->PendingRenderInputs = *inputs;
				StageHistoryWrites(context, Command);
				return true;
			}
		);

		frameNodes.Set(
			core::Name("weather-summary"),
			[this, weatherInputs, refuseWeather](const graph::RunContext &context) {
				if (context.Reads.size() != 2 || context.Writes.size() != 1) return false;
				const auto inputs = weatherInputs();
				if (!State->Caps.HasCompute || !State->WeatherSummaryCompute || !inputs) return true;
				auto *cells = GraphBuffer(context.Reads[0], context, false);
				auto *summaries = GraphBuffer(context.Writes[0], context, true);
				if (!cells || !summaries) return refuseWeather();
				const auto found = std::find_if(
					State->WeatherGenerations.begin(),
					State->WeatherGenerations.end(),
					[this, cells](const WeatherGpuGeneration &generation) {
						return generation.World == Request.World && generation.Cells == cells;
					}
				);
				if (found == State->WeatherGenerations.end()) return refuseWeather();
				const uint64_t signature =
					!inputs->Stateful && found->FieldWords == 0
						? found->Field.SignatureFor(Command)
						: scene::MixSignature(
							  scene::MixSignature(
								  WeatherSignature(inputs->Uniforms), found->Residency.SignatureFor(Command)
							  ),
							  CurrentLighting.EnvironmentState.CloudProvider.Id
						  );
				if (!found->Field.Matches(signature, Command)) return refuseWeather();
				if (found->Summaries == summaries && found->Summary.Matches(signature, Command)) {
					core::Metrics::Count("render.weather.summary_cache_hits", 1);
					StageHistoryWrites(context, Command);
					return true;
				}
				EnterNamedPass(context.Name);
				if (!RecordWeatherSummary(
						Command,
						State->WeatherSummaryCompute,
						cells,
						found->Pages,
						summaries,
						inputs->Uniforms,
						found->Clock.Buffer
					))
					return refuseWeather();
				found->Summaries = summaries;
				found->Summary.Stage(signature, Command);
				++Result.ComputeDispatches;
				StageHistoryWrites(context, Command);
				return true;
			}
		);

		frameNodes.Set(
			core::Name("weather-integrate"), [this, weatherInputs](const graph::RunContext &context) {
				if (context.Reads.size() != 6 || context.Writes.size() != 2) return false;
				const auto inputs = weatherInputs();
				if (WeatherFallbackApplied || !State->Caps.HasCompute || !State->WeatherRaysCompute ||
					!inputs || !CurrentLighting.EnvironmentState.CloudLayer.Enabled)
					return true;
				const auto generation = std::find_if(
					State->WeatherGenerations.begin(),
					State->WeatherGenerations.end(),
					[this](const WeatherGpuGeneration &candidate) {
						return candidate.World == Request.World && candidate.Pipeline == Pipeline->Name;
					}
				);
				if (generation == State->WeatherGenerations.end() ||
					!WeatherProductsReady(&*generation, Command))
					return true;
				if (!inputs->Stateful && generation->FieldWords == 0) return true;
				const auto depth = GraphTexture(context.Reads[1], context, false);
				const auto target = GraphTexture(context.Writes[0], context, true);
				const auto cloudDepth = GraphTexture(context.Writes[1], context, true);
				auto *cells = GraphBuffer(context.Reads[2], context, false);
				auto *summaries = GraphBuffer(context.Reads[3], context, false);
				auto *pages = GraphBuffer(context.Reads[4], context, false);
				auto *previousPages = GraphBuffer(context.Reads[5], context, false);
				if (!depth.IsValid() || !target.IsValid() || !cloudDepth.IsValid() || !cells || !summaries ||
					!pages || !previousPages || target.Format != SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT ||
					cloudDepth.Format != target.Format || cloudDepth.Width != target.Width ||
					cloudDepth.Height != target.Height)
					return true;
				const auto &environment = CurrentLighting.EnvironmentState;
				const auto &controls = environment.Weather;
				const auto &appearance = environment.CloudLayer;
				WeatherRayUniforms uniforms;
				uniforms.InverseViewProjection = Uniforms.InverseViewProjection;
				uniforms.ImageExtent = {depth.Width, depth.Height, 0, 0};
				uniforms.Eye = Uniforms.Eye;
				uniforms.Minimum = {
					-controls.ExtentMetres.X * .5f,
					controls.BaseHeightMetres,
					-controls.ExtentMetres.Z * .5f,
					controls.CellSizeMetres
				};
				uniforms.Maximum = {
					controls.ExtentMetres.X * .5f,
					controls.BaseHeightMetres + controls.ThicknessMetres,
					controls.ExtentMetres.Z * .5f,
					0
				};
				uniforms.Maximum.w = glm::length(glm::vec3(uniforms.Maximum - uniforms.Minimum)) +
									 glm::length(glm::vec3(uniforms.Eye - uniforms.Minimum));
				uniforms.ColourExtinction = {
					appearance.Colour.R,
					appearance.Colour.G,
					appearance.Colour.B,
					weather::ExtinctionPerMetre(controls.CondensateScale) * appearance.Density *
						std::clamp(appearance.Cover, 0.0f, 1.0f)
				};
				const auto cloudLighting = WeatherLightingOf(CurrentLighting);
				uniforms.Sun = cloudLighting.Direction;
				uniforms.SunColour = cloudLighting.Direct;
				uniforms.Ambient = cloudLighting.Ambient;
				if (generation->Pages != pages) return true;
				uniforms.Quality = {
					std::clamp(environment.CloudVolume.Steps, 2u, 64u) * 4u,
					std::bit_cast<uint32_t>(controls.LodError),
					generation->PreviousPageCount,
					std::bit_cast<uint32_t>(generation->TransitionAlpha)
				};
				uniforms.Field = inputs->Uniforms;
				EnterNamedPass(context.Name);
				if (!RecordWeatherRays(
						Command,
						State->WeatherRaysCompute,
						cells,
						summaries,
						pages,
						previousPages,
						depth.Texture,
						target.Texture,
						cloudDepth.Texture,
						State->OverlaySampler,
						target.Width,
						target.Height,
						uniforms,
						generation->Clock.Buffer
					))
					return true;
				++Result.ComputeDispatches;
				WeatherIntegrationRecorded = true;
				StageHistoryWrites(context, Command);
				return true;
			}
		);

		frameNodes.Set(
			core::Name("weather-history"), [this, weatherInputs](const graph::RunContext &context) {
				if (!context.Reads.empty() || context.Writes.size() != 2) return false;
				const auto inputs = weatherInputs();
				const auto generation = std::find_if(
					State->WeatherGenerations.begin(),
					State->WeatherGenerations.end(),
					[this](const WeatherGpuGeneration &candidate) {
						return candidate.World == Request.World && candidate.Pipeline == Pipeline->Name;
					}
				);
				const bool empty = inputs && !inputs->Stateful &&
								   generation != State->WeatherGenerations.end() &&
								   generation->FieldWords == 0;
				if (!inputs || !CurrentLighting.EnvironmentState.CloudLayer.Enabled || empty)
					State->ReleaseWeatherTemporalSlot(Request.TargetSlot);
				// Previous images are optional on the first frame. Reconstruction binds
				// initialized current samples until a submitted history is available.
				EnterNamedPass(context.Name);
				return true;
			}
		);

		frameNodes.Set(
			core::Name("weather-reconstruct"), [this, weatherInputs](const graph::RunContext &context) {
				if (context.Reads.size() != 6 || context.Writes.size() != 2) return false;
				const auto inputs = weatherInputs();
				const auto generation = std::find_if(
					State->WeatherGenerations.begin(),
					State->WeatherGenerations.end(),
					[this](const WeatherGpuGeneration &candidate) {
						return candidate.World == Request.World && candidate.Pipeline == Pipeline->Name;
					}
				);
				const bool empty = inputs && !inputs->Stateful &&
								   generation != State->WeatherGenerations.end() &&
								   generation->FieldWords == 0;
				const auto fallback = [&]() {
					State->ReleaseWeatherTemporalSlot(Request.TargetSlot);
					if (!WeatherFallbackApplied && !empty && WeatherVolumeAvailable() &&
						VisibleWeatherMedium(CurrentLighting.EnvironmentState)) {
						const auto source = GraphTexture(context.Reads[0], context, false);
						const auto depth = GraphTexture(context.Reads[1], context, false);
						const auto target = GraphTexture(context.Writes[1], context, true);
						if (RecordWeatherFallback(context, source, depth, target, {}, false, true, true)) {
							WeatherFallbackApplied = true;
							return true;
						}
					}
					return PublishIdentity(context, context.Reads[0], context.Writes[1]);
				};
				if (!WeatherIntegrationRecorded || !inputs || !State->WeatherTemporalCompute ||
					!State->WeatherReconstructCompute ||
					!CurrentLighting.EnvironmentState.CloudLayer.Enabled || empty) {
					return fallback();
				}
				const auto source = GraphTexture(context.Reads[0], context, false);
				const auto depth = GraphTexture(context.Reads[1], context, false);
				const auto clouds = GraphTexture(context.Reads[2], context, false);
				const auto cloudDepth = GraphTexture(context.Reads[3], context, false);
				const auto resolved = GraphTexture(context.Writes[0], context, true);
				const auto target = GraphTexture(context.Writes[1], context, true);
				if (!source.IsValid() || !depth.IsValid() || !clouds.IsValid() || !cloudDepth.IsValid() ||
					!resolved.IsValid() || !target.IsValid() || source.Width != target.Width ||
					source.Height != target.Height || depth.Width != target.Width ||
					depth.Height != target.Height || clouds.Width != resolved.Width ||
					clouds.Height != resolved.Height || cloudDepth.Width != resolved.Width ||
					cloudDepth.Height != resolved.Height ||
					source.Format != SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT ||
					target.Format != source.Format || clouds.Format != source.Format ||
					cloudDepth.Format != source.Format || resolved.Format != source.Format)
					return fallback();
				auto &history = State->WeatherTemporalAt(Request.TargetSlot, Request.World, Pipeline->Name);
				if (!EnsureWeatherTemporalHistory(State->Device, history, resolved.Width, resolved.Height))
					return fallback();
				const auto &environment = CurrentLighting.EnvironmentState;
				auto identity = inputs->Uniforms;
				identity.Time = {};
				identity.Phase.x = 0;
				identity.Phase.y = 0;
				identity.Phase.z = 0;
				uint64_t content = WeatherSignature(identity);
				content = scene::MixSignature(content, environment.CloudVolume.Steps);
				for (float value :
					 {environment.CloudLayer.Colour.R,
					  environment.CloudLayer.Colour.G,
					  environment.CloudLayer.Colour.B,
					  environment.CloudLayer.Density,
					  environment.CloudLayer.Cover,
					  environment.Weather.CondensateScale,
					  State->Sun.x,
					  State->Sun.y,
					  State->Sun.z})
					content = scene::MixSignature(content, std::bit_cast<uint32_t>(value));
				const auto cloudLighting = WeatherLightingOf(CurrentLighting);
				for (const auto colour :
					 {cloudLighting.Direct, cloudLighting.Ambient, cloudLighting.Direction})
					for (size_t lane = 0; lane < 3; ++lane)
						content = scene::MixSignature(content, std::bit_cast<uint32_t>(colour[lane]));
				WeatherTemporalFrame current;
				current.Key.World = Request.World;
				current.Key.Provider = environment.CloudProvider.Id;
				current.Key.Content = content;
				current.Key.Epoch = environment.Clock.Epoch;
				current.Key.TimelineRevision = scene::MixSignature(
					environment.Clock.DefinitionRevision, environment.WeatherHistoryRevision
				);
				current.Key.Tick = inputs->Stateful ? uint64_t(inputs->Uniforms.Phase.x) |
														  (uint64_t(inputs->Uniforms.Phase.y) << 32)
													: inputs->Time.PhaseTick;
				current.Key.TickRate = environment.Weather.TickRate;
				current.Key.Width = resolved.Width;
				current.Key.Height = resolved.Height;
				current.Key.CellSizeMetres = environment.Weather.CellSizeMetres;
				current.Key.DisplacementQ8 = inputs->Time.DisplacementQ8;
				current.Key.CameraCut = Request.Source && Request.Source->CameraCut;
				current.ViewProjection = Matrices.ViewProjection;
				current.Eye = glm::vec3(Uniforms.Eye);
				WeatherReconstructionUniforms uniforms;
				uniforms.InverseViewProjection = Uniforms.InverseViewProjection;
				uniforms.PreviousViewProjection = history.Accepted.ViewProjection;
				uniforms.Eye = Uniforms.Eye;
				uniforms.PreviousEye = glm::vec4(history.Accepted.Eye, 0);
				uniforms.Extents = {target.Width, target.Height, resolved.Width, resolved.Height};
				const bool compatible = history.Ready && !history.PendingCommand &&
										inputs->Uniforms.Phase.z != UINT32_MAX &&
										WeatherTemporalCompatible(history.Accepted, current);
				if (compatible)
					uniforms.CloudDisplacement =
						glm::vec4(WeatherCloudDisplacement(history.Accepted.Key, current.Key), 0);
				else {
					uniforms.Limits.x = 0;
					core::Metrics::Count("render.weather.history_resets", 1);
				}
				history.Stage(current, Command);
				EnterNamedPass(context.Name);
				if (!RecordWeatherReconstruction(
						Command,
						State->WeatherTemporalCompute,
						State->WeatherReconstructCompute,
						source.Texture,
						depth.Texture,
						clouds.Texture,
						cloudDepth.Texture,
						resolved.Texture,
						target.Texture,
						history,
						State->OverlaySampler,
						uniforms
					))
					return fallback();
				Result.ComputeDispatches += 2;
				StageHistoryWrites(context, Command);
				return true;
			}
		);

		frameNodes.Set(core::Name("skybox-compute"), [this](const graph::RunContext &context) {
			// Tier B intentionally retains the sky draw but omits this compute-only
			// producer. Its sky handler binds the procedural fallback below.
			if (!State->Caps.HasCompute) return true;
			if (context.Reads.size() != 0 || context.Writes.size() != 1) return false;
			const Impl::NamedTexture target = GraphTexture(context.Writes.front(), context, true);
			if (!target.IsValid()) return CurrentLighting.EnvironmentState.HasWeatherCompute;
			if (target.Format != SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT) return false;
			const bool recorded = State->RecordEnvironmentSkybox(
				CurrentLighting.EnvironmentState,
				Command,
				target.Texture,
				target.Width,
				target.Height,
				Result.ComputeDispatches,
				[this, name = context.Name] { EnterNamedPass(name); }
			);
			if (recorded) {
				OpaqueSkyTexture = target.Texture;
				StageHistoryWrites(context, Command);
			}
			return recorded || CurrentLighting.EnvironmentState.HasWeatherCompute;
		});

		frameNodes.Set(core::Name("clouds-compute"), [this](const graph::RunContext &context) {
			if (!State->Caps.HasCompute) return true;
			if (context.Reads.size() != 1 || context.Writes.size() != 1) {
				ENGINE_ERROR(
					"clouds compute '{}' requires one sky input and one clouds output, got {} and {}",
					context.Name.Text(),
					context.Reads.size(),
					context.Writes.size()
				);
				return false;
			}
			const Impl::NamedTexture source = GraphTexture(context.Reads.front(), context, false);
			const Impl::NamedTexture target = GraphTexture(context.Writes.front(), context, true);
			if (!source.IsValid()) {
				ENGINE_ERROR("clouds compute '{}' could not allocate environment-sky", context.Name.Text());
				return CurrentLighting.EnvironmentState.HasWeatherCompute;
			}
			if (!target.IsValid()) {
				ENGINE_ERROR(
					"clouds compute '{}' could not allocate environment-clouds", context.Name.Text()
				);
				return CurrentLighting.EnvironmentState.HasWeatherCompute;
			}
			if (source.Format != SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT || target.Format != source.Format) {
				ENGINE_ERROR(
					"clouds compute '{}' requires matching RGBA16F sky and clouds textures",
					context.Name.Text()
				);
				return false;
			}
			if (target.Width != source.Width || target.Height != source.Height) {
				ENGINE_ERROR(
					"clouds compute '{}' requires matching sky and clouds extents, got {}x{} and {}x{}",
					context.Name.Text(),
					source.Width,
					source.Height,
					target.Width,
					target.Height
				);
				return false;
			}
			const bool recorded = State->RecordEnvironmentClouds(
				CurrentLighting.EnvironmentState,
				Command,
				source.Texture,
				target.Texture,
				target.Width,
				target.Height,
				Result.ComputeDispatches,
				OpaqueSkyTexture == source.Texture,
				[this, name = context.Name] { EnterNamedPass(name); },
				WeatherVolumeAvailable()
			);
			if (!recorded) {
				ENGINE_ERROR("clouds compute '{}' could not record its compute pass", context.Name.Text());
			}
			if (recorded) StageHistoryWrites(context, Command);
			if (!recorded && CurrentLighting.EnvironmentState.HasWeatherCompute)
				return PublishIdentity(context, context.Reads[0], context.Writes[0]);
			return recorded;
		});

		frameNodes.Set(core::Name("sky"), [this](const graph::RunContext &context) {
			if ((context.Reads.size() != 2 && context.Reads.size() != 3) || context.Writes.size() != 1)
				return false;
			const auto environment = context.Reads.size() == 3
										 ? GraphTexture(context.Reads[2], context, false)
										 : Impl::NamedTexture{};
			const auto source = GraphTexture(context.Reads[0], context, false);
			const auto depth = GraphTexture(context.Reads[1], context, false);
			const bool blackSky =
				source.IsValid() && source.Texture == BlackSkyLit && depth.Texture == EmptyHardwareDepth &&
				State->IsBlackEnvironment(CurrentLighting.EnvironmentState, Command, environment.Texture);
			const auto &authored = CurrentLighting.EnvironmentState;
			const bool volume = WeatherVolumeAvailable();
			const auto generation = std::find_if(
				State->WeatherGenerations.begin(),
				State->WeatherGenerations.end(),
				[this](const auto &entry) {
					return entry.World == Request.World && entry.Pipeline == Pipeline->Name;
				}
			);
			const bool products =
				generation != State->WeatherGenerations.end() && WeatherProductsReady(&*generation, Command);
			const bool legacyClouds =
				!volume &&
				std::any_of(State->Environments.begin(), State->Environments.end(), [&](const auto &entry) {
					return entry.CloudTarget == environment.Texture && entry.Cloud.SignatureFor(Command) != 0;
				});
			const bool cloudFallback =
				VisibleWeatherMedium(authored) && !(volume && products) && !legacyClouds;
			const auto modes = EnvironmentModesOf(authored);
			const bool skyFallback =
				!environment.IsValid() && (modes.Skybox != 0 || modes.Atmosphere != 0 || cloudFallback);
			Activity.Sky = (environment.IsValid() && !blackSky) || skyFallback || cloudFallback;
			Uniforms.Fog.w = Activity.Sky ? 1.0f : 0.0f;
			if (!Activity.Sky) return PublishIdentity(context, context.Reads[0], context.Writes[0]);
			const auto target = GraphTexture(context.Writes[0], context, true);
			if (!source.IsValid() || !target.IsValid() || source.Texture == target.Texture) return false;
			if (skyFallback || cloudFallback) {
				if (RecordWeatherFallback(
						context, source, depth, target, environment, true, cloudFallback, false
					)) {
					WeatherFallbackApplied = cloudFallback;
					return true;
				}
				return PublishIdentity(context, context.Reads[0], context.Writes[0]);
			}
			const std::array bindings{
				SDL_GPUTextureSamplerBinding{source.Texture, Sampler},
				SDL_GPUTextureSamplerBinding{DepthTarget.texture, Sampler},
				SDL_GPUTextureSamplerBinding{environment.Texture, Sampler},
			};
			Fullscreen(
				context.Name,
				State->SkyPipeline,
				target.Texture,
				target.Width,
				target.Height,
				bindings,
				&Uniforms,
				nullptr,
				SDL_FColor{}
			);
			StageHistoryWrites(context, Command);
			return true;
		});

		frameNodes.Set(core::Name("fog"), [this](const graph::RunContext &context) {
			if (context.Reads.empty() || context.Writes.size() != 1) return false;
			Activity.Fog = Uniforms.VolumeCount.x >= 0.5f || Uniforms.CloudCentreEnabled.w >= 0.5f;
			if (!Activity.Fog) return PublishIdentity(context, context.Reads[0], context.Writes[0]);
			const auto source = GraphTexture(context.Reads[0], context, false);
			const auto target = GraphTexture(context.Writes[0], context, true);
			if (!source.IsValid() || !target.IsValid() || source.Texture == target.Texture ||
				!UploadCloudDensity())
				return false;
			const std::array bindings{
				SDL_GPUTextureSamplerBinding{source.Texture, Sampler},
				SDL_GPUTextureSamplerBinding{DepthTarget.texture, Sampler},
			};
			Fullscreen(
				context.Name,
				State->VolumePipeline,
				target.Texture,
				target.Width,
				target.Height,
				bindings,
				&Uniforms,
				nullptr,
				SDL_FColor{},
				nullptr,
				0,
				nullptr,
				State->CloudDensityBuffer
			);
			StageHistoryWrites(context, Command);
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
			struct LensPass {
				const ViewRecording::LensGroup *Group = nullptr;
				SDL_GPUGraphicsPipeline *Pipeline = nullptr;
			};
			std::array<LensPass, ViewRecording::MAX_LENS_SHADER_RUNS> passes{};
			if (recording.LensGroupCount > passes.size()) return false;
			size_t passCount = 0;

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
				passes[passCount++] = LensPass{&group, pipeline};
			}

			recording.Activity.ShaderLenses = passCount != 0;
			const auto sourceId = detail::IdentityPort(*node, false);
			const auto targetId = detail::IdentityPort(*node, true);
			if (passCount == 0) return recording.PublishIdentity(context, sourceId, targetId);
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
			// Select the first target so the last effective pass always publishes
			// directly to output.
			SDL_GPUTexture *target = passCount % 2 == 0 ? scratch.Texture : output.Texture;
			for (size_t index = 0; index < passCount; ++index) {
				const LensPass &pass = passes[index];
				const ViewRecording::LensGroup &group = *pass.Group;
				LensPassUniforms &uniforms = recording.LensPassData;
				std::copy_n(recording.LensEntries.begin() + group.First, group.Count, uniforms.Lenses);
				uniforms.TimeCount.y = static_cast<float>(group.Count);
				const std::array bindings{
					SDL_GPUTextureSamplerBinding{source, recording.Sampler},
					SDL_GPUTextureSamplerBinding{depth.Texture, recording.Sampler},
				};
				recording.Fullscreen(
					context.Name,
					pass.Pipeline,
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

			auto history = context;
			if (passCount == 1) history.Writes = std::span<const graph::ResourceId>(&targetId, 1);
			recording.StageHistoryWrites(history, recording.Command);
			return true;
		});

		frameNodes.Set(core::Name("bloom"), [this](const graph::RunContext &context) {
			ViewRecording &recording = *this;
			Impl *const State = recording.State;
			if (context.Reads.size() != 1 || context.Writes.size() != 1) return false;
			const Impl::NamedTexture source = recording.GraphTexture(context.Reads.front(), context, false);
			const Impl::NamedTexture target = recording.GraphTexture(context.Writes.front(), context, true);
			if (!source.IsValid() || !target.IsValid() ||
				source.Format != SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT ||
				target.Format != SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT || source.Width != target.Width ||
				source.Height != target.Height)
				return false;

			if (State->BloomIntensity <= 0.0f || State->BloomRadius <= 0.0f)
				return recording.ClearImage(context.Name, target, SDL_FColor{});
			if (!State->EnsureLightingEffectPipeline(State->BloomPipeline, "bloom.frag", 1)) return false;

			const BloomUniforms uniforms{
				.Settings = glm::vec4{State->BloomIntensity, State->BloomThreshold, State->BloomRadius, 0.0f},
				.Target = glm::vec4{
					static_cast<float>(target.Width),
					static_cast<float>(target.Height),
					1.0f / static_cast<float>(target.Width),
					1.0f / static_cast<float>(target.Height),
				},
			};
			const std::array bindings{SDL_GPUTextureSamplerBinding{source.Texture, recording.Sampler}};
			recording.Fullscreen(
				context.Name,
				State->BloomPipeline,
				target.Texture,
				target.Width,
				target.Height,
				bindings,
				nullptr,
				nullptr,
				SDL_FColor{},
				&uniforms,
				sizeof(uniforms)
			);
			return true;
		});

		frameNodes.Set(core::Name("dof"), [this](const graph::RunContext &context) {
			ViewRecording &recording = *this;
			Impl *const State = recording.State;
			if (context.Reads.size() != 2 || context.Writes.size() != 1) return false;
			recording.Activity.DepthOfField =
				State->DepthOfFieldIntensity > 0.0f && State->DepthOfFieldRadius > 0.0f;
			if (!recording.Activity.DepthOfField)
				return recording.PublishIdentity(context, context.Reads[0], context.Writes[0]);
			if (recording.Sampler == nullptr && !State->EnsureSurfaceSampler()) return false;
			recording.Sampler = State->SurfaceSampler;
			const Impl::NamedTexture colour = recording.GraphTexture(context.Reads[0], context, false);
			const Impl::NamedTexture depth = recording.GraphTexture(context.Reads[1], context, false);
			const Impl::NamedTexture target = recording.GraphTexture(context.Writes.front(), context, true);
			if (!colour.IsValid() || !depth.IsValid() || !target.IsValid() ||
				colour.Format != SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT ||
				depth.Format != SDL_GPU_TEXTUREFORMAT_R32_FLOAT ||
				target.Format != SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT || colour.Width != target.Width ||
				colour.Height != target.Height || depth.Width != target.Width ||
				depth.Height != target.Height)
				return false;
			if (!State->EnsureLightingEffectPipeline(State->DepthOfFieldPipeline, "dof.frag", 2))
				return false;
			const LightingEffectsUniforms uniforms{
				.DepthOfField =
					glm::vec4{
						State->DepthOfFieldIntensity,
						State->DepthOfFieldFocusDistance,
						State->DepthOfFieldFocusRange,
						State->DepthOfFieldRadius,
					},
				.Target = glm::vec4{
					0.0f,
					0.0f,
					1.0f / static_cast<float>(target.Width),
					1.0f / static_cast<float>(target.Height),
				},
			};
			const std::array bindings{
				SDL_GPUTextureSamplerBinding{colour.Texture, recording.Sampler},
				SDL_GPUTextureSamplerBinding{depth.Texture, recording.Sampler},
			};
			recording.Fullscreen(
				context.Name,
				State->DepthOfFieldPipeline,
				target.Texture,
				target.Width,
				target.Height,
				bindings,
				nullptr,
				nullptr,
				SDL_FColor{},
				&uniforms,
				sizeof(uniforms)
			);
			recording.StageHistoryWrites(context, recording.Command);
			return true;
		});

		frameNodes.Set(core::Name("god-rays"), [this](const graph::RunContext &context) {
			ViewRecording &recording = *this;
			Impl *const State = recording.State;
			if (context.Reads.size() != 2 || context.Writes.size() != 1) return false;
			if (!recording.Activity.GodRays)
				return recording.PublishIdentity(context, context.Reads[0], context.Writes[0]);
			if (recording.Sampler == nullptr && !State->EnsureSurfaceSampler()) return false;
			recording.Sampler = State->SurfaceSampler;
			const Impl::NamedTexture source = recording.GraphTexture(context.Reads[0], context, false);
			const Impl::NamedTexture depth = recording.GraphTexture(context.Reads[1], context, false);
			const Impl::NamedTexture target = recording.GraphTexture(context.Writes.front(), context, true);
			if (!source.IsValid() || !depth.IsValid() || !target.IsValid() ||
				source.Format != SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT ||
				depth.Format != SDL_GPU_TEXTUREFORMAT_R32_FLOAT ||
				target.Format != SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT || source.Width != target.Width ||
				source.Height != target.Height || depth.Width != target.Width ||
				depth.Height != target.Height)
				return false;

			if (!State->EnsureLightingEffectPipeline(State->GodRaysPipeline, "god-rays.frag", 2))
				return false;
			const auto clip = recording.GodRayClip;
			const float reciprocalW = 1.0f / clip.w;
			const LightingEffectsUniforms uniforms{
				// Standard linear-depth clears to FarPlane. Keep that value with the
				// effect so the shader can distinguish sky from an opaque sample.
				.DepthOfField = glm::vec4{recording.DrawCamera.FarPlane},
				.GodRays =
					glm::vec4{
						State->GodRayIntensity,
						State->GodRayThreshold,
						State->GodRayRadius,
						1.0f,
					},
				.Target = glm::vec4{
					clip.x * reciprocalW * 0.5f + 0.5f,
					0.5f - clip.y * reciprocalW * 0.5f,
					1.0f / static_cast<float>(target.Width),
					1.0f / static_cast<float>(target.Height),
				},
			};
			const std::array bindings{
				SDL_GPUTextureSamplerBinding{source.Texture, recording.Sampler},
				SDL_GPUTextureSamplerBinding{depth.Texture, recording.Sampler},
			};
			recording.Fullscreen(
				context.Name,
				State->GodRaysPipeline,
				target.Texture,
				target.Width,
				target.Height,
				bindings,
				nullptr,
				nullptr,
				SDL_FColor{},
				&uniforms,
				sizeof(uniforms)
			);
			recording.StageHistoryWrites(context, recording.Command);
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
			if (!target.IsValid()) return false;
			// Portal previews retain their plain tonemap; this grade belongs to the view.
			const auto postprocess = State->PostProcessPipelines.find(Request.Source->ContentOwner.Id());
			if (postprocess != State->PostProcessPipelines.end()) {
				const std::array bindings{SDL_GPUTextureSamplerBinding{source.Texture, recording.Sampler}};
				recording.Fullscreen(
					context.Name,
					postprocess->second.Pipeline,
					target.Texture,
					target.Width,
					target.Height,
					bindings,
					nullptr,
					nullptr,
					colourTarget.clear_color
				);
			} else {
				const Impl::NamedTexture bloom =
					context.Reads.size() > 1 ? recording.GraphTexture(context.Reads[1], context, false)
											 : Impl::NamedTexture{};
				const bool useBloom =
					bloom.IsValid() && bloom.Width == source.Width && bloom.Height == source.Height;
				const BloomUniforms uniforms{
					.Settings =
						glm::vec4{
							useBloom ? State->BloomIntensity : 0.0f,
							State->BloomThreshold,
							State->BloomRadius,
							0.0f,
						},
					.Target = glm::vec4{
						static_cast<float>(target.Width),
						static_cast<float>(target.Height),
						1.0f / static_cast<float>(target.Width),
						1.0f / static_cast<float>(target.Height),
					},
				};
				const std::array bindings{
					SDL_GPUTextureSamplerBinding{source.Texture, recording.Sampler},
					SDL_GPUTextureSamplerBinding{
						useBloom ? bloom.Texture : source.Texture, recording.Sampler
					},
				};
				recording.Fullscreen(
					context.Name,
					State->TonemapPipeline,
					target.Texture,
					target.Width,
					target.Height,
					bindings,
					nullptr,
					nullptr,
					colourTarget.clear_color,
					&uniforms,
					sizeof(uniforms)
				);
			}

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
