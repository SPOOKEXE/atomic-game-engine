#include "EnvironmentModes.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/render/Animation.hpp>
#include <engine/render/WorldPresentation.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Attachments.hpp>
#include <engine/scene/CameraContinuation.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Interpolation.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Skinning.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/scene/Visibility.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>
#include <limits>
#include <numbers>
#include <string>
#include <type_traits>

namespace engine::render {
	void SelectFirstPersonBody(const ecs::Store &store, View &view) {
		view.EyeRig = 0;
		view.EyePlayer.reset();
		const auto *active = store.Resource<scene::ActiveCamera>();
		const auto *controller = store.Resource<scene::CameraController>();
		if (!active || !controller || controller->Mode != scene::CameraMode::LockFirstPerson) return;
		const auto *subject = store.Get<scene::CameraSubject>(active->Entity);
		if (!subject || !store.Has<scene::Humanoid>(subject->Target)) return;
		const auto root = scene::CameraSubjectRoot(store, active->Entity);
		if (root == ecs::NULL_ENTITY) return;
		view.EyeRig = root.Id;
		const auto player = scene::PlayerOf(store, store.ParentOf(root));
		if (const auto *identity = store.Get<scene::PlayerIdentity>(player))
			view.EyePlayer = identity->UserId;
		const auto *held = store.Resource<scene::CameraCharacterHold>();
		const auto *body = store.Resource<scene::CameraBodyPose>();
		if (held && held->Active && held->Root == root &&
			(store.Alive(held->SourceRoot) || (body && body->SourceRoot == held->SourceRoot)))
			view.EyeRig = held->SourceRoot.Id;
	}

	void ResolveEyeBody(ecs::Store &store, View &view) {
		view.EyeRig = 0;
		if (!view.EyePlayer) return;
		ENGINE_PROFILE_CAT("eye body resolve", core::ProfileCategory::Render);
		const auto *held = store.Resource<scene::CameraCharacterHold>();
		bool ambiguous = false;
		store.Each<const scene::PlayerIdentity, const scene::PlayerCharacter>(
			[&](ecs::Entity player, const scene::PlayerIdentity &identity, const scene::PlayerCharacter &) {
				if (identity.UserId != *view.EyePlayer || (held && player == held->Player)) return;
				const auto *rig = store.Get<scene::Character>(scene::CharacterOf(store, player));
				if (!rig || rig->Owner != player || !store.Alive(rig->Root)) return;
				ambiguous = ambiguous || view.EyeRig != 0;
				view.EyeRig = rig->Root.Id;
			}
		);
		if (ambiguous) view.EyeRig = 0;
	}

	namespace {
		uint64_t FoldPresentation(uint64_t signature, uint64_t word) {
			return scene::MixSignature(signature, word);
		}

		template <typename Value> uint64_t FoldPresentationObject(uint64_t signature, const Value &value) {
			static_assert(std::is_trivially_copyable_v<Value>);
			const auto bytes = std::as_bytes(std::span<const Value>(&value, 1));
			uint64_t word = 1469598103934665603ull;
			for (const std::byte byte : bytes) {
				word = (word ^ std::to_integer<uint8_t>(byte)) * 1099511628211ull;
			}
			return FoldPresentation(signature, word);
		}

		uint64_t ProjectionSignature(const View &view) {
			uint64_t signature = FoldPresentation(0, view.Projection.has_value() ? 1u : 0u);
			signature = FoldPresentation(signature, view.SurfaceBudget.has_value());
			if (view.SurfaceBudget) {
				signature = FoldPresentation(signature, view.SurfaceBudget->Depth);
				signature = FoldPresentation(signature, view.SurfaceBudget->Pixels);
			}
			if (view.Projection) {
				for (int column = 0; column < 4; column++) {
					for (int row = 0; row < 4; row++) {
						const float value = (*view.Projection)[column][row];
						signature = FoldPresentationObject(signature, value == 0 ? 0.0f : value);
					}
				}
			}
			return signature;
		}

		template <typename Value>
		uint64_t FoldPresentationSpan(uint64_t signature, std::span<const Value> values) {
			signature = FoldPresentation(signature, values.size());
			for (const Value &value : values) {
				signature = FoldPresentationObject(signature, value);
			}
			return signature;
		}
	}

	bool EnvironmentLayerPresent(const scene::WorldLighting &lighting) {
		const scene::Environment &environment = lighting.EnvironmentState;
		const EnvironmentUniformModes modes = EnvironmentModesOf(environment);
		const bool texturedSky =
			modes.Skybox == 1 &&
			(environment.Textures.Front.IsValid() || environment.Textures.Back.IsValid() ||
			 environment.Textures.Left.IsValid() || environment.Textures.Right.IsValid() ||
			 environment.Textures.Up.IsValid() || environment.Textures.Down.IsValid());
		return texturedSky || modes.Skybox == 2 || modes.Atmosphere != 0 || modes.Clouds != 0;
	}

	uint64_t ParticleVisibilitySignature(const View &view) {
		if (view.Particles.empty() && view.RibbonRuns.empty()) {
			return 0;
		}

		uint64_t signature = FoldPresentationObject(0, view.CameraFrame);
		signature = FoldPresentationObject(signature, view.Camera);
		signature = FoldPresentation(signature, ProjectionSignature(view));
		signature = FoldPresentation(signature, view.World);
		signature = FoldPresentation(signature, view.WorldName.Id());
		signature = FoldPresentation(signature, view.ParticleLayoutRevision);
		signature = FoldPresentation(signature, view.ParticleResidentRevision);
		signature = FoldPresentation(signature, view.Particles.size());
		signature = FoldPresentation(signature, view.ParticleSeams.empty() ? 0u : 1u);
		return signature;
	}

