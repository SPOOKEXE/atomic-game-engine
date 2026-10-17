// The sun's shadow map, and the portal beam atlas beside it.
//
// **One node, two render passes, and they are one node because they are one
// answer.** The sun's map says what the world occludes; the beam atlas says
// what a hole carries of that occlusion into the room on its other side. Both
// are depth-only draws of the same caster runs against different matrices, and
// there is no order in which a graph could run one without the other and be
// right. `graph::FitPortalLight` is the derivation.

#include "PortalBeamSelection.hpp"
#include "Primitives.hpp"
#include "ViewRecording.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/graph/Shadow.hpp>
#include <engine/scene/SurfaceCameras.hpp>

#include <algorithm>

namespace engine::render {

	bool ViewRecording::RecordLocalLightShadow(const glm::mat4 &viewProjection) {
		if (!State->EnsureShadow() || State->ShadowTexture == nullptr) return false;
		SDL_GPUDepthStencilTargetInfo target{};
		target.texture = State->ShadowTexture;
		target.clear_depth = 1.0f;
		target.load_op = SDL_GPU_LOADOP_CLEAR;
		target.store_op = SDL_GPU_STOREOP_STORE;
		target.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
		target.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
		target.cycle = true;
		auto *pass = SDL_BeginGPURenderPass(Command, nullptr, 0, &target);
		if (pass == nullptr) return false;
		State->BindPipeline(pass, State->ShadowPipeline, Impl::PipelineFamily::Other);
		State->BindInstanceBuffers(pass);
		const SDL_GPUBufferBinding indices{State->Meshes.Indices(), 0};
		SDL_BindGPUIndexBuffer(pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
		SDL_PushGPUVertexUniformData(Command, 0, &viewProjection, sizeof(viewProjection));
		uint64_t triangles = 0;
		if (ReflectedCasters > 0)
			Result.DrawCalls += State->DrawSlots(
				Command, pass, 0, ReflectedCasters, nullptr, nullptr, nullptr, nullptr, nullptr, 0, triangles
			);
		if (SurfaceCasters > 0)
			Result.DrawCalls += State->DrawSlots(
				Command,
				pass,
				SceneReflected,
				SurfaceCasters,
				nullptr,
				nullptr,
				nullptr,
				nullptr,
				nullptr,
				0,
				triangles
			);
		SDL_EndGPURenderPass(pass);
		return true;
	}

	void ViewRecording::RegisterShadowNodes(NodeTable &frameNodes) {
		frameNodes.Set(core::Name("shadow"), [this](const graph::RunContext &context) {
			ViewRecording &recording = *this;
			Impl *const State = recording.State;
			FrameResult &result = recording.Result;
			SDL_GPUCommandBuffer *const command = recording.Command;
			const core::Vector3 &behaviourEye = recording.Request.VisibilityCameraFrame.Position;
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
			enterNamedPass(context.Name);
			if (!haveShadow) {
				return recording.Request.Source->ImportedDirectionalShadow == 0;
			}
			const bool seeded = recording.Request.Source->ImportedDirectionalShadow != 0;
			if (seeded) {
				auto *source = State->FindPortalShadow(recording.Request.Source->ImportedDirectionalShadow);
				if (!source || !State->RecordPortalShadowImport(command, *source)) return false;
				ENGINE_PROFILE("portal shadow seed");
				if (source->Packed) {
					if (!State->PackedShadowPipeline) return false;
					SDL_GPUDepthStencilTargetInfo target{};
					target.texture = State->ShadowTexture;
					target.load_op = SDL_GPU_LOADOP_DONT_CARE;
					target.store_op = SDL_GPU_STOREOP_STORE;
					target.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
					target.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
					// Prepared nodes reuse this scratch in queue order until one final fence.
					target.cycle = State->PortalTreeJob.Prepared == 0;
					auto *decode = SDL_BeginGPURenderPass(command, nullptr, 0, &target);
					if (!decode) return false;
					State->BindPipeline(decode, State->PackedShadowPipeline, Impl::PipelineFamily::Other);
					SDL_BindGPUFragmentStorageBuffers(decode, 0, &source->Packed, 1);
					SDL_DrawGPUPrimitives(decode, 3, 1, 0, 0);
					SDL_EndGPURenderPass(decode);
					++result.DrawCalls;
					core::Metrics::Count("render.portal_shadow.packed_seed_bytes", source->GpuBytes);
				} else {
					auto *copy = SDL_BeginGPUCopyPass(command);
					if (!copy) return false;
					SDL_GPUTextureLocation from{}, to{};
					from.texture = source->Texture;
					to.texture = State->ShadowTexture;
					SDL_CopyGPUTextureToTexture(
						copy, &from, &to, SHADOW_RESOLUTION, SHADOW_RESOLUTION, 1, false
					);
					SDL_EndGPUCopyPass(copy);
				}
				core::Metrics::Count("render.portal_shadow.seed_bytes", PORTAL_SHADOW_BYTES);
				core::Metrics::Count("render.portal_shadow.seeds", 1);
			}

			{
				ENGINE_PROFILE_CAT("shadow pass", core::ProfileCategory::Render);

				SDL_GPUDepthStencilTargetInfo shadowTarget{};
				SDL_GPURenderPass *pass = nullptr;
				{
					ENGINE_PROFILE_CAT("shadow setup", core::ProfileCategory::Render);
					shadowTarget.texture = State->ShadowTexture;
					shadowTarget.clear_depth = 1.0f;
					shadowTarget.load_op = seeded ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;

					// **Stored, unlike the colour pass's depth.** This one is read by
					// the next pass, which is the entire point of rendering it.
					shadowTarget.store_op = SDL_GPU_STOREOP_STORE;
					shadowTarget.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
					shadowTarget.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
					shadowTarget.cycle = !seeded;

					pass = SDL_BeginGPURenderPass(command, nullptr, 0, &shadowTarget);
					if (!pass) return false;
					if (!seeded) core::Metrics::Count("render.shadow.clears", 1);
					// Empty source captures need a clear without nonexistent instance buffers.
					if (reflectedCasters > 0 || surfaceCasters > 0) {
						State->BindPipeline(pass, State->ShadowPipeline, Impl::PipelineFamily::Other);
						State->BindInstanceBuffers(pass);
						const SDL_GPUBufferBinding indexBinding{State->Meshes.Indices(), 0};
						SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
						SDL_PushGPUVertexUniformData(
							command, 0, &lightViewProjection, sizeof(lightViewProjection)
						);
					}
				}

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
				{
					ENGINE_PROFILE_CAT("shadow draws", core::ProfileCategory::Render);
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
				}

				{
					ENGINE_PROFILE_CAT("shadow end", core::ProfileCategory::Render);
					SDL_EndGPURenderPass(pass);
				}
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

				// Every fragment tests every live beam, so the count remains bounded.
				// Rank a beam by the visible receivers in its mapped volume instead
				// of by the doorway position. A doorway outside the eye can still cast
				// onto visible ground.
				struct Beam {
					PortalBeamProjector Projector;
					PortalBeamRank Rank;
				};

				Beam ordered[scene::MAX_SURFACES];
				size_t candidates = 0;

				for (size_t slot = 0; slot < scene::MAX_SURFACES; slot++) {
					const PortalView *const portal = portalOf[slot];
					if (portal == nullptr || portal->Partner < 0) {
						continue;
					}

					// The partner is the source aperture in the mapped chart. A pane
					// without that aperture cannot carry source-side occlusion.
					const PortalView *const partner = portalOf[static_cast<uint8_t>(portal->Partner)];
					if (partner == nullptr) {
						continue;
					}

					const core::Vector3 sun{State->Sun.x, State->Sun.y, State->Sun.z};
					const PortalBeamProjector projector =
						PortalBeamFromPair(*portal, *partner, sceneBounds, sun);
					float influence = PortalBeamInfluenceDistanceSquared(
						projector, State->VisibleInstances, State->DrawOrder, behaviourEye
					);
					if (!std::isfinite(influence)) {
						// A visible portal can show a child view whose receivers did not
						// survive the main camera cull. Keep its beam as a conservative
						// fallback so the child view does not lose transported occlusion.
						for (const uint32_t row : State->DrawOrder) {
							if (row < State->VisibleInstances.size() &&
								State->VisibleInstances[row].Surface == portal->Index) {
								const float distance = scene::RectangleDistance(
									portal->Centre, portal->First, portal->Second, behaviourEye
								);
								influence = distance * distance;
								break;
							}
						}
					}
					if (!std::isfinite(influence)) continue;
					ordered[candidates++] = Beam{projector, {static_cast<uint32_t>(slot), influence}};
				}

				std::sort(ordered, ordered + candidates, [](const Beam &left, const Beam &right) {
					return PortalBeamRanksBefore(left.Rank, right.Rank);
				});

				if (candidates > State->MaximumBeamCandidatesWarned) {
					// **Logged rather than dropped quietly.** A shadow that stops
					// crossing when a fifth pane comes on screen reads as the feature
					// not working at all, which is a much harder thing to look for
					// than a line saying which holes were left out.
					ENGINE_WARN(
						"{} portal beams reach visible receivers and only {} may run",
						candidates,
						MAX_PORTAL_BEAMS
					);
					State->MaximumBeamCandidatesWarned = candidates;
				}

				const auto live = static_cast<uint32_t>(std::min<size_t>(candidates, MAX_PORTAL_BEAMS));

				for (uint32_t index = 0; index < live; index++) {
					const Beam &beam = ordered[index];
					// The receiver is carried from the far room back into this
					// pane's source chart by this pane's warp. The light matrix and
					// source casters are both in that chart.
					State->Beams.Light[index] = beam.Projector.Light;

					State->Beams.Back[index] = scene::SeamMatrix(beam.Projector.Back);

					State->Beams.Plane[index] = glm::vec4{
						beam.Projector.PlaneNormal.X,
						beam.Projector.PlaneNormal.Y,
						beam.Projector.PlaneNormal.Z,
						beam.Projector.PlaneOffset
					};

					// **The quadrant, once.** The lookup window, the viewport and
					// the scissor below are the same rectangle said three ways,
					// and they were written out three times until v0.19 - see
					// `BeamQuadrant`, which `tests/Primitives.cpp` checks tiles
					// the atlas exactly.
					const AtlasQuadrant quadrant = BeamQuadrant(index, PORTAL_BEAM_RESOLUTION);
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
