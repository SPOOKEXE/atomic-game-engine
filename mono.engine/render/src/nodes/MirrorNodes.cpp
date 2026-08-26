// What a pane shows, and the panes put back over the frame.
//
// **The recursion is here rather than beside the node, and that is the point of
// the split.** `fillMirror` derives every level's camera from the level above
// with `scene::ReflectCamera`, so reflections compose by construction - and it
// is the only caller of itself and of `wouldDescend`. While it lived as a
// `std::function` local to `RenderView` it was reachable from every other pass
// in the module; here nothing outside `mirror-capture` can name it.
//
// `render/AGENTS.md` carries the invariant the two nodes rest on: a pane inside
// another pane's picture is drawn from *that* pane's camera and never from the
// eye.

#include "ViewRecording.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/graph/Cull.hpp>
#include <engine/scene/SurfaceCameras.hpp>

#include <algorithm>
#include <functional>

namespace engine::render {

	void ViewRecording::RegisterMirrorNodes(NodeTable &frameNodes) {
		// --- surface pass ----------------------------------------------------
		//
		// The same scene range, from each surface camera, into that surface's
		// own texture. What a mirror shows next frame - the one-frame staleness
		// `ViewChannel` already assumed, and what breaks the dependency cycle
		// between a mirror and what it reflects.
		//
		// **One pass per surface, and each one draws the other surfaces.** A
		// mirror still may not appear in its own reflection: it sits between its
		// camera and the world and would fill the texture with itself. Every
		// *other* mirror is drawn, from the half of its pair this frame is not
		// writing - so what you see in a mirror of a mirror is one frame old per
		// bounce. There is no order that would avoid that, because each surface
		// is being rendered for the others.
		//
		// **Only the surfaces whose signature moved.** A pass that would redraw
		// the texture its slot already holds is not run: its pair keeps the
		// frame it has, its matrices keep describing the camera that drew that
		// frame, and the screen pass samples it exactly as if it had just been
		// rendered. See `SignatureOf` for what counts as a change and, more to
		// the point, what deliberately does not.
		frameNodes.Set(core::Name("mirror-capture"), [this](const graph::RunContext &context) {
			ViewRecording &recording = *this;
			Impl *const State = recording.State;
			FrameResult &result = recording.Result;
			SDL_GPUCommandBuffer *const command = recording.Command;
			FrameOverlayHook *const gameInterfaceHook = recording.Request.GameInterfaceHook;
			const size_t targetSlot = recording.Request.TargetSlot;
			const bool haveInstances = recording.HaveInstances;
			auto &accepted = recording.Accepted;
			const size_t acceptedCount = recording.AcceptedCount;
			const auto &claimed = recording.Claimed;
			Impl::SurfaceBank &bank = *recording.Bank;
			const auto &panes = recording.Panes;
			const auto &havePanes = recording.HavePanes;
			const auto &paneWidth = recording.PaneWidth;
			const auto &paneHeight = recording.PaneHeight;
			const bool anyPane = recording.AnyPane;
			const glm::mat4 &lightViewProjection = recording.LightViewProjection;
			const bool mirrorHistory = recording.MirrorHistory;
			const uint32_t surfaceBounces = recording.SurfaceBounces;
			scene::SurfaceBounceProbe &surfaceDepth = recording.SurfaceDepth;
			const bool wantSurface = recording.WantSurface;
			const double frameSeconds = recording.FrameSeconds;
			const uint64_t surfaceSignature = recording.SurfaceSignature;
			const size_t refreshCount = recording.RefreshCount;
			const uint32_t ownCount = recording.OwnCount;
			const scene::ScenePlan &plan = recording.Plan;
			const uint32_t sceneCount = recording.SceneCount;
			const LightUniforms &lightUniforms = recording.SceneLights;
			const scene::WorldLighting &currentLighting = recording.CurrentLighting;
			const uint32_t mirrorLevels = recording.MirrorLevels;
			const auto enterNamedPass = [&recording](
											core::Name name, SDL_GPUCommandBuffer *recordedCommand = nullptr
										) { recording.EnterNamedPass(name, recordedCommand); };
			const auto lightingFrom = [&recording](
										  const scene::WorldLighting &worldLighting,
										  const core::Vector3 &eye,
										  float surfaceMode,
										  float imageOpacity
									  ) {
				return recording.LightingFrom(worldLighting, eye, surfaceMode, imageOpacity);
			};
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

			// Whether one more level of the recursion would have drawn anything.
			//
			// **The whole of the automatic depth's measurement, and it asks the
			// descent's own two questions rather than an approximation of them.** A
			// pane in the draw list is not enough - most of them are behind this
			// level's camera - and a pane in the frustum is not enough either, since
			// one seen edge-on has no continuous orientation to reflect through and
			// renders nothing. Answering either of those looser questions would ask
			// for a level that comes back empty, measure one shallower for it, and
			// ask again: the depth would sit oscillating between two values for as
			// long as the viewer stood still.
			//
			// Up to sixteen frustum tests and a reflection, once per pane at the
			// bottom level only, beside a scene render each.
			const auto wouldDescend = [&](const glm::mat4 &from, const core::CFrame &frame, int8_t skip) {
				for (size_t slot = 0; slot < scene::MAX_SURFACES; slot++) {
					if (!havePanes[slot] || static_cast<int8_t>(slot) == skip) {
						continue;
					}

					const scene::SurfacePane &pane = panes[slot];
					if (!graph::VisiblePane(from, pane.Centre, pane.First, pane.Second)) {
						continue;
					}

					if (scene::ReflectCamera(pane, frame, {}).Renders) {
						return true;
					}
				}

				return false;
			};

			// One level: fill `bank.Mirrors[level][i]` for every mirror `i` this
			// camera can see, then leave them for the caller to sample.
			//
			// A `std::function` because it calls itself and captures the frame, which
			// is `fillLevel`'s arrangement below and is paid once per pane per level
			// beside a whole scene render.
			//
			// @param from    The viewer's matrices, for the per-level pane cull.
			// @param frame   Where the viewer stands, which is what gets reflected.
			// @param level   Which level of the pool this call fills.
			// @param skip    The pane this camera is *on*. Nothing sees itself in its
			//                own reflection, and descending into it would put the pane
			//                in front of its own camera.
			std::function<void(const glm::mat4 &, const core::CFrame &, uint32_t, int8_t)> fillMirror;

			fillMirror = [&](const glm::mat4 &from, const core::CFrame &frame, uint32_t level, int8_t skip) {
				for (size_t slot = 0; slot < scene::MAX_SURFACES; slot++) {
					if (!havePanes[slot] || static_cast<int8_t>(slot) == skip) {
						continue;
					}

					const scene::SurfacePane &pane = panes[slot];

					// **Per pane per level, which is what stops the cost being
					// `panes ^ depth`.** A mirror behind this level's camera costs
					// nothing, and in a room most of them are. `graph::VisiblePane` is
					// the portal pass's test unchanged - the question is the same one.
					if (!graph::VisiblePane(from, pane.Centre, pane.First, pane.Second)) {
						continue;
					}

					// **The one statement of what a mirror does to a camera**, applied
					// to this level's viewer rather than to the eye. `scene::
					// AimSurfaceCameras` calls the same function for the top level, so
					// a chain cannot drift from the screen by a sign.
					//
					// **No frustum corners handed down.** The clamp they buy is a
					// sharpness optimisation for a pane the viewer is close to, and
					// the viewer at these levels is a fitted off-axis camera whose
					// lens this pass does not carry. Unclamped is the correct image at
					// a coarser resolution, which is the right trade for a reflection
					// that is already a reflection of a reflection.
					const scene::MirrorEye eye = scene::ReflectCamera(pane, frame, {});
					if (!eye.Renders) {
						continue;
					}

					const scene::CameraMatrices matrices =
						scene::ResolveSurfaceCamera(eye.Frame, scene::SurfaceProjection(eye.Lens, eye.Frame));

					// **Deeper first**, so this level's own draws can sample what the
					// level below just wrote. The pool is per level, so the targets
					// filled here survive exactly until this loop has finished with
					// them.
					//
					// **At the bottom the same question is asked and not acted on**,
					// which is what tells the next frame whether the budget was the
					// thing that stopped it. This pane is about to be drawn flat
					// inside its own picture; whether that is the end of the chain or
					// the end of the allowance is exactly `wouldDescend`.
					if (level > 0) {
						fillMirror(matrices.ViewProjection, eye.Frame, level - 1, static_cast<int8_t>(slot));
					} else {
						surfaceDepth.Deeper =
							surfaceDepth.Deeper ||
							wouldDescend(matrices.ViewProjection, eye.Frame, static_cast<int8_t>(slot));
					}

					// **The authored size, with no screen-coverage scaling.** A pane's
					// footprint on screen decides how sharp the *top* level has to be
					// - `SurfaceScale` - and these are levels inside that one, where a
					// pane covers a fraction of a fraction. Scaling them by the top
					// pane's coverage would allocate the deepest, smallest images at
					// the highest resolution in the frame.
					Impl::MirrorTarget *target =
						State->EnsureMirror(targetSlot, level, slot, paneWidth[slot], paneHeight[slot]);
					if (target == nullptr) {
						continue;
					}

					// **Not cycled, unlike a surface slot.** A level is written once
					// and sampled once, by the pass above it, in that order - see
					// `Impl::PortalTarget`, where cycling anyway made the device hang
					// more often rather than less.
					SDL_GPURenderPass *const pass =
						openScenePass(target->Colour, target->Depth, false, nullptr, lightUniforms);

					const LightingUniforms levelLighting = lightingAt(eye.Frame.Position, 0.0f, 1.0f);

					const FrameUniforms levelFrame{
						matrices.ViewProjection,
						lightViewProjection,
						glm::mat4{1.0f},
					};
					SDL_PushGPUVertexUniformData(command, 0, &levelFrame, sizeof(levelFrame));

					drawWorldInto(pass, levelLighting, pane.TagFilter);

					// The other panes, put back one at a time, each sampling the level
					// below. At the recursion bound it samples that pane's completed
					// image from the prior frame. This preserves a continuing corridor
					// without adding another same-frame scene draw. A pane with no
					// history yet draws as its own lit material for the first frame.
					//
					// **The opaque runs only, exactly as the portal pass does it.** A
					// pane that went transparent left the opaque head, and
					// `drawBlendedInto` excludes the panes in the tail - so a *faded*
					// mirror inside a deep reflection shows as glass rather than as a
					// reflection. The top level still composites it, which is where
					// anybody would notice; paying a level of recursion for the second
					// bounce of a pane somebody has faded is not a trade this makes.
					const ShadowBinding shadow = shadowBinding();

					for (size_t seen = 0; seen < scene::MAX_SURFACES; seen++) {
						if (seen == slot) {
							continue;
						}

						const scene::SurfaceRun &run = plan.Runs[seen];
						if (run.OpaqueCount == 0) {
							continue;
						}

						const Impl::MirrorTarget *below = level > 0 && bank.Mirrors.size() >= level
															  ? &bank.Mirrors[level - 1].Targets[seen]
															  : nullptr;

						LightingUniforms paneLighting = levelLighting;
						SDL_GPUTexture *paneTexture = nullptr;

						if (below != nullptr && below->Ready) {
							// 1 is the projected-image branch - see `opaque.frag`.
							paneLighting.Flags.z = 1.0f;
							paneLighting.Flags.w = 1.0f;

							const FrameUniforms seenFrame{
								matrices.ViewProjection,
								lightViewProjection,
								below->Sampling,
							};
							SDL_PushGPUVertexUniformData(command, 0, &seenFrame, sizeof(seenFrame));
							paneTexture = below->Colour;
						} else if (mirrorHistory && bank.Surfaces[seen].Ready) {
							const Impl::SurfaceSlotState &history = bank.Surfaces[seen];
							paneLighting = lightingAt(eye.Frame.Position, 1.0f, history.ImageOpacity);
							paneLighting.Mirror.x = static_cast<float>(history.Effect);
							const FrameUniforms historyFrame{
								matrices.ViewProjection,
								lightViewProjection,
								history.Sampling,
							};
							SDL_PushGPUVertexUniformData(command, 0, &historyFrame, sizeof(historyFrame));
							paneTexture = history.Texture[history.Slot];
						} else {
							SDL_PushGPUVertexUniformData(command, 0, &levelFrame, sizeof(levelFrame));
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
							pane.TagFilter,
							result.Triangles
						);
					}

					if (drawInterface) {
						result.DrawCalls += gameInterfaceHook->RecordWorld(
							command,
							pass,
							matrices.ViewProjection,
							eye.Frame,
							core::Color3{State->Ambient.x, State->Ambient.y, State->Ambient.z},
							core::Vector3{State->Sun.x, State->Sun.y, State->Sun.z},
							paneWidth[slot],
							paneHeight[slot],
							false
						);
					}

					drawBlendedInto(pass, levelFrame, levelLighting, pane.TagFilter, false);

					if (drawInterface) {
						result.DrawCalls += gameInterfaceHook->RecordWorld(
							command,
							pass,
							matrices.ViewProjection,
							eye.Frame,
							core::Color3{State->Ambient.x, State->Ambient.y, State->Ambient.z},
							core::Vector3{State->Sun.x, State->Sun.y, State->Sun.z},
							paneWidth[slot],
							paneHeight[slot],
							true
						);
					}

					SDL_EndGPURenderPass(pass);

					// **Written after the pass has ended**, which is the invariant the
					// whole ordering rests on: a level is readable only once the draw
					// that filled it has been submitted.
					target->Sampling = matrices.ViewProjection;
					target->Ready = true;
					result.SurfacePasses++;

					// **Counted from the eye rather than from the pool.** The pool
					// index runs the other way - `mirrorLevels - 1` is the level
					// nearest the screen and zero is the deepest - and what the next
					// frame's arithmetic needs is a depth, with the surface pass
					// itself as one.
					surfaceDepth.Resolved = std::max(surfaceDepth.Resolved, mirrorLevels + 1u - level);
				}
			};

			enterNamedPass(context.Name);
			if (wantSurface && haveInstances && sceneCount > 0 && refreshCount > 0) {
				ENGINE_PROFILE_CAT("surface pass", core::ProfileCategory::Render);

				// **The whole pass, run once per bounce, which is how depth used to be
				// had - and is not how it is had any more where a pane carries its
				// rectangle.** `D00112`'s remaining half was that a surface samples the
				// *other* surfaces from the textures they had last frame, so a chain
				// resolved over frames rather than within one. Iterating fixed that:
				// bounce zero draws every surface sampling last frame's neighbours, the
				// flip makes bounce zero's output the read side, and bounce one
				// therefore samples *this* frame's neighbours.
				//
				// **What it never fixed is the viewpoint, and no number of runs
				// could.** Every bounce redrew the same eye-derived cameras. The
				// pictures got fresher and stayed taken from where nobody was standing,
				// which is the flat slab `fillMirror` above exists to remove.
				//
				// So: **one run when the panes carry rectangles**, because the depth is
				// the recursion's now and running the whole pass again would only
				// redraw the same answer at the same viewpoints. A view with no
				// rectangle - a camera parented to the world, or a cross-world pane -
				// still resolves its chain by iterating, which is what it always did
				// and is still the best available for it: nothing here can reflect a
				// camera through a pane it was never told about.
				//
				// **The ping-pong stays either way.** With the recursion nothing
				// samples a slot being written, so the pair is not load-bearing for
				// mirrors - but the screen pass and the iterating path both still read
				// `Slot ^ 1`, and a surface skipped this frame must keep the matrices
				// that drew what it holds.
				//
				// **`graph::VisibleSurfaces` is given `surfaceBounces` regardless**,
				// above, where `cullRounds` is computed - and it must be. That decides
				// how many levels of surface-seen-in-surface are *marked visible*, and
				// a level the recursion draws without being marked is a level culled,
				// which is what made a mirror's deeper reflections vanish as the viewer
				// turned. The two numbers describe the same depth by two routes.
				const uint32_t bounces =
					anyPane ? 1u : (acceptedCount > 1 ? std::max(surfaceBounces, 1u) : 1u);

				for (uint32_t bounce = 0; bounce < bounces; bounce++) {

					// **Flipped for every refreshing surface before the first pass runs,
					// not inside the loop.** A surface pass samples the other surfaces'
					// read slots, so every slot has to have finished flipping before any
					// of them is read - flipping inside the loop would have the second
					// pass sample the first surface's *new* texture, which is this
					// frame's half-drawn image and the exact self-reference the pair
					// exists to make impossible.
					//
					// **A skipped surface does not flip, and its matrices do not move.**
					// Both halves of that are one fact: the slot still holds the frame it
					// held, so `ViewProjection` must still be the camera that drew it and
					// `PreviousViewProjection` the one before. Advancing either for a
					// surface that did not render would project a texture with a camera
					// that never took it - a reflection sliding across a pane that
					// nothing in the scene is moving, which is the hardest possible
					// version of this bug to attribute.
					for (size_t index = 0; index < acceptedCount; index++) {
						if (!accepted[index].Refresh) {
							continue;
						}

						Impl::SurfaceSlotState &state = bank.Surfaces[accepted[index].Index];
						state.PreviousViewProjection = state.ViewProjection;
						state.PreviousSampling = state.Sampling;
						state.ViewProjection = accepted[index].ViewProjection;
						state.Sampling = accepted[index].Sampling;
						state.Slot ^= 1u;
					}

					for (size_t index = 0; index < acceptedCount; index++) {
						if (!accepted[index].Refresh) {
							continue;
						}

						const size_t self = accepted[index].Index;

						// **Taken here rather than at each draw**, because `drawMirrors`
						// below shadows `index` with its own loop over surfaces - so
						// `accepted[index]` inside it would name a different view
						// entirely, and the filter would silently be another surface's.
						const uint32_t surfaceFilter = accepted[index].View->TagFilter;
						Impl::SurfaceSlotState &state = bank.Surfaces[self];

						// **The levels below this one, filled from *this* camera and
						// not from the eye.** That is the whole of the fix: every pane
						// this pass is about to draw needs a picture taken from where
						// this camera stands, and `fillMirror` is what takes it.
						//
						// **Here rather than once for the frame**, because the pool is
						// per level and not per level per parent: the targets filled
						// now are consumed by the draws a few lines down and then
						// overwritten by the next pane's descent. Hoisting this out of
						// the loop would give every pane the last one's reflections.
						//
						// **And with no levels below it, the same question is asked
						// here instead**, because one bounce is a depth the automatic
						// rule has to be able to climb out of: a corridor at one level
						// draws every inner pane flat, and nothing else in the frame
						// would say that a second level had anything to show.
						if (mirrorLevels > 0 && havePanes[self]) {
							fillMirror(
								accepted[index].ViewProjection,
								accepted[index].View->Frame,
								mirrorLevels - 1,
								static_cast<int8_t>(self)
							);
						} else if (havePanes[self]) {
							surfaceDepth.Deeper = surfaceDepth.Deeper || wouldDescend(
																			 accepted[index].ViewProjection,
																			 accepted[index].View->Frame,
																			 static_cast<int8_t>(self)
																		 );
						}

						// **Cycled**, because this half of the pair is written here and
						// read by the screen pass in the same frame - see
						// `openScenePass` for what the other caller does instead. The
						// whole target is the pass, so there is no viewport to set.
						const SurfaceView &capturedView = *accepted[index].View;
						const scene::WorldLighting &surfaceWorldLighting =
							capturedView.OverrideLighting ? capturedView.Lighting : currentLighting;
						const LightUniforms surfaceLights =
							capturedView.OverrideLighting
								? ToGpu(std::span<const SceneLight>(capturedView.Lights))
								: lightUniforms;
						SDL_GPURenderPass *const pass = openScenePass(
							state.Texture[state.Slot], state.Depth, true, nullptr, surfaceLights
						);

						// **Shadowed, and pointedly not surfaced.** The mirror's own view
						// gets the shadow map, so what it reflects is lit the way the
						// screen lights it.
						//
						// **`Flags.z` is zero for the world, and it has to be.** It means
						// "this draw samples a surface texture instead of its own tint",
						// and it is set below for exactly the mirror runs. Setting it for
						// the whole pass is what made the floor sample the previous
						// frame's reflection and come out as the clear colour wherever
						// that projection landed on untouched texels - a black wedge in
						// the mirror that survived deleting every caster, the frame and
						// the near-plane hack, and moved when the camera was re-aimed but
						// not when the floor was.
						const core::Vector3 surfaceEye = capturedView.Frame.Position;
						// **Every draw in this pass leaves display-encoded**, because
						// every one of them lands in the surface texture and the pane
						// reads that back as a display colour. See `Encode` in
						// `opaque.frag` for the round trip and the measurement.
						LightingUniforms surfaceLighting =
							lightingFrom(surfaceWorldLighting, surfaceEye, 0.0f, 1.0f);
						surfaceLighting.Mirror.z = 1.0f;

						const ShadowBinding shadow = shadowBinding();

						// **The samplers are bound per draw now rather than per run**,
						// because the third one - the colour map - changes with the
						// mesh being drawn and the other two do not. `DrawSlots` binds
						// all three together; what is left here is remembering which
						// surface texture the next draws should sample.
						SDL_GPUTexture *surfaceTexture = nullptr;
						const auto bindSurface = [&](SDL_GPUTexture *texture) { surfaceTexture = texture; };

						const FrameUniforms worldFrame{
							state.ViewProjection,
							lightViewProjection,
							glm::mat4{1.0f},
						};

						const auto plainly = [&]() {
							SDL_PushGPUVertexUniformData(command, 0, &worldFrame, sizeof(worldFrame));
							bindSurface(nullptr);
						};

						plainly();

						// **Another world's instances, when the host handed some over.**
						// `SurfaceView::InstanceCount` says why this bypasses the plan:
						// the plan partitions *this* world's draw list and knows nothing
						// about the tail behind it, so a foreign surface is one plain run
						// and no mirror runs at all. That is what lets a portal in one
						// world show a live second world.
						//
						// **`ownCount` is what turns the host's index into a slot.** The
						// host counts from zero within its own `foreign` list, because
						// nothing outside this call knows where this world's rows end or
						// that the plan reordered them.
						//
						// **`continue`, so the plan-driven draws below are skipped
						// entirely.** Drawing both would put this world's floor into the
						// far world's picture, which reads as the two rooms bleeding
						// into each other.
						if (accepted[index].View->InstanceCount > 0) {
							result.DrawCalls += State->DrawSlots(
								command,
								pass,
								ownCount + accepted[index].View->InstanceFirst,
								accepted[index].View->InstanceCount,
								&surfaceLighting,
								shadow.Texture,
								shadow.Sampler,
								surfaceTexture,
								State->SurfaceSampler,
								surfaceFilter,
								result.Triangles
							);

							// **Ended and left for the sweep below to mark ready**, which
							// is the invariant this must not shortcut: a surface written
							// this frame may not be sampled as another surface's
							// "previous" within the same frame.
							SDL_EndGPURenderPass(pass);
							continue;
						}

						drawWorldInto(pass, surfaceLighting, surfaceFilter);

						// **Every mirror except this one, one draw each.** `self` is
						// skipped because nothing sees itself in its own reflection -
						// drawing it would fill this texture with the pane it belongs
						// to, and the mirror would show itself rather than the room.
						//
						// A surface that has no frame yet, or that no camera is
						// rendering this frame, is drawn **plainly** rather than skipped.
						// A pane that vanishes until its mirror warms up is worse than
						// one that is briefly its own colour, and a pane naming an index
						// nothing renders is a scene mistake that should be visible as a
						// flat pane rather than as a hole in the geometry.
						//
						// Sampled draws project with the matrix that *rendered* the
						// texture being read - `PreviousViewProjection`, not the one
						// just resolved - because the image is a frame old and
						// projecting it with a fresh camera slides it across the pane.
						//
						// **And the recursion's level is preferred to the slot's own
						// texture whenever there is one.** The slot holds the pane as
						// the *eye* sees it; the level holds it as this camera sees it,
						// which is the only one of the two that belongs in this
						// picture. The slot is still the fallback for a pane the
						// recursion could not reach - off screen from here, edge-on, or
						// a camera with no rectangle at all - because a stale
						// reflection of a reflection is a better answer than a blank
						// one.
						const auto drawMirrors = [&](bool blended) {
							for (size_t index = 0; index < scene::MAX_SURFACES; index++) {
								if (index == self) {
									continue;
								}

								const scene::SurfaceRun &run = plan.Runs[index];
								const uint32_t count = blended ? run.BlendedCount : run.OpaqueCount;
								const uint32_t first = blended ? run.BlendedFirst : run.OpaqueFirst;
								if (count == 0) {
									continue;
								}

								const Impl::MirrorTarget *const level =
									mirrorLevels > 0 && bank.Mirrors.size() >= mirrorLevels
										? &bank.Mirrors[mirrorLevels - 1].Targets[index]
										: nullptr;

								if (level != nullptr && level->Ready) {
									const Impl::SurfaceSlotState &shown = bank.Surfaces[index];

									const FrameUniforms levelFrame{
										state.ViewProjection,
										lightViewProjection,
										level->Sampling,
									};
									// Into the same surface texture, so encoded like
									// everything else here.
									LightingUniforms levelLighting =
										lightingAt(surfaceEye, 1.0f, shown.ImageOpacity);
									levelLighting.Mirror.x = static_cast<float>(shown.Effect);
									levelLighting.Mirror.z = 1.0f;

									SDL_PushGPUVertexUniformData(command, 0, &levelFrame, sizeof(levelFrame));
									bindSurface(level->Colour);

									result.DrawCalls += State->DrawSlots(
										command,
										pass,
										first,
										count,
										&levelLighting,
										shadow.Texture,
										shadow.Sampler,
										surfaceTexture,
										State->SurfaceSampler,
										surfaceFilter,
										result.Triangles
									);
									continue;
								}

								const Impl::SurfaceSlotState &shown = bank.Surfaces[index];
								if (!shown.Ready || !claimed[index]) {
									plainly();
									result.DrawCalls += State->DrawSlots(
										command,
										pass,
										first,
										count,
										&surfaceLighting,
										shadow.Texture,
										shadow.Sampler,
										surfaceTexture,
										State->SurfaceSampler,
										surfaceFilter,
										result.Triangles
									);
									continue;
								}

								const FrameUniforms mirrorFrame{
									state.ViewProjection,
									lightViewProjection,
									shown.PreviousSampling,
								};
								LightingUniforms mirrorLighting =
									lightingAt(surfaceEye, 1.0f, shown.ImageOpacity);

								// Which grade this surface's image goes through. On the
								// composite rather than on the render, so switching one
								// costs no redraw of the texture.
								mirrorLighting.Mirror.x = static_cast<float>(shown.Effect);

								// **This capture is sampled, not presented, so it has to
								// leave here display-encoded.** The pane reads the surface
								// texture as a display colour and the frame's own tonemap
								// encodes the result again; a linear capture through that
								// round trip measured 0.0588 against 0.2843 for the same
								// floor seen directly. `portal-capture` gets the same
								// treatment from a `portal-tonemap` node instead, which is
								// why this is a flag rather than something the shader does
								// unconditionally. See `Encode` in `opaque.frag`.
								mirrorLighting.Mirror.z = 1.0f;

								SDL_PushGPUVertexUniformData(command, 0, &mirrorFrame, sizeof(mirrorFrame));
								bindSurface(shown.Texture[shown.Slot ^ 1u]);

								result.DrawCalls += State->DrawSlots(
									command,
									pass,
									first,
									count,
									&mirrorLighting,
									shadow.Texture,
									shadow.Sampler,
									surfaceTexture,
									State->SurfaceSampler,
									surfaceFilter,
									result.Triangles
								);
							}
						};

						drawMirrors(false);

						if (drawInterface && accepted[index].View->InstanceCount == 0) {
							result.DrawCalls += gameInterfaceHook->RecordWorld(
								command,
								pass,
								state.ViewProjection,
								accepted[index].View->Frame,
								surfaceWorldLighting.Ambient,
								surfaceWorldLighting.Direction,
								state.Width,
								state.Height,
								false
							);
						}

						// **`panesFollow` is true here**, because the blended mirrors
						// below go onto the same pipeline - so a tail of nothing but
						// mirrors still has to have it bound.
						drawBlendedInto(pass, worldFrame, surfaceLighting, surfaceFilter, true);

						// And the blended mirrors that are not this one, last of
						// everything drawn into this texture.
						if (plan.TransparentSurfaces > 0) {
							drawMirrors(true);
						}

						if (drawInterface && accepted[index].View->InstanceCount == 0) {
							result.DrawCalls += gameInterfaceHook->RecordWorld(
								command,
								pass,
								state.ViewProjection,
								accepted[index].View->Frame,
								surfaceWorldLighting.Ambient,
								surfaceWorldLighting.Direction,
								state.Width,
								state.Height,
								true
							);
						}

						SDL_EndGPURenderPass(pass);
					}
				}

				// **Marked ready only after every bounce has run**, so a surface that
				// was written this frame cannot be sampled as another surface's
				// "previous" within the same frame. From here the screen pass may
				// sample what was just written and the next frame's surface passes
				// may sample it as their previous.
				//
				// **The signature is recorded here and not where it was computed**,
				// which is what makes a surface that failed to render try again. A
				// slot only claims to be drawn with this signature once a pass has
				// actually drawn it; storing it up front would mark a skipped or
				// abandoned surface as current and leave it holding the wrong image
				// until something else in the scene moved.
				for (size_t index = 0; index < acceptedCount; index++) {
					if (!accepted[index].Refresh) {
						continue;
					}

					Impl::SurfaceSlotState &state = bank.Surfaces[accepted[index].Index];
					state.Ready = true;
					state.Signature = surfaceSignature;

					// **Stamped here for the same reason the signature is.** A slot
					// only claims to have drawn at a time once a pass has actually
					// drawn it; stamping up front would start the interval for a
					// surface that was then skipped or abandoned, and the picture
					// would end up an interval staler than the cap promises.
					state.Drawn = frameSeconds;
					result.SurfacePasses++;

					// The surface pass is level one, so a frame that drew any
					// surface at all has resolved at least that much.
					surfaceDepth.Resolved = std::max(surfaceDepth.Resolved, 1u);
				}

				// **Written back only where the pass actually ran**, which is the
				// half that is easy to get wrong and was. A surface whose signature
				// has not moved is deliberately not redrawn, so on a still scene most
				// frames draw no surface at all - and a measurement written on those
				// frames says "nothing was resolved", which reads back as one level
				// and throws away a depth the frames that did draw had worked out. A
				// skipped pass has measured nothing rather than measured zero.
				//
				// **A render fact, and it stays one.** Nothing here reaches an
				// `ecs::Store`: next frame's depth is derived from last frame's
				// picture, which is exactly the sort of thing `AGENTS.md` rule 5
				// refuses to let into a tick. A recorded run replays byte-identically
				// whatever this measured, because the simulation never sees it.
				bank.Bounces = surfaceDepth;
			}
			return true;
		});

		frameNodes.Set(core::Name("mirror-overlay"), [this](const graph::RunContext &context) {
			ViewRecording &recording = *this;
			Impl *const State = recording.State;
			FrameResult &result = recording.Result;
			SDL_GPUCommandBuffer *const command = recording.Command;
			const core::CFrame &cameraFrame = recording.Request.CameraFrame;
			const scene::Camera &drawCamera = recording.DrawCamera;
			const uint32_t sceneWidth = recording.SceneWidth;
			const uint32_t sceneHeight = recording.SceneHeight;
			const bool haveInstances = recording.HaveInstances;
			const auto &claimed = recording.Claimed;
			Impl::SurfaceBank &bank = *recording.Bank;
			const auto &portalOf = recording.PortalOf;
			const glm::mat4 &lightViewProjection = recording.LightViewProjection;
			const uint32_t surfaceInCamera = recording.SurfaceInCamera;
			const auto &cameraRuns = recording.CameraRuns;
			const uint32_t transparentSurfaces = recording.TransparentSurfaces;
			const uint32_t sceneCount = recording.SceneCount;
			const LightUniforms &lightUniforms = recording.SceneLights;
			SDL_GPUColorTargetInfo &colourTarget = recording.ColourTarget;
			SDL_GPUDepthStencilTargetInfo &depthTarget = recording.DepthTarget;
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

			ENGINE_PROFILE_CAT("mirror overlay pass", core::ProfileCategory::Render);
			if (!recordUploads()) {
				return false;
			}
			enterNamedPass(context.Name);

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

			colourTarget.texture = target.Texture;
			colourTarget.load_op = SDL_GPU_LOADOP_LOAD;
			colourTarget.store_op = SDL_GPU_STOREOP_STORE;
			colourTarget.cycle = false;
			depthTarget.load_op = SDL_GPU_LOADOP_LOAD;
			depthTarget.store_op = SDL_GPU_STOREOP_STORE;
			depthTarget.cycle = false;

			SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &colourTarget, 1, &depthTarget);
			SDL_PushGPUFragmentUniformData(command, 1, &lightUniforms, sizeof(lightUniforms));
			SDL_PushGPUFragmentUniformData(command, 2, &State->Beams, sizeof(State->Beams));
			SDL_SetGPUViewport(pass, &sceneViewport);
			SDL_SetGPUScissor(pass, &sceneScissor);

			if (haveInstances && (surfaceInCamera > 0 || transparentSurfaces > 0)) {
				State->BindPipeline(pass, State->OpaquePipeline, Impl::PipelineFamily::Opaque);
				State->BindInstanceBuffers(pass);
				const SDL_GPUBufferBinding indexBinding{State->Meshes.Indices(), 0};
				SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

				const float aspect = static_cast<float>(sceneWidth) / static_cast<float>(sceneHeight);
				const glm::mat4 viewProjection =
					scene::ResolveCamera(cameraFrame, drawCamera, aspect).ViewProjection;
				const FrameUniforms frameUniforms{
					viewProjection,
					lightViewProjection,
					glm::mat4{1.0f},
				};
				SDL_PushGPUVertexUniformData(command, 0, &frameUniforms, sizeof(frameUniforms));

				const LightingUniforms plainLighting = lightingAt(cameraFrame.Position, 0.0f, 0.0f);
				const ShadowBinding shadow = shadowBinding();
				const bool surfaceImagesEnabled = graphEnabled(core::Name("mirror-capture"));
				LightingUniforms mirroredUniforms{};

				const auto drawMirrors = [&](bool blended) {
					for (size_t index = 0; index < scene::MAX_SURFACES; index++) {
						if (portalOf[index] != nullptr) {
							continue;
						}
						const scene::SurfaceRun &run = cameraRuns[index];
						const uint32_t count = blended ? run.BlendedCount : run.OpaqueCount;
						const uint32_t first = blended ? run.BlendedFirst : run.OpaqueFirst;
						if (count == 0) {
							continue;
						}

						const Impl::SurfaceSlotState &shown = bank.Surfaces[index];
						const LightingUniforms *paneLighting = &plainLighting;
						SDL_GPUTexture *image = nullptr;

						// **`claimed` as well as `Ready`, which is what the
						// recursive pass above already tests.** `Ready` says the
						// slot has been drawn into at some point; `claimed` says
						// something aimed at it *this* frame. A pane still naming
						// a slot whose camera has gone passes the first and fails
						// the second, and without the second it samples whatever
						// that camera last drew - a mirror deleted in the editor
						// leaving a frozen picture where it stood. Scene-side,
						// `ReleaseUnaimedSurfaces` takes the pane off the slot;
						// this makes the ghost impossible for every other reason
						// a slot stops being drawn as well.
						if (surfaceImagesEnabled && shown.Ready && claimed[index]) {
							const FrameUniforms mirrorFrame{
								viewProjection,
								lightViewProjection,
								shown.Sampling,
							};
							mirroredUniforms = lightingAt(cameraFrame.Position, 1.0f, shown.ImageOpacity);
							mirroredUniforms.Mirror.x = static_cast<float>(shown.Effect);
							SDL_PushGPUVertexUniformData(command, 0, &mirrorFrame, sizeof(mirrorFrame));
							paneLighting = &mirroredUniforms;
							image = shown.Texture[shown.Slot];
							result.SurfaceInstances += count;
						}

						result.DrawCalls += State->DrawSlots(
							command,
							pass,
							sceneCount + first,
							count,
							paneLighting,
							shadow.Texture,
							shadow.Sampler,
							image,
							State->SurfaceSampler,
							0,
							result.Triangles
						);
						SDL_PushGPUVertexUniformData(command, 0, &frameUniforms, sizeof(frameUniforms));
					}
				};

				if (surfaceInCamera > 0) {
					drawMirrors(false);
				}
				if (transparentSurfaces > 0) {
					State->BindPipeline(pass, State->TransparentPipeline, Impl::PipelineFamily::Transparent);
					drawMirrors(true);
				}
			}

			SDL_EndGPURenderPass(pass);
			return true;
		});
	}
}