	ScenePresentationSignatures
	ScenePresentationSignaturesOf(const View &view, const ScenePresentationState &state) {
		ScenePresentationSignatures signatures;
		const uint64_t projection = ProjectionSignature(view);
		uint64_t &objects = signatures.Objects;
		const bool objectLayer = !view.Instances.empty() || view.Grid.Enabled || state.PostProcess.IsValid();
		if (objectLayer) {
			objects = scene::SignatureOf(view.Instances);
			objects = FoldPresentationSpan(objects, view.JointFrames);
			objects = FoldPresentationObject(objects, view.CameraFrame);
			objects = FoldPresentationObject(objects, view.Camera);
			objects = FoldPresentation(objects, projection);
			objects = FoldPresentation(objects, view.World);
			objects = FoldPresentation(objects, view.WorldName.Id());
			objects = FoldPresentation(objects, view.EyeRig);
			objects = FoldPresentationSpan(objects, view.EyeHiddenRows);
			objects = FoldPresentation(objects, view.Pipeline.Id());
			objects = FoldPresentationObject(objects, view.Grid);
			objects = FoldPresentation(objects, state.Animation);
			objects = FoldPresentation(objects, state.Resources);
			objects = FoldPresentation(objects, state.PostProcess.Id());
			objects = FoldPresentation(objects, state.Untextured ? 1u : 0u);
			objects = FoldPresentationSpan(objects, view.Lights);
			objects = FoldPresentationObject(objects, state.Lighting.Direction);
			objects = FoldPresentationObject(objects, state.Lighting.Ambient);
			objects = FoldPresentationObject(objects, state.Lighting.OutdoorAmbient);
			objects = FoldPresentationObject(objects, state.Lighting.Direct);
			objects = FoldPresentationObject(objects, state.Lighting.FogColor);
			objects = FoldPresentationObject(objects, state.Lighting.FogStart);
			objects = FoldPresentationObject(objects, state.Lighting.FogEnd);
			objects = FoldPresentation(objects, view.OverrideLighting ? 1u : 0u);
			if (view.OverrideLighting) {
				objects = FoldPresentationObject(objects, view.Lighting);
			}
		}

		uint64_t &environmentSignature = signatures.Environment;
		// Only the selected environment enters the pixel signature. Lower siblings
		// and providers outside Lighting cannot reach the sky node, so changing one
		// of them must not redraw an identical scene.
		const scene::Environment &environment = state.Lighting.EnvironmentState;
		const EnvironmentUniformModes environmentModes = EnvironmentModesOf(environment);
		if (!EnvironmentLayerPresent(state.Lighting)) {
			environmentSignature = 0;
		} else {
			environmentSignature = FoldPresentation(environmentSignature, projection);
			environmentSignature = FoldPresentationObject(environmentSignature, state.Lighting.Direction);
			environmentSignature = FoldPresentationObject(environmentSignature, state.Lighting.Ambient);
			environmentSignature =
				FoldPresentationObject(environmentSignature, state.Lighting.OutdoorAmbient);
			environmentSignature = FoldPresentationObject(environmentSignature, state.Lighting.Direct);
			environmentSignature =
				FoldPresentation(environmentSignature, static_cast<uint8_t>(environment.Skybox));
			if (environment.Skybox == scene::SkyboxSource::Textures) {
				environmentSignature =
					FoldPresentation(environmentSignature, environment.Textures.Enabled ? 1u : 0u);
				if (environment.Textures.Enabled) {
					for (const core::Name face :
						 {environment.Textures.Front,
						  environment.Textures.Back,
						  environment.Textures.Left,
						  environment.Textures.Right,
						  environment.Textures.Up,
						  environment.Textures.Down}) {
						environmentSignature = FoldPresentation(environmentSignature, face.Id());
					}
				}
			} else if (environment.Skybox == scene::SkyboxSource::Compute) {
				environmentSignature =
					FoldPresentation(environmentSignature, environment.SkyCompute.Enabled ? 1u : 0u);
				if (environment.SkyCompute.Enabled) {
					environmentSignature =
						FoldPresentationObject(environmentSignature, environment.SkyCompute);
				}
			}
			if (environmentModes.Atmosphere != 0) {
				environmentSignature = FoldPresentationObject(environmentSignature, environment.Air);
				if (environmentModes.Atmosphere == 2) {
					environmentSignature =
						FoldPresentationObject(environmentSignature, environment.AirCompute);
				}
			}
			if (environmentModes.Clouds != 0) {
				environmentSignature = FoldPresentationObject(environmentSignature, environment.CloudLayer);
				if (environmentModes.Clouds == 2) {
					environmentSignature =
						FoldPresentationObject(environmentSignature, environment.CloudVolume);
				}
			}
			environmentSignature = FoldPresentation(environmentSignature, view.OverrideLighting ? 1u : 0u);
			if (view.OverrideLighting) {
				environmentSignature = FoldPresentationObject(environmentSignature, view.Lighting);
			}
		}

		uint64_t &particles = signatures.Particles;
		const bool particleLayer = !view.Particles.empty() || !view.RibbonRuns.empty();
		if (particleLayer) {
			particles = FoldPresentation(particles, projection);
			particles = FoldPresentation(particles, view.ParticleRevision);
			particles = FoldPresentation(particles, view.ParticleLayoutRevision);
			particles = FoldPresentation(particles, view.ParticleResidentRevision);
			particles = FoldPresentationSpan(particles, view.RibbonRuns);
			particles = FoldPresentationSpan(particles, view.RibbonVertices);
		}

		uint64_t &portals = signatures.Portals;
		const bool eyeLayer = view.EyeImage != 0 || view.EyeImageKey.IsValid() ||
							  std::any_of(
								  view.EyeTransparentImages.begin(),
								  view.EyeTransparentImages.end(),
								  [](uint64_t image) { return image != 0; }
							  );
		const bool portalLayer =
			eyeLayer || !view.Portals.empty() || !view.Surfaces.empty() || !view.Foreign.empty();
		if (portalLayer) {
			if (eyeLayer) {
				portals = FoldPresentation(portals, view.EyeImage);
				for (const auto image : view.EyeTransparentImages)
					portals = FoldPresentation(portals, image);
				portals = FoldPresentation(portals, view.EyeImageKey.Id());
				portals = FoldPresentation(portals, view.World);
				portals = FoldPresentation(portals, view.WorldName.Id());
				portals = FoldPresentation(portals, view.Slot);
			}
			portals = FoldPresentation(portals, projection);
			portals = FoldPresentation(portals, state.SurfaceBounces);
			portals = FoldPresentation(portals, state.SurfaceLimit);
			portals = FoldPresentation(portals, scene::SignatureOf(view.Foreign));
			portals = FoldPresentationSpan(portals, view.ForeignJointFrames);

			portals = FoldPresentation(portals, view.Portals.size());
			for (const PortalView &portal : view.Portals) {
				portals = FoldPresentationObject(portals, portal.Index);
				portals = FoldPresentationObject(portals, portal.Partner);
				portals = FoldPresentationObject(portals, portal.Centre);
				portals = FoldPresentationObject(portals, portal.Normal);
				portals = FoldPresentationObject(portals, portal.First);
				portals = FoldPresentationObject(portals, portal.Second);
				portals = FoldPresentationObject(portals, portal.Warp);
				portals = FoldPresentationObject(portals, portal.TagFilter);
				portals = FoldPresentationObject(portals, portal.ExternalImage);
				portals = FoldPresentationObject(portals, portal.ImportedImage);
				portals = FoldPresentation(portals, portal.ImagePortal.Id());
			}

			portals = FoldPresentation(portals, view.Surfaces.size());
			for (const SurfaceView &surface : view.Surfaces) {
				portals = FoldPresentationObject(portals, surface.Index);
				portals = FoldPresentationObject(portals, surface.Frame);
				portals = FoldPresentationObject(portals, surface.PaneCentre);
				portals = FoldPresentationObject(portals, surface.PaneNormal);
				portals = FoldPresentationObject(portals, surface.PaneFirst);
				portals = FoldPresentationObject(portals, surface.PaneSecond);
				portals = FoldPresentationObject(portals, surface.PaneNear);
				portals = FoldPresentationObject(portals, surface.PaneFar);
				portals = FoldPresentationObject(portals, surface.Projection);
				portals = FoldPresentationObject(portals, surface.Mapping);
				portals = FoldPresentationObject(portals, surface.Width);
				portals = FoldPresentationObject(portals, surface.Height);
				portals = FoldPresentationObject(portals, surface.ImageOpacity);
				portals = FoldPresentationObject(portals, surface.Effect);
				portals = FoldPresentationObject(portals, surface.TagFilter);
				portals = FoldPresentationObject(portals, surface.FPS);
				portals = FoldPresentationObject(portals, surface.InstanceFirst);
				portals = FoldPresentationObject(portals, surface.InstanceCount);
				portals = FoldPresentationObject(portals, surface.Lighting);
				portals = FoldPresentationSpan(portals, std::span<const SceneLight>(surface.Lights));
				portals = FoldPresentation(portals, surface.OverrideLighting ? 1u : 0u);
			}
		}
		return signatures;
	}

	uint64_t ScenePresentationSignature(const View &view, const ScenePresentationState &state) {
		const ScenePresentationSignatures signatures = ScenePresentationSignaturesOf(view, state);
		return FoldPresentation(
			FoldPresentation(signatures.Objects, signatures.Particles),
			FoldPresentation(signatures.Environment, signatures.Portals)
		);
	}

	uint64_t ViewportPresentationSignature(uint32_t width, uint32_t height) {
		return FoldPresentation(FoldPresentation(0, width), height);
	}
	using engine::ecs::Entity;
	using engine::ecs::Store;
	using engine::scene::Bone;
	using engine::scene::Bounds;
	using engine::scene::CharacterLimb;
	using engine::scene::DrawInstance;
	using engine::scene::LocalTransparency;
	using engine::scene::PreviousTransform;
	using engine::scene::Rendered;
	using engine::scene::Skeleton;
	using engine::scene::SurfaceAppearance;
	using engine::scene::Tags;
	using engine::scene::Transform;
	using engine::scene::Visual;

	namespace {
		enum SourceRevision : size_t {
			TRANSFORM_REVISION,
			PREVIOUS_TRANSFORM_REVISION,
			BOUNDS_REVISION,
			VISUAL_REVISION,
			SURFACE_REVISION,
			TAGS_REVISION,
			TRANSPARENCY_REVISION,
			LIMB_REVISION,
			SKELETON_REVISION,
			BONE_REVISION,
			RENDERED_REVISION,
		};

		struct DrawSourceChanges {
			bool Pose = false;
			bool Full = false;
		};

		bool DrawableSource(const Store &store, Entity entity) {
			return store.Has<Transform>(entity) && store.Has<PreviousTransform>(entity) &&
				   store.Has<Bounds>(entity) && store.Has<Visual>(entity) &&
				   store.Has<SurfaceAppearance>(entity) && store.Has<Tags>(entity) &&
				   store.Has<LocalTransparency>(entity) && store.Has<Rendered>(entity);
		}

