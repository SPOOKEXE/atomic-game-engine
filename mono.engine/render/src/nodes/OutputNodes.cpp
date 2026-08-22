// Everything that composes or hands out the finished image.
//
// **The order in this file is the order a frame reaches them**, which is not
// the order the graph declares - `interface` draws the game's own UI into an
// image, `overlay` composes that and the debug panels onto the scene,
// `present` copies one image into another, `viewer` and `capture` take copies
// away, and `output-image` is what actually reaches the swapchain. Host chrome
// is deliberately not here: `ViewRecording::Finish` records it after this node,
// so a graph preview or an authored capture holds only the game image.

#include "ViewRecording.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>

#include <algorithm>
#include <filesystem>
#include <string>

namespace engine::render {

	void ViewRecording::RegisterOutputNodes(NodeTable &frameNodes) {
		frameNodes.Set(core::Name("interface"), [this](const graph::RunContext &context) {
			ViewRecording &recording = *this;
			FrameResult &result = recording.Result;
			SDL_GPUCommandBuffer *const command = recording.Command;
			FrameOverlayHook *const gameInterfaceHook = recording.Request.GameInterfaceHook;
			const bool drawInterface = recording.DrawInterface;
			const auto enterNamedPass = [&recording](
											core::Name name, SDL_GPUCommandBuffer *recordedCommand = nullptr
										) { recording.EnterNamedPass(name, recordedCommand); };
			const auto graphTexture =
				[&recording](graph::ResourceId resource, const graph::RunContext &runContext, bool make) {
					return recording.GraphTexture(resource, runContext, make);
				};

			enterNamedPass(context.Name);
			Impl::NamedTexture target;
			for (const graph::ResourceId resource : context.Writes) {
				target = graphTexture(resource, context, true);
				if (target.IsValid()) {
					break;
				}
			}
			if (!target.IsValid()) {
				ENGINE_WARN("'{}' has no image target", context.Name.Text());
				return true;
			}

			SDL_GPUColorTargetInfo interfaceTarget{};
			interfaceTarget.texture = target.Texture;
			interfaceTarget.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f};
			interfaceTarget.load_op = SDL_GPU_LOADOP_CLEAR;
			interfaceTarget.store_op = SDL_GPU_STOREOP_STORE;
			interfaceTarget.cycle = true;
			SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &interfaceTarget, 1, nullptr);
			if (!drawInterface) {
				SDL_EndGPURenderPass(pass);
				return true;
			}
			ENGINE_PROFILE_CAT("interface pass", core::ProfileCategory::Render);
			gameInterfaceHook->Record(command, pass);
			SDL_EndGPURenderPass(pass);
			result.DrawCalls++;
			return true;
		});

		frameNodes.Set(core::Name("overlay"), [this](const graph::RunContext &context) {
			ViewRecording &recording = *this;
			Impl *const State = recording.State;
			const bool haveOverlay = recording.HaveOverlay;
			const auto graphEnabled = [&recording](core::Name kind) { return recording.GraphEnabled(kind); };
			const auto enterNamedPass = [&recording](
											core::Name name, SDL_GPUCommandBuffer *recordedCommand = nullptr
										) { recording.EnterNamedPass(name, recordedCommand); };
			const auto recordUploads = [&recording] { return recording.RecordUploads(); };
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
			const auto drawOverlayImage =
				[&recording](SDL_GPUTexture *source, const Impl::NamedTexture &target, SDL_GPULoadOp load) {
					return recording.DrawOverlayImage(source, target, load);
				};

			enterNamedPass(context.Name);
			Impl::NamedTexture sceneImage;
			Impl::NamedTexture interfaceImage;
			Impl::NamedTexture target;
			if (!context.Reads.empty()) {
				sceneImage = graphTexture(context.Reads[0], context, false);
			}
			if (context.Reads.size() > 1) {
				interfaceImage = graphTexture(context.Reads[1], context, false);
			}
			for (const graph::ResourceId resource : context.Writes) {
				target = graphTexture(resource, context, true);
				if (target.IsValid()) {
					break;
				}
			}
			if (!drawImage(sceneImage, target, SDL_GPU_LOADOP_CLEAR)) {
				ENGINE_WARN("'{}' has no scene image to compose", context.Name.Text());
				return true;
			}

			if (haveOverlay) {
				if (!recordUploads()) {
					return false;
				}
				ENGINE_PROFILE_CAT("debug image overlay", core::ProfileCategory::Render);
				drawOverlayImage(State->OverlayTexture, target, SDL_GPU_LOADOP_LOAD);
			}
			if (graphEnabled(core::Name("interface")) && interfaceImage.IsValid()) {
				drawOverlayImage(interfaceImage.Texture, target, SDL_GPU_LOADOP_LOAD);
			}
			return true;
		});

		frameNodes.Set(core::Name("present"), [this](const graph::RunContext &context) {
			ViewRecording &recording = *this;
			const auto enterNamedPass = [&recording](
											core::Name name, SDL_GPUCommandBuffer *recordedCommand = nullptr
										) { recording.EnterNamedPass(name, recordedCommand); };
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

			enterNamedPass(context.Name);
			Impl::NamedTexture source;
			Impl::NamedTexture target;
			for (const graph::ResourceId resource : context.Reads) {
				source = graphTexture(resource, context, false);
				if (source.IsValid()) {
					break;
				}
			}
			for (const graph::ResourceId resource : context.Writes) {
				target = graphTexture(resource, context, true);
				if (target.IsValid()) {
					break;
				}
			}
			if (!drawImage(source, target, SDL_GPU_LOADOP_CLEAR)) {
				ENGINE_WARN("'{}' needs one readable image and one writable image", context.Name.Text());
			}
			return true;
		});

		frameNodes.Set(core::Name("viewer"), [this](const graph::RunContext &context) {
			ViewRecording &recording = *this;
			Impl *const State = recording.State;
			const Impl::NamedPipeline *const selectedPipeline = recording.Pipeline;
			const size_t targetSlot = recording.Request.TargetSlot;
			const auto enterNamedPass = [&recording](
											core::Name name, SDL_GPUCommandBuffer *recordedCommand = nullptr
										) { recording.EnterNamedPass(name, recordedCommand); };
			const auto graphTexture =
				[&recording](graph::ResourceId resource, const graph::RunContext &runContext, bool make) {
					return recording.GraphTexture(resource, runContext, make);
				};

			enterNamedPass(context.Name);
			for (const graph::ResourceId resource : context.Reads) {
				const graph::ResourceDesc *desc = selectedPipeline->Graph.FindResource(resource);
				const Impl::NamedTexture source = graphTexture(resource, context, false);
				if (desc == nullptr || !source.IsValid()) {
					continue;
				}
				const graph::Node *node = selectedPipeline->Graph.Find(context.Node);
				const size_t slot = node != nullptr ? node->Integer(core::Name("view"), 0) : targetSlot;
				// Onto the later-transfer buffer, per the traffic plan: this node
				// reads finished images, and the main buffer is submitted first.
				(void)State->RequestPreview(
					State->DownloadBuffer(),
					source.Texture,
					source.Width,
					source.Height,
					desc->Name,
					slot,
					source.Format
				);
				break;
			}
			return true;
		});

		frameNodes.Set(core::Name("capture"), [this](const graph::RunContext &context) {
			ViewRecording &recording = *this;
			Impl *const State = recording.State;
			const Impl::NamedPipeline *const selectedPipeline = recording.Pipeline;
			Impl::NamedTexture &authoredCapture = recording.AuthoredCapture;
			std::filesystem::path &authoredCapturePath = recording.AuthoredCapturePath;
			core::Name &authoredCaptureNode = recording.AuthoredCaptureNode;
			bool &authoredCaptureOnce = recording.AuthoredCaptureOnce;
			const auto enterNamedPass = [&recording](
											core::Name name, SDL_GPUCommandBuffer *recordedCommand = nullptr
										) { recording.EnterNamedPass(name, recordedCommand); };
			const auto graphTexture =
				[&recording](graph::ResourceId resource, const graph::RunContext &runContext, bool make) {
					return recording.GraphTexture(resource, runContext, make);
				};

			enterNamedPass(context.Name);
			const graph::Node *node = selectedPipeline->Graph.Find(context.Node);
			const std::string *path = node != nullptr ? node->Parameter(core::Name("path")) : nullptr;
			if (node == nullptr || path == nullptr || path->empty() || authoredCapture.IsValid()) {
				return true;
			}
			const std::string *mode = node->Parameter(core::Name("capture.mode"));
			authoredCaptureOnce = mode == nullptr || *mode != "every-frame";
			if (authoredCaptureOnce) {
				const auto done = std::find_if(
					State->GraphCaptureReceipts.begin(),
					State->GraphCaptureReceipts.end(),
					[&](const Impl::GraphCaptureReceipt &receipt) {
						// A file path is process-wide even when the same authored
						// pipeline is installed once per world. Claiming by path keeps
						// those instances from overwriting one another in the same frame.
						return receipt.Node == context.Name && receipt.Path == *path;
					}
				);
				if (done != State->GraphCaptureReceipts.end()) {
					return true;
				}
			} else {
				const auto claimed = State->GraphCaptureFrames.find(*path);
				if (claimed != State->GraphCaptureFrames.end() && claimed->second == State->FrameCounter) {
					return true;
				}
				State->GraphCaptureFrames[*path] = State->FrameCounter;
			}
			for (const graph::ResourceId resource : context.Reads) {
				const Impl::NamedTexture source = graphTexture(resource, context, false);
				if (!source.IsValid()) {
					continue;
				}
				if (source.Format != SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM &&
					source.Format != SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB &&
					source.Format != SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM &&
					source.Format != SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB) {
					ENGINE_WARN("capture node '{}' needs a four-byte display target", context.Name.Text());
					return true;
				}
				authoredCapture = source;
				authoredCapturePath = *path;
				authoredCaptureNode = context.Name;
				if (authoredCaptureOnce) {
					State->GraphCaptureReceipts.push_back(
						{selectedPipeline->Name, authoredCaptureNode, authoredCapturePath.string()}
					);
				}
				break;
			}
			return true;
		});

		frameNodes.Set(core::Name("output-image"), [this](const graph::RunContext &context) {
			ViewRecording &recording = *this;
			Impl *const State = recording.State;
			const Impl::NamedPipeline *const selectedPipeline = recording.Pipeline;
			const size_t targetSlot = recording.Request.TargetSlot;
			SDL_GPUTexture *const swapchain = recording.Swapchain;
			const uint32_t width = recording.Width;
			const uint32_t height = recording.Height;
			const bool offscreen = recording.Offscreen;
			const uint32_t sceneWidth = recording.SceneWidth;
			const uint32_t sceneHeight = recording.SceneHeight;
			SDL_GPUTexture *const viewTarget = recording.ViewTarget;
			SDL_GPUColorTargetInfo &windowTarget = recording.WindowTarget;
			const auto enterNamedPass = [&recording](
											core::Name name, SDL_GPUCommandBuffer *recordedCommand = nullptr
										) { recording.EnterNamedPass(name, recordedCommand); };
			const auto resourceTexture =
				[&recording](graph::ResourceId resource, size_t selectedSlot, bool make) {
					return recording.ResourceTexture(resource, selectedSlot, make);
				};
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

			enterNamedPass(context.Name);
			for (Impl::ResourcePreviewTarget &preview : State->ResourcePreviews) {
				if (!preview.Refresh || preview.Route.Pipeline != selectedPipeline->Name ||
					preview.Route.Slot != targetSlot) {
					continue;
				}
				graph::ResourceId resource;
				for (uint32_t value = 1; value <= selectedPipeline->Graph.ResourceCount(); value++) {
					const graph::ResourceId candidate{value};
					const graph::ResourceDesc *desc = selectedPipeline->Graph.FindResource(candidate);
					if (desc != nullptr && desc->Name == preview.Route.Resource) {
						resource = candidate;
						break;
					}
				}
				const Impl::NamedTexture source = resource.IsValid()
													  ? resourceTexture(resource, preview.Route.Slot, false)
													  : Impl::NamedTexture{};
				if (!source.IsValid()) {
					continue;
				}
				constexpr uint32_t PREVIEW_SIDE = 256;
				uint32_t previewWidth = source.Width;
				uint32_t previewHeight = source.Height;
				if (std::max(previewWidth, previewHeight) > PREVIEW_SIDE) {
					if (previewWidth >= previewHeight) {
						previewHeight = std::max(1u, previewHeight * PREVIEW_SIDE / previewWidth);
						previewWidth = PREVIEW_SIDE;
					} else {
						previewWidth = std::max(1u, previewWidth * PREVIEW_SIDE / previewHeight);
						previewHeight = PREVIEW_SIDE;
					}
				}
				if (preview.Width != previewWidth || preview.Height != previewHeight) {
					// The interface draw list was recorded before this graph node and
					// can still name the visible image. Retire both at the next frame
					// boundary instead of releasing either under that draw list.
					for (SDL_GPUTexture *&texture : preview.Textures) {
						if (texture != nullptr) {
							State->RetiredScenes.push_back(texture);
							texture = nullptr;
						}
					}
					preview.Slots.Reset();
					preview.Width = 0;
					preview.Height = 0;
				}

				const uint8_t writeSlot = preview.Slots.Writable();
				SDL_GPUTexture *&writeTexture = preview.Textures[writeSlot];
				if (writeTexture == nullptr) {
					SDL_GPUTextureCreateInfo info{};
					info.type = SDL_GPU_TEXTURETYPE_2D;
					info.format = State->ColourFormat();
					info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
					info.width = previewWidth;
					info.height = previewHeight;
					info.layer_count_or_depth = 1;
					info.num_levels = 1;
					info.sample_count = SDL_GPU_SAMPLECOUNT_1;
					writeTexture = SDL_CreateGPUTexture(State->Device, &info);
					preview.Width = writeTexture != nullptr ? previewWidth : 0;
					preview.Height = writeTexture != nullptr ? previewHeight : 0;
				}
				const Impl::NamedTexture target{
					writeTexture, preview.Width, preview.Height, State->ColourFormat()
				};
				if (drawImage(source, target, SDL_GPU_LOADOP_CLEAR, preview.ReverseSpectrum)) {
					preview.Slots.Publish(writeSlot);
					preview.Refresh = false;
				}
			}
			if (swapchain == nullptr && !offscreen) {
				return true;
			}
			Impl::NamedTexture source;
			for (const graph::ResourceId resource : context.Reads) {
				source = graphTexture(resource, context, false);
				if (source.IsValid()) {
					break;
				}
			}
			const Impl::NamedTexture target =
				offscreen ? Impl::NamedTexture{viewTarget, sceneWidth, sceneHeight, State->ColourFormat()}
						  : Impl::NamedTexture{swapchain, width, height, State->ColourFormat()};
			if (!drawImage(source, target, SDL_GPU_LOADOP_CLEAR)) {
				ENGINE_WARN("'{}' has no image wired into it", context.Name.Text());
				return true;
			}
			if (State->EnsureHistory(targetSlot, sceneWidth, sceneHeight)) {
				Impl::SceneSlot &history = State->SlotAt(targetSlot);
				const Impl::NamedTexture historyTarget{
					history.History,
					history.HistoryWidth,
					history.HistoryHeight,
					State->ColourFormat(),
				};
				if (source.Texture == historyTarget.Texture ||
					drawImage(source, historyTarget, SDL_GPU_LOADOP_CLEAR)) {
					history.HistoryReady = true;
				}
			}
			windowTarget.load_op = SDL_GPU_LOADOP_LOAD;
			return true;
		});
	}
}
