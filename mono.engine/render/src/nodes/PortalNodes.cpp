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
			const scene::Camera &drawCamera = recording.DrawCamera;
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
			const auto openScenePass = [&recording](
										   SDL_GPUTexture *colour,
										   SDL_GPUTexture *depth,
										   bool cycle,
										   const SDL_GPUViewport *viewport,
										   const LightUniforms &passLights,
										   const SDL_FColor *clearColour = nullptr
									   ) {
				return recording.OpenScenePass(colour, depth, cycle, viewport, passLights, clearColour);
			};
			const auto drawWorldInto =
				[&recording](
					SDL_GPURenderPass *pass, const LightingUniforms &plainLighting, uint32_t filter
				) { recording.DrawWorldInto(pass, plainLighting, filter); };
			const auto drawBlendedInto = [&recording](
											 SDL_GPURenderPass *pass,
											 const FrameUniforms &frame,
											 const LightingUniforms &plainLighting,
											 uint32_t filter,
											 bool panesFollow
										 ) {
				recording.DrawBlendedInto(pass, frame, plainLighting, filter, panesFollow);
			};
			const bool drawInterface = recording.DrawInterface;

			enterNamedPass(context.Name);

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
				const float portalAspect = static_cast<float>(sceneWidth) / static_cast<float>(sceneHeight);
				const glm::mat4 screenProjection =
					scene::ResolveCamera(cameraFrame, drawCamera, portalAspect).Projection;

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
						if (portal.Index == skip) {
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
						SDL_GPURenderPass *const pass = openScenePass(
							target->Colour, target->Depth, false, &portalViewport, lightUniforms
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

							if (seen != nullptr && seen->Colour != nullptr) {
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
								false
							);
						}

						drawBlendedInto(pass, subFrameUniforms, subLighting, portal.TagFilter, false);

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
								true
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

				// --- the seam light-field captures -------------------------------
				//
				// **Each mouth's far room, rendered against a lit void.** A
				// stand-in eye at the mouth's centre looks through the hole and
				// is carried by the same warp a body crosses by, so what it sees
				// is the light arriving at the seam. The clear is the world's
				// ambient rather than the fog - a lit void - and the fog is
				// pushed out of reach, so the capture holds room lighting and
				// nothing atmospheric. `deferred-lighting.frag`'s `SeamSpill`
				// projects the matching capture back out of the entrance.
				//
				// **Viewer-independent, unlike the recursion above.** Light
				// spills out of a doorway whether or not anybody is looking at
				// the pane, so this does not test `VisiblePane` - a pair costs
				// two 128x128 forward passes per frame while its mouths are
				// enabled, and a disabled mouth never reaches this loop.
				for (size_t slot = 0; slot < scene::MAX_SURFACES; slot++) {
					if (portalOf[slot] == nullptr) {
						continue;
					}
					const PortalView &portal = *portalOf[slot];

					// **The mouth's own face, and nothing about where anybody
					// is standing.** A doorway throws its light into the room it
					// opens onto, which is a fact about the doorway; the partner
					// mouth faces the other room and serves that one the same
					// way. `PortalView::Normal` is that face's normal.
					//
					// This used to derive the side from the viewer, the way
					// `subCameraFor` does. There it is right - that sub-camera is
					// genuinely placed from the eye, so its clip plane has to
					// face the way *it* looks. Here the eye is a stand-in built
					// out of `outward` a few lines down, so taking the sign from
					// the viewer made a light probe move when a player walked:
					// crossing the pane's plane flipped the whole spill
					// half-space in one frame, and `SeamSpill`'s `depth <= 0.0`
					// test then dropped the pool on one side of the pane and
					// painted it on the other. Measured on `PortalLightMix` from
					// a side-on eye, that snap moved about nine thousand lit
					// pixels for less than half a stud of travel.
					//
					// The other half of the same mistake was quieter: a mouth
					// the viewer is nowhere near took its side from the viewer
					// too, so the far room's own doorway projected into the void
					// behind itself for as long as somebody stood in the near
					// room.
					const core::Vector3 outward = portal.Normal;

					// Far enough off the plane that the oblique clip below stays
					// in front of the eye: the bias is derived from this same
					// distance, and a plane that lands behind the camera inverts
					// the frustum and captures nothing.
					constexpr float STAND_OFF = 0.5f;
					const core::Vector3 standPosition = portal.Centre + outward * STAND_OFF;
					const core::Vector3 upAxis = std::abs(outward.Y) > 0.99f
													 ? core::Vector3{0.0f, 0.0f, 1.0f}
													 : core::Vector3{0.0f, 1.0f, 0.0f};
					const core::CFrame stand =
						core::CFrame::LookAt(standPosition, standPosition - outward, upAxis);
					const core::CFrame placed = portal.Warp.Place(stand);

					// Wide and square: the capture is a light probe of a room,
					// not a picture, and a narrow lens would miss the lamps
					// standing beside the doorway.
					scene::Camera captureCamera = drawCamera;
					captureCamera.FieldOfViewRadians = 1.9f;
					captureCamera.NearPlane = 0.05f;
					const glm::mat4 captureProjection =
						scene::ResolveCamera(placed, captureCamera, 1.0f).Projection;

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
						scene::ObliqueProjection(
							captureProjection, placed, clipNormal, clipNormal.Dot(clipPoint)
						)
					);

					Impl::SeamLightTarget *seamLight = State->EnsureSeamLight(targetSlot, slot);
					if (seamLight == nullptr) {
						continue;
					}

					const SDL_GPUViewport seamViewport{
						0.0f,
						0.0f,
						static_cast<float>(seamLight->Width),
						static_cast<float>(seamLight->Height),
						0.0f,
						1.0f
					};

					// The lit void. `Ambient` is already in the linear working
					// space the pass writes, which is the space a clear on an
					// sRGB target is given in.
					const SDL_FColor voidColour{
						State->Ambient.x,
						State->Ambient.y,
						State->Ambient.z,
						1.0f,
					};

					SDL_GPURenderPass *const pass = openScenePass(
						seamLight->Colour, seamLight->Depth, false, &seamViewport, lightUniforms, &voidColour
					);

					const FrameUniforms captureUniforms{
						captureMatrices.ViewProjection,
						lightViewProjection,
						glm::mat4{1.0f},
					};
					SDL_PushGPUVertexUniformData(command, 0, &captureUniforms, sizeof(captureUniforms));

					LightingUniforms voidLighting = lightingAt(placed.Position, 0.0f, 1.0f);
					// No fog in a light probe: what falls to distance falls to
					// the void the clear already painted.
					voidLighting.Fog = glm::vec4{1.0e6f, 1.0e6f + 1.0f, 0.0f, 0.0f};

					drawWorldInto(pass, voidLighting, portal.TagFilter);
					drawBlendedInto(pass, captureUniforms, voidLighting, portal.TagFilter, false);

					SDL_EndGPURenderPass(pass);

					seamLight->Centre = glm::vec4{portal.Centre.X, portal.Centre.Y, portal.Centre.Z, 1.0f};

					// The spill reaches about a doorway's span into the room:
					// past that the window falloff has taken it below anything
					// the ambient does not already cover.
					const float reach =
						2.0f * std::max(portal.First.Magnitude() + portal.Second.Magnitude(), 1.0f);
					seamLight->Outward = glm::vec4{outward.X, outward.Y, outward.Z, reach};
					seamLight->First = glm::vec4{portal.First.X, portal.First.Y, portal.First.Z, 0.0f};
					seamLight->Second = glm::vec4{portal.Second.X, portal.Second.Y, portal.Second.Z, 0.0f};
					seamLight->Ready = true;
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
				if (portalOf[index] == nullptr || portal.Colour == nullptr || portal.Display == nullptr) {
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
			const auto submitUploads = [&recording] { return recording.SubmitUploads(); };
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
			if (!submitUploads()) {
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
				State->BindPipeline(pass, State->OpaquePipeline, Impl::PipelineFamily::Opaque);
				const SDL_GPUBufferBinding vertexBindings[] = {
					{State->Meshes.Vertices(), 0},
					{State->InstanceBuffer, 0},
				};
				SDL_BindGPUVertexBuffers(pass, 0, vertexBindings, 2);
				const SDL_GPUBufferBinding indexBinding{State->Meshes.Indices(), 0};
				SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
				SDL_PushGPUVertexUniformData(command, 0, &frameUniforms, sizeof(frameUniforms));

				const LightingUniforms portalLighting = lightingAt(cameraFrame.Position, 0.0f, 0.0f);
				const ShadowBinding shadow = shadowBinding();
				const auto drawPortals = [&](bool blended) {
					if (blended) {
						State->BindPipeline(
							pass, State->TransparentPipeline, Impl::PipelineFamily::Transparent
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
						if (captured != nullptr && captured->Display != nullptr) {
							paneLighting.Flags.z = 2.0f;
							paneLighting.Flags.w = 1.0f;
							paneLighting.PaneNormal = glm::vec4{
								portalOf[index]->Normal.X,
								portalOf[index]->Normal.Y,
								portalOf[index]->Normal.Z,
								0.0f,
							};
							image = captured->Display;
							result.SurfaceInstances += count;
						}

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