		template <class Component, class Relevant>
		bool
		SourceRevisionChanged(Store &store, DrawList &drawList, SourceRevision slot, Relevant &&relevant) {
			store.Observe<Component>();
			const uint64_t revision = store.ComponentChangeVersion<Component>();
			uint64_t &seen = drawList.SourceRevisions[slot];
			if (!drawList.SourcesReady || revision == seen) {
				seen = revision;
				return false;
			}
			seen = revision;

			bool visited = false;
			bool changed = false;
			store.EachChanged<Component>([&](Entity entity, Component &) {
				visited = true;
				changed |= relevant(entity);
			});
			// A world may have ticked while it was not presented. Its monotonic
			// revision survives, but the row bits do not, so an empty walk is not
			// proof that the cached list is current.
			return changed || !visited;
		}

		template <class Component>
		bool SourceRevisionAdvanced(Store &store, DrawList &drawList, SourceRevision slot) {
			store.Observe<Component>();
			const uint64_t revision = store.ComponentChangeVersion<Component>();
			uint64_t &seen = drawList.SourceRevisions[slot];
			const bool changed = drawList.SourcesReady && revision != seen;
			seen = revision;
			return changed;
		}

		DrawSourceChanges DrawSourcesChanged(
			Store &store, DrawList &drawList, size_t matching, size_t skeletons, size_t bones
		) {
			DrawSourceChanges changes;
			changes.Full = !drawList.SourcesReady || matching != drawList.SourceEntityCount ||
						   skeletons != drawList.SkeletonCount || bones != drawList.BoneCount;
			const auto drawable = [&store](Entity entity) { return DrawableSource(store, entity); };

			// Pose columns only affect the interpolated frame or the skin palette.
			// Their monotonic epoch is enough: walking every changed transform and
			// asking eight membership questions per row cost more than updating the
			// packed frame columns it was trying to avoid.
			changes.Pose |= SourceRevisionAdvanced<Transform>(store, drawList, TRANSFORM_REVISION);
			changes.Pose |=
				SourceRevisionAdvanced<PreviousTransform>(store, drawList, PREVIOUS_TRANSFORM_REVISION);
			changes.Full |= SourceRevisionChanged<Bounds>(store, drawList, BOUNDS_REVISION, drawable);
			changes.Full |= SourceRevisionChanged<Visual>(store, drawList, VISUAL_REVISION, drawable);
			changes.Full |=
				SourceRevisionChanged<SurfaceAppearance>(store, drawList, SURFACE_REVISION, drawable);
			changes.Full |= SourceRevisionChanged<Tags>(store, drawList, TAGS_REVISION, drawable);
			changes.Full |=
				SourceRevisionChanged<LocalTransparency>(store, drawList, TRANSPARENCY_REVISION, drawable);
			changes.Full |= SourceRevisionChanged<CharacterLimb>(store, drawList, LIMB_REVISION, drawable);
			changes.Pose |= SourceRevisionAdvanced<Skeleton>(store, drawList, SKELETON_REVISION);
			changes.Pose |= SourceRevisionAdvanced<Bone>(store, drawList, BONE_REVISION);

			store.Observe<Rendered>();
			const uint64_t renderedRevision = store.ComponentChangeVersion<Rendered>();
			changes.Full |=
				drawList.SourcesReady && renderedRevision != drawList.SourceRevisions[RENDERED_REVISION];
			drawList.SourceRevisions[RENDERED_REVISION] = renderedRevision;
			drawList.SourceEntityCount = matching;
			drawList.SkeletonCount = skeletons;
			drawList.BoneCount = bones;
			drawList.SourcesReady = true;
			return changes;
		}

		bool SameFrame(const core::CFrame &left, const core::CFrame &right) {
			return left.Position == right.Position && left.QuaternionX == right.QuaternionX &&
				   left.QuaternionY == right.QuaternionY && left.QuaternionZ == right.QuaternionZ &&
				   left.QuaternionW == right.QuaternionW;
		}

		size_t UpdateDrawFrames(Store &store, DrawList &drawList, float alpha, size_t grain) {
			std::atomic_bool hasInterpolation = false;
			DrawInstance *const out = drawList.Instances.data();
			const size_t capacity = drawList.Instances.size();
			const auto write = [out, capacity, alpha, &hasInterpolation](
								   size_t base,
								   size_t first,
								   size_t rows,
								   const Transform *transforms,
								   const PreviousTransform *previous
							   ) {
				const size_t at = base + first;
				if (at >= capacity) {
					return;
				}
				rows = std::min(rows, capacity - at);
				bool foundInterpolation = false;
				for (size_t row = 0; row < rows; row++) {
					const bool moving = !SameFrame(previous[row].Frame, transforms[row].Frame);
					foundInterpolation |= moving;
					out[at + row].Frame = moving ? previous[row].Frame.NLerp(transforms[row].Frame, alpha)
												 : transforms[row].Frame;
				}
				if (foundInterpolation) {
					hasInterpolation.store(true, std::memory_order_relaxed);
				}
			};

			const size_t loose = store
									 .Query<
										 const Transform,
										 const PreviousTransform,
										 const Bounds,
										 const Visual,
										 const SurfaceAppearance,
										 const Tags,
										 const LocalTransparency>()
									 .With<Rendered>()
									 .Without<CharacterLimb>()
									 .EachBatchParallel(
										 [&write](
											 size_t first,
											 size_t rows,
											 const Transform *transforms,
											 const PreviousTransform *previous,
											 const Bounds *,
											 const Visual *,
											 const SurfaceAppearance *,
											 const Tags *,
											 const LocalTransparency *
										 ) { write(0, first, rows, transforms, previous); },
										 grain
									 );
			const size_t rigged = store
									  .Query<
										  const Transform,
										  const PreviousTransform,
										  const Bounds,
										  const Visual,
										  const SurfaceAppearance,
										  const Tags,
										  const LocalTransparency,
										  const CharacterLimb>()
									  .With<Rendered>()
									  .EachBatchParallel(
										  [&write, loose](
											  size_t first,
											  size_t rows,
											  const Transform *transforms,
											  const PreviousTransform *previous,
											  const Bounds *,
											  const Visual *,
											  const SurfaceAppearance *,
											  const Tags *,
											  const LocalTransparency *,
											  const CharacterLimb *
										  ) { write(loose, first, rows, transforms, previous); },
										  grain
									  );
			drawList.HasInterpolation = hasInterpolation.load(std::memory_order_relaxed);
			return loose + rigged;
		}
	}

	// The smallest run of instances worth handing to another worker.
	//
	// **Reasoned by analogy and not measured, which is the whole of what is
	// known about it.** The number it is copied from is
	// `physics::INTEGRATE_GRAIN`, and the analogy is close enough to be worth
	// making: that body carries a whole `core::CFrame` per row through a
	// quaternion product and a normalise, and this one carries two through an
	// `NLerp` - the same shape of arithmetic, the same reciprocal square
	// root, over roughly three times the bytes. `Integrate.hpp` measures its
	// crossover at 8,000 rows, and 1024 puts this loop's floor at the same
	// 8192.
	//
	// **What it replaces is the default, and the default was certainly
	// wrong.** `Jobs::DEFAULT_GRAIN` is calibrated for three float adds per
	// row; taking it put this loop's floor at 32,768 instances, so a scene of
	// twenty thousand parts ran the whole draw list on one thread - the exact
	// failure `Integrate.hpp` records for the same reason, where the default
	// cost 73.5 us against 27.3 us for a dispatch it declined to make. Being
	// approximately right beats being precisely calibrated for somebody
	// else's body.
	//
	// **1024 rather than the 512 the analogy would also allow**, because a
	// range is not free: `engine.parallel.bench.dispatch` fits the handover at
	// about 6.2 us to wake the pool plus 0.19 us a range, and `Integrate.hpp`
	// measured 9 to 18 per cent lost above the floor when its grain was the
	// narrower one. Halving the grain doubles the ranges to buy a floor this
	// loop has no measurement for.
	//
	// **`engine.ecs.bench.iteration` is the suite that would settle it**, over
	// this body rather than over three float adds, laddering either side of
	// 8192. Until that exists this constant is an estimate, and a reading that
	// disagrees with it should win.
	constexpr size_t DRAW_LIST_GRAIN = 1024;

