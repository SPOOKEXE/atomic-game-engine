// The recursion through a hole, its display copy, and the mouths drawn over the
// frame.
//
// **The same recursion as `fillMirror`, by a different map**, which is why the
// two families sit beside each other and share nothing but
// `ViewRecording::OpenScenePass`. A hole's sub-render is the screen's own
// frustum, so its pane reads the texel it is standing on; a mirror's is fitted
// to its own rectangle, so its pane reads by projecting its world position.
// Neither lookup is expressible in the other's target. `NON-EUCLIDEAN.md`
// Part III is the argument.

#include "PortalImageSampling.hpp"
#include "ViewRecording.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/graph/Cull.hpp>
#include <engine/graph/Shadow.hpp>
#include <engine/scene/SurfaceCameras.hpp>

#include <algorithm>
#include <cmath>
#include <functional>

namespace engine::render {

	bool ViewRecording::CaptureSeamLights(WorldColourTarget colour) {
		ENGINE_PROFILE("capture seam light fields");
		const auto &portalOf = PortalOf;
		const size_t targetSlot = Request.TargetSlot;
		const auto &drawCamera = DrawCamera;
		const auto &lightUniforms = SceneLights;
		const auto &lightViewProjection = LightViewProjection;
		auto *const command = Command;
		// Supplemental local-light and emissive radiance arriving from the far room.
		// Ambient, sky and the shared world sun already illuminate the receiver;
		// including them here would add the same global illumination twice.
		// Transformed directional/sky transport needs a separate lighting model.
		// Probes are viewer-independent: a doorway emits even behind the camera.
		for (size_t slot = 0; slot < scene::MAX_SURFACES; slot++) {
			if (portalOf[slot] == nullptr) {
				continue;
			}
			const PortalView &portal = *portalOf[slot];
			if (portal.ExternalImage) {
				continue;
			}

			// The authored mouth determines the receiving half-space. Deriving it
			// from the viewer would flip the light pool when the camera crosses.
			const core::Vector3 outward = portal.Normal;

			// Far enough off the plane that the oblique clip below stays
			// in front of the eye: the bias is derived from this same
			// distance, and a plane that lands behind the camera inverts
			// the frustum and captures nothing.
			constexpr float STAND_OFF = 0.5f;
			const core::Vector3 standPosition = portal.Centre + outward * STAND_OFF;
			const core::Vector3 upAxis = std::abs(outward.Y) > 0.99f ? core::Vector3{0.0f, 0.0f, 1.0f}
																	 : core::Vector3{0.0f, 1.0f, 0.0f};
			const core::CFrame stand = core::CFrame::LookAt(standPosition, standPosition - outward, upAxis);
			const core::CFrame placed = portal.Warp.Place(stand);

			// Wide and square: the capture is a light probe of a room,
			// not a picture, and a narrow lens would miss the lamps
			// standing beside the doorway.
			scene::Camera captureCamera = drawCamera;
			captureCamera.FieldOfViewRadians = 1.9f;
			captureCamera.NearPlane = 0.05f;
			const glm::mat4 captureProjection = scene::ResolveCamera(placed, captureCamera, 1.0f).Projection;

			// The same backward-pointing clip as `subCameraFor`, so the
			// wall the far mouth is set into does not fill the capture.
			// The bias is the stand-in eye's own seam distance rather
			// than the viewer's - `PortalClipBias` halves it, keeping
			// the plane in front of an eye the viewer's bias could put
			// it behind.
			const core::Vector3 clipNormal = portal.Warp.Rotate(outward) * -1.0f;
			const core::Vector3 clipPoint =
				portal.Warp.Point(portal.Centre) - clipNormal * scene::PortalClipBias(STAND_OFF);
			const scene::CameraMatrices captureMatrices = scene::ResolveSurfaceCamera(
				placed,
				scene::ObliqueProjection(captureProjection, placed, clipNormal, clipNormal.Dot(clipPoint))
			);

			Impl::SeamLightTarget *seamLight = State->EnsureSeamLight(
				targetSlot,
				slot,
				colour == WorldColourTarget::Hdr ? SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT
												 : State->ColourFormat()
			);
			if (seamLight == nullptr) {
				return false;
			}

			const SDL_GPUViewport seamViewport{
				0.0f,
				0.0f,
				static_cast<float>(seamLight->Width),
				static_cast<float>(seamLight->Height),
				0.0f,
				1.0f
			};

			const SDL_FColor voidColour{0.0f, 0.0f, 0.0f, 1.0f};

			SDL_GPURenderPass *const pass = OpenScenePass(
				seamLight->Colour, seamLight->Depth, false, &seamViewport, lightUniforms, &voidColour, colour
			);

			const FrameUniforms captureUniforms{
				captureMatrices.ViewProjection,
				lightViewProjection,
				glm::mat4{1.0f},
			};
			if (pass == nullptr) {
				return false;
			}
			SDL_PushGPUVertexUniformData(command, 0, &captureUniforms, sizeof(captureUniforms));

			LightingUniforms voidLighting = LightingAt(placed.Position, 0.0f, 1.0f);
			voidLighting.Ambient = glm::vec4{0.0f};
			voidLighting.OutdoorAmbient = glm::vec4{0.0f};
			voidLighting.Direct = glm::vec4{0.0f};
			// No fog in a light probe: what falls to distance falls to
			// the void the clear already painted.
			voidLighting.Fog = glm::vec4{1.0e6f, 1.0e6f + 1.0f, 0.0f, 0.0f};

			DrawWorldInto(pass, voidLighting, portal.TagFilter);
			DrawBlendedInto(pass, captureUniforms, voidLighting, portal.TagFilter, false, colour);

			SDL_EndGPURenderPass(pass);

			seamLight->Centre = glm::vec4{portal.Centre.X, portal.Centre.Y, portal.Centre.Z, 1.0f};

			// The spill reaches about a doorway's span into the room:
			// past that the window falloff has taken it below anything
			// the ambient does not already cover.
			const float reach = 2.0f * std::max(portal.First.Magnitude() + portal.Second.Magnitude(), 1.0f);
			seamLight->Outward = glm::vec4{outward.X, outward.Y, outward.Z, reach};
			seamLight->First = glm::vec4{portal.First.X, portal.First.Y, portal.First.Z, 0.0f};
			seamLight->Second = glm::vec4{portal.Second.X, portal.Second.Y, portal.Second.Z, 0.0f};
			seamLight->Ready = true;
		}
		return true;
	}

