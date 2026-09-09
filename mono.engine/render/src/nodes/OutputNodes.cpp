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
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>

#include <algorithm>
#include <filesystem>
#include <string>

namespace engine::render {

	void ViewRecording::RegisterOutputNodes(NodeTable &frameNodes) {
		frameNodes.Set(core::Name("spatial-overlay"), [this](const graph::RunContext &context) {
			const auto *node = Pipeline->Graph.Find(context.Node);
			if (!node || context.Writes.size() != 2) return false;
			const auto resolve = [&](core::Name port) {
				const auto found = std::find(node->WritePorts.begin(), node->WritePorts.end(), port);
				return found == node->WritePorts.end()
						   ? Impl::NamedTexture{}
						   : GraphTexture(context.Writes[found - node->WritePorts.begin()], context, true);
			};
			const auto colour = resolve(core::Name("colour")), depth = resolve(core::Name("z"));
			if (!colour.IsValid() || !depth.IsValid() ||
				colour.Format != SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT ||
				depth.Format != SDL_GPU_TEXTUREFORMAT_D32_FLOAT || colour.Width != depth.Width ||
				colour.Height != depth.Height)
				return false;
			EnterNamedPass(context.Name);
			SDL_GPUColorTargetInfo target{};
			target.texture = colour.Texture;
			target.load_op = SDL_GPU_LOADOP_CLEAR;
			target.store_op = SDL_GPU_STOREOP_STORE;
			target.cycle = true;
			SDL_GPUDepthStencilTargetInfo z{};
			z.texture = depth.Texture;
			z.clear_depth = 1;
			z.load_op = SDL_GPU_LOADOP_CLEAR;
			z.store_op = SDL_GPU_STOREOP_DONT_CARE;
			z.cycle = true;
			auto *pass = SDL_BeginGPURenderPass(Command, &target, 1, &z);
			if (!pass) return false;
			const SDL_GPUViewport viewport{0, 0, float(colour.Width), float(colour.Height), 0, 1};
			SDL_SetGPUViewport(pass, &viewport);
			if (DrawInterface && Request.GameInterfaceHook) {
				const auto lighting = LightingAt(Request.CameraFrame.Position, 0, 0);
				Result.DrawCalls += Request.GameInterfaceHook->RecordWorld(
					Command,
					pass,
					Matrices.ViewProjection,
					Request.CameraFrame,
					{lighting.Ambient.x, lighting.Ambient.y, lighting.Ambient.z},
					{lighting.Direction.x, lighting.Direction.y, lighting.Direction.z},
					colour.Width,
					colour.Height,
					true,
					WorldColourTarget::Hdr
				);
			}
			SDL_EndGPURenderPass(pass);
			core::Metrics::Count(
				"render.spatial_overlay.output_bytes", uint64_t(colour.Width) * colour.Height * 8
			);
			return true;
		});

		frameNodes.Set(core::Name("eye-image"), [this](const graph::RunContext &context) {
			EnterNamedPass(context.Name);
			if (Request.Source == nullptr || context.Writes.empty() || context.Writes.size() > 6)
				return false;
			const auto &view = *Request.Source;
			const auto *node = Pipeline->Graph.Find(context.Node);
			if (node == nullptr) return false;
			const auto *layer = node->Parameter(core::Name("layer"));
			const bool spatialOverlay = layer && *layer == "spatial-overlay";
			const uint8_t layerIndex = layer && *layer == "transparent-0"	? 1
									   : layer && *layer == "transparent-1" ? 2
									   : spatialOverlay						? 3
																			: 0;
			static_assert(
				std::tuple_size_v<decltype(view.EyeTransparentImages)> == MAX_PORTAL_TRANSPARENT_LAYERS
			);
			const auto handle = spatialOverlay	  ? view.EyeSpatialOverlayImage
								: layerIndex == 0 ? view.EyeImage
												  : view.EyeTransparentImages[layerIndex - 1];
			const auto *scope = node->Parameter(core::Name("scope"));
			const auto expectedScope = scope && *scope == "opaque-lighting" ? PortalImageScope::OpaqueLighting
																			: PortalImageScope::CompleteWorld;
			const auto *projection = node->Parameter(core::Name("projection"));
			const auto expectedProjection = projection && *projection == "seam" ? PortalImageProjection::Seam
																				: PortalImageProjection::Eye;
			graph::ResourceId colourId = context.Writes.front(), depthId{}, normalId{}, responseId{},
							  baselineId{}, directionalId{};
			for (size_t i = 0; i < node->WritePorts.size(); ++i) {
				if (node->WritePorts[i] == core::Name("depth"))
					depthId = context.Writes[i];
				else if (node->WritePorts[i] == core::Name("normal"))
					normalId = context.Writes[i];
				else if (node->WritePorts[i] == core::Name("ambient-response"))
					responseId = context.Writes[i];
				else if (node->WritePorts[i] == core::Name("lighting-baseline"))
					baselineId = context.Writes[i];
				else if (node->WritePorts[i] == core::Name("directional-response"))
					directionalId = context.Writes[i];
				else if (node->WritePorts[i] == core::Name("colour"))
					colourId = context.Writes[i];
			}
			const auto target = GraphTexture(colourId, context, true);
			if (!target.IsValid() || target.Format != SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT) return false;
			const bool paired = depthId.IsValid();
			const bool directional = directionalId.IsValid();
			const bool ambient = normalId.IsValid() || responseId.IsValid() || baselineId.IsValid();
			if (directional && !ambient) return false;
			if (ambient && (!normalId.IsValid() || !responseId.IsValid() || !baselineId.IsValid() ||
							!paired || layerIndex != 0))
				return false;
			const auto normal = ambient ? GraphTexture(normalId, context, true) : Impl::NamedTexture{};
			const auto response = ambient ? GraphTexture(responseId, context, true) : Impl::NamedTexture{};
			const auto baseline = ambient ? GraphTexture(baselineId, context, true) : Impl::NamedTexture{};
			if (ambient && (!normal.IsValid() || !response.IsValid() || !baseline.IsValid() ||
							baseline.Format != SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT ||
							baseline.Width != target.Width || baseline.Height != target.Height ||
							normal.Format != SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM ||
							response.Format != SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT ||
							normal.Width != target.Width || normal.Height != target.Height ||
							response.Width != target.Width || response.Height != target.Height))
				return false;
			const auto directionalResponse =
				directional ? GraphTexture(directionalId, context, true) : Impl::NamedTexture{};
			if (directional &&
				(!directionalResponse.IsValid() ||
				 directionalResponse.Format != SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT ||
				 directionalResponse.Width != target.Width || directionalResponse.Height != target.Height))
				return false;
			if (spatialOverlay ? paired : (layerIndex != 0 && !paired)) return false;
			const auto depth = paired ? GraphTexture(depthId, context, true) : Impl::NamedTexture{};
			if (paired && (!depth.IsValid() || depth.Format != SDL_GPU_TEXTUREFORMAT_R32_FLOAT ||
						   depth.Width != target.Width || depth.Height != target.Height))
				return false;
			State->RecordPortalImports(Command, view, Request.TargetSlot);
			for (const auto &image : State->ImportedPortals) {
				const auto &owner = image.Binding;
				if (handle == 0 || image.Handle != handle || owner.Layer != layerIndex ||
					owner.World != view.World || owner.WorldName != view.WorldName ||
					owner.ViewSlot != Request.TargetSlot || owner.Portal != view.EyeImageKey ||
					owner.ExpectedProjection != expectedProjection || owner.ExpectedScope != expectedScope ||
					image.Texture == nullptr || (!image.Ready && image.Recorded != Command))
					continue;
				if (layerIndex != 0) {
					const auto base = std::find_if(
						State->ImportedPortals.begin(),
						State->ImportedPortals.end(),
						[&](const auto &candidate) {
							return candidate.Handle != 0 && candidate.Handle == view.EyeImage;
						}
					);
					if (base == State->ImportedPortals.end() || base->Binding.Layer != 0 ||
						base->Binding.World != owner.World || base->Binding.WorldName != owner.WorldName ||
						base->Binding.ViewSlot != owner.ViewSlot || base->Binding.Portal != owner.Portal ||
						base->Binding.Index != owner.Index || base->LayerSet != image.LayerSet ||
						base->Binding.Expected != owner.Expected ||
						base->Binding.ExpectedScope != owner.ExpectedScope ||
						base->Binding.ExpectedProjection != owner.ExpectedProjection ||
						base->Binding.Sampling != owner.Sampling || base->Width != image.Width ||
						base->Height != image.Height || base->ContentRevision != image.ContentRevision ||
						base->LightingRevision != image.LightingRevision ||
						(!base->Ready && base->Recorded != Command))
						return false;
				}
				if (directional && !image.DirectionalResponseTexture) return false;
				if (paired && image.DepthTexture == nullptr) return false;
				if (ambient && (!image.NormalTexture || !image.AmbientResponseTexture ||
								!image.LightingBaselineTexture || image.Width != target.Width ||
								image.Height != target.Height))
					return false;
				SDL_GPUBlitInfo blit{};
				blit.source.texture = image.Texture;
				blit.source.w = image.Width;
				blit.source.h = image.Height;
				blit.destination.texture = target.Texture;
				blit.destination.w = target.Width;
				blit.destination.h = target.Height;
				blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
				// Paired samples describe one surface; color filtering across depths breaks occlusion.
				blit.filter = paired ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
				blit.cycle = true;
				SDL_BlitGPUTexture(Command, &blit);
				if (paired) {
					blit.source.texture = image.DepthTexture;
					blit.destination.texture = depth.Texture;
					blit.filter = SDL_GPU_FILTER_NEAREST;
					SDL_BlitGPUTexture(Command, &blit);
				}
				if (ambient) {
					blit.source.texture = image.NormalTexture;
					blit.destination.texture = normal.Texture;
					SDL_BlitGPUTexture(Command, &blit);
					blit.source.texture = image.AmbientResponseTexture;
					blit.destination.texture = response.Texture;
					SDL_BlitGPUTexture(Command, &blit);
					blit.source.texture = image.LightingBaselineTexture;
					blit.destination.texture = baseline.Texture;
					SDL_BlitGPUTexture(Command, &blit);
				}
				if (directional) {
					blit.source.texture = image.DirectionalResponseTexture;
					blit.destination.texture = directionalResponse.Texture;
					SDL_BlitGPUTexture(Command, &blit);
				}
				core::Metrics::Count(
					"render.eye_image.blits",
					directional ? 6
					: ambient	? 5
					: paired	? 2
								: 1
				);
				core::Metrics::Count(
					"render.eye_image.output_bytes",
					uint64_t(target.Width) * target.Height *
						(directional ? 64
						 : ambient	 ? 48
						 : paired	 ? 12
									 : 8)
				);
				return true;
			}
			if (paired || spatialOverlay) return false;
			// A stale or wrong-owner image must not leave the last room visible.
			SDL_GPUColorTargetInfo clear{};
			clear.texture = target.Texture;
			clear.clear_color = {0, 0, 0, 1};
			clear.load_op = SDL_GPU_LOADOP_CLEAR;
			clear.store_op = SDL_GPU_STOREOP_STORE;
			clear.cycle = true;
			auto *pass = SDL_BeginGPURenderPass(Command, &clear, 1, nullptr);
			if (pass == nullptr) return false;
			SDL_EndGPURenderPass(pass);
			return true;
		});

		frameNodes.Set(core::Name("interface"), [this](const graph::RunContext &context) {
			ViewRecording &recording = *this;
			if (!recording.Request.Damage.GameInterface) {
				return true;
			}
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
			if (!recording.Request.Damage.SceneImage() && !recording.UploadOverlay) {
				return true;
			}
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
			if (!recording.Request.Damage.SceneImage() && !recording.UploadOverlay) {
				return true;
			}
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

		frameNodes.Set(core::Name("shadow-capture"), [this](const graph::RunContext &context) {
			const auto *node = Pipeline->Graph.Find(context.Node);
			if (!node || context.Reads.size() != 1 || !context.Writes.empty() ||
				node->Scope != graph::NodeScope::View)
				return false;
			const auto *desc = Pipeline->Graph.FindResource(context.Reads.front());
			if (!desc) return false;
			ResourceShadowCapture metadata;
			metadata.SourceEmpty = Instances.empty();
			metadata.SourceBounds = metadata.SourceEmpty ? core::AABB{} : SceneBounds;
			if (const auto *seed = State->FindPortalShadow(Request.Source->ImportedDirectionalShadow);
				seed && !seed->Binding.ExpectedSnapshot.SourceEmpty) {
				const auto &bounds = seed->Binding.ExpectedSnapshot.SourceBounds;
				const core::AABB source{{bounds[0], bounds[1], bounds[2]}, {bounds[3], bounds[4], bounds[5]}};
				metadata.SourceBounds = metadata.SourceEmpty ? source : metadata.SourceBounds.Union(source);
				metadata.SourceEmpty = false;
			}
			metadata.DomainBounds = DirectionalShadowBounds;
			for (size_t column = 0; column < 4; ++column)
				for (size_t row = 0; row < 4; ++row)
					metadata.LightViewProjection[column * 4 + row] = LightViewProjection[column][row];
			EnterNamedPass(context.Name);
			State->RecordShadowResourceImages(
				Command,
				Pipeline->Name,
				node->Name,
				Request.TargetSlot,
				desc->Name,
				HaveShadow && Request.Damage.Scene ? GraphTexture(context.Reads.front(), context, false)
												   : Impl::NamedTexture{},
				metadata
			);
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
			if (node != nullptr && !context.Reads.empty() && context.Reads.size() <= 6) {
				const size_t slot = context.View == graph::RunContext::WHOLE_FRAME
										? node->Integer(core::Name("view"), 0)
										: recording.Request.TargetSlot;
				graph::ResourceId colorId = context.Reads[0], depthId{}, normalId{}, ambientResponseId{},
								  lightingBaselineId{}, directionalResponseId{};
				for (size_t i = 0; i < node->ReadPorts.size(); ++i) {
					if (node->ReadPorts[i] == core::Name("depth"))
						depthId = context.Reads[i];
					else if (node->ReadPorts[i] == core::Name("normal"))
						normalId = context.Reads[i];
					else if (node->ReadPorts[i] == core::Name("ambient-response"))
						ambientResponseId = context.Reads[i];
					else if (node->ReadPorts[i] == core::Name("lighting-baseline"))
						lightingBaselineId = context.Reads[i];
					else if (node->ReadPorts[i] == core::Name("directional-response"))
						directionalResponseId = context.Reads[i];
					else
						colorId = context.Reads[i];
				}
				const auto *desc = selectedPipeline->Graph.FindResource(colorId);
				const auto *depthDesc = selectedPipeline->Graph.FindResource(depthId);
				const auto *normalDesc = selectedPipeline->Graph.FindResource(normalId);
				const auto *ambientResponseDesc = selectedPipeline->Graph.FindResource(ambientResponseId);
				const auto *lightingBaselineDesc = selectedPipeline->Graph.FindResource(lightingBaselineId);
				const auto *directionalResponseDesc =
					selectedPipeline->Graph.FindResource(directionalResponseId);
				const auto source = recording.ResourceTexture(colorId, slot, false);
				const auto depth =
					depthDesc ? recording.ResourceTexture(depthId, slot, false) : Impl::NamedTexture{};
				auto normal =
					normalDesc ? recording.ResourceTexture(normalId, slot, false) : Impl::NamedTexture{};
				if (normal.IsValid() && slot < State->PbrSlots.size()) {
					const auto &pbr = State->PbrSlots[slot];
					if (normal.Texture == pbr.Normal && source.Width == pbr.Dimensions.ViewWidth &&
						source.Height == pbr.Dimensions.ViewHeight && normal.Width >= source.Width &&
						normal.Height >= source.Height) {
						// Native GBuffer storage is padded; its rendered viewport begins at (0, 0).
						// Copy that rectangle directly, preserving every packed normal bit.
						normal.Width = source.Width;
						normal.Height = source.Height;
					}
				}
				State->RecordResourceImages(
					recording.Command,
					selectedPipeline->Name,
					context.Name,
					slot,
					desc != nullptr ? desc->Name : core::Name{},
					source,
					depthDesc != nullptr ? depthDesc->Name : core::Name{},
					depth,
					normalDesc ? normalDesc->Name : core::Name{},
					normal,
					ambientResponseDesc ? ambientResponseDesc->Name : core::Name{},
					ambientResponseDesc ? recording.ResourceTexture(ambientResponseId, slot, false)
										: Impl::NamedTexture{},
					lightingBaselineDesc ? lightingBaselineDesc->Name : core::Name{},
					lightingBaselineDesc ? recording.ResourceTexture(lightingBaselineId, slot, false)
										 : Impl::NamedTexture{},
					directionalResponseDesc ? directionalResponseDesc->Name : core::Name{},
					directionalResponseDesc ? recording.ResourceTexture(directionalResponseId, slot, false)
											: Impl::NamedTexture{}
				);
			}
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
			for (size_t read = 0; read < context.Reads.size(); ++read) {
				if (read < node->ReadPorts.size() && node->ReadPorts[read] == core::Name("depth")) continue;
				const auto resource = context.Reads[read];
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
					writeTexture = gpu::CreateTexture(State->Device, &info);
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