	// The one phase that turns simulation state into something to draw. It
	// reads the simulation and writes only the draw list, which is what
	// "PreRender never mutates simulation state" means in practice.
	void CollectInstances(Store &store) {
		const float alpha = store.Time().Alpha;

		auto *drawList = store.ResourceMutable<DrawList>();

		// Split into spans that cost nothing to separate.
		//
		// The counting, the sizing and the arithmetic are three different
		// answers to "why is this system slow" - a cached query that is not
		// as cached as it looks, a vector reallocating every frame, or the
		// interpolation itself. One number covering all three cannot tell
		// them apart.
		//
		// It stops here. Going finer means a scope *inside* the row loop,
		// and a scope costs a clock read and a push - several times what a
		// quaternion multiply costs. That measurement would be mostly of
		// itself.
		size_t matching = 0;
		{
			ENGINE_PROFILE_CAT("count entities", engine::core::ProfileCategory::Simulation);
			matching = store.CountMatching<
				Transform,
				PreviousTransform,
				Bounds,
				Visual,
				SurfaceAppearance,
				Tags,
				LocalTransparency,
				Rendered>();
		}
		const size_t skeletons = store.CountMatching<Skeleton>();
		const size_t bones = skeletons == 0 ? 0 : store.CountMatching<Bone>();
		DrawSourceChanges sourceChanges;
		{
			ENGINE_PROFILE_CAT("source changes", engine::core::ProfileCategory::Simulation);
			sourceChanges = DrawSourcesChanged(store, *drawList, matching, skeletons, bones);
		}
		if (!drawList->HasInterpolation && !sourceChanges.Pose && !sourceChanges.Full) {
			ENGINE_PROFILE_CAT("reuse draw list", engine::core::ProfileCategory::Simulation);
			drawList->Instances.resize(drawList->BaseInstanceCount);
			engine::core::Metrics::Count("render.instances", static_cast<double>(drawList->Instances.size()));
			(void)engine::scene::CutAndCloneSeams(store, drawList->Instances);
			return;
		}
		if (!sourceChanges.Full && !drawList->HasFilteredSources) {
			ENGINE_PROFILE_CAT("update draw frames", engine::core::ProfileCategory::Simulation);
			drawList->Instances.resize(drawList->BaseInstanceCount);
			const size_t written = UpdateDrawFrames(store, *drawList, alpha, DRAW_LIST_GRAIN);
			if (written == drawList->BaseInstanceCount) {
				engine::core::Metrics::Count(
					"render.instances", static_cast<double>(drawList->Instances.size())
				);
				if (skeletons == 0) {
					drawList->JointFrames.clear();
				} else {
					CollectSkinPalettes(store, *drawList);
				}
				(void)engine::scene::CutAndCloneSeams(store, drawList->Instances);
				return;
			}
			// A source query changed shape without a matching component epoch. The
			// full rebuild below repairs the cache instead of publishing partial rows.
			sourceChanges.Full = true;
		}

		{
			// Sized once, then written by index. The vector is not cleared
			// first, so on a steady scene this is a no-op: the buffer is the
			// size it already was, and no element is value-initialised only
			// to be overwritten a moment later. A reading above zero here
			// means the scene changed size or the capacity is being lost.
			//
			// The count is a floor rather than a contract - it comes from a
			// different query than the one EachBatch walks, and this system
			// does not get to assume the two agree. The batches decide the
			// real size, and the shrink below settles it.
			ENGINE_PROFILE_CAT("size draw list", engine::core::ProfileCategory::Simulation);
			drawList->Instances.resize(matching);
		}

		size_t written = 0;
		std::atomic_bool hasFullyTransparent = false;
		std::atomic_bool hasInterpolation = false;
		{
			// Parallel, and this is the loop that earns it. The arithmetic
			// stopped being the cost once the interpolation lost its
			// transcendentals; what is left is a hundred and fifty bytes of
			// traffic per entity, over half of it the instance being written.
			// A memory-bound loop is the case where more threads means more
			// loads in flight, so it is the one that crosses over soonest.
			//
			// **Which is why it passes `DRAW_LIST_GRAIN` and stopped taking
			// the default.** This paragraph and a dispatch floor of 32,768
			// instances contradicted each other for as long as both were
			// here, and the floor was the one winning.
			//
			// Each slice is told where its rows land in the output, so the
			// workers never touch the same bytes and the array comes out in
			// the same order every frame. No atomic, no locking, no
			// frame-to-frame reshuffling of the draw list.
			ENGINE_PROFILE_CAT("interpolate", engine::core::ProfileCategory::Simulation);

			// Taken once, outside. A worker cannot grow the vector - that is
			// a reallocation under every other worker's feet - so the buffer
			// is sized before the loop starts and the body writes into it.
			DrawInstance *const out = drawList->Instances.data();
			const size_t capacity = drawList->Instances.size();

			// **`Rendered` is in the signature and nothing reads it**, which
			// is the point of a tag: it is a term in the query, so the
			// archetype walk never reaches a row that has not been marked as
			// a visible descendant of `Workspace`. A branch here could not
			// have done the same job - this loop writes `out[first + row]`
			// so that no two workers touch the same bytes, and skipping a
			// row would leave a hole in the draw list and make `written` a
			// lie. `scene/Visibility.hpp` has the whole argument.
			// **`SurfaceAppearance` and `Tags` are columns rather than an
			// optional join**, which is the whole reason both live on
			// `BasePart` rather than on `MeshPart`. A batched parallel walk
			// is handed fixed columns; a component that only some rows had
			// could not be read here without splitting the query.
			// **One writer, two walks.** A limb has to carry the rig it
			// belongs to - see `DrawInstance::Rig`, which is what lets a
			// portal cut a character in one piece instead of a dozen times -
			// and `CharacterLimb` is on some drawable rows and not others.
			// A batched parallel walk has no optional join, so the query is
			// split on the component instead
			// and each half is a walk with a required column list. Every
			// other field is written by the same function in both.
			const auto write = [out, capacity, alpha, &hasFullyTransparent, &hasInterpolation](
								   size_t base,
								   size_t first,
								   size_t rows,
								   const Entity *entities,
								   const Transform *transforms,
								   const PreviousTransform *previous,
								   const Bounds *bounds,
								   const Visual *visuals,
								   const SurfaceAppearance *appearances,
								   const Tags *tags,
								   const LocalTransparency *locals,
								   const CharacterLimb *limbs
							   ) {
				// The count came from a different query than the one being
				// walked. They agree, and this is what happens if they ever
				// stop: instances go missing and the number on the panel
				// drops, rather than a worker writing past the end of the
				// buffer.
				const size_t at = base + first;
				if (at >= capacity) {
					return;
				}
				rows = std::min(rows, capacity - at);
				bool foundFullyTransparent = false;
				bool foundInterpolation = false;

				for (size_t row = 0; row < rows; row++) {
					foundInterpolation |= !SameFrame(previous[row].Frame, transforms[row].Frame);
					// Interpolated, not the tick position. At 300 fps
					// against a 60 Hz tick, drawing tick positions shows
					// each one five times and then jumps - which reads as
					// a frame-rate problem rather than as a tick-rate one.
					//
					// NLerp, not Lerp. The endpoints are one simulation
					// tick apart - a few degrees at most - and over an arc
					// that short the two agree to well inside a pixel.
					// Lerp's constant angular speed costs an acos and
					// three sin calls per entity, which on this loop was
					// the single most expensive thing in the frame.
					//
					// A `CFrame` and a half-extent, not a matrix: this is
					// what the world knows, and `render` is what turns it
					// into something a GPU binds.
					//
					// The fields come from `scene::MakeDrawInstance`, which
					// is the only place that list is written - the
					// replicated collector fills the same row from a
					// snapshot. Both components are required columns of
					// *this* query, so the addresses are always good.
					out[at + row] = engine::scene::MakeDrawInstance(
						previous[row].Frame.NLerp(transforms[row].Frame, alpha),
						bounds[row],
						visuals[row],
						&appearances[row],
						&tags[row],
						entities[row].Id,
						&locals[row],
						limbs == nullptr ? nullptr : &limbs[row]
					);
					foundFullyTransparent |= out[at + row].Transparency >= 1.0f;
				}
				if (foundFullyTransparent) {
					hasFullyTransparent.store(true, std::memory_order_relaxed);
				}
				if (foundInterpolation) {
					hasInterpolation.store(true, std::memory_order_relaxed);
				}
			};

			const size_t loose = store
									 .Query<
										 const Transform,
										 const PreviousTransform,
										 const Bounds,
										 const Visual,
										 const SurfaceAppearance,
										 const Tags,
										 const LocalTransparency>()
									 .With<Rendered>()
									 .Without<CharacterLimb>()
									 .EachBatchEntitiesParallel(
										 [&write](
											 size_t first,
											 size_t rows,
											 const Entity *entities,
											 const Transform *transforms,
											 const PreviousTransform *previous,
											 const Bounds *bounds,
											 const Visual *visuals,
											 const SurfaceAppearance *appearances,
											 const Tags *tags,
											 const LocalTransparency *locals
										 ) {
											 write(
												 0,
												 first,
												 rows,
												 entities,
												 transforms,
												 previous,
												 bounds,
												 visuals,
												 appearances,
												 tags,
												 locals,
												 nullptr
											 );
										 },
										 DRAW_LIST_GRAIN
									 );

			// After the loose rows, so the two halves cannot overlap. Their
			// order relative to each other is not a promise anything reads -
			// `EachBatch` already says a batch boundary is not a unit
			// anybody declared - and within each half it is as deterministic
			// as it was.
			const size_t rigged = store
									  .Query<
										  const Transform,
										  const PreviousTransform,
										  const Bounds,
										  const Visual,
										  const SurfaceAppearance,
										  const Tags,
										  const LocalTransparency,
										  const CharacterLimb>()
									  .With<Rendered>()
									  .EachBatchEntitiesParallel(
										  [&write, loose](
											  size_t first,
											  size_t rows,
											  const Entity *entities,
											  const Transform *transforms,
											  const PreviousTransform *previous,
											  const Bounds *bounds,
											  const Visual *visuals,
											  const SurfaceAppearance *appearances,
											  const Tags *tags,
											  const LocalTransparency *locals,
											  const CharacterLimb *limbs
										  ) {
											  write(
												  loose,
												  first,
												  rows,
												  entities,
												  transforms,
												  previous,
												  bounds,
												  visuals,
												  appearances,
												  tags,
												  locals,
												  limbs
											  );
										  },
										  DRAW_LIST_GRAIN
									  );

			written = loose + rigged;
		}
		drawList->HasInterpolation = hasInterpolation.load(std::memory_order_relaxed);
		drawList->HasFilteredSources = hasFullyTransparent.load(std::memory_order_relaxed);

		{
			ENGINE_PROFILE_CAT("publish draw list", engine::core::ProfileCategory::Simulation);

			// Whatever the count said, this is how many there are. Shrinking
			// a vector writes nothing and keeps the capacity, so the frame
			// after an entity is destroyed still does not allocate.
			drawList->Instances.resize(std::min(written, drawList->Instances.size()));
			if (hasFullyTransparent.load(std::memory_order_relaxed)) {
				std::erase_if(drawList->Instances, [](const scene::DrawInstance &instance) {
					return instance.Transparency >= 1.0f;
				});
			}

			engine::core::Metrics::Count("render.instances", static_cast<double>(drawList->Instances.size()));
		}

		// Every row above was assigned from a fresh `DrawInstance`, whose skin run
		// is empty. Avoid an entity lookup per drawable in worlds with no rigs.
		if (skeletons == 0) {
			drawList->JointFrames.clear();
		} else {
			CollectSkinPalettes(store, *drawList);
		}
		drawList->BaseInstanceCount = drawList->Instances.size();

		// Seam clones belong to scene presentation; diagnostic face markers
		// are appended only by callers explicitly requesting inspection geometry.
		// **One far-side copy and not two, which is what this used to
		// draw.** There were two passes producing it - one walked the world
		// for things that can move, the other walked the draw list - and
		// calling both put two copies of every straddling body on the far
		// side, z-fighting each other. Worse, the list pass reads the list
		// it appends to, so it also copied the entity pass's output: a copy
		// sits across the *far* pane by construction, so it was mapped back
		// again and a third landed on top of the original. What that looks
		// like is a spare character standing near the hole.
		//
		// **`CutAndCloneSeams` is the one pass now**, and it is the list one
		// because only a list walk holds the row the original is in - which
		// is what lets it *cut* the body at the plane rather than leave two
		// whole copies straddling two panes. The same call serves a replica,
		// which has a draw list and no simulation behind it.
		(void)engine::scene::CutAndCloneSeams(store, drawList->Instances);
	}

