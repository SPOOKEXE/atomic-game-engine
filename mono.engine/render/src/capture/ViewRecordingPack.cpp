// The instance packing phase of a view recording: scene rows become device
// layout rows and only changed ranges are staged for upload.

#include "DisplayColour.hpp"
#include "SurfaceScale.hpp"
#include "capture/ViewRecording.hpp"
#include "presentation/GpuCommandIdentity.hpp"

#include <engine/assets/Builtin.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/graph/Cull.hpp>
#include <engine/graph/EntityFlow.hpp>
#include <engine/graph/Shadow.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/render/presentation/WorldPresentation.hpp>
#include <engine/scene/Tagging.hpp>
#include <engine/scene/cameras/ActiveCamera.hpp>
#include <engine/scene/cameras/SurfaceCameras.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <limits>
#include <sstream>
#include <vector>

namespace engine::render {
	namespace {

		template <typename Value>
		void StageRanges(
			Value *destination, std::span<const InstanceUploadRange> ranges, std::span<const Value> source
		) {
			for (const InstanceUploadRange &range : ranges) {
				std::memcpy(
					destination + range.First,
					source.data() + range.First,
					static_cast<size_t>(range.Count) * sizeof(Value)
				);
			}
		}
	}

	// The instance rows, the slot tables beside them, and the occlusion plan
	// that partitions the opaque head.
	//
	// **This is where a `CFrame` and a `Color3` become a `mat4` and an RGBA,
	// and it is the only place in the engine that happens.** A world produces
	// scene data; a device layout is this module's business. The conversion
	// updates stable host slots, then only changed packed rows are staged for the
	// resident device pool. Visibility and order are a separate uint stream.
	//
	// **A private phase of `Begin` rather than a node**, because nothing here
	// touches the device queue: the copy pass it stages for is submitted by
	// `RecordUploads`, which is what the `delta-upload` node calls.
	void ViewRecording::PackInstances() {
		ENGINE_PROFILE_CAT("pack instances", core::ProfileCategory::Render);
		Impl *const State = this->State;
		const core::CFrame &behaviourFrame = Request.VisibilityCameraFrame;
		const uint32_t uploadCount = UploadCount;
		const uint32_t sceneCount = SceneCount;
		const uint32_t cameraBase = sceneCount;
		const uint32_t ownCount = OwnCount;
		const uint32_t plainOpaque = PlainOpaque;
		const bool occlusionCulling = OcclusionCulling;
		bool &haveInstances = HaveInstances;
		Impl::SceneSlot &target = State->SlotAt(Request.TargetSlot);
		// Resident rows belong to the store they came from. A Play client view
		// shares its authority's content identity, but its rows are keyed by its
		// own world, and sharing one residency made each view release the
		// other's rows and upload them again.
		const bool ownStore = Request.Source->ParticleWorldName.IsValid();
		Impl::InstanceWorld &residentWorld = State->InstanceWorldFor(
			ownStore ? Request.Source->ParticleWorld : Request.Source->World,
			ownStore ? Request.Source->ParticleWorldName : Request.Source->WorldName
		);
		State->ActiveInstanceWorld = &residentWorld;
		InstanceResidency &residency = residentWorld.Instances;
		{
			ENGINE_PROFILE_CAT("begin instance residency", core::ProfileCategory::Render);
			residency.BeginFrame(State->FrameCounter);
		}
		{
			ENGINE_PROFILE_CAT("prepare instance metadata", core::ProfileCategory::Render);
			target.InstanceIndices.resize(uploadCount);

			State->SlotMesh.resize(uploadCount);
			State->SlotTexture.resize(uploadCount);
			State->SlotContentOwner.resize(uploadCount);
			State->SlotShaderOwner.resize(uploadCount);
			State->SlotImageGraphWorld.resize(uploadCount);
			State->SlotImageGraphFallbackWorld.resize(uploadCount);
			State->SlotNormalMap.resize(uploadCount);
			State->SlotRoughnessMap.resize(uploadCount);
			State->SlotOcclusionMap.resize(uploadCount);
			State->SlotHeightMap.resize(uploadCount);
			State->SlotMetalnessMap.resize(uploadCount);
			State->SlotEmissiveMap.resize(uploadCount);
			State->SlotPackedPbrMap.resize(uploadCount);
			State->SlotSpecularFactor.resize(uploadCount);
			State->SlotTransmissionFactor.resize(uploadCount);
			State->SlotIndexOfRefraction.resize(uploadCount);
			State->SlotThickness.resize(uploadCount);
			State->SlotThicknessMap.resize(uploadCount);
			State->SlotSecondaryRoughnessMap.resize(uploadCount);
			State->SlotSkin.resize(uploadCount);
			State->SlotSkinLobe.resize(uploadCount);
			State->SlotPackedPbrChannels.resize(uploadCount);
			State->SlotResample.resize(uploadCount);
			State->SlotShadowDetail.resize(uploadCount);
			State->SlotTwoSided.resize(uploadCount);
			State->SlotShader.resize(uploadCount);
			State->SlotTags.resize(uploadCount);
			State->SlotRig.resize(uploadCount);
			State->SlotSeam.resize(uploadCount);
			State->SlotSeamFirst.resize(uploadCount);
			State->SlotSeamSecond.resize(uploadCount);
			State->SlotSeamCentre.resize(uploadCount);
			State->SlotSeamLight.resize(uploadCount);
			State->SlotInstanceKey.resize(sceneCount);
			State->SlotInstanceCurrent.resize(sceneCount);
			State->SlotLod.assign(uploadCount, NO_LOD_DRAW);
			State->SceneSlotOfSource.resize(ownCount);
			State->LodFrame.Clear();
			State->Lod.Ready = false;
		}
		bool haveOwnSources = target.InstanceSourcesReady && target.InstanceSources.size() == ownCount;
		// Targets drawing one world share its resident rows, so a frame of another
		// target can release a slot this one retained, or this target can now draw
		// a different world. Retained slots are reused only while still held.
		if (haveOwnSources && target.InstanceSourcesGeneration != residency.Generation()) {
			ENGINE_PROFILE_CAT("resident.scene.revalidate", core::ProfileCategory::Render);
			for (const Impl::SceneSlot::InstanceSourceRow &cached : target.InstanceSources) {
				if (!residency.Holds(cached.ResidentSlot, cached.Key)) {
					haveOwnSources = false;
					target.InstanceSourcesReady = false;
					break;
				}
			}
			if (haveOwnSources) target.InstanceSourcesGeneration = residency.Generation();
		}
		const bool rebuildOwnSources = Request.Damage.Objects || !haveOwnSources;
		bool sourceOrderRetained = false;
		if (!haveOwnSources) {
			target.InstanceSources.resize(ownCount);
			target.InstanceSourcesReady = false;
		}

		const core::Name viewWorld = Request.Source->WorldName;
		const auto writeMetadata = [&](uint32_t drawSlot,
									   const scene::DrawInstance &instance,
									   const MeshEntry *mesh) {
			State->SlotMesh[drawSlot] = mesh;
			State->SlotTexture[drawSlot] = instance.Texture;
			State->SlotContentOwner[drawSlot] = Request.Source->ContentOwnerOf(instance.SourceWorld);
			State->SlotShaderOwner[drawSlot] = State->ShaderOwnerOf(*Request.Source, instance.SourceWorld);
			State->SlotImageGraphWorld[drawSlot] =
				instance.SourceWorld.IsValid() ? instance.SourceWorld : viewWorld;
			State->SlotImageGraphFallbackWorld[drawSlot] = viewWorld;
			State->SlotNormalMap[drawSlot] = instance.NormalMap;
			State->SlotRoughnessMap[drawSlot] = instance.RoughnessMap;
			State->SlotOcclusionMap[drawSlot] = instance.OcclusionMap;
			State->SlotHeightMap[drawSlot] = instance.HeightMap;
			State->SlotMetalnessMap[drawSlot] = instance.MetalnessMap;
			State->SlotEmissiveMap[drawSlot] = instance.EmissiveMap;
			State->SlotPackedPbrMap[drawSlot] = instance.PackedPbrMap;
			State->SlotSpecularFactor[drawSlot] = instance.SpecularFactor;
			State->SlotTransmissionFactor[drawSlot] = instance.TransmissionFactor;
			State->SlotIndexOfRefraction[drawSlot] = instance.IndexOfRefraction;
			State->SlotThickness[drawSlot] = instance.Thickness;
			State->SlotThicknessMap[drawSlot] = instance.ThicknessMap;
			State->SlotSecondaryRoughnessMap[drawSlot] = instance.SecondaryRoughnessMap;
			State->SlotSkin[drawSlot] = glm::vec4{
				instance.SkinShading ? 1.0f : 0.0f,
				static_cast<float>(instance.SubsurfaceProfile),
				instance.ScatterRadius,
				instance.Thickness
			};
			State->SlotSkinLobe[drawSlot] = glm::vec4{
				instance.SecondaryRoughness,
				instance.SecondarySpecular,
				static_cast<float>(instance.SecondarySpecularChannel),
				0.0f
			};
			State->SlotPackedPbrChannels[drawSlot] = glm::vec4{
				static_cast<float>(instance.RoughnessChannel),
				static_cast<float>(instance.OcclusionChannel),
				static_cast<float>(instance.HeightChannel),
				static_cast<float>(instance.MetalnessChannel),
			};
			State->SlotResample[drawSlot] = instance.Resample;
			State->SlotShadowDetail[drawSlot] =
				instance.Alpha != scene::AlphaMode::Opaque || instance.SeamNormal.MagnitudeSquared() > 0.0f;
			State->SlotTwoSided[drawSlot] = instance.Surface >= 0;
			State->SlotShader[drawSlot] = instance.Shader;
			State->SlotTags[drawSlot] = instance.TagMask;
			State->SlotRig[drawSlot] = instance.Rig;
			State->SlotSeam[drawSlot] = glm::vec4{
				instance.SeamNormal.X,
				instance.SeamNormal.Y,
				instance.SeamNormal.Z,
				instance.SeamOffset,
			};
			State->SlotSeamFirst[drawSlot] = glm::vec4{
				instance.SeamFirst.X, instance.SeamFirst.Y, instance.SeamFirst.Z, float(instance.SeamMask)
			};
			State->SlotSeamSecond[drawSlot] =
				glm::vec4{instance.SeamSecond.X, instance.SeamSecond.Y, instance.SeamSecond.Z, 0.0f};
			State->SlotSeamCentre[drawSlot] =
				glm::vec4{instance.SeamCentre.X, instance.SeamCentre.Y, instance.SeamCentre.Z, 0.0f};
			State->SlotSeamLight[drawSlot] =
				glm::vec4{instance.SeamLight.X, instance.SeamLight.Y, instance.SeamLight.Z, 0.0f};
		};
		const auto probe = [&](uint32_t drawSlot,
							   const scene::DrawInstance &instance,
							   uint32_t fallback,
							   const Impl::SceneSlot::InstanceSourceRow *cached = nullptr) {
			const MeshEntry &mesh = State->Meshes.Resolve(
				instance.Mesh,
				State->ViewMeshOwner(
					instance.Mesh,
					Request.Source->ContentOwnerOf(instance.SourceWorld),
					instance.SourceWorld.IsValid() ? instance.SourceWorld : viewWorld
				)
			);
			writeMetadata(drawSlot, instance, &mesh);

			InstanceKey &key = State->SlotInstanceKey[drawSlot];
			key = InstanceKey{
				instance.SourceWorld.IsValid() ? instance.SourceWorld : viewWorld,
				instance.Source,
				instance.Variant,
				instance.Source == 0 ? fallback + 1u : 0u,
			};
			uint32_t residentSlot = std::numeric_limits<uint32_t>::max();
			State->SlotInstanceCurrent[drawSlot] =
				cached != nullptr && cached->Key == key
					? residency.ProbeSlot(cached->ResidentSlot, key, instance, mesh)
					: residency.Probe(key, instance, mesh, residentSlot);
			if (cached != nullptr && cached->Key == key) {
				residentSlot = cached->ResidentSlot;
			}
			target.InstanceIndices[drawSlot] = residentSlot;
		};
		const auto rememberSource = [&](uint32_t source, uint32_t drawSlot) {
			target.InstanceSources[source] = {
				.Key = State->SlotInstanceKey[drawSlot],
				.Mesh = State->SlotMesh[drawSlot],
				.ResidentSlot = target.InstanceIndices[drawSlot],
			};
		};
		const auto finishResident =
			[&](uint32_t drawSlot, const scene::DrawInstance &instance, const MeshEntry &mesh) {
				if (State->SlotInstanceCurrent[drawSlot] != 0) {
					residency.Touch(target.InstanceIndices[drawSlot]);
					return;
				}
				target.InstanceIndices[drawSlot] = residency.UpsertSlot(
					target.InstanceIndices[drawSlot],
					State->SlotInstanceKey[drawSlot],
					ToGpu(instance, mesh),
					instance,
					mesh
				);
			};

		{
			ENGINE_PROFILE_CAT("resolve resident instances", core::ProfileCategory::Render);
			{
				ENGINE_PROFILE_CAT("resident.scene", core::ProfileCategory::Render);
				{
					ENGINE_PROFILE_CAT("resident.scene.order check", core::ProfileCategory::Render);
					sourceOrderRetained = target.InstanceSourceOrder == State->SceneOrder;
				}
				if (!rebuildOwnSources) {
					const bool metadataRetained =
						State->PackedMetadataTarget == Request.TargetSlot && sourceOrderRetained;
					if (!metadataRetained) {
						ENGINE_PROFILE_CAT("resident.scene.reuse copy", core::ProfileCategory::Render);
						for (uint32_t index = 0; index < State->SceneOrder.size(); index++) {
							const uint32_t source = State->SceneOrder[index];
							const Impl::SceneSlot::InstanceSourceRow &cached = target.InstanceSources[source];
							writeMetadata(index, State->SceneInstances[source], cached.Mesh);
							target.InstanceIndices[index] = cached.ResidentSlot;
							State->SceneSlotOfSource[source] = index;
						}
					} else {
						// Slot metadata, resident indices and the inverse order are all still
						// the exact prefix this target packed on its previous view.
						ENGINE_PROFILE_CAT("resident.scene.reuse retained", core::ProfileCategory::Render);
					}
					{
						ENGINE_PROFILE_CAT("resident.scene.reuse touch", core::ProfileCategory::Render);
						for (uint32_t index = 0; index < State->SceneOrder.size(); index++) {
							residency.Touch(target.InstanceIndices[index]);
						}
					}
				} else {
					// At 100,000 resident rows this probe and metadata pass measured 10.9 ms
					// serial in release. Sixteen thousand rows leaves far more work than the
					// job system's measured handover. Smaller scenes keep the same two phases
					// visible so a resident rebuild says whether lookup or packing cost it.
					constexpr size_t RESIDENT_GRAIN = 4096;
					constexpr size_t RESIDENT_PARALLEL_MINIMUM = 16'384;
					if (State->SceneOrder.size() < RESIDENT_PARALLEL_MINIMUM) {
						ENGINE_PROFILE_CAT("resident.scene.rebuild serial", core::ProfileCategory::Render);
						{
							ENGINE_PROFILE_CAT("resident.scene.serial probe", core::ProfileCategory::Render);
							for (uint32_t index = 0; index < State->SceneOrder.size(); index++) {
								const uint32_t source = State->SceneOrder[index];
								probe(
									index,
									State->SceneInstances[source],
									source,
									haveOwnSources ? &target.InstanceSources[source] : nullptr
								);
							}
						}
						{
							ENGINE_PROFILE_CAT("resident.scene.serial finish", core::ProfileCategory::Render);
							for (uint32_t index = 0; index < State->SceneOrder.size(); index++) {
								const uint32_t source = State->SceneOrder[index];
								const scene::DrawInstance &instance = State->SceneInstances[source];
								State->SceneSlotOfSource[source] = index;
								finishResident(index, instance, *State->SlotMesh[index]);
								rememberSource(source, index);
							}
						}
					} else {
						{
							ENGINE_PROFILE_CAT(
								"resident.scene.parallel probe", core::ProfileCategory::Render
							);
							parallel::Jobs::For(
								State->SceneOrder.size(),
								RESIDENT_GRAIN,
								[&](size_t begin, size_t end) {
									for (size_t index = begin; index < end; index++) {
										const uint32_t source = State->SceneOrder[index];
										probe(
											static_cast<uint32_t>(index),
											State->SceneInstances[source],
											source,
											haveOwnSources ? &target.InstanceSources[source] : nullptr
										);
									}
								},
								RESIDENT_PARALLEL_MINIMUM
							);
						}
						{
							ENGINE_PROFILE_CAT(
								"resident.scene.parallel finish", core::ProfileCategory::Render
							);
							for (uint32_t index = 0; index < State->SceneOrder.size(); index++) {
								const uint32_t source = State->SceneOrder[index];
								State->SceneSlotOfSource[source] = index;
								finishResident(index, State->SceneInstances[source], *State->SlotMesh[index]);
								rememberSource(source, index);
							}
						}
					}
					target.InstanceSourcesReady = true;
				}
				target.InstanceSourcesGeneration = residency.Generation();
				if (!sourceOrderRetained) {
					ENGINE_PROFILE_CAT("resident.scene.order store", core::ProfileCategory::Render);
					target.InstanceSourceOrder.assign(State->SceneOrder.begin(), State->SceneOrder.end());
				}
				State->PackedMetadataTarget = Request.TargetSlot;
			}
			{
				ENGINE_PROFILE_CAT("resident.foreign", core::ProfileCategory::Render);
				for (uint32_t index = ownCount; index < sceneCount; index++) {
					probe(index, State->SceneInstances[index], index);
					finishResident(index, State->SceneInstances[index], *State->SlotMesh[index]);
				}
			}
			{
				ENGINE_PROFILE_CAT("resident.camera", core::ProfileCategory::Render);
				const auto mapCameraRows = [&](size_t begin, size_t end) {
					for (size_t index = begin; index < end; index++) {
						const uint32_t source = State->DrawOrder[index];
						const uint32_t drawSlot = cameraBase + static_cast<uint32_t>(index);
						const uint32_t sceneSlot = State->SceneSlotOfSource[source];
						target.InstanceIndices[drawSlot] = target.InstanceIndices[sceneSlot];
						State->SlotMesh[drawSlot] = State->SlotMesh[sceneSlot];
						State->SlotTexture[drawSlot] = State->SlotTexture[sceneSlot];
						State->SlotContentOwner[drawSlot] = State->SlotContentOwner[sceneSlot];
						State->SlotShaderOwner[drawSlot] = State->SlotShaderOwner[sceneSlot];
						State->SlotImageGraphWorld[drawSlot] = State->SlotImageGraphWorld[sceneSlot];
						State->SlotImageGraphFallbackWorld[drawSlot] =
							State->SlotImageGraphFallbackWorld[sceneSlot];
						State->SlotNormalMap[drawSlot] = State->SlotNormalMap[sceneSlot];
						State->SlotRoughnessMap[drawSlot] = State->SlotRoughnessMap[sceneSlot];
						State->SlotOcclusionMap[drawSlot] = State->SlotOcclusionMap[sceneSlot];
						State->SlotHeightMap[drawSlot] = State->SlotHeightMap[sceneSlot];
						State->SlotMetalnessMap[drawSlot] = State->SlotMetalnessMap[sceneSlot];
						State->SlotEmissiveMap[drawSlot] = State->SlotEmissiveMap[sceneSlot];
						State->SlotPackedPbrMap[drawSlot] = State->SlotPackedPbrMap[sceneSlot];
						State->SlotPackedPbrChannels[drawSlot] = State->SlotPackedPbrChannels[sceneSlot];
						State->SlotSpecularFactor[drawSlot] = State->SlotSpecularFactor[sceneSlot];
						State->SlotTransmissionFactor[drawSlot] = State->SlotTransmissionFactor[sceneSlot];
						State->SlotIndexOfRefraction[drawSlot] = State->SlotIndexOfRefraction[sceneSlot];
						State->SlotThickness[drawSlot] = State->SlotThickness[sceneSlot];
						State->SlotThicknessMap[drawSlot] = State->SlotThicknessMap[sceneSlot];
						State->SlotSecondaryRoughnessMap[drawSlot] =
							State->SlotSecondaryRoughnessMap[sceneSlot];
						State->SlotSkin[drawSlot] = State->SlotSkin[sceneSlot];
						State->SlotSkinLobe[drawSlot] = State->SlotSkinLobe[sceneSlot];
						State->SlotResample[drawSlot] = State->SlotResample[sceneSlot];
						State->SlotShadowDetail[drawSlot] = State->SlotShadowDetail[sceneSlot];
						State->SlotTwoSided[drawSlot] = State->SlotTwoSided[sceneSlot];
						State->SlotShader[drawSlot] = State->SlotShader[sceneSlot];
						State->SlotTags[drawSlot] = State->SlotTags[sceneSlot];
						State->SlotRig[drawSlot] = State->SlotRig[sceneSlot];
						State->SlotSeam[drawSlot] = State->SlotSeam[sceneSlot];
						State->SlotSeamFirst[drawSlot] = State->SlotSeamFirst[sceneSlot];
						State->SlotSeamSecond[drawSlot] = State->SlotSeamSecond[sceneSlot];
						State->SlotSeamCentre[drawSlot] = State->SlotSeamCentre[sceneSlot];
						State->SlotSeamLight[drawSlot] = State->SlotSeamLight[sceneSlot];
					}
				};
				constexpr size_t CAMERA_ROWS_GRAIN = 4096;
				constexpr size_t CAMERA_ROWS_PARALLEL_MINIMUM = 16'384;
				parallel::Jobs::For(
					State->DrawOrder.size(), CAMERA_ROWS_GRAIN, mapCameraRows, CAMERA_ROWS_PARALLEL_MINIMUM
				);
			}
		}

		{
			ENGINE_PROFILE_CAT("build authored lod", core::ProfileCategory::Render);
			const auto instanceAt = [&](uint32_t slot) -> const scene::DrawInstance & {
				if (slot < ownCount) {
					return State->SceneInstances[State->SceneOrder[slot]];
				}
				if (slot < sceneCount) {
					return State->SceneInstances[slot];
				}
				return State->SceneInstances[State->DrawOrder[slot - cameraBase]];
			};
			for (uint32_t slot = 0; slot < uploadCount; ++slot) {
				const scene::DrawInstance &instance = instanceAt(slot);
				if (instance.LodStrategyMode == scene::LodStrategy::None || instance.LodLevels < 2) {
					continue;
				}

				std::array<const MeshEntry *, scene::LOD_LEVELS> levels{};
				levels[0] = State->SlotMesh[slot];
				uint32_t levelCount = 1;
				const uint32_t wanted = std::clamp<uint32_t>(instance.LodLevels, 1u, scene::LOD_LEVELS);
				const core::Name billboard = instance.LodBillboard;
				const core::Name billboardOwner = State->TextureContentOwnerWithFallback(
					billboard,
					State->SlotContentOwner[slot],
					State->SlotImageGraphWorld[slot],
					State->SlotImageGraphFallbackWorld[slot]
				);
				const bool billboardResident =
					billboard.IsValid() && State->Textures.Find(billboard, billboardOwner) != nullptr;
				const MeshEntry *billboardMesh = nullptr;
				scene::DrawInstance billboardInstance;
				const uint32_t meshLimit = billboardResident ? wanted - 1 : wanted;
				for (uint32_t level = 1; level < meshLimit; ++level) {
					const core::Name name = instance.LodMeshes[level - 1];
					const core::Name owner = State->ViewMeshOwner(
						name, State->SlotContentOwner[slot], State->SlotImageGraphWorld[slot]
					);
					if (!name.IsValid() || !State->Meshes.Has(name, owner)) {
						break;
					}
					levels[level] = &State->Meshes.Resolve(name, owner);
					levelCount++;
				}
				if (billboardResident && levelCount < wanted) {
					billboardMesh = &State->Meshes.Resolve(
						core::Name(assets::BuiltinName(assets::BuiltinMesh::Billboard))
					);
					billboardInstance = instance;
					billboardInstance.Frame = core::CFrame::FromMatrix(
						instance.Frame.Position, behaviourFrame.RightVector(), -behaviourFrame.LookVector()
					);
					billboardInstance.HalfExtent.Z = instance.HalfExtent.Y;
					billboardInstance.Texture = billboard;
					billboardInstance.Alpha = scene::AlphaMode::Transparency;
					// The final page is a real textured plane. The preceding mesh remains
					// selected when the authored texture has not become resident.
					levels[levelCount] = levels[levelCount - 1];
					levelCount++;
				}
				const uint32_t draw = static_cast<uint32_t>(State->LodFrame.Draws.size());
				if (AppendAuthoredLod(
						State->LodFrame,
						slot,
						instance,
						std::span<const MeshEntry *const>(levels.data(), levelCount),
						billboardResident ? billboard : core::Name{},
						billboardMesh,
						billboardResident ? &billboardInstance : nullptr
					)) {
					State->SlotLod[slot] = draw;
				}
			}
		}

		// --- the occlusion plan --------------------------------
		//
		// Each opaque slot-run is partitioned in place: the
		// instances big and near enough to occlude move to its
		// head and draw in the early phase; the tail waits on the
		// GPU test against the pyramid the early phase's depth
		// seeds. **Only resident-slot indices move.** Every per-slot
		// array is constant across a run by `SlotsShareRun`'s
		// definition, so swapping rows inside one changes no run
		// boundary and no other pass's picture - an opaque draw
		// is order-independent under the depth test.
		State->OcclusionFrame.Reset();
		if (occlusionCulling && plainOpaque > 0) {
			ENGINE_PROFILE_CAT("occlusion plan", core::ProfileCategory::Render);
			Impl::OcclusionPlan &occlusionPlan = State->OcclusionFrame;

			// Drawn as an occluder when its widest world extent
			// subtends at least this fraction of its distance -
			// roughly six degrees. Lower drafts more occluders and
			// costs early-phase overdraw; higher seeds the pyramid
			// with too little to cull against. Not measured
			// finely; revisit with a real scene if the cull rate
			// disappoints.
			constexpr float OCCLUDER_SCORE = 0.1f;

			const glm::vec3 eye{
				behaviourFrame.Position.X, behaviourFrame.Position.Y, behaviourFrame.Position.Z
			};
			const auto base = static_cast<uint32_t>(cameraBase);
			const uint32_t opaqueEnd = base + plainOpaque;

			std::vector<uint32_t> &earlyRows = occlusionPlan.EarlyRows;
			std::vector<uint32_t> &lateRows = occlusionPlan.LateRows;
			uint32_t slot = base;
			while (slot < opaqueEnd) {
				uint32_t run = 1;
				while (slot + run < opaqueEnd && State->SlotsShareRun(slot, slot + run)) {
					run++;
				}

				const MeshEntry &runMesh = *State->SlotMesh[slot];
				const glm::vec3 meshCentre{runMesh.Centre.X, runMesh.Centre.Y, runMesh.Centre.Z};
				const glm::vec3 meshExtent{runMesh.Extent.X, runMesh.Extent.Y, runMesh.Extent.Z};

				const auto runIndex = static_cast<uint32_t>(occlusionPlan.RunFirstSlot.size());
				earlyRows.clear();
				lateRows.clear();
				for (uint32_t at = slot; at < slot + run; at++) {
					const uint32_t residentSlot = target.InstanceIndices[at];
					const GpuInstance &row = residency.Row(residentSlot);

					// The world box, from the same matrix the
					// vertex shader draws with: the mesh's own box
					// mapped through the model. `ToGpu` built the
					// model to fill the part's box exactly, so
					// this bound is tight rather than approximate.
					//
					// **Rebuilt from the packed row rather than
					// carried on it.** The row holds a rotation, a
					// scale and a translation since v0.19, and
					// `ModelMatrixOf` decodes them exactly as
					// `instance.glsl` does - so the box still
					// bounds the geometry that is drawn rather than
					// the geometry that was asked for, which is a
					// quantisation error apart.
					const glm::mat4 model = ModelMatrixOf(row);
					const glm::mat3 basis{model};
					const glm::vec3 centre = glm::vec3(model * glm::vec4(meshCentre, 1.0f));
					const glm::vec3 extent = glm::abs(basis[0]) * meshExtent.x +
											 glm::abs(basis[1]) * meshExtent.y +
											 glm::abs(basis[2]) * meshExtent.z;

					const float widest = std::max(extent.x, std::max(extent.y, extent.z));
					const float away = std::max(glm::distance(centre, eye), 0.01f);
					if (widest >= away * OCCLUDER_SCORE) {
						earlyRows.push_back(residentSlot);
						occlusionPlan.EarlyInstances.push_back(
							State->SceneInstances[State->DrawOrder[at - base]]
						);
					} else {
						lateRows.push_back(residentSlot);
						occlusionPlan.LateInstances.push_back(
							State->SceneInstances[State->DrawOrder[at - base]]
						);
						occlusionPlan.CandidatePairs.emplace_back(centre, std::bit_cast<float>(runIndex));
						occlusionPlan.CandidatePairs.emplace_back(extent, std::bit_cast<float>(residentSlot));
					}
				}

				const auto earlyCount = static_cast<uint32_t>(earlyRows.size());
				std::copy(earlyRows.begin(), earlyRows.end(), target.InstanceIndices.begin() + slot);
				std::copy(
					lateRows.begin(), lateRows.end(), target.InstanceIndices.begin() + slot + earlyCount
				);

				occlusionPlan.RunFirstSlot.push_back(slot);
				occlusionPlan.RunEarly.push_back(earlyCount);
				occlusionPlan.RunCandidates.push_back(static_cast<uint32_t>(lateRows.size()));
				occlusionPlan.EarlyTotal += earlyCount;
				occlusionPlan.ArgCount += DrawArgumentCount(runMesh);
				slot += run;
			}

			occlusionPlan.RunCount = static_cast<uint32_t>(occlusionPlan.RunFirstSlot.size());
			occlusionPlan.CandidateCount = static_cast<uint32_t>(occlusionPlan.CandidatePairs.size() / 2);

			// Nothing big enough to seed the pyramid means nothing
			// can be culled, and nothing to test means nothing to
			// cull either way - both fall back to the plain draw,
			// which is what "conservative" costs in the worst case.
			occlusionPlan.Active = occlusionPlan.EarlyTotal > 0 && occlusionPlan.CandidateCount > 0 &&
								   occlusionPlan.ArgCount > 0;
		}
		residency.EndFrame();

		if (uploadCount == 0) {
			target.ResidentIndices.Plan(
				static_cast<uint32_t>(State->FrameCounter % IndexResidency::VERSIONS), {}, false
			);
			target.ResidentIndices.Acknowledge();
			Result.InstanceChunks = 0;
			Result.InstanceChunksDirty = 0;
			Result.InstanceRows = 0;
			Result.InstanceRowsDirty = 0;
			return;
		}

		{
			ENGINE_PROFILE_CAT("prepare skin palettes", core::ProfileCategory::Render);
			target.SkinOffsets.assign(std::max<uint32_t>(residency.SlotCount(), 1), UINT32_MAX);
			const auto assignSkin = [&](uint32_t drawSlot, const scene::DrawInstance &instance) {
				const uint32_t residentSlot = target.InstanceIndices[drawSlot];
				const MeshEntry *mesh = State->SlotMesh[drawSlot];
				const uint64_t end = static_cast<uint64_t>(instance.SkinFirst) + instance.SkinCount;
				// A palette may be longer than the slots a mesh names: a script rig
				// binds every bone it built, and an editable mesh counts only the
				// slots its weights reach.
				if (residentSlot < target.SkinOffsets.size() && mesh != nullptr && mesh->JointCount != 0 &&
					mesh->JointCount <= instance.SkinCount && end <= State->SceneJointFrames.size()) {
					target.SkinOffsets[residentSlot] = instance.SkinFirst;
				}
			};
			for (uint32_t drawSlot = 0; drawSlot < ownCount; drawSlot++) {
				assignSkin(drawSlot, State->SceneInstances[State->SceneOrder[drawSlot]]);
			}
			for (uint32_t drawSlot = ownCount; drawSlot < sceneCount; drawSlot++) {
				assignSkin(drawSlot, State->SceneInstances[drawSlot]);
			}
			uint64_t skinOffsetSignature = scene::MixSignature(1, target.SkinOffsets.size());
			for (const uint32_t offset : target.SkinOffsets) {
				skinOffsetSignature = scene::MixSignature(skinOffsetSignature, offset);
			}
			target.SkinOffsetsDirty = target.SkinOffsetsDirty || !target.SkinOffsetsReady ||
									  target.SkinOffsetSignature != skinOffsetSignature;
			target.SkinOffsetSignature = skinOffsetSignature;
			target.SkinOffsetsReady = true;

			target.JointWords.assign(
				std::max<size_t>(State->SceneJointFrames.size() * GPU_JOINT_WORDS, GPU_JOINT_WORDS), 0
			);
			for (size_t index = 0; index < State->SceneJointFrames.size(); index++) {
				const core::CFrame &frame = State->SceneJointFrames[index];
				const PackedRotation rotation = PackRotation(frame.Rotation());
				const size_t word = index * GPU_JOINT_WORDS;
				target.JointWords[word] = std::bit_cast<uint32_t>(frame.Position.X);
				target.JointWords[word + 1] = std::bit_cast<uint32_t>(frame.Position.Y);
				target.JointWords[word + 2] = std::bit_cast<uint32_t>(frame.Position.Z);
				target.JointWords[word + 3] = rotation.Words[0];
				target.JointWords[word + 4] = rotation.Words[1];
				target.JointWords[word + 5] = rotation.Words[2];
				target.JointWords[word + 6] = rotation.Words[3];
			}
			uint64_t jointWordSignature = scene::MixSignature(1, target.JointWords.size());
			for (const uint32_t word : target.JointWords) {
				jointWordSignature = scene::MixSignature(jointWordSignature, word);
			}
			target.JointWordsDirty = target.JointWordsDirty || !target.JointWordsReady ||
									 target.JointWordSignature != jointWordSignature;
			target.JointWordSignature = jointWordSignature;
			target.JointWordsReady = true;

			const auto ensureSkinBuffer = [&](SDL_GPUBuffer *&buffer,
											  SDL_GPUTransferBuffer *&transfer,
											  uint32_t &capacity,
											  uint32_t words,
											  const char *label,
											  bool &dirty) {
				if (buffer != nullptr && transfer != nullptr && capacity >= words) {
					return true;
				}
				uint32_t grown = capacity == 0 ? 256 : capacity;
				while (grown < words) {
					grown *= 2;
				}
				if (buffer != nullptr) {
					gpu::ReleaseBuffer(State->Device, buffer);
				}
				if (transfer != nullptr) {
					gpu::ReleaseTransferBuffer(State->Device, transfer);
				}
				SDL_GPUBufferCreateInfo bufferInfo{};
				bufferInfo.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
				bufferInfo.size = grown * sizeof(uint32_t);
				buffer = gpu::CreateBuffer(State->Device, &bufferInfo);
				SDL_GPUTransferBufferCreateInfo transferInfo{};
				transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
				transferInfo.size = bufferInfo.size;
				transfer = gpu::CreateTransferBuffer(State->Device, &transferInfo);
				if (buffer == nullptr || transfer == nullptr) {
					ENGINE_ERROR("{} buffer of {} words: {}", label, grown, SDL_GetError());
					capacity = 0;
					return false;
				}
				capacity = grown;
				dirty = true;
				return true;
			};
			if (!ensureSkinBuffer(
					target.SkinOffsetBuffer,
					target.SkinOffsetTransfer,
					target.SkinOffsetCapacity,
					static_cast<uint32_t>(target.SkinOffsets.size()),
					"skin offset",
					target.SkinOffsetsDirty
				) ||
				!ensureSkinBuffer(
					target.JointBuffer,
					target.JointTransfer,
					target.JointWordCapacity,
					static_cast<uint32_t>(target.JointWords.size()),
					"joint palette",
					target.JointWordsDirty
				)) {
				return;
			}

			{
				ENGINE_PROFILE_CAT("stage skin palettes", core::ProfileCategory::Render);
				if (target.SkinOffsetsDirty) {
					void *skinMapped =
						SDL_MapGPUTransferBuffer(State->Device, target.SkinOffsetTransfer, true);
					if (skinMapped == nullptr) {
						ENGINE_ERROR("skin offsets: SDL_MapGPUTransferBuffer: {}", SDL_GetError());
						return;
					}
					std::memcpy(
						skinMapped, target.SkinOffsets.data(), target.SkinOffsets.size() * sizeof(uint32_t)
					);
					SDL_UnmapGPUTransferBuffer(State->Device, target.SkinOffsetTransfer);
				}
				if (target.JointWordsDirty) {
					void *jointMapped = SDL_MapGPUTransferBuffer(State->Device, target.JointTransfer, true);
					if (jointMapped == nullptr) {
						ENGINE_ERROR("joint palettes: SDL_MapGPUTransferBuffer: {}", SDL_GetError());
						return;
					}
					std::memcpy(
						jointMapped, target.JointWords.data(), target.JointWords.size() * sizeof(uint32_t)
					);
					SDL_UnmapGPUTransferBuffer(State->Device, target.JointTransfer);
				}
			}
			State->SkinOffsetBuffer = target.SkinOffsetBuffer;
			State->SkinOffsetTransfer = target.SkinOffsetTransfer;
			State->JointBuffer = target.JointBuffer;
			State->JointTransfer = target.JointTransfer;
		}

		bool rowsReallocated = false;
		bool indicesReallocated = false;
		{
			ENGINE_PROFILE_CAT("ensure instance capacity", core::ProfileCategory::Render);
			if (!State->EnsureInstanceCapacity(
					residency.SlotCount(), uploadCount, rowsReallocated, indicesReallocated
				)) {
				return;
			}
			if (rowsReallocated) {
				residency.MarkAllDirty();
			}

			constexpr uint32_t METRIC_CHUNK_ROWS = 256;
			const uint32_t residentChunks =
				(residency.SlotCount() + METRIC_CHUNK_ROWS - 1) / METRIC_CHUNK_ROWS;
			if (residentWorld.MetricFrame != State->FrameCounter) {
				Result.InstanceChunks = residentChunks;
				Result.InstanceRows = residency.LiveCount();
				residentWorld.MetricFrame = State->FrameCounter;
			}
			uint32_t dirtyChunks = 0;
			uint32_t coveredChunkEnd = 0;
			for (const InstanceUploadRange &range : residency.DirtyRanges()) {
				const uint32_t first = range.First / METRIC_CHUNK_ROWS;
				const uint32_t end = (range.First + range.Count - 1) / METRIC_CHUNK_ROWS + 1;
				// Ordered ranges can share a metric chunk even though their rows do not overlap.
				dirtyChunks += end - std::max(first, coveredChunkEnd);
				coveredChunkEnd = end;
			}
			Result.InstanceChunksDirty = dirtyChunks;
			Result.InstanceRowsDirty = residency.DirtyCount();
			target.ResidentIndices.Plan(
				static_cast<uint32_t>(State->FrameCounter % IndexResidency::VERSIONS),
				target.InstanceIndices,
				indicesReallocated
			);

			if (target.ResidentIndices.DirtyCount() > 0) {
				ENGINE_PROFILE_CAT("stage instance indices", core::ProfileCategory::Render);
				void *mapped = SDL_MapGPUTransferBuffer(State->Device, State->InstanceIndexTransfer, true);
				if (mapped == nullptr) {
					ENGINE_ERROR("instance indices: SDL_MapGPUTransferBuffer: {}", SDL_GetError());
					return;
				}
				auto *staged = static_cast<uint32_t *>(mapped);
				StageRanges<uint32_t>(staged, target.ResidentIndices.DirtyRanges(), target.InstanceIndices);
				SDL_UnmapGPUTransferBuffer(State->Device, State->InstanceIndexTransfer);
			} else {
				target.ResidentIndices.Acknowledge();
			}

			haveInstances = true;

			if (!State->LodFrame.Selections.empty()) {
				LodPlan &lod = State->LodFrame;
				if (!State->EnsureLodResources(
						static_cast<uint32_t>(lod.Selections.size()),
						static_cast<uint32_t>(lod.Instances.size()),
						static_cast<uint32_t>(lod.Commands.size())
					)) {
					lod.Clear();
				} else {
					void *mapped = SDL_MapGPUTransferBuffer(State->Device, State->Lod.Transfer, true);
					if (mapped == nullptr) {
						ENGINE_ERROR("lod staging: SDL_MapGPUTransferBuffer: {}", SDL_GetError());
						lod.Clear();
					} else {
						const LodTransferLayout layout = TransferLayoutOf(lod);
						auto *bytes = static_cast<uint8_t *>(mapped);
						std::memcpy(
							bytes + layout.Selections,
							lod.Selections.data(),
							lod.Selections.size() * sizeof(GpuLodSelection)
						);
						std::memcpy(
							bytes + layout.Clusters,
							lod.Clusters.data(),
							lod.Clusters.size() * sizeof(GpuLodCluster)
						);
						std::memcpy(
							bytes + layout.Instances,
							lod.Instances.data(),
							lod.Instances.size() * sizeof(GpuInstance)
						);
						std::memcpy(
							bytes + layout.Indices, lod.Indices.data(), lod.Indices.size() * sizeof(uint32_t)
						);
						std::memcpy(
							bytes + layout.SkinOffsets,
							lod.SkinOffsets.data(),
							lod.SkinOffsets.size() * sizeof(uint32_t)
						);
						std::memcpy(
							bytes + layout.Arguments,
							lod.Commands.data(),
							lod.Commands.size() * sizeof(SDL_GPUIndexedIndirectDrawCommand)
						);
						SDL_UnmapGPUTransferBuffer(State->Device, State->Lod.Transfer);
					}
				}
			}

			// Stage what the cull reads and the indirect draws consume. Its
			// own transfer rather than a tail on the instance one, so the
			// plain path pays nothing for a feature it never authored.
			if (State->OcclusionFrame.Active) {
				Impl::OcclusionPlan &occlusionPlan = State->OcclusionFrame;
				if (!State->EnsureOcclusionResources(
						occlusionPlan.ArgCount,
						occlusionPlan.CandidateCount,
						occlusionPlan.RunCount,
						plainOpaque
					)) {
					occlusionPlan.Active = false;
				} else {
					void *staged = SDL_MapGPUTransferBuffer(State->Device, State->Occlusion.Transfer, true);
					if (staged == nullptr) {
						ENGINE_ERROR("occlusion staging: SDL_MapGPUTransferBuffer: {}", SDL_GetError());
						occlusionPlan.Active = false;
					} else {
						const auto cameraBase = static_cast<uint32_t>(sceneCount);
						auto *commands = static_cast<SDL_GPUIndexedIndirectDrawCommand *>(staged);
						auto *lateArgRuns = reinterpret_cast<uint32_t *>(
							reinterpret_cast<uint8_t *>(staged) +
							static_cast<size_t>(occlusionPlan.ArgCount) * 2 *
								sizeof(SDL_GPUIndexedIndirectDrawCommand) +
							static_cast<size_t>(occlusionPlan.CandidateCount) * 2 * sizeof(glm::vec4) +
							static_cast<size_t>(occlusionPlan.RunCount) * sizeof(uint32_t)
						);

						// The early and late commands for one draw differ
						// only in what fills their instance count and where
						// their instances start: the early phase draws the
						// run's occluder head out of the instance buffer,
						// the late phase draws the compacted survivors out
						// of the late buffer, whose slots drop `cameraBase`
						// so a view's opaque head starts it at zero.
						uint32_t argument = 0;
						for (uint32_t runIndex = 0; runIndex < occlusionPlan.RunCount; runIndex++) {
							const MeshEntry &runMesh = *State->SlotMesh[occlusionPlan.RunFirstSlot[runIndex]];
							const uint32_t firstSlot = occlusionPlan.RunFirstSlot[runIndex];
							const uint32_t lateFirst =
								firstSlot - cameraBase + occlusionPlan.RunEarly[runIndex];
							const auto emitArgs = [&](const MeshRange &range) {
								if (range.IndexCount == 0) {
									return;
								}
								commands[argument] = SDL_GPUIndexedIndirectDrawCommand{
									range.IndexCount,
									occlusionPlan.RunEarly[runIndex],
									range.FirstIndex,
									range.VertexOffset,
									firstSlot,
								};
								commands[occlusionPlan.ArgCount + argument] =
									SDL_GPUIndexedIndirectDrawCommand{
										range.IndexCount,
										0,
										range.FirstIndex,
										range.VertexOffset,
										lateFirst,
									};
								lateArgRuns[argument] = runIndex;
								argument++;
							};
							if (runMesh.Runs.empty()) {
								emitArgs(runMesh.Whole);
							} else {
								for (const MeshRange &range : runMesh.Runs) {
									emitArgs(range);
								}
							}
						}

						auto *cursor = reinterpret_cast<uint8_t *>(staged) +
									   static_cast<size_t>(occlusionPlan.ArgCount) * 2 *
										   sizeof(SDL_GPUIndexedIndirectDrawCommand);
						std::memcpy(
							cursor,
							occlusionPlan.CandidatePairs.data(),
							occlusionPlan.CandidatePairs.size() * sizeof(glm::vec4)
						);
						cursor += occlusionPlan.CandidatePairs.size() * sizeof(glm::vec4);

						auto *runTable = reinterpret_cast<uint32_t *>(cursor);
						for (uint32_t runIndex = 0; runIndex < occlusionPlan.RunCount; runIndex++) {
							runTable[runIndex] = occlusionPlan.RunFirstSlot[runIndex] - cameraBase +
												 occlusionPlan.RunEarly[runIndex];
						}
						cursor += static_cast<size_t>(occlusionPlan.RunCount) * sizeof(uint32_t) +
								  static_cast<size_t>(occlusionPlan.ArgCount) * sizeof(uint32_t);

						// The zeros the cull's atomics count up from.
						std::memset(
							cursor, 0, static_cast<size_t>(occlusionPlan.RunCount) * sizeof(uint32_t)
						);

						SDL_UnmapGPUTransferBuffer(State->Device, State->Occlusion.Transfer);
					}
				}
			}
		}
	}

}