	void ViewRecording::RegisterPortalNodes(NodeTable &frameNodes) {
		frameNodes.Set(core::Name("portal-capture"), [this](const graph::RunContext &context) {
			ViewRecording &recording = *this;
			Impl *const State = recording.State;
			FrameResult &result = recording.Result;
			SDL_GPUCommandBuffer *const command = recording.Command;
			const core::CFrame &cameraFrame = recording.Request.CameraFrame;
			FrameOverlayHook *const gameInterfaceHook = recording.Request.GameInterfaceHook;
			const size_t targetSlot = recording.Request.TargetSlot;
			const float nearestPane = recording.NearestPane;
			const uint32_t sceneWidth = recording.SceneWidth;
			const uint32_t sceneHeight = recording.SceneHeight;
			const uint32_t targetWidth = recording.TargetWidth;
			const uint32_t targetHeight = recording.TargetHeight;
			const bool haveInstances = recording.HaveInstances;
			Impl::SurfaceBank &bank = *recording.Bank;
			const auto &portalOf = recording.PortalOf;
			const bool havePortals = recording.HavePortals;
			const uint32_t portalLevels = recording.PortalLevels;
			const glm::mat4 &cameraMatrix = recording.CameraMatrix;
			const glm::mat4 &lightViewProjection = recording.LightViewProjection;
			const scene::ScenePlan &plan = recording.Plan;
			const uint32_t sceneCount = recording.SceneCount;
			const LightUniforms &lightUniforms = recording.SceneLights;
			const auto enterNamedPass = [&recording](
											core::Name name, SDL_GPUCommandBuffer *recordedCommand = nullptr
										) { recording.EnterNamedPass(name, recordedCommand); };
			const auto lightingAt = [&recording](
										const core::Vector3 &eye, float surfaceMode, float imageOpacity
									) { return recording.LightingAt(eye, surfaceMode, imageOpacity); };
			const auto shadowBinding = [&recording] { return recording.ShadowBindings(); };
			const auto drawWorldInto =
				[&recording](
					SDL_GPURenderPass *pass, const LightingUniforms &plainLighting, uint32_t filter
				) { recording.DrawWorldInto(pass, plainLighting, filter); };
			const bool drawInterface = recording.DrawInterface;

			enterNamedPass(context.Name);
			State->RecordPortalImports(command, *recording.Request.Source, targetSlot);

			// Last frame's light fields are for mouths that may be gone - a
			// disabled `Portal` reaches here as no `PortalView` at all, and its
			// spill has to go out with it. See `SeamLightTarget::Ready`.
			for (Impl::SeamLightTarget &seamLight : bank.SeamLights) {
				seamLight.Ready = false;
			}

			// --- the portal capture ----------------------------------------------
			//
			// **The same recursion as `fillMirror`, by a different map.** Both derive
			// each level's camera from the level above - that is what makes either one
			// compose, and the mirror pass was an iteration until v0.15 and wrong at
			// every level past the first for exactly the want of it.
			//
			// Here the derivation is the warp applied to *that* camera's frame, and
			// that camera's own projection skewed onto the mapped pane - exactly as
			// `Portal::Draw` composes `portalCam.worldView *= warp->delta`.
			// `NON-EUCLIDEAN.md`'s Part III is the whole argument.
			//
			// **What stays separate is the map and the lookup**, which is why the two
			// share `openScenePass`, `drawWorldInto` and `drawBlendedInto` and nothing
			// above them. A hole's sub-render is the screen's own frustum, so its pane
			// reads the texel it is standing on; a mirror's is fitted to its own
			// rectangle, so its pane reads by projecting its world position. Neither
			// lookup is expressible in the other's target.
			//
			// **Depth first, and every level's targets survive until the level above
			// has drawn all of its panes.** That is why the pool is indexed by level
			// *and* slot: level `L` renders the world and then draws every hole it can
			// see, so all of level `L-1` is live at once.
			if (havePortals && haveInstances && sceneCount > 0 && portalLevels > 0) {
				ENGINE_PROFILE_CAT("portal pass", core::ProfileCategory::Render);

				// **The unskewed screen projection, kept and re-skewed at every
				// level.** `scene::ObliqueProjection` substitutes the whole depth row,
				// reading two of the entries it is about to overwrite - so skewing an
				// already-skewed matrix is not the same as skewing the original
				// against the new plane, which is the arrangement `Camera::ClipOblique`
				// gets for free by writing the row from the untouched half of the
				// matrix. Starting from this every time is what makes each level's
				// frustum the screen's own, which is what makes the screen-position
				// lookup in `opaque.frag` exact.
				const glm::mat4 screenProjection = recording.Matrices.Projection;

				// **Made before anything is captured, because a world of nothing but
				// holes never reaches `EnsureSurface`.** The sampler used to be
				// created there, so a portal-only scene took a null one into
				// `SDL_BindGPUFragmentSamplers` and died inside the backend - and a
				// scene with one mirror in it hid that completely.
				(void)State->EnsureSurfaceSampler();

				const ShadowBinding shadow = shadowBinding();

				// Where a hole's sub-camera stands, and what it looks through.
				//
				// **Which warp is a question about this level's camera, asked again at
				// every level.** A pane is a hole from either side, and one map serves
				// both: it carries the pane's front hemisphere to the far pane's back
				// one and its back to the far pane's front, so a sub-camera that has
				// stepped through and is now on the other side of something is carried
				// by the same matrix, the other way, for free. CodeParade's
				// `Portal::Connect` writes the same `delta` into both warps.
				//
				// Which *side* still has to be asked, because the clip plane's normal
				// is the way this camera is looking and that does flip.
				struct SubCamera {
					core::CFrame Frame;
					scene::CameraMatrices Matrices;
				};

				const auto subCameraFor = [&](const PortalView &portal, const core::CFrame &from) {
					const float side = (from.Position - portal.Centre).Dot(portal.Normal);
					const scene::SeamTransform &warp = portal.Warp;

					const core::CFrame placed = warp.Place(from);

					// **The clip normal points back through the hole**, which is the
					// one sign here worth deriving rather than trying. The map sends
					// the eye's side of the source pane to the *opposite* side of the
					// far one, so a sub-camera placed from an eye at `+outward` lands
					// behind the mapped pane looking back along `outward`'s image.
					// What has to survive clipping is everything beyond the mapped
					// pane, so the normal is the way this camera is looking and not
					// the way the pane faces.
					const core::Vector3 outward = portal.Normal * (side >= 0.0f ? 1.0f : -1.0f);
					const core::Vector3 clipNormal = warp.Rotate(outward) * -1.0f;

					// **Moved back towards this camera by a sliver, so the plane
					// keeps a little more rather than a little less.** The oblique
					// substitution makes this plane the near plane, so everything
					// between the sub-camera and it is thrown away - and the far
					// room's own geometry meets the mapped pane exactly, which after
					// two matrix products means some of it lands a float either side.
					// The half that lands short is clipped, and what that looks like
					// is a hairline of background around the inside of every hole,
					// with parts poking through it.
					//
					// **The sign is the whole of it and it is worth deriving rather
					// than trying.** `clipNormal` is the way this camera looks, so
					// adding along it pushes the plane deeper into the far room and
					// removes a slab of whatever is standing in the hole - a body
					// straddling the seam loses its far half and reads as a character
					// cut in two. CodeParade's `extra_clip` subtracts for this
					// reason: `pos - normal*extra_clip` with `normal` pointing away
					// from the camera is the pane moved *towards* it.
					const core::Vector3 clipPoint =
						warp.Point(portal.Centre) - clipNormal * scene::PortalClipBias(nearestPane);

					return SubCamera{
						placed,
						scene::ResolveSurfaceCamera(
							placed,
							scene::ObliqueProjection(
								screenProjection, placed, clipNormal, clipNormal.Dot(clipPoint)
							)
						),
					};
				};

				// One level: fill `bank.Portals[level][i]` for every hole `i` this
				// camera can see, then leave them for the caller to sample.
				//
				// A `std::function` because it calls itself and captures the frame.
				// The depth is bounded by `MAX_PORTAL_DEPTH`, so the recursion is four
				// deep at worst and the indirection is paid once per hole per level
				// beside a whole scene render.
				std::function<void(const scene::CameraMatrices &, const core::CFrame &, uint32_t, int8_t)>
					fillLevel;

				fillLevel = [&](const scene::CameraMatrices &from,
								const core::CFrame &fromFrame,
								uint32_t level,
								int8_t skip) {
					for (size_t slot = 0; slot < scene::MAX_SURFACES; slot++) {
						if (portalOf[slot] == nullptr) {
							continue;
						}

						const PortalView &portal = *portalOf[slot];

						// **The hole this camera just came out of**, which is at this
						// level's own clip plane and would render a scene that is then
						// entirely clipped away. CodeParade's `skipPortal` argument.
						if (portal.ExternalImage || portal.Index == skip) {
							continue;
						}

						// **Per portal per level, which is what stops the cost being
						// `holes ^ depth`.** A hole behind this level's camera costs
						// nothing, and most of them are.
						if (!graph::VisiblePane(
								from.ViewProjection, portal.Centre, portal.First, portal.Second
							)) {
							continue;
						}

						const SubCamera sub = subCameraFor(portal, fromFrame);

						if (level > 0) {
							fillLevel(sub.Matrices, sub.Frame, level - 1, portal.Partner);
						}

						if (!recording.AdmitSurfaceCapture(sceneWidth, sceneHeight, portalLevels - level)) {
							continue;
						}

						Impl::PortalTarget *target =
							State->EnsurePortal(targetSlot, level, slot, targetWidth, targetHeight);
						if (target == nullptr) {
							continue;
						}

						// **The same rectangle as the level above draws into.** The
						// target is the attachment's size, and the world fills the
						// viewport's corner of it - so a pane in the level above reads
						// the texel it is standing on. Setting a different one here is
						// the whole of what would make the picture slide.
						const SDL_GPUViewport portalViewport{
							0.0f,
							0.0f,
							static_cast<float>(sceneWidth),
							static_cast<float>(sceneHeight),
							0.0f,
							1.0f
						};

						// **Not cycled, unlike a surface slot, and it was measured
						// rather than reasoned.** Cycling hands back a fresh allocation
						// per write, which is the right answer when two passes in one
						// frame share a texture - and at one level nothing does: a
						// target is written once and sampled once, by the pass above it,
						// in that order. Asking for a fresh allocation anyway made the
						// device hang more often rather than less. `Impl::PortalDepth`
						// carries what happens above one level, which is where the same
						// target *is* written twice.
						SDL_GPURenderPass *const pass = recording.OpenScenePass(
							target->Colour,
							target->Depth,
							false,
							&portalViewport,
							lightUniforms,
							nullptr,
							WorldColourTarget::Hdr
						);

						const FrameUniforms subFrameUniforms{
							sub.Matrices.ViewProjection,
							lightViewProjection,
							glm::mat4{1.0f},
						};
						SDL_PushGPUVertexUniformData(command, 0, &subFrameUniforms, sizeof(subFrameUniforms));

						const LightingUniforms subLighting = lightingAt(sub.Frame.Position, 0.0f, 1.0f);

						drawWorldInto(pass, subLighting, portal.TagFilter);

						// The holes this level can see, put back one at a time. Their
						// targets are `level - 1`, filled by the call above and still
						// untouched - which is why the pool is per level per slot.
						for (size_t seenSlot = 0; seenSlot < scene::MAX_SURFACES; seenSlot++) {
							if (portalOf[seenSlot] == nullptr ||
								portalOf[seenSlot]->Index == portal.Partner) {
								continue;
							}

							const PortalView &inner = *portalOf[seenSlot];
							const scene::SurfaceRun &run = plan.Runs[seenSlot];
							if (run.OpaqueCount == 0) {
								continue;
							}

							if (!graph::VisiblePane(
									sub.Matrices.ViewProjection, inner.Centre, inner.First, inner.Second
								)) {
								continue;
							}

							const Impl::PortalTarget *seen = level > 0 && bank.Portals.size() >= level
																 ? &bank.Portals[level - 1].Targets[seenSlot]
																 : nullptr;

							LightingUniforms paneLighting = subLighting;
							SDL_GPUTexture *paneTexture = nullptr;

							FrameUniforms paneFrame = subFrameUniforms;
							paneFrame.ViewProjection = PortalImageSampling(
								paneFrame.ViewProjection, sub.Frame.Position, inner.Centre, inner.Normal
							);
							const auto *imported = State->FindPortalImport(
								*recording.Request.Source, targetSlot, inner, command
							);
							if (inner.ExternalImage && imported != nullptr) {
								paneLighting.Flags.z = 4.0f;
								paneLighting.Flags.w = 1.0f;
								paneTexture = imported->Texture;
								paneFrame.SurfaceViewProjection = PortalImageSampling(
									imported->Binding.Sampling, sub.Frame.Position, inner.Centre, inner.Normal
								);
								paneLighting.PaneNormal =
									glm::vec4{inner.Normal.X, inner.Normal.Y, inner.Normal.Z, 0};
							} else if (!inner.ExternalImage && seen != nullptr && seen->Colour != nullptr) {
								// 2 is the screen-position lookup - see `opaque.frag`.
								paneLighting.Flags.z = 2.0f;
								paneTexture = seen->Colour;
								paneLighting.PaneNormal =
									glm::vec4{inner.Normal.X, inner.Normal.Y, inner.Normal.Z, 0.0f};
							} else {
								// **The terminus, and it is a shade rather than the
								// pane's own material.** This is the deepest level the
								// recursion goes to, so a hole seen here has nothing
								// behind it - and a lit grey slab at the end of a
								// corridor of holes reads as a wall somebody built,
								// which is the one thing the corridor is trying not to
								// look like. CodeParade draws pink here, deliberately
								// wrong, because their demo is about the mechanism; a
								// shipped world wants the chain to fade.
								//
								// The ambient is what it fades to, which is the far
								// room's own unlit tone and needs no second uniform to
								// say - 3 is the flat branch in `opaque.frag`.
								paneLighting.Flags.z = 3.0f;
							}

							SDL_PushGPUVertexUniformData(command, 0, &paneFrame, sizeof(paneFrame));
							result.DrawCalls += State->DrawSlots(
								command,
								pass,
								run.OpaqueFirst,
								run.OpaqueCount,
								&paneLighting,
								shadow.Texture,
								shadow.Sampler,
								paneTexture,
								State->SurfaceSampler,
								portal.TagFilter,
								result.Triangles
							);
						}

						// **`panesFollow` is false here**, because this level's panes
						// were drawn with the opaque head above - nothing follows that
						// needs the transparent pipeline bound for it.
						if (drawInterface) {
							result.DrawCalls += gameInterfaceHook->RecordWorld(
								command,
								pass,
								sub.Matrices.ViewProjection,
								sub.Frame,
								core::Color3{State->Ambient.x, State->Ambient.y, State->Ambient.z},
								core::Vector3{State->Sun.x, State->Sun.y, State->Sun.z},
								sceneWidth,
								sceneHeight,
								false,
								WorldColourTarget::Hdr
							);
						}

						recording.DrawBlendedInto(
							pass,
							subFrameUniforms,
							subLighting,
							portal.TagFilter,
							false,
							WorldColourTarget::Hdr
						);

						if (recording.ParticleCount > 0) {
							result.DrawCalls += State->DrawParticles(
								command,
								pass,
								sub.Matrices.ViewProjection,
								sub.Frame,
								result.Triangles,
								result.ParticlesDrawn,
								result.Culled,
								WorldColourTarget::Hdr
							);
						}
						if (recording.RibbonCount > 0) {
							result.DrawCalls += State->DrawRibbons(
								command,
								pass,
								sub.Matrices.ViewProjection,
								sub.Frame,
								recording.Request.RibbonRuns,
								result.Triangles,
								WorldColourTarget::Hdr
							);
						}

						if (drawInterface) {
							result.DrawCalls += gameInterfaceHook->RecordWorld(
								command,
								pass,
								sub.Matrices.ViewProjection,
								sub.Frame,
								core::Color3{State->Ambient.x, State->Ambient.y, State->Ambient.z},
								core::Vector3{State->Sun.x, State->Sun.y, State->Sun.z},
								sceneWidth,
								sceneHeight,
								true,
								WorldColourTarget::Hdr
							);
						}

						SDL_EndGPURenderPass(pass);
						result.PortalPasses++;
					}
				};

				fillLevel(
					scene::CameraMatrices{
						glm::inverse(cameraFrame.ToMatrix()), screenProjection, cameraMatrix
					},
					cameraFrame,
					portalLevels - 1,
					-1
				);

				if (!recording.CaptureSeamLights(WorldColourTarget::Display)) {
					return false;
				}
			}
			return true;
		});

		frameNodes.Set(core::Name("portal-tonemap"), [this](const graph::RunContext &context) {
			ViewRecording &recording = *this;
			Impl *const State = recording.State;
			Impl::SurfaceBank &bank = *recording.Bank;
			const auto &portalOf = recording.PortalOf;
			const bool havePortals = recording.HavePortals;
			const uint32_t portalLevels = recording.PortalLevels;
			SDL_GPUColorTargetInfo &colourTarget = recording.ColourTarget;
			const auto enterNamedPass = [&recording](
											core::Name name, SDL_GPUCommandBuffer *recordedCommand = nullptr
										) { recording.EnterNamedPass(name, recordedCommand); };
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

			enterNamedPass(context.Name);
			if (!havePortals || portalLevels == 0 || bank.Portals.size() < portalLevels) {
				return true;
			}

			Impl::PortalLevel &top = bank.Portals[portalLevels - 1];
			for (size_t index = 0; index < scene::MAX_SURFACES; index++) {
				Impl::PortalTarget &portal = top.Targets[index];
				if (portalOf[index] == nullptr || portalOf[index]->ExternalImage ||
					portal.Colour == nullptr || portal.Display == nullptr) {
					continue;
				}
				const std::array bindings = {
					SDL_GPUTextureSamplerBinding{portal.Colour, State->SurfaceSampler}
				};
				fullscreen(
					context.Name,
					State->TonemapPipeline,
					portal.Display,
					portal.Width,
					portal.Height,
					bindings,
					nullptr,
					nullptr,
					colourTarget.clear_color
				);
			}
			return true;
		});

		frameNodes.Set(core::Name("portal-overlay"), [this](const graph::RunContext &context) {
			ViewRecording &recording = *this;
			Impl *const State = recording.State;
			FrameResult &result = recording.Result;
			SDL_GPUCommandBuffer *const command = recording.Command;
			const core::CFrame &cameraFrame = recording.Request.CameraFrame;
			const bool haveInstances = recording.HaveInstances;
			Impl::SurfaceBank &bank = *recording.Bank;
			const auto &portalOf = recording.PortalOf;
			const uint32_t portalLevels = recording.PortalLevels;
			const auto &cameraRuns = recording.CameraRuns;
			const uint32_t sceneCount = recording.SceneCount;
			const LightUniforms &lightUniforms = recording.SceneLights;
			SDL_GPUDepthStencilTargetInfo &depthTarget = recording.DepthTarget;
			const FrameUniforms &frameUniforms = recording.Frame;
			const SDL_GPUViewport &sceneViewport = recording.SceneViewport;
			const SDL_Rect &sceneScissor = recording.SceneScissor;
			const auto graphEnabled = [&recording](core::Name kind) { return recording.GraphEnabled(kind); };
			const auto enterNamedPass = [&recording](
											core::Name name, SDL_GPUCommandBuffer *recordedCommand = nullptr
										) { recording.EnterNamedPass(name, recordedCommand); };
			const auto recordUploads = [&recording] { return recording.RecordUploads(); };
			const auto lightingAt = [&recording](
										const core::Vector3 &eye, float surfaceMode, float imageOpacity
									) { return recording.LightingAt(eye, surfaceMode, imageOpacity); };
			const auto shadowBinding = [&recording] { return recording.ShadowBindings(); };
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
			if (!recordUploads()) {
				return false;
			}

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
			SDL_GPUColorTargetInfo portalTarget{};
			portalTarget.texture = target.Texture;
			portalTarget.load_op = SDL_GPU_LOADOP_LOAD;
			portalTarget.store_op = SDL_GPU_STOREOP_STORE;
			portalTarget.cycle = false;
			depthTarget.load_op = SDL_GPU_LOADOP_LOAD;
			depthTarget.store_op = SDL_GPU_STOREOP_STORE;
			depthTarget.cycle = false;
			SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &portalTarget, 1, &depthTarget);
			SDL_PushGPUFragmentUniformData(command, 1, &lightUniforms, sizeof(lightUniforms));
			SDL_PushGPUFragmentUniformData(command, 2, &State->Beams, sizeof(State->Beams));
			SDL_SetGPUViewport(pass, &sceneViewport);
			SDL_SetGPUScissor(pass, &sceneScissor);

			if (haveInstances) {
				State->BindPipeline(
					pass,
					hdr ? State->HdrOpaquePipeline : State->OpaquePipeline,
					hdr ? Impl::PipelineFamily::HdrOpaque : Impl::PipelineFamily::Opaque
				);
				State->BindInstanceBuffers(pass);
				const SDL_GPUBufferBinding indexBinding{State->Meshes.Indices(), 0};
				SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
				SDL_PushGPUVertexUniformData(command, 0, &frameUniforms, sizeof(frameUniforms));

				const LightingUniforms portalLighting = lightingAt(cameraFrame.Position, 0.0f, 0.0f);
				const ShadowBinding shadow = shadowBinding();
				const auto drawPortals = [&](bool blended) {
					if (blended) {
						State->BindPipeline(
							pass,
							hdr ? State->HdrTransparentPipeline : State->TransparentPipeline,
							hdr ? Impl::PipelineFamily::HdrTransparent : Impl::PipelineFamily::Transparent
						);
					}
					for (size_t index = 0; index < scene::MAX_SURFACES; index++) {
						if (portalOf[index] == nullptr) {
							continue;
						}
						const scene::SurfaceRun &run = cameraRuns[index];
						const uint32_t count = blended ? run.BlendedCount : run.OpaqueCount;
						const uint32_t first = blended ? run.BlendedFirst : run.OpaqueFirst;
						if (count == 0) {
							continue;
						}

						const Impl::PortalTarget *captured =
							graphEnabled(core::Name("portal-capture")) && portalLevels > 0 &&
									bank.Portals.size() >= portalLevels
								? &bank.Portals[portalLevels - 1].Targets[index]
								: nullptr;
						LightingUniforms paneLighting = portalLighting;
						SDL_GPUTexture *image = nullptr;
						FrameUniforms paneFrame = frameUniforms;
						const auto &portalView = *portalOf[index];
						// The slab's face must not cover geometry before the capture plane.
						paneFrame.ViewProjection = PortalImageSampling(
							paneFrame.ViewProjection,
							cameraFrame.Position,
							portalView.Centre,
							portalView.Normal
						);
						if (!portalView.ExternalImage && portalLevels == 0 &&
							recording.Request.Source->SurfaceBudget) {
							result.SurfaceBudgetExceeded = true;
						}
						const auto *imported = State->FindPortalImport(
							*recording.Request.Source, recording.Request.TargetSlot, portalView, command
						);
						if (portalView.ExternalImage) {
							paneLighting.Flags.z = imported != nullptr ? 4.0f : 3.0f;
							paneLighting.Flags.w = 1.0f;
							paneLighting.Mirror.z = hdr ? 0.0f : 1.0f;
							paneLighting.PaneNormal =
								glm::vec4{portalView.Normal.X, portalView.Normal.Y, portalView.Normal.Z, 0};
							if (imported != nullptr) {
								image = imported->Texture;
								paneFrame.SurfaceViewProjection = PortalImageSampling(
									imported->Binding.Sampling,
									cameraFrame.Position,
									portalView.Centre,
									portalView.Normal
								);
								result.SurfaceInstances += count;
							}
						} else if (captured != nullptr &&
								   (hdr ? captured->Colour : captured->Display) != nullptr) {
							paneLighting.Flags.z = 2.0f;
							paneLighting.Flags.w = 1.0f;
							paneLighting.PaneNormal = glm::vec4{
								portalOf[index]->Normal.X,
								portalOf[index]->Normal.Y,
								portalOf[index]->Normal.Z,
								0.0f,
							};
							image = hdr ? captured->Colour : captured->Display;
							result.SurfaceInstances += count;
						}

						SDL_PushGPUVertexUniformData(command, 0, &paneFrame, sizeof(paneFrame));
						result.DrawCalls += State->DrawSlots(
							command,
							pass,
							sceneCount + first,
							count,
							&paneLighting,
							shadow.Texture,
							shadow.Sampler,
							image,
							State->SurfaceSampler,
							0,
							result.Triangles
						);
					}
				};

				drawPortals(false);
				drawPortals(true);
			}
			SDL_EndGPURenderPass(pass);
			return true;
		});
	}
}