	void CollectSkinPalettes(ecs::Store &store, DrawList &drawList) {
		ENGINE_PROFILE_CAT("build skin palettes", engine::core::ProfileCategory::Simulation);
		drawList.JointFrames.clear();
		for (scene::DrawInstance &instance : drawList.Instances) {
			instance.SkinFirst = 0;
			instance.SkinCount = 0;

			const ecs::Entity source(instance.Source);
			const scene::Skeleton *skeleton = store.Get<scene::Skeleton>(source);
			if (skeleton == nullptr || skeleton->JointCount == 0 ||
				skeleton->JointCount > scene::MAX_JOINTS || !std::isfinite(skeleton->PoseScale) ||
				skeleton->PoseScale <= 0) {
				continue;
			}

			instance.SkinFirst = static_cast<uint32_t>(drawList.JointFrames.size());
			instance.SkinCount = skeleton->JointCount;
			drawList.JointFrames.resize(drawList.JointFrames.size() + skeleton->JointCount);
			store.EachDescendant(source, [&](ecs::Entity descendant) {
				const scene::Bone *bone = store.Get<scene::Bone>(descendant);
				if (bone != nullptr && bone->Joint < skeleton->JointCount) {
					auto frame = instance.Frame.Inverse() * scene::SkinningFrameOf(*bone);
					frame.Position = frame.Position * (1.0f / skeleton->PoseScale);
					drawList.JointFrames[instance.SkinFirst + bone->Joint] = frame;
				}
			});
		}
	}

	void RebaseSkinPalettes(
		std::span<scene::DrawInstance> instances,
		std::span<const core::CFrame> source,
		std::vector<core::CFrame> &destination
	) {
		for (scene::DrawInstance &instance : instances) {
			const uint64_t end = static_cast<uint64_t>(instance.SkinFirst) + instance.SkinCount;
			if (instance.SkinCount == 0 || end > source.size() ||
				destination.size() > std::numeric_limits<uint32_t>::max() - instance.SkinCount) {
				instance.SkinFirst = 0;
				instance.SkinCount = 0;
				continue;
			}

			const size_t first = instance.SkinFirst;
			instance.SkinFirst = static_cast<uint32_t>(destination.size());
			destination.insert(destination.end(), source.begin() + first, source.begin() + end);
		}
	}

	void ParticleFrame::Detach() {
		if (Detached) {
			return;
		}
		Blocks.clear();
		SpawnStates.clear();
		RuntimeStates.clear();
		Blocks.reserve(Batches.size());
		SpawnStates.reserve(Batches.size());
		RuntimeStates.reserve(Batches.size());
		for (const ParticleBatch &batch : Batches) {
			if (batch.Block != nullptr && batch.Spawn != nullptr && batch.Runtime != nullptr) {
				Blocks.push_back(*batch.Block);
				SpawnStates.push_back(*batch.Spawn);
				RuntimeStates.push_back(*batch.Runtime);
			}
		}

		// Reserve before repointing so no later growth invalidates an earlier
		// batch pointer.
		size_t at = 0;
		for (ParticleBatch &batch : Batches) {
			if (batch.Block != nullptr && batch.Spawn != nullptr && batch.Runtime != nullptr) {
				batch.Block = Blocks.data() + at;
				batch.Spawn = SpawnStates.data() + at;
				batch.Runtime = RuntimeStates.data() + at;
				at++;
			}
		}
		Detached = true;
	}

	void ParticleFrame::Clear() {
		Batches.clear();
		Seams.clear();
		Blocks.clear();
		SpawnStates.clear();
		RuntimeStates.clear();
		Pool = 0;
		BlockCount = 0;
		SourceWorld = {};
		SourceRevision = 0;
		SourceLayoutRevision = 0;
		SourceResidentRevision = 0;
		SourceSelectionRevision = 0;
		SourceSelection = {};
		Detached = false;
	}

