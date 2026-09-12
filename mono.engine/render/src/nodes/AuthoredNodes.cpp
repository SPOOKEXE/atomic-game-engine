// The two nodes a pipeline document can author a shader into.
//
// **Everything else in `src/nodes/` is a pass this module wrote; these two are
// passes a `.pipeline` document wrote.** The shader comes from
// `ShaderLibrary::Resolve`, the pipeline from `Impl::GraphRasterFor` or
// `Impl::GraphComputeFor`, and what the node reads and writes is whatever the
// graph wired into it - so neither of them names a texture of this module's.

#include "Compositor.hpp"
#include "GraphHistory.hpp"
#include "HardRender.hpp"
#include "ViewRecording.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/graph/ExecutionPlan.hpp>
#include <engine/graph/Schedule.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace engine::render {
	namespace {
		bool AttachmentDemanded(
			std::span<const scene::DrawInstance> instances,
			core::Name node,
			scene::RenderEffectStage stage,
			const scene::WorldLighting &lighting,
			const scene::Camera &camera,
			uint32_t supported
		) {
			const uint32_t required = scene::FeatureBit(
				stage == scene::RenderEffectStage::Compute ? scene::RenderFeature::ComputeEffects
														   : scene::RenderFeature::PostProcessing
			);
			for (const scene::DrawInstance &instance : instances) {
				if ((scene::ResolveRenderFeatures(
						 scene::ALL_RENDER_FEATURES,
						 lighting.RenderFeatures,
						 camera.RenderFeatures,
						 instance.RenderFeatures,
						 supported
					 )
						 .Enabled &
					 required) == 0) {
					continue;
				}
				const size_t count =
					std::min<size_t>(instance.Effects.Count, scene::MAX_RENDER_EFFECT_ATTACHMENTS);
				for (size_t index = 0; index < count; ++index) {
					const scene::RenderEffectAttachment &attachment = instance.Effects.Attachments[index];
					const bool selected = attachment.SelectionMask == UINT32_MAX ||
										  (attachment.SelectionMask & instance.TagMask) != 0;
					if (attachment.Enabled && selected && attachment.Node == node &&
						attachment.Stage == stage) {
						return true;
					}
				}
			}
			return false;
		}

		GraphPassUniforms AuthoredPassUniforms(
			const DeviceCaps &caps,
			double animationSeconds,
			const scene::CameraMatrices &matrices,
			const core::CFrame &cameraFrame,
			const scene::Camera &camera,
			const scene::WorldLighting &lighting,
			uint32_t width,
			uint32_t height,
			uint32_t instanceCount,
			const graph::Node &node
		) {
			GraphPassUniforms uniforms;
			uniforms.ViewProjection = matrices.ViewProjection;
			uniforms.InverseViewProjection = glm::inverse(matrices.ViewProjection);
			uniforms.Target = glm::vec4{
				static_cast<float>(width),
				static_cast<float>(height),
				1.0f / static_cast<float>(width),
				1.0f / static_cast<float>(height),
			};
			uniforms.View = glm::vec4{
				static_cast<float>(animationSeconds),
				camera.FieldOfViewRadians,
				static_cast<float>(width) / static_cast<float>(height),
				static_cast<float>(instanceCount),
			};
			uniforms.Eye =
				glm::vec4{cameraFrame.Position.X, cameraFrame.Position.Y, cameraFrame.Position.Z, 1.0f};
			const core::Vector3 forward = cameraFrame.LookVector();
			uniforms.CameraDepth =
				glm::vec4{forward.X, forward.Y, forward.Z, -forward.Dot(cameraFrame.Position)};
			uniforms.RenderFeatures = glm::uvec4{
				SupportedRenderFeatures(caps),
				scene::ApplyRenderFeaturePolicy(scene::ALL_RENDER_FEATURES, lighting.RenderFeatures),
				camera.RenderFeatures.Enable & scene::ALL_RENDER_FEATURES,
				camera.RenderFeatures.Disable & scene::ALL_RENDER_FEATURES,
			};
			const CompositorParameters compositor = CompositorParametersFor(node);
			std::copy(compositor.begin(), compositor.end(), uniforms.Parameters);
			return uniforms;
		}
	}

	void ViewRecording::RegisterAuthoredNodes(NodeTable &frameNodes) {
		const NodeHandler rasterHandler = [this](const graph::RunContext &context) {
			ViewRecording &recording = *this;
			Impl *const State = recording.State;
			FrameResult &result = recording.Result;
			const Impl::NamedPipeline *const selectedPipeline = recording.Pipeline;
			SDL_GPUCommandBuffer *const command = recording.Command;
			const scene::Camera &drawCamera = recording.DrawCamera;
			const scene::CameraMatrices &matrices = recording.Matrices;
			const auto enterNamedPass = [&recording](
											core::Name name, SDL_GPUCommandBuffer *recordedCommand = nullptr
										) { recording.EnterNamedPass(name, recordedCommand); };
			const auto graphTexture =
				[&recording](graph::ResourceId resource, const graph::RunContext &runContext, bool make) {
					return recording.GraphTexture(resource, runContext, make);
				};
			const auto textureBindings = [&recording](const graph::RunContext &runContext) {
				return recording.TextureBindings(runContext);
			};

			enterNamedPass(context.Name);
			const graph::Node *node = selectedPipeline->Graph.Find(context.Node);
			if (node == nullptr) {
				return false;
			}
			const std::string *attachment = node->Parameter(core::Name("attachment"));
			const bool attachmentNode = attachment != nullptr && *attachment == "visual";
			const uint32_t supported = SupportedRenderFeatures(State->Caps);
			const bool demanded = !attachmentNode ||
								  AttachmentDemanded(
									  recording.Instances,
									  node->Name,
									  scene::RenderEffectStage::PostProcess,
									  recording.CurrentLighting,
									  recording.DrawCamera,
									  supported
								  ) ||
								  AttachmentDemanded(
									  recording.Foreign,
									  node->Name,
									  scene::RenderEffectStage::PostProcess,
									  recording.CurrentLighting,
									  recording.DrawCamera,
									  supported
								  );
			if (!demanded) {
				if (context.Reads.size() != 1 || context.Writes.size() != 1) {
					ENGINE_WARN(
						"inactive visual attachment '{}' cannot pass through its graph shape",
						context.Name.Text()
					);
					return false;
				}
				const Impl::NamedTexture source = graphTexture(context.Reads.front(), context, false);
				const Impl::NamedTexture target = graphTexture(context.Writes.front(), context, true);
				if (!source.IsValid() || !target.IsValid()) {
					return false;
				}
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
				SDL_BlitGPUTexture(command, &blit);
				return true;
			}
			std::vector<Impl::NamedTexture> targets;
			std::vector<SDL_GPUTextureFormat> formats;
			for (const graph::ResourceId resource : context.Writes) {
				const graph::ResourceDesc *desc = selectedPipeline->Graph.FindResource(resource);
				if (desc != nullptr && desc->Kind == graph::ResourceKind::Colour) {
					Impl::NamedTexture target = graphTexture(resource, context, true);
					if (target.IsValid()) {
						targets.push_back(target);
						formats.push_back(target.Format);
					}
				}
			}
			if (targets.empty()) {
				ENGINE_WARN("'{}' has no colour target to draw into", context.Name.Text());
				return true;
			}
			for (const Impl::NamedTexture &target : targets) {
				if (target.Width != targets.front().Width || target.Height != targets.front().Height) {
					ENGINE_WARN("'{}' writes colour targets with different extents", context.Name.Text());
					return true;
				}
			}
			const std::vector<SDL_GPUTextureSamplerBinding> bindings = textureBindings(context);
			SDL_GPUGraphicsPipeline *raster =
				State->GraphRasterFor(*selectedPipeline, *node, formats, bindings.size());
			if (raster == nullptr) {
				return true;
			}

			const GraphPassUniforms passUniforms = AuthoredPassUniforms(
				State->Caps,
				State->AnimationSeconds,
				matrices,
				recording.Request.CameraFrame,
				drawCamera,
				recording.CurrentLighting,
				targets.front().Width,
				targets.front().Height,
				recording.InstanceCount,
				*node
			);

			std::vector<SDL_GPUColorTargetInfo> colours(targets.size());
			const std::string *load = node->Parameter(core::Name("load"));
			for (size_t index = 0; index < targets.size(); ++index) {
				SDL_GPUColorTargetInfo &colour = colours[index];
				colour.texture = targets[index].Texture;
				colour.clear_color = SDL_FColor{
					node->Number(core::Name("clear.r"), 0.0f),
					node->Number(core::Name("clear.g"), 0.0f),
					node->Number(core::Name("clear.b"), 0.0f),
					node->Number(core::Name("clear.a"), 1.0f),
				};
				colour.load_op =
					load != nullptr && *load == "load" ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
				colour.store_op = SDL_GPU_STOREOP_STORE;
				colour.cycle = true;
			}
			SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(
				command, colours.data(), static_cast<uint32_t>(colours.size()), nullptr
			);
			SDL_BindGPUGraphicsPipeline(pass, raster);
			if (!bindings.empty()) {
				SDL_BindGPUFragmentSamplers(pass, 0, bindings.data(), static_cast<uint32_t>(bindings.size()));
			}
			SDL_PushGPUFragmentUniformData(command, 0, &passUniforms, sizeof(passUniforms));
			SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
			SDL_EndGPURenderPass(pass);
			result.DrawCalls++;
			return true;
		};
		frameNodes.Set(core::Name("raster"), rasterHandler);
		for (const char *kind : {
				 "fxaa",
				 "taa",
				 "smaa-edges",
				 "smaa-blend",
				 "smaa-resolve",
				 "exposure-grade",
				 "hsv",
				 "mix",
				 "transform-crop",
				 "blur",
			 }) {
			frameNodes.Set(core::Name(kind), rasterHandler);
		}

		const NodeHandler dispatchHandler = [this](const graph::RunContext &context) {
			ViewRecording &recording = *this;
			Impl *const State = recording.State;
			FrameResult &result = recording.Result;
			const Impl::NamedPipeline *const selectedPipeline = recording.Pipeline;
			SDL_GPUCommandBuffer *const command = recording.Command;
			uint32_t &timingSlot = recording.TimingSlot;
			bool &mainGpuWorkRecorded = recording.MainGpuWorkRecorded;
			bool &dedicatedComputeSubmitted = recording.DedicatedComputeSubmitted;
			const auto scheduledFor = [&recording](graph::NodeId id) { return recording.ScheduledFor(id); };
			const auto closePass = [&recording] { recording.ClosePass(); };
			const auto enterNamedPass = [&recording](
											core::Name name, SDL_GPUCommandBuffer *recordedCommand = nullptr
										) { recording.EnterNamedPass(name, recordedCommand); };
			const auto graphTexture =
				[&recording](graph::ResourceId resource, const graph::RunContext &runContext, bool make) {
					return recording.GraphTexture(resource, runContext, make);
				};
			const auto textureBindings = [&recording](const graph::RunContext &runContext) {
				return recording.TextureBindings(runContext);
			};

			const graph::Node *node = selectedPipeline->Graph.Find(context.Node);
			if (node == nullptr) {
				return false;
			}
			graph::Node inactiveNode;
			const std::string *attachment = node->Parameter(core::Name("attachment"));
			const bool attachmentNode = attachment != nullptr && *attachment == "visual";
			const uint32_t supported = SupportedRenderFeatures(State->Caps);
			const bool demanded = !attachmentNode ||
								  AttachmentDemanded(
									  recording.Instances,
									  node->Name,
									  scene::RenderEffectStage::Compute,
									  recording.CurrentLighting,
									  recording.DrawCamera,
									  supported
								  ) ||
								  AttachmentDemanded(
									  recording.Foreign,
									  node->Name,
									  scene::RenderEffectStage::Compute,
									  recording.CurrentLighting,
									  recording.DrawCamera,
									  supported
								  );
			if (!demanded) {
				if (context.Reads.size() != 1 || context.Writes.size() != 1) {
					ENGINE_WARN(
						"inactive visual attachment '{}' cannot pass through its graph shape",
						context.Name.Text()
					);
					return false;
				}
				inactiveNode = *node;
				inactiveNode.Name = core::Name(std::string(node->Name.Text()) + ".inactive");
				std::erase_if(inactiveNode.Parameters, [](const graph::NodeParameter &parameter) {
					return parameter.Key == core::Name("shader") || parameter.Key == core::Name("source");
				});
				inactiveNode.Parameters.push_back({core::Name("shader"), "attachment-copy.comp"});
				node = &inactiveNode;
			}
			std::vector<SDL_GPUStorageTextureReadWriteBinding> writes;
			Impl::NamedTexture firstTarget;
			for (const graph::ResourceId resource : context.Writes) {
				const graph::ResourceDesc *desc = selectedPipeline->Graph.FindResource(resource);
				if (desc == nullptr || desc->Kind != graph::ResourceKind::Storage) {
					continue;
				}
				Impl::NamedTexture target = graphTexture(resource, context, true);
				if (!target.IsValid()) {
					continue;
				}
				if (!firstTarget.IsValid()) {
					firstTarget = target;
				}
				SDL_GPUStorageTextureReadWriteBinding binding{};
				binding.texture = target.Texture;
				binding.cycle = true;
				writes.push_back(binding);
			}
			if (writes.empty()) {
				ENGINE_WARN("'{}' has no storage target to dispatch into", context.Name.Text());
				return true;
			}
			const std::vector<SDL_GPUTextureSamplerBinding> bindings = textureBindings(context);
			const uint32_t localX = demanded ? node->Integer(core::Name("local.x"), 8) : 8;
			const uint32_t localY = demanded ? node->Integer(core::Name("local.y"), 8) : 8;
			const uint32_t localZ = demanded ? node->Integer(core::Name("local.z"), 1) : 1;
			const std::string *instances = node->Parameter(core::Name("instances"));
			const bool readInstances = demanded && instances != nullptr && *instances == "resident";
			const bool traceUniforms = IsTraceNode(node->Kind);
			const std::string *uniforms = node->Parameter(core::Name("uniforms"));
			// Slot one is the trace parameter block, so slot zero must exist.
			const bool readViewUniforms =
				traceUniforms || !demanded || (uniforms != nullptr && *uniforms == "view");
			if (localX == 0 || localY == 0 || localZ == 0) {
				ENGINE_WARN("'{}' asks for a zero-sized compute thread group", context.Name.Text());
				return true;
			}
			if (readInstances && State->InstanceBuffer == nullptr) {
				ENGINE_WARN("'{}' asks for resident instances before any are available", context.Name.Text());
				return true;
			}
			SDL_GPUComputePipeline *compute = State->GraphComputeFor(
				*selectedPipeline,
				*node,
				bindings.size(),
				writes.size(),
				readInstances ? 1u : 0u,
				(readViewUniforms ? 1u : 0u) + (traceUniforms ? 1u : 0u),
				localX,
				localY,
				localZ
			);
			if (compute == nullptr) {
				return true;
			}

			const graph::ScheduledNode *scheduled = scheduledFor(context.Node);

			// The traffic plan decides which command buffer this dispatch
			// belongs to. A compute buffer ahead of the plan's first graphics
			// buffer may submit on its own before the main stream; the runtime
			// guards below keep that promise when a batch or execution order
			// has already put work in the main buffer.
			//
			// Dependency-bound compute - a compute buffer the plan places
			// between graphics buffers - stays in the main stream on SDL: one
			// unified queue offers no overlap to win, the present is bound to
			// the buffer that acquired the swapchain so the graphics stream
			// cannot be cut around it, and a pass recorded for later submission
			// could read textures a later main-stream pass cycles. The plan
			// still carries the boundary, so a backend with an independent
			// compute queue can lift it without re-planning.
			const auto planLeadsGraphics = [&](graph::NodeId node) {
				for (const graph::PlannedCommandBuffer &buffer : selectedPipeline->Buffers) {
					if (buffer.Class == graph::CommandBufferClass::Graphics) {
						return false;
					}
					if (buffer.Class == graph::CommandBufferClass::Compute &&
						std::find(buffer.Nodes.begin(), buffer.Nodes.end(), node) != buffer.Nodes.end()) {
						return true;
					}
				}
				return false;
			};
			const bool separateCommand = scheduled != nullptr && scheduled->AsyncEligible &&
										 planLeadsGraphics(context.Node) && !mainGpuWorkRecorded &&
										 !dedicatedComputeSubmitted &&
										 (!State->BatchActive || State->BatchFirst);
			SDL_GPUCommandBuffer *dispatchCommand = command;
			if (separateCommand) {
				dispatchCommand = SDL_AcquireGPUCommandBuffer(State->Device);
				if (dispatchCommand == nullptr) {
					ENGINE_ERROR(
						"'{}': SDL_AcquireGPUCommandBuffer: {}", context.Name.Text(), SDL_GetError()
					);
					return false;
				}

				// `Begin` put the query reset in the main command buffer, which is
				// submitted after this prefix. Use another slot whose reset and marks
				// travel together on the command buffer that reaches the queue first.
				const uint32_t laterReset = timingSlot;
				State->Timestamps.Abandon(laterReset);
				if (laterReset < VulkanTimestamps::SLOTS) {
					State->PendingMarks[laterReset].clear();
					State->TimingSequence[laterReset] = 0;
				}
				timingSlot = State->Timestamps.Begin(dispatchCommand, laterReset);
				if (State->BatchActive) {
					State->BatchTimingSlot = timingSlot;
				}
			}
			enterNamedPass(context.Name, dispatchCommand);
			SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(
				dispatchCommand, writes.data(), static_cast<uint32_t>(writes.size()), nullptr, 0
			);
			if (pass == nullptr) {
				ENGINE_ERROR("'{}': SDL_BeginGPUComputePass: {}", context.Name.Text(), SDL_GetError());
				closePass();
				if (separateCommand) {
					State->Timestamps.Abandon(timingSlot);
					if (timingSlot < VulkanTimestamps::SLOTS) {
						State->PendingMarks[timingSlot].clear();
					}
					timingSlot = VulkanTimestamps::NO_SLOT;
					if (State->BatchActive) {
						State->BatchTimingSlot = timingSlot;
					}
					SDL_CancelGPUCommandBuffer(dispatchCommand);
				}
				return false;
			}
			SDL_BindGPUComputePipeline(pass, compute);
			if (!bindings.empty()) {
				SDL_BindGPUComputeSamplers(pass, 0, bindings.data(), static_cast<uint32_t>(bindings.size()));
			}
			if (readInstances && State->InstanceBuffer != nullptr) {
				SDL_GPUBuffer *const instanceBuffer = State->InstanceBuffer;
				SDL_BindGPUComputeStorageBuffers(pass, 0, &instanceBuffer, 1);
			}
			if (readViewUniforms) {
				const GraphPassUniforms passUniforms = AuthoredPassUniforms(
					State->Caps,
					State->AnimationSeconds,
					recording.Matrices,
					recording.Request.CameraFrame,
					recording.DrawCamera,
					recording.CurrentLighting,
					firstTarget.Width,
					firstTarget.Height,
					recording.InstanceCount,
					*node
				);
				SDL_PushGPUComputeUniformData(dispatchCommand, 0, &passUniforms, sizeof(passUniforms));
			}
			if (traceUniforms) {
				bool historyAvailable = false;
				for (const graph::ResourceId resource : context.Reads) {
					const graph::ResourceDesc *desc = selectedPipeline->Graph.FindResource(resource);
					if (desc != nullptr && desc->Lifetime == graph::ResourceLifetime::History) {
						historyAvailable = graphTexture(resource, context, false).IsValid();
						break;
					}
				}
				const TraceOptions options = TraceOptionsFor(*node, historyAvailable);
				SDL_PushGPUComputeUniformData(dispatchCommand, 1, &options, sizeof(options));
			}
			const std::string *mode = node->Parameter(core::Name("dispatch.mode"));
			const bool coverTarget = mode == nullptr || *mode != "groups";
			const uint32_t groupsX = coverTarget ? (firstTarget.Width + localX - 1) / localX
												 : node->Integer(core::Name("dispatch.x"), 1);
			const uint32_t groupsY = coverTarget ? (firstTarget.Height + localY - 1) / localY
												 : node->Integer(core::Name("dispatch.y"), 1);
			const uint32_t groupsZ = coverTarget ? 1 : node->Integer(core::Name("dispatch.z"), 1);
			SDL_DispatchGPUCompute(pass, groupsX, groupsY, groupsZ);
			SDL_EndGPUComputePass(pass);
			recording.StageHistoryWrites(context, dispatchCommand);
			result.ComputeDispatches++;
			if (separateCommand) {
				closePass();
				if (!SDL_SubmitGPUCommandBuffer(dispatchCommand)) {
					ENGINE_ERROR("'{}': SDL_SubmitGPUCommandBuffer: {}", context.Name.Text(), SDL_GetError());
					State->Timestamps.Abandon(timingSlot);
					if (timingSlot < VulkanTimestamps::SLOTS) {
						State->PendingMarks[timingSlot].clear();
					}
					State->DiscardPendingGraphHistoryWrites(dispatchCommand);
					return false;
				}
				State->CommitPendingGraphHistoryWrites(dispatchCommand);
				dedicatedComputeSubmitted = true;
				result.AsyncComputeCommandBuffers++;
			} else {
				mainGpuWorkRecorded = true;
			}
			return true;
		};
		frameNodes.Set(core::Name("dispatch"), dispatchHandler);
		for (const char *kind : {"tessellate", "global-illumination", "raytrace", "pathtrace"}) {
			frameNodes.Set(core::Name(kind), dispatchHandler);
		}
	}
}
