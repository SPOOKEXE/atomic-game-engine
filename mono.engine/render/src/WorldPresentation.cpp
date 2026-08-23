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
#include <engine/render/WorldPresentation.hpp>
#include <engine/scene/Attachments.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Interpolation.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/scene/Visibility.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>
#include <numbers>
#include <string>
#include <type_traits>

namespace engine::render {
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

	ScenePresentationSignatures
	ScenePresentationSignaturesOf(const View &view, const ScenePresentationState &state) {
		ScenePresentationSignatures signatures;
		uint64_t &objects = signatures.Objects;
		const bool objectLayer = !view.Instances.empty() || view.Grid.Enabled || state.PostProcess.IsValid();
		if (objectLayer) {
			objects = scene::SignatureOf(view.Instances);
			objects = FoldPresentationObject(objects, view.CameraFrame);
			objects = FoldPresentationObject(objects, view.Camera);
			objects = FoldPresentation(objects, view.World);
			objects = FoldPresentation(objects, view.WorldName.Id());
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
			particles = FoldPresentation(particles, view.ParticleRevision);
			particles = FoldPresentation(particles, view.ParticleLayoutRevision);
			particles = FoldPresentation(particles, view.ParticleResidentRevision);
			particles = FoldPresentationSpan(particles, view.RibbonRuns);
			particles = FoldPresentationSpan(particles, view.RibbonVertices);
		}

		uint64_t &portals = signatures.Portals;
		const bool portalLayer = !view.Portals.empty() || !view.Surfaces.empty() || !view.Foreign.empty();
		if (portalLayer) {
			portals = FoldPresentation(portals, state.SurfaceBounces);
			portals = FoldPresentation(portals, state.SurfaceLimit);
			portals = FoldPresentation(portals, scene::SignatureOf(view.Foreign));

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
	using engine::scene::Bounds;
	using engine::scene::CharacterLimb;
	using engine::scene::DrawInstance;
	using engine::scene::LocalTransparency;
	using engine::scene::PreviousTransform;
	using engine::scene::Rendered;
	using engine::scene::SurfaceAppearance;
	using engine::scene::Tags;
	using engine::scene::Transform;
	using engine::scene::Visual;

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
			const auto write = [out, capacity, alpha](
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

				for (size_t row = 0; row < rows; row++) {
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

		{
			ENGINE_PROFILE_CAT("publish draw list", engine::core::ProfileCategory::Simulation);

			// Whatever the count said, this is how many there are. Shrinking
			// a vector writes nothing and keeps the capacity, so the frame
			// after an entity is destroyed still does not allocate.
			drawList->Instances.resize(std::min(written, drawList->Instances.size()));

			engine::core::Metrics::Count("render.instances", static_cast<double>(drawList->Instances.size()));
		}

		// **After the metric, deliberately.** `render.instances` answers
		// "how much scene is there", and a number that moved when somebody
		// turned a debugging aid on would stop being comparable across the
		// runs it exists to compare.
		//
		// The markers are appended rather than written by the loop above
		// because they are not entities: nothing in the world matches the
		// query, so there is no row to size the list against. `push_back`
		// past the shrink costs one reallocation on the frame a mirror is
		// created and nothing after it - the capacity stays.
		// **Before the markers, so a marker is never cloned.** A face bar is
		// a debugging aid lying on a pane, which means it straddles that
		// pane by construction - and a bar cloned onto the far side would
		// mark a face nothing projects off.
		//
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

		(void)engine::scene::AppendSurfaceFaceMarkers(store, drawList->Instances);
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

		const std::string suffix = "#" + std::to_string(world);
		for (const core::Name key : renderer.Pipelines()) {
			if (key.Text().ends_with(suffix)) {
				(void)renderer.RemovePipeline(key);
			}
		}

		graph::PipelineSet defaults;
		const graph::PipelineSet *available = &profiles;
		if (profiles.Count() == 0) {
			defaults.Set(core::Name("Default PBR"), graph::DefaultPbrDocument());
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
				}
			}
		);
	}
}