	size_t
	CollectParticleBatches(ecs::Store &store, ParticleFrame &frame, const ParticleBatchSelection &selection) {
		auto *system = store.ResourceMutable<effects::ParticleSystem>();
		const core::Name sourceWorld(store.Name());
		const uint64_t sourceRevision = system == nullptr ? 0 : system->PresentationRevision;
		const uint64_t sourceLayoutRevision = system == nullptr ? 0 : system->LayoutRevision;
		const uint64_t sourceResidentRevision = system == nullptr ? 0 : system->ResidentRevision;
		if (frame.SourceWorld == sourceWorld && frame.SourceRevision == sourceRevision &&
			frame.SourceLayoutRevision == sourceLayoutRevision &&
			frame.SourceResidentRevision == sourceResidentRevision &&
			frame.SourceSelection == selection.Name && frame.SourceSelectionRevision == selection.Revision) {
			return frame.Batches.size();
		}

		bool rebuildLayout =
			frame.SourceWorld != sourceWorld || frame.SourceLayoutRevision != sourceLayoutRevision ||
			frame.SourceSelection != selection.Name || frame.SourceSelectionRevision != selection.Revision;
		if (!rebuildLayout && frame.Detached &&
			(frame.Blocks.size() != frame.Batches.size() ||
			 frame.SpawnStates.size() != frame.Batches.size() ||
			 frame.RuntimeStates.size() != frame.Batches.size())) {
			// A malformed detached frame cannot safely be refreshed in place. Treat
			// it as a cold snapshot rather than leaving one batch pointing outside
			// the copied block array.
			rebuildLayout = true;
		}

		if (rebuildLayout) {
			frame.Clear();
			frame.LayoutRevision++;
		} else {
			frame.Seams.clear();
		}
		frame.SourceWorld = sourceWorld;
		frame.SourceRevision = sourceRevision;
		frame.SourceLayoutRevision = sourceLayoutRevision;
		frame.SourceSelection = selection.Name;
		frame.SourceSelectionRevision = selection.Revision;
		const bool refreshResident = rebuildLayout || frame.SourceResidentRevision != sourceResidentRevision;
		frame.SourceResidentRevision = sourceResidentRevision;
		frame.Revision++;
		if (refreshResident) {
			frame.ResidentRevision++;
		}

		if (system == nullptr || system->Blocks.empty()) {
			return 0;
		}
		frame.Pool = system->Capacity;
		frame.BlockCount = static_cast<uint32_t>(system->Blocks.size());

		// Flatten portals once per frame. Particle positions live on the device,
		// so the seam crosses this boundary and the per-particle decision does not.
		static thread_local std::vector<scene::PortalSeam> seams;
		if (scene::GatherPortalSeams(store, seams) > 0) {
			for (const scene::PortalSeam &seam : seams) {
				if (seam.Crosses) {
					continue;
				}

				const scene::SeamTransform map = scene::SeamMapping(seam);
				ParticleSeam flat;
				flat.Centre = seam.Centre;
				flat.Normal = seam.Normal;
				flat.First = seam.First;
				flat.Second = seam.Second;
				flat.Mapping = map.Frame;
				flat.Scale = map.Scale;
				frame.Seams.push_back(flat);
			}
		}

		if (!rebuildLayout && refreshResident) {
			for (size_t at = 0; at < frame.Batches.size(); at++) {
				ParticleBatch &batch = frame.Batches[at];
				assert(batch.Index < system->Blocks.size());
				if (frame.Detached) {
					frame.Blocks[at] = system->Blocks[batch.Index];
					frame.SpawnStates[at] = system->SpawnStates[batch.Index];
					frame.RuntimeStates[at] = system->RuntimeStates[batch.Index];
					batch.Block = frame.Blocks.data() + at;
					batch.Spawn = frame.SpawnStates.data() + at;
					batch.Runtime = frame.RuntimeStates.data() + at;
				} else {
					batch.Block = system->Blocks.data() + batch.Index;
					batch.Spawn = system->SpawnStates.data() + batch.Index;
					batch.Runtime = system->RuntimeStates.data() + batch.Index;
				}
			}
		}
		if (!rebuildLayout) {
			return frame.Batches.size();
		}

		// Walk the emitter column because it owns presentation properties. The
		// block only owns resident simulation state. This walk is a layout rebuild,
		// not a simulation-revision cost: unchanged emitters retain this ordered
		// metadata while changed block values refresh around it.
		store.Each<const effects::ParticleEmitter, const effects::EmitterSlot>(
			[&](ecs::Entity entity,
				const effects::ParticleEmitter &emitter,
				const effects::EmitterSlot &slot) {
				if (selection.Includes != nullptr && !selection.Includes(store, entity, emitter)) {
					return;
				}
				if (slot.Index == effects::NO_SLOT || slot.Index >= system->Blocks.size()) {
					return;
				}

				const effects::EmitterBlock &block = system->Blocks[slot.Index];
				if (block.Capacity == 0) {
					return;
				}

				ParticleBatch batch;
				batch.Block = &block;
				batch.Spawn = &system->SpawnStates[slot.Index];
				batch.Runtime = &system->RuntimeStates[slot.Index];
				batch.Index = slot.Index;
				batch.Texture = emitter.Texture;
				batch.FlipbookSide = static_cast<float>(effects::FlipbookSide(emitter.Flipbook));
				batch.ZOffset = emitter.ZOffset;
				batch.LightEmission = emitter.LightEmission;
				batch.LightInfluence = emitter.LightInfluence;
				batch.Additive = emitter.Additive;
				batch.WorldUp = emitter.Orientation == effects::ParticleOrientation::FacingCameraWorldUp;
				batch.SoftParticles = emitter.SoftParticles;
				frame.Batches.push_back(batch);
			}
		);

		return frame.Batches.size();
	}

	size_t CollectLights(ecs::Store &store, const core::Vector3 &eye, std::vector<SceneLight> &lights) {
		lights.clear();

		store.Each<const scene::Light>([&](ecs::Entity entity, const scene::Light &bulb) {
			if (!bulb.Enabled || bulb.Brightness <= 0.0f || bulb.Range <= 0.0f) {
				return;
			}

			const ecs::Entity parent = store.ParentOf(entity);
			if (parent == ecs::NULL_ENTITY) {
				return;
			}

			core::CFrame frame;
			if (const auto *point = store.Get<scene::Attachment>(parent)) {
				frame = point->WorldFrame;
			} else if (const auto *placement = store.Get<scene::Transform>(parent)) {
				frame = placement->Frame;
			} else {
				return;
			}

			SceneLight light;
			light.Position = frame.Position;
			light.Range = bulb.Range;
			light.Colour = core::Color3{
				bulb.Colour.R * bulb.Brightness,
				bulb.Colour.G * bulb.Brightness,
				bulb.Colour.B * bulb.Brightness,
			};

			if (bulb.Kind == scene::LightKind::Point) {
				light.ConeCosine = -1.0f;
			} else {
				light.Direction = frame.VectorToWorldSpace(scene::NormalOf(bulb.Face));
				light.ConeCosine = std::cos(
					std::clamp(bulb.Angle, 0.0f, 180.0f) * 0.5f * std::numbers::pi_v<float> / 180.0f
				);
			}

			lights.push_back(light);
		});

		// Transport local lights through same-world portals once. Copies are not
		// recursively copied because the fixed light budget would become geometric.
		static thread_local std::vector<scene::PortalSeam> seams;
		if (scene::GatherPortalSeams(store, seams) > 0) {
			const size_t own = lights.size();
			for (size_t index = 0; index < own; index++) {
				for (const scene::PortalSeam &seam : seams) {
					if (seam.Crosses ||
						scene::SeamDistance(seam, lights[index].Position) >= lights[index].Range) {
						continue;
					}

					const scene::SeamTransform through = scene::SeamMapping(seam);
					SceneLight copy = lights[index];
					copy.Position = through.Point(lights[index].Position);
					copy.Range = through.Length(lights[index].Range);
					copy.Direction = through.Rotate(lights[index].Direction);
					lights.push_back(copy);
				}
			}
		}

		if (lights.size() > MAX_SCENE_LIGHTS) {
			std::partial_sort(
				lights.begin(),
				lights.begin() + MAX_SCENE_LIGHTS,
				lights.end(),
				[&eye](const SceneLight &left, const SceneLight &right) {
					const core::Vector3 leftOffset = left.Position - eye;
					const core::Vector3 rightOffset = right.Position - eye;
					return leftOffset.Dot(leftOffset) < rightOffset.Dot(rightOffset);
				}
			);
			lights.resize(MAX_SCENE_LIGHTS);
		}

		return lights.size();
	}

