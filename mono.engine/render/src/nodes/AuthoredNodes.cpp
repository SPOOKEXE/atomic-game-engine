// The two nodes a pipeline document can author a shader into.
//
// **Everything else in `src/nodes/` is a pass this module wrote; these two are
// passes a `.pipeline` document wrote.** The shader comes from
// `ShaderLibrary::Resolve`, the pipeline from `Impl::GraphRasterFor` or
// `Impl::GraphComputeFor`, and what the node reads and writes is whatever the
// graph wired into it - so neither of them names a texture of this module's.

#include "ViewRecording.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/graph/ExecutionPlan.hpp>
#include <engine/graph/Schedule.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace engine::render {

	void ViewRecording::RegisterAuthoredNodes(NodeTable &frameNodes) {
		frameNodes.Set(core::Name("raster"), [this](const graph::RunContext &context) {
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
			Impl::NamedTexture target;
			for (const graph::ResourceId resource : context.Writes) {
				const graph::ResourceDesc *desc = selectedPipeline->Graph.FindResource(resource);
				if (desc != nullptr && desc->Kind == graph::ResourceKind::Colour) {
					target = graphTexture(resource, context, true);
					break;
				}
			}
			if (!target.IsValid()) {
				ENGINE_WARN("'{}' has no colour target to draw into", context.Name.Text());
				return true;
			}
			const std::vector<SDL_GPUTextureSamplerBinding> bindings = textureBindings(context);
			SDL_GPUGraphicsPipeline *raster =
				State->GraphRasterFor(*selectedPipeline, *node, target.Format, bindings.size());
			if (raster == nullptr) {
				return true;
			}

			GraphPassUniforms passUniforms;
			passUniforms.ViewProjection = matrices.ViewProjection;
			passUniforms.InverseViewProjection = glm::inverse(matrices.ViewProjection);
			passUniforms.Target = glm::vec4{
				static_cast<float>(target.Width),
				static_cast<float>(target.Height),
				1.0f / static_cast<float>(target.Width),
				1.0f / static_cast<float>(target.Height),
			};
			passUniforms.View = glm::vec4{
				static_cast<float>(State->AnimationSeconds),
				drawCamera.FieldOfViewRadians,
				static_cast<float>(target.Width) / static_cast<float>(target.Height),
				0.0f,
			};

			SDL_GPUColorTargetInfo colour{};
			colour.texture = target.Texture;
			colour.clear_color = SDL_FColor{
				node->Number(core::Name("clear.r"), 0.0f),
				node->Number(core::Name("clear.g"), 0.0f),
				node->Number(core::Name("clear.b"), 0.0f),
				node->Number(core::Name("clear.a"), 1.0f),
			};
			const std::string *load = node->Parameter(core::Name("load"));
			colour.load_op = load != nullptr && *load == "load" ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
			colour.store_op = SDL_GPU_STOREOP_STORE;
			colour.cycle = true;
			SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &colour, 1, nullptr);
			SDL_BindGPUGraphicsPipeline(pass, raster);
			if (!bindings.empty()) {
				SDL_BindGPUFragmentSamplers(pass, 0, bindings.data(), static_cast<uint32_t>(bindings.size()));
			}
			SDL_PushGPUFragmentUniformData(command, 0, &passUniforms, sizeof(passUniforms));
			SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
			SDL_EndGPURenderPass(pass);
			result.DrawCalls++;
			return true;
		});

		frameNodes.Set(core::Name("dispatch"), [this](const graph::RunContext &context) {
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
			const uint32_t localX = node->Integer(core::Name("local.x"), 8);
			const uint32_t localY = node->Integer(core::Name("local.y"), 8);
			const uint32_t localZ = node->Integer(core::Name("local.z"), 1);
			if (localX == 0 || localY == 0 || localZ == 0) {
				ENGINE_WARN("'{}' asks for a zero-sized compute thread group", context.Name.Text());
				return true;
			}
			SDL_GPUComputePipeline *compute = State->GraphComputeFor(
				*selectedPipeline, *node, bindings.size(), writes.size(), localX, localY, localZ
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
			const std::string *mode = node->Parameter(core::Name("dispatch.mode"));
			const bool coverTarget = mode == nullptr || *mode != "groups";
			const uint32_t groupsX = coverTarget ? (firstTarget.Width + localX - 1) / localX
												 : node->Integer(core::Name("dispatch.x"), 1);
			const uint32_t groupsY = coverTarget ? (firstTarget.Height + localY - 1) / localY
												 : node->Integer(core::Name("dispatch.y"), 1);
			const uint32_t groupsZ = coverTarget ? 1 : node->Integer(core::Name("dispatch.z"), 1);
			SDL_DispatchGPUCompute(pass, groupsX, groupsY, groupsZ);
			SDL_EndGPUComputePass(pass);
			result.ComputeDispatches++;
			if (separateCommand) {
				closePass();
				if (!SDL_SubmitGPUCommandBuffer(dispatchCommand)) {
					ENGINE_ERROR("'{}': SDL_SubmitGPUCommandBuffer: {}", context.Name.Text(), SDL_GetError());
					State->Timestamps.Abandon(timingSlot);
					if (timingSlot < VulkanTimestamps::SLOTS) {
						State->PendingMarks[timingSlot].clear();
					}
					return false;
				}
				dedicatedComputeSubmitted = true;
				result.AsyncComputeCommandBuffers++;
			} else {
				mainGpuWorkRecorded = true;
			}
			return true;
		});
	}
}
