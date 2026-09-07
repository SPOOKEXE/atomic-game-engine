#include "PortalImageSampling.hpp"
#include "ViewRecording.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/effects/Ribbon.hpp>
#include <engine/graph/Cull.hpp>

#include <algorithm>

namespace engine::render {

	void ViewRecording::RegisterSurfaceNodes(NodeTable &nodes) {
		nodes.Set(core::Name("surface-capture"), [this](const graph::RunContext &context) {
			ENGINE_PROFILE("capture mixed surfaces");
			EnterNamedPass(context.Name);
			auto &bank = *Bank;
			for (auto &light : bank.SeamLights) {
				light.Ready = false;
			}
			if (Result.SurfaceBudgetExceeded) {
				return true;
			}
			State->RecordPortalImports(Command, *Request.Source, Request.TargetSlot);
			if (!State->EnsureSurfaceSampler()) {
				return false;
			}
			const auto &captures = bank.CapturePlan;
			std::array<bool, scene::MAX_SURFACES> refresh{};
			for (const auto &portal : Request.Portals) {
				refresh[portal.Index] = true;
			}
			for (size_t index = 0; index < AcceptedCount; ++index) {
				refresh[Accepted[index].Index] = Accepted[index].Refresh;
			}
			const auto uploadRibbons = [&](std::span<const effects::RibbonVertex> vertices) {
				if (vertices.empty()) {
					return true;
				}
				const uint32_t count = State->PrepareRibbons(vertices);
				if (count != vertices.size()) {
					return false;
				}
				auto *copy = SDL_BeginGPUCopyPass(Command);
				if (copy == nullptr) {
					return false;
				}
				const SDL_GPUTransferBufferLocation source{State->RibbonTransfer, 0};
				const SDL_GPUBufferRegion destination{
					State->RibbonBuffer, 0, count * uint32_t(sizeof(effects::RibbonVertex))
				};
				SDL_UploadToGPUBuffer(copy, &source, &destination, true);
				SDL_EndGPUCopyPass(copy);
				Result.UploadedBytes += destination.size;
				return true;
			};
			const auto textureOf = [&](const SurfaceCaptureEntry &entry) -> SDL_GPUTexture * {
				const uint32_t level = PortalLevels - entry.Depth;
				if (entry.Kind == SurfaceCaptureKind::Portal) {
					return bank.Portals[level].Targets[entry.Slot].Colour;
				}
				if (entry.Depth == 1) {
					const auto &surface = bank.Surfaces[entry.Slot];
					return surface.Texture[surface.Slot];
				}
				return bank.Mirrors[level].Targets[entry.Slot].Colour;
			};
			for (const uint16_t index : captures.Postorder) {
				const auto &entry = captures.Entries[index];
				if (!refresh[entry.RootSlot]) {
					continue;
				}
				const uint32_t level = PortalLevels - entry.Depth;
				SDL_GPUTexture *colour = nullptr;
				SDL_GPUTexture *depth = nullptr;
				Impl::SurfaceSlotState *root = nullptr;
				Impl::MirrorTarget *mirror = nullptr;
				uint32_t filter = 0;
				if (entry.Kind == SurfaceCaptureKind::Portal) {
					auto *target =
						State->EnsurePortal(Request.TargetSlot, level, entry.Slot, entry.Width, entry.Height);
					if (target == nullptr) {
						return false;
					}
					colour = target->Colour;
					depth = target->Depth;
					filter = Request.Portals[entry.Source].TagFilter;
				} else {
					filter = Request.Surfaces[entry.Source].TagFilter;
					if (entry.Depth == 1) {
						root = &bank.Surfaces[entry.Slot];
						colour = root->Texture[1u - root->Slot];
						depth = root->Depth;
					} else {
						mirror = State->EnsureMirror(
							Request.TargetSlot,
							level,
							entry.Slot,
							entry.Width,
							entry.Height,
							SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT
						);
						if (mirror == nullptr) {
							return false;
						}
						colour = mirror->Colour;
						depth = mirror->Depth;
					}
				}
				if (RibbonCount > 0) {
					if (!effects::FaceRibbonVertices(
							Request.RibbonVertices,
							Request.RibbonRuns,
							entry.Frame.Position,
							bank.CaptureRibbons
						) ||
						!uploadRibbons(bank.CaptureRibbons)) {
						return false;
					}
				}
				if (!OrderCaptureTransparency(
						State->SceneInstances,
						State->SceneOrder,
						uint32_t(SceneOpaque),
						SceneTransparent,
						entry.Frame.Position,
						bank.CaptureBlendOrder
					)) {
					return false;
				}
				const SDL_GPUViewport viewport{0, 0, float(entry.Width), float(entry.Height), 0, 1};
				auto *pass = OpenScenePass(
					colour, depth, false, &viewport, SceneLights, nullptr, WorldColourTarget::Hdr
				);
				if (pass == nullptr) {
					return false;
				}
				const FrameUniforms frame{entry.Matrices.ViewProjection, LightViewProjection, glm::mat4{1}};
				SDL_PushGPUVertexUniformData(Command, 0, &frame, sizeof(frame));
				const auto lighting = LightingAt(entry.Frame.Position, 0, 1);
				DrawWorldInto(pass, lighting, filter);
				const auto shadow = ShadowBindings();
				const auto drawPane = [&](size_t slot, uint32_t firstSlot, uint32_t count) {
					if (int16_t(slot) == entry.Arrival) {
						return;
					}
					if (count == 0) {
						return;
					}
					const PortalView *portal = PortalOf[slot];
					const SurfaceView *surface = nullptr;
					if (portal == nullptr) {
						for (const auto &candidate : Request.Surfaces) {
							if (candidate.Index == int16_t(slot)) {
								surface = &candidate;
								break;
							}
						}
					}
					if (portal == nullptr && surface == nullptr) {
						SDL_PushGPUVertexUniformData(Command, 0, &frame, sizeof(frame));
						Result.DrawCalls += State->DrawSlots(
							Command,
							pass,
							firstSlot,
							count,
							&lighting,
							shadow.Texture,
							shadow.Sampler,
							nullptr,
							State->SurfaceSampler,
							filter,
							Result.Triangles
						);
						return;
					}
					const auto centre = portal ? portal->Centre : surface->PaneCentre;
					const auto first = portal ? portal->First : surface->PaneFirst;
					const auto second = portal ? portal->Second : surface->PaneSecond;
					if (!graph::VisiblePane(entry.Matrices.ViewProjection, centre, first, second)) {
						return;
					}
					auto paneLighting = lighting;
					auto paneFrame = frame;
					SDL_GPUTexture *texture = nullptr;
					const uint16_t childIndex = entry.Children[slot];
					// A terminal aperture fades to its world's ambient. No previous-frame
					// image is substituted for an unrequested camera.
					paneLighting.Flags.z = 3;
					if (childIndex != NO_SURFACE_CAPTURE) {
						const auto &child = captures.Entries[childIndex];
						texture = textureOf(child);
						paneLighting.Flags.z = portal ? 2 : 1;
						paneLighting.Flags.w = surface ? surface->ImageOpacity : 1;
						paneFrame.SurfaceViewProjection = child.Matrices.ViewProjection;
					}
					if (portal) {
						paneLighting.PaneNormal = {portal->Normal.X, portal->Normal.Y, portal->Normal.Z, 0};
						if (portal->ExternalImage) {
							const auto *imported = State->FindPortalImport(
								*Request.Source, Request.TargetSlot, *portal, Command
							);
							if (imported) {
								texture = imported->Texture;
								paneLighting.Flags.z = 4;
								paneFrame.SurfaceViewProjection = PortalImageSampling(
									imported->Binding.Sampling,
									entry.Frame.Position,
									portal->Centre,
									portal->Normal
								);
							}
						}
					} else {
						paneLighting.Mirror.x = float(surface->Effect);
					}
					paneLighting.Mirror.z = 0;
					SDL_PushGPUVertexUniformData(Command, 0, &paneFrame, sizeof(paneFrame));
					Result.DrawCalls += State->DrawSlots(
						Command,
						pass,
						firstSlot,
						count,
						&paneLighting,
						shadow.Texture,
						shadow.Sampler,
						texture,
						State->SurfaceSampler,
						filter,
						Result.Triangles
					);
				};
				for (size_t slot = 0; slot < scene::MAX_SURFACES; ++slot) {
					const auto &run = Plan.Runs[slot];
					drawPane(slot, run.OpaqueFirst, run.OpaqueCount);
				}
				const auto drawInterface = [&](bool onTop) {
					if (DrawInterface) {
						Result.DrawCalls += Request.GameInterfaceHook->RecordWorld(
							Command,
							pass,
							entry.Matrices.ViewProjection,
							entry.Frame,
							core::Color3{State->Ambient.x, State->Ambient.y, State->Ambient.z},
							core::Vector3{State->Sun.x, State->Sun.y, State->Sun.z},
							entry.Width,
							entry.Height,
							onTop,
							WorldColourTarget::Hdr
						);
					}
				};
				drawInterface(false);
				if (!bank.CaptureBlendOrder.empty()) {
					State->BindPipeline(
						pass, State->HdrTransparentPipeline, Impl::PipelineFamily::HdrTransparent
					);
				}
				for (size_t order = 0; order < bank.CaptureBlendOrder.size();) {
					const auto &draw = bank.CaptureBlendOrder[order];
					const int16_t surface = State->SceneInstances[draw.Source].Surface;
					uint32_t count = 1;
					// Adjacent slots with the same image binding retain ordinary draw batching.
					while (order + count < bank.CaptureBlendOrder.size()) {
						const auto &next = bank.CaptureBlendOrder[order + count];
						if (next.Slot != draw.Slot + count ||
							State->SceneInstances[next.Source].Surface != surface) {
							break;
						}
						++count;
					}
					if (surface >= 0 && size_t(surface) < scene::MAX_SURFACES) {
						drawPane(surface, draw.Slot, count);
					} else {
						SDL_PushGPUVertexUniformData(Command, 0, &frame, sizeof(frame));
						Result.DrawCalls += State->DrawSlots(
							Command,
							pass,
							draw.Slot,
							count,
							&lighting,
							shadow.Texture,
							shadow.Sampler,
							nullptr,
							State->SurfaceSampler,
							filter,
							Result.Triangles
						);
					}
					order += count;
				}
				if (ParticleCount > 0) {
					Result.DrawCalls += State->DrawParticles(
						Command,
						pass,
						entry.Matrices.ViewProjection,
						entry.Frame,
						Result.Triangles,
						Result.ParticlesDrawn,
						Result.Culled,
						WorldColourTarget::Hdr
					);
				}
				if (RibbonCount > 0) {
					Result.DrawCalls += State->DrawRibbons(
						Command,
						pass,
						entry.Matrices.ViewProjection,
						entry.Frame,
						Request.RibbonRuns,
						Result.Triangles,
						WorldColourTarget::Hdr
					);
				}
				drawInterface(true);
				SDL_EndGPURenderPass(pass);
				if (root) {
					root->PreviousViewProjection = root->ViewProjection;
					root->PreviousSampling = root->Sampling;
					root->Slot = 1u - root->Slot;
					root->ViewProjection = entry.Matrices.ViewProjection;
					root->Sampling = root->ViewProjection;
					root->Ready = true;
					root->Signature = SurfaceSignature;
					root->Drawn = FrameSeconds;
				}
				if (mirror) {
					mirror->Sampling = entry.Matrices.ViewProjection;
					mirror->Ready = true;
				}
				if (entry.Kind == SurfaceCaptureKind::Portal) {
					++Result.PortalPasses;
				} else {
					++Result.SurfacePasses;
					SurfaceDepth.Resolved = std::max(SurfaceDepth.Resolved, entry.Depth);
				}
			}
			if (RibbonCount > 0 && !uploadRibbons(Request.RibbonVertices)) {
				return false;
			}
			return CaptureSeamLights(WorldColourTarget::Hdr);
		});
	}
}