	core::Name InstallWorldPipeline(
		const graph::PipelineSet &profiles, Renderer &renderer, uint64_t world, core::Name selected
	) {
		graph::RegisterRenderNodeKinds();
		DefaultPipelineTier tier = DefaultPipelineTier::A;
		PipelineTierDecision decision;
		if (renderer.Backend().Device != nullptr) {
			decision = ChooseDefaultPipeline(renderer.Capabilities());
			tier = decision.Tier;
			for (const PipelineTierRejection &rejected : decision.Fallthrough) {
				ENGINE_INFO(
					"{} default skipped: {}{}{}",
					Describe(rejected.Tier),
					Describe(rejected.Cause.Status),
					rejected.Cause.Status == CapabilityStatus::MissingFormat ? ": " : "",
					rejected.Cause.Status == CapabilityStatus::MissingFormat
						? graph::Describe(rejected.Cause.Format)
						: ""
				);
			}
		}
		const auto defaultDocument = [tier] {
			switch (tier) {
			case DefaultPipelineTier::A:
				return graph::DefaultPbrDocument();
			case DefaultPipelineTier::B:
				return graph::DefaultPbrTierBDocument();
			case DefaultPipelineTier::C:
				return graph::DefaultForwardTierCDocument();
			case DefaultPipelineTier::Unavailable:
				return graph::PipelineDocument{};
			}
			return graph::PipelineDocument{};
		};

		const std::string suffix = "#" + std::to_string(world);
		for (const core::Name key : renderer.Pipelines()) {
			if (key.Text().ends_with(suffix)) {
				(void)renderer.RemovePipeline(key);
			}
		}

		graph::PipelineSet defaults;
		const graph::PipelineSet *available = &profiles;
		if (profiles.Count() == 0) {
			defaults.Set(core::Name("Default PBR"), defaultDocument());
			available = &defaults;
		}

		std::vector<core::Name> candidates;
		const auto addCandidate = [&](core::Name name) {
			if (name.IsValid() && available->Find(name) != nullptr &&
				std::find(candidates.begin(), candidates.end(), name) == candidates.end()) {
				candidates.push_back(name);
			}
		};
		addCandidate(selected);
		addCandidate(core::Name("Default PBR"));
		for (const core::Name name : available->Names()) {
			addCandidate(name);
		}

		for (const core::Name name : candidates) {
			const graph::PipelineDocument *document = available->Find(name);
			assert(document != nullptr);

			graph::RenderGraph pipeline;
			core::Name offender;
			const graph::PipelineDocumentStatus status = graph::Build(*document, pipeline, offender);
			if (status != graph::PipelineDocumentStatus::Ok) {
				ENGINE_ERROR(
					"pipeline '{}' does not build: {} at '{}'",
					name.Text(),
					graph::Describe(status),
					offender.Text()
				);
				continue;
			}

			const core::Name key(std::format("{}#{}", name.Text(), world));
			if (renderer.SetPipeline(key, pipeline)) {
				return key;
			}
		}

		if (profiles.Count() > 0 && tier != DefaultPipelineTier::Unavailable) {
			graph::RenderGraph pipeline;
			core::Name offender;
			if (graph::Build(defaultDocument(), pipeline, offender) == graph::PipelineDocumentStatus::Ok) {
				const core::Name key(std::format("Default PBR#{}", world));
				if (renderer.SetPipeline(key, pipeline)) {
					return key;
				}
			}
		}
		return {};
	}

	void RegisterPresentationComponents() {
		// Preserve the stable name written by earlier saves even though ownership
		// has moved from the client library into the render module.
		ecs::Components::Register<DrawList>(
			"client.DrawList",
			[](core::ByteWriter &, const void *, size_t) {},
			[](core::ByteReader &, void *destination, size_t count) {
				auto *lists = static_cast<DrawList *>(destination);
				for (size_t index = 0; index < count; index++) {
					lists[index].Instances.clear();
					lists[index].JointFrames.clear();
					lists[index].BaseInstanceCount = 0;
					lists[index].SourceRevisions.fill(0);
					lists[index].SourceEntityCount = 0;
					lists[index].SkeletonCount = 0;
					lists[index].BoneCount = 0;
					lists[index].SourcesReady = false;
					lists[index].HasInterpolation = false;
				}
			}
		);
		ecs::Components::Register<AnimationCatalogue>(
			"render.AnimationCatalogue",
			[](core::ByteWriter &, const void *, size_t) {},
			[](core::ByteReader &, void *destination, size_t count) {
				auto *catalogues = static_cast<AnimationCatalogue *>(destination);
				for (size_t index = 0; index < count; index++) {
					catalogues[index].Clips.clear();
					catalogues[index].Buffers.clear();
				}
			}
		);
	}
	namespace {
		using OrderedView = std::pair<uint32_t, SurfaceView>;
		std::vector<OrderedView> &OrderedSurfaceViews() {
			static thread_local std::vector<OrderedView> ordered;
			return ordered;
		}
	}
	void ApplySurfaceSlots(
		std::span<scene::DrawInstance> instances, std::span<const scene::SurfaceSlot> slots, core::Name world
	) {
		static thread_local std::vector<scene::SurfaceSlot> parts;
		parts.assign(slots.begin(), slots.end());
		// Multiple cameras on one part follow AimSurfaceCameras' last-camera
		// ownership. The highest camera retains that decision without a sort allocation.
		std::sort(parts.begin(), parts.end(), [](const auto &left, const auto &right) {
			return left.Part.Id == right.Part.Id ? left.Camera.Id < right.Camera.Id
												 : left.Part.Id < right.Part.Id;
		});
		for (auto &instance : instances) {
			if (instance.SourceWorld.IsValid() && instance.SourceWorld != world) {
				continue;
			}
			const auto found = std::upper_bound(
				parts.begin(), parts.end(), instance.Source, [](uint64_t source, const auto &part) {
					return source < part.Part.Id;
				}
			);
			if (found != parts.begin() && (found - 1)->Part.Id == instance.Source) {
				instance.Surface = (found - 1)->Index;
			}
		}
	}

	size_t CollectPortalViews(
		ecs::Store &store, std::vector<PortalView> &portals, std::span<const scene::SurfaceSlot> slots
	) {
		portals.clear();

		// **`GatherPortalSeams`, and never a second measurement of the same
		// hole.** The rectangle and the map are what `CrossPortals` moves a body
		// through; a picture that derived them its own way would be a picture
		// that disagrees with where somebody comes out, which is the exact class
		// of bug that made the camera and the body pick different panes.
		static thread_local std::vector<scene::PortalSeam> seams;
		if (scene::GatherPortalSeams(store, seams) == 0) {
			return 0;
		}

		for (auto &seam : seams) {
			for (const auto &slot : slots) {
				if (slot.Camera == seam.Camera) {
					seam.Surface = slot.Index;
					break;
				}
			}
		}

		for (const scene::PortalSeam &seam : seams) {
			// **A cross-world pane stays on the surface path**, because a warp
			// into another world's coordinate space is a stated frame rather than
			// a derived one - `Portal::DestinationWorld` and
			// `AttachForeignSurfaces` are the whole of that arrangement, and it
			// does not recurse.
			if (seam.Crosses || seam.Surface < 0) {
				continue;
			}

			PortalView portal;
			portal.Index = seam.Surface;
			portal.Centre = seam.Centre;
			portal.Normal = seam.Normal;
			portal.First = seam.First;
			portal.Second = seam.Second;

			// **The same one map a body is carried by.** `SeamMapping` states it
			// once for the pane rather than once per side, so what the hole shows
			// and where walking into it puts you are the same arithmetic.
			portal.Warp = scene::SeamMapping(seam);
			portal.TagFilter = seam.TagFilter;

			// The hole at the far end, so the level this one opens can skip it.
			// A pair is two seams whose `Far` and `Pane` cross over, which is the
			// only place that pairing is written down.
			for (const scene::PortalSeam &other : seams) {
				if (other.Pane == seam.Far && !other.Crosses) {
					portal.Partner = other.Surface;
					break;
				}
			}

			portals.push_back(portal);
		}

		return portals.size();
	}

