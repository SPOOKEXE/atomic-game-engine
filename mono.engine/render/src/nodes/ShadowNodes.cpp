// The sun's shadow map, and the portal beam atlas beside it.
//
// **One node, two render passes, and they are one node because they are one
// answer.** The sun's map says what the world occludes; the beam atlas says
// what a hole carries of that occlusion into the room on its other side. Both
// are depth-only draws of the same caster runs against different matrices, and
// there is no order in which a graph could run one without the other and be
// right. `graph::FitPortalLight` is the derivation.

#include "Primitives.hpp"
#include "ViewRecording.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/graph/Shadow.hpp>
#include <engine/scene/SurfaceCameras.hpp>

#include <algorithm>

namespace engine::render {

	void ViewRecording::RegisterShadowNodes(NodeTable &frameNodes) {
		frameNodes.Set(core::Name("shadow"), [this](const graph::RunContext &context) {
			ViewRecording &recording = *this;
			Impl *const State = recording.State;
			FrameResult &result = recording.Result;
			SDL_GPUCommandBuffer *const command = recording.Command;
			const core::CFrame &cameraFrame = recording.Request.CameraFrame;
			const core::AABB &sceneBounds = recording.SceneBounds;
			const glm::mat4 &lightViewProjection = recording.LightViewProjection;
			const uint32_t sceneReflected = recording.SceneReflected;
			const uint32_t reflectedCasters = recording.ReflectedCasters;
			const uint32_t surfaceCasters = recording.SurfaceCasters;
			const bool haveShadow = recording.HaveShadow;
			const bool havePortals = recording.HavePortals;
			const auto &portalOf = recording.PortalOf;
			const auto enterNamedPass = [&recording](
											core::Name name, SDL_GPUCommandBuffer *recordedCommand = nullptr
										) { recording.EnterNamedPass(name, recordedCommand); };
			const auto submitUploads = [&recording] { return recording.SubmitUploads(); };

			enterNamedPass(context.Name);
			if (!haveShadow) {
				return true;
			}
			if (!submitUploads()) {
				return false;
			}

			{
				ENGINE_PROFILE_CAT("shadow pass", core::ProfileCategory::Render);

				SDL_GPUDepthStencilTargetInfo shadowTarget{};
				shadowTarget.texture = State->ShadowTexture;
				shadowTarget.clear_depth = 1.0f;
				shadowTarget.load_op = SDL_GPU_LOADOP_CLEAR;

				// **Stored, unlike the colour pass's depth.** This one is read by
				// the next pass, which is the entire point of rendering it.
				shadowTarget.store_op = SDL_GPU_STOREOP_STORE;
				shadowTarget.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
				shadowTarget.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
				shadowTarget.cycle = true;

				SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, nullptr, 0, &shadowTarget);
				State->BindPipeline(pass, State->ShadowPipeline, Impl::PipelineFamily::Other);

				State->BindInstanceBuffers(pass);

				const SDL_GPUBufferBinding indexBinding{State->Meshes.Indices(), 0};
				SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

				SDL_PushGPUVertexUniformData(command, 0, &lightViewProjection, sizeof(lightViewProjection));

				// **Only the opaque part of the scene casts**, and of that only what
				// `Visual::CastShadow` left switched on. A transparent pane writing
				// full depth into the shadow map would cast a solid shadow, which is
				// the most obviously wrong thing glass can do; an opaque thing that
				// should not occlude is the case the author decides, and it arrives
				// here as the caster runs `partition casters` produced.
				//
				// Two draws because the two runs are not adjacent - the surface
				// partition sits between them. The second is empty in every scene
				// with no mirror in it, which is almost all of them.
				// Depth only, with the colour sampler and compact fragment state that
				// let clipped surfaces cast their authored silhouette. The null
				// lighting pointer selects that path.
				uint64_t shadowTriangles = 0;
				if (reflectedCasters > 0) {
					result.DrawCalls += State->DrawSlots(
						command,
						pass,
						0,
						reflectedCasters,
						nullptr,
						nullptr,
						nullptr,
						nullptr,
						nullptr,
						0,
						shadowTriangles
					);
				}
				if (surfaceCasters > 0) {
					result.DrawCalls += State->DrawSlots(
						command,
						pass,
						sceneReflected,
						surfaceCasters,
						nullptr,
						nullptr,
						nullptr,
						nullptr,
						nullptr,
						0,
						shadowTriangles
					);
				}

				SDL_EndGPURenderPass(pass);
			}

			// --- portal beams ----------------------------------------------------
			//
			// **A hole carries occlusion as well as a picture.** Both rooms already
			// have the world's sun, so what a portal transports is not light but the
			// *absence* of it: a caster standing in front of a hole darkens the floor
			// beyond it, and a body cut at the seam is shadowed by whatever shadows
			// its other half. Adding a second contribution instead would double-light
			// every floor near a doorway.
			//
			// **The casters are left where they are and the receiver is mapped
			// back**, which is the whole reason this is affordable. The obvious
			// arrangement renders the near room's casters *transformed* into the far
			// room, and that needs a second instance buffer holding a mapped copy of
			// the world. Mapping the other way needs none: a far-side fragment goes
			// through `Back` into the near room and is looked up there, where the
			// casters already are. `NON-EUCLIDEAN.md` Part V.3 is the derivation.
			//
			// **The frustum is the aperture.** `graph::FitPortalLight` fits the sides
			// of the box to the pane's own rectangle, so a fragment the beam does not
			// reach projects outside `0..1` and the lookup already reads that as lit.
			// There is no rectangle test in the shader because the matrix is one.
			if (havePortals && haveShadow && State->EnsureBeams()) {
				ENGINE_PROFILE_CAT("portal beams", core::ProfileCategory::Render);

				// The receiver holes nearest the eye, because every fragment tests
				// every live beam and four is what a corridor needs. A directional
				// beam starts at `Pane` and arrives at `Partner`; ranking the source
				// drops the incoming beam for the room the eye is actually in when
				// several pairs compete for the budget.
				struct Beam {
					const PortalView *Pane = nullptr;
					const PortalView *Partner = nullptr;
					float Distance = 0.0f;
				};

				Beam ordered[scene::MAX_SURFACES];
				size_t candidates = 0;

				for (size_t slot = 0; slot < scene::MAX_SURFACES; slot++) {
					const PortalView *const portal = portalOf[slot];
					if (portal == nullptr || portal->Partner < 0) {
						continue;
					}

					// **A pane with no partner in this frame's set carries nothing**,
					// because the map back is the partner's own warp - one map per
					// pane, and a pair's two are each other's inverse. Deriving an
					// inverse here would be a second arithmetic to get wrong.
					const PortalView *const partner = portalOf[static_cast<uint8_t>(portal->Partner)];
					if (partner == nullptr) {
						continue;
					}

					ordered[candidates++] = Beam{
						portal,
						partner,
						scene::RectangleDistance(
							partner->Centre, partner->First, partner->Second, cameraFrame.Position
						)
					};
				}

				std::sort(ordered, ordered + candidates, [](const Beam &left, const Beam &right) {
					return left.Distance < right.Distance;
				});

				if (candidates > State->MaximumBeamCandidatesWarned) {
					// **Logged rather than dropped quietly.** A shadow that stops
					// crossing when a fifth pane comes on screen reads as the feature
					// not working at all, which is a much harder thing to look for
					// than a line saying which holes were left out.
					ENGINE_WARN(
						"{} holes could carry a shadow and only {} may; the farther ones do not",
						candidates,
						MAX_PORTAL_BEAMS
					);
					State->MaximumBeamCandidatesWarned = candidates;
				}

				const auto live = static_cast<uint32_t>(std::min<size_t>(candidates, MAX_PORTAL_BEAMS));

				for (uint32_t index = 0; index < live; index++) {
					const Beam &beam = ordered[index];
					const core::Vector3 sun{State->Sun.x, State->Sun.y, State->Sun.z};

					// The receiver is carried from the far room back into this
					// pane's chart by the partner's warp. Its light ray has to take
					// that same rotation. Mapping only the position makes a turned
					// portal cast the right silhouette in the wrong direction.
					const core::Vector3 beamDirection = beam.Partner->Warp.Rotate(sun);

					State->Beams.Light[index] = graph::FitPortalLight(
						sceneBounds, beam.Pane->Centre, beam.Pane->First, beam.Pane->Second, beamDirection
					);

					State->Beams.Back[index] = scene::SeamMatrix(beam.Partner->Warp);

					State->Beams.Plane[index] = glm::vec4{
						beam.Pane->Normal.X,
						beam.Pane->Normal.Y,
						beam.Pane->Normal.Z,
						beam.Pane->Normal.Dot(beam.Pane->Centre)
					};

					// **The quadrant, once.** The lookup window, the viewport and
					// the scissor below are the same rectangle said three ways,
					// and they were written out three times until v0.19 - see
					// `BeamQuadrant`, which `tests/Primitives.cpp` checks tiles
					// the atlas exactly.
					const AtlasQuadrant quadrant = BeamQuadrant(index, SHADOW_RESOLUTION);
					State->Beams.Region[index] = quadrant.Window;

					SDL_GPUDepthStencilTargetInfo beamTarget{};
					beamTarget.texture = State->BeamTexture;
					beamTarget.clear_depth = 1.0f;

					// **The first beam clears the whole atlas and the rest load it.**
					// A clear is not confined by the viewport, so clearing per beam
					// would wipe the ones already drawn - and a quadrant nobody wrote
					// stays at the far plane, which the lookup reads as lit.
					beamTarget.load_op = index == 0 ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
					beamTarget.store_op = SDL_GPU_STOREOP_STORE;
					beamTarget.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
					beamTarget.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
					beamTarget.cycle = false;

					SDL_GPURenderPass *beamPass = SDL_BeginGPURenderPass(command, nullptr, 0, &beamTarget);
					State->BindPipeline(beamPass, State->ShadowPipeline, Impl::PipelineFamily::Other);

					const SDL_GPUViewport beamViewport{
						quadrant.X, quadrant.Y, quadrant.Width, quadrant.Height, 0.0f, 1.0f
					};
					SDL_SetGPUViewport(beamPass, &beamViewport);

					const SDL_Rect beamScissor{
						static_cast<int>(quadrant.X),
						static_cast<int>(quadrant.Y),
						static_cast<int>(quadrant.Width),
						static_cast<int>(quadrant.Height)
					};
					SDL_SetGPUScissor(beamPass, &beamScissor);

					State->BindInstanceBuffers(beamPass);

					const SDL_GPUBufferBinding beamIndices{State->Meshes.Indices(), 0};
					SDL_BindGPUIndexBuffer(beamPass, &beamIndices, SDL_GPU_INDEXELEMENTSIZE_32BIT);

					SDL_PushGPUVertexUniformData(command, 0, &State->Beams.Light[index], sizeof(glm::mat4));

					// The same caster runs the world's own shadow map draws, for the
					// same reason: a caster outside the beam is culled by the matrix
					// rather than by a list.
					uint64_t beamTriangles = 0;
					if (reflectedCasters > 0) {
						result.DrawCalls += State->DrawSlots(
							command,
							beamPass,
							0,
							reflectedCasters,
							nullptr,
							nullptr,
							nullptr,
							nullptr,
							nullptr,
							0,
							beamTriangles
						);
					}
					if (surfaceCasters > 0) {
						result.DrawCalls += State->DrawSlots(
							command,
							beamPass,
							sceneReflected,
							surfaceCasters,
							nullptr,
							nullptr,
							nullptr,
							nullptr,
							nullptr,
							0,
							beamTriangles
						);
					}

					SDL_EndGPURenderPass(beamPass);
				}

				State->Beams.Count.x = static_cast<float>(live);
			}
			return true;
		});
	}
}