	size_t CollectSurfaceViews(
		ecs::Store &store,
		std::vector<SurfaceView> &views,
		std::span<const PortalView> portals,
		const View *viewer,
		std::span<const scene::SurfaceSlot> slots
	) {
		views.clear();
		OrderedSurfaceViews().clear();
		static thread_local std::vector<scene::SurfaceSlot> requestSlots;
		if (viewer != nullptr && slots.empty()) {
			scene::GatherSurfaceSlots(store, requestSlots);
			slots = requestSlots;
		}

		// **The panes, measured once for the whole walk.** A mirror's camera is a
		// function of its pane and whoever is looking at it, so the renderer needs
		// the rectangle in order to place that camera for a viewer deeper than the
		// eye - see `render::SurfaceView::PaneNormal`. Measuring a face is
		// `GatherSurfacePanes`' business and not this file's: `ReachOf` and the
		// face's two axes were re-derived in three places once, and a marker drawn
		// on a face the camera was not projecting off is a debugging aid that lies.
		//
		// **Only the mirrors are in here.** A linked portal is a warp rather than a
		// reflection and the gatherer leaves it out, so a hole cannot pick up a
		// rectangle that would tell the pass to reflect through it.
		static thread_local std::vector<scene::SurfacePane> panes;
		(void)scene::GatherSurfacePanes(store, panes);

		store.Each<const scene::SurfaceCamera, const scene::Camera, const scene::Transform>(
			// `panes` needs no capture: it has static storage duration, for the
			// reason every other scratch buffer in this file does - a per-frame
			// allocation in a walk that runs once per world per frame.
			[&store, portals, viewer, slots](
				ecs::Entity entity,
				const scene::SurfaceCamera &target,
				const scene::Camera &lens,
				const scene::Transform &placement
			) {
				if (const scene::Portal *portal = store.template Get<scene::Portal>(entity);
					portal != nullptr && !portal->Enabled) {
					return;
				}

				// **A slot the recursive pass owns gets no surface camera.** Both
				// would draw the same pane - one from a camera derived from this
				// level and one from a camera placed off the eye - and the second
				// is the viewpoint error the pass exists to remove. Skipped here
				// rather than refused in the renderer so the cost of aiming it is
				// the only thing wasted.
				const auto claimed = [&](int16_t slot) {
					for (const PortalView &portal : portals) {
						if (portal.Index == slot) {
							return true;
						}
					}
					return false;
				};

				int16_t index = target.Surface;
				if (viewer != nullptr) {
					for (const auto &slot : slots) {
						if (slot.Camera == entity) {
							index = slot.Index;
							break;
						}
					}
				}
				if (index < 0 || claimed(index)) {
					return;
				}

				SurfaceView view;
				view.Index = index;
				view.Frame = placement.Frame;
				view.Width = target.Width;
				view.Height = target.Height;

				// **The rectangle, when this camera is a mirror on a part.** Left
				// zero otherwise, which is what tells the pass it may not descend
				// into this pane: a camera parented to the world has no face to
				// reflect through, and one showing a second world has no local
				// geometry behind the glass. Both keep the one eye-derived image
				// they have always had, which is the arrangement that works today.
				//
				// **Matched by entity and not by slot.** Two cameras naming one
				// index is a scene mistake the renderer resolves by keeping the
				// first, and matching on the number here would hand the survivor
				// the loser's rectangle - a camera reflecting through a pane it is
				// not on, which reads as a mirror showing the wrong room.
				for (const scene::SurfacePane &pane : panes) {
					if (pane.Camera != entity) {
						continue;
					}

					view.PaneCentre = pane.Centre;
					view.PaneNormal = pane.Normal;
					view.PaneFirst = pane.First;
					view.PaneSecond = pane.Second;
					view.PaneNear = pane.NearPlane;
					view.PaneFar = pane.FarPlane;
					if (viewer != nullptr) {
						const auto reflected = scene::ReflectCamera(pane, viewer->CameraFrame, {});
						if (!reflected.Renders) {
							return;
						}
						view.Frame = reflected.Frame;
						view.Projection = scene::SurfaceProjection(reflected.Lens, reflected.Frame);
						view.Mapping = scene::SurfaceMapping(reflected.Lens);
					}

					break;
				}

				// **The fitted frustum when there is one, and the plain camera
				// when there is not.** `AimSurfaceCameras` writes a
				// `SurfaceLens` for every camera it places - which is every one
				// parented to a part - and that lens is off-axis and possibly
				// obliquely clipped, neither of which a field of view can say.
				//
				// A surface camera parented to the *world* is placed by whoever
				// authored it and gets no lens, so it keeps the ordinary
				// perspective build from its `Camera`. `SurfaceCameras.hpp`
				// promises that arrangement still works, and this is where the
				// promise is kept.
				if (viewer != nullptr && view.PaneNormal.Dot(view.PaneNormal) > 0) {
					// The explicit viewer already resolved this mirror above.
				} else if (const scene::SurfaceLens *fitted = store.template Get<scene::SurfaceLens>(entity);
						   fitted != nullptr) {
					view.Projection = scene::SurfaceProjection(*fitted, placement.Frame);

					// **And what took the pane to where that frustum was
					// fitted**, which for a portal is not nothing. The image is
					// read back by projecting the pane's own world position, so
					// a camera fitted three hundred units away needs the pane
					// carried there too - see `scene::SurfaceLens::Mapping`. A
					// mirror's is the identity and this line is free.
					//
					// **Composed by `SurfaceMapping` rather than here**, because
					// the lens holds a rotation, a centre and a scale and the
					// order those go in is the sort of thing that is wrong once
					// and then wrong everywhere.
					view.Mapping = scene::SurfaceMapping(*fitted);
				} else {
					const float aspect = static_cast<float>(target.Width) /
										 static_cast<float>(std::max<uint16_t>(target.Height, 1));
					view.Projection = scene::ResolveCamera(placement.Frame, lens, aspect).Projection;
				}

				// **Opacity here, transparency in the component**, and the flip
				// happens once. `scene::SurfaceCamera::ImageTransparency` is
				// authored the way a script thinks - 0 is solid, like every
				// other transparency in this engine - and the shader multiplies
				// by the opposite, so converting at the boundary beats one
				// subtraction in a shader nobody can put a breakpoint in.
				// **Not clamped here.** The property setter is the authored gate
				// and `Renderer` clamps again at its own boundary because
				// `SurfaceView` is a public struct any host fills - a third copy
				// in between makes none of the three read as the authority, and
				// a future widening of the range has to find all of them.
				view.ImageOpacity = 1.0f - target.ImageTransparency;

				// Copied straight across: the renderer applies it and nothing
				// between here and there has an opinion about it.
				view.Effect = target.Effect;

				// **And how often it may redraw**, which is the same kind of
				// pass-through. A surface is a whole scene render and there is
				// no reason it should keep the screen's rate - see
				// `scene::SurfaceCamera::FPS` for why the default is a rate
				// rather than "every frame".
				view.FPS = target.FPS;

				// **Copied rather than resolved.** The filter is already a mask
				// on the component, because a name would be a lookup per
				// instance per pass; whatever authored the camera did the
				// registration once.
				view.TagFilter = target.TagFilter;

				// **Kept beside its entity id, because `SurfaceView` does not
				// carry one and should not.** It is what the renderer takes, and
				// an entity handle in it would be a world's identifier in a type
				// the device layer reads.
				//
				// The order matters: two cameras claiming one index is a scene
				// mistake the renderer refuses by keeping the *first*, and
				// without a stable order there is no first. `Each` walks
				// archetypes in an order that moves whenever anything changes a
				// component set.
				OrderedSurfaceViews().push_back({entity.Id, view});
			}
		);

		std::sort(
			OrderedSurfaceViews().begin(),
			OrderedSurfaceViews().end(),
			[](const OrderedView &left, const OrderedView &right) { return left.first < right.first; }
		);

		views.reserve(OrderedSurfaceViews().size());
		for (const OrderedView &ordered : OrderedSurfaceViews()) {
			views.push_back(ordered.second);
		}

		return views.size();
	}

}
