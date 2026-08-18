#include <engine/assets/Builtin.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/core/Random.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Property.hpp>
#include <engine/effects/ParticleSystem.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/effects/Ribbon.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/physics/Characters.hpp>
#include <engine/physics/Query.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Attachments.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Input.hpp>
#include <engine/scene/Interpolation.hpp>
#include <engine/scene/Materials.hpp>
#include <engine/scene/MeshCatalogue.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/scene/Visibility.hpp>
#include <engine/script/Runtime.hpp>

#include <algorithm>
#include <client/Scene.hpp>
#include <cmath>
#include <format>
#include <numbers>
#include <span>

namespace client {

	using engine::core::CFrame;
	using engine::core::Color3;
	using engine::core::Random;
	using engine::core::Vector3;
	using engine::ecs::Entity;
	using engine::ecs::Phase;
	using engine::ecs::Scheduler;
	using engine::ecs::Store;
	using engine::scene::ActiveCamera;
	using engine::scene::Bounds;
	using engine::scene::DrawInstance;
	using engine::scene::PreviousTransform;
	using engine::scene::Rendered;
	using engine::scene::SurfaceAppearance;
	using engine::scene::Tags;
	using engine::scene::Transform;
	using engine::scene::Visual;
	using engine::scene::WorldBounds;

	namespace {
		constexpr float TAU = 2.0f * std::numbers::pi_v<float>;

		// The deterministic sequence used to be an integer mixer written out
		// here, and the same one again in mono.server/src/Simulation.cpp. It is
		// engine::core::Random now - same reason, one copy, and a specified
		// algorithm rather than three constants nobody can check.

		// --- systems -------------------------------------------------------
		//
		// Every one of these is a plain function. None captures anything,
		// because there is nothing outside the world for it to capture - which
		// is what makes them registerable from bindings, replayable from a
		// recording, and reusable by a second world.

		// The camera is part of the scene, so it moves in the simulation on
		// simulated time. Driving it from wall time would slide it past
		// everything it is looking at whenever the frame rate changed.
		//
		// It is a row like anything else with a place in the world: a
		// `scene::Camera` and a `scene::Transform` on an entity, with the
		// `ActiveCamera` resource naming which of a world's cameras is live.
		// That is what makes a second one - a spectator, a mirror - a create
		// rather than a rewrite of this function.
		void MoveCamera(Store &store) {
			const ActiveCamera *active = store.Resource<ActiveCamera>();
			if (active == nullptr) {
				return;
			}

			// Read out before anything is written: `Set` may move the row this
			// resource's entity handle resolves to, and holding a pointer
			// across that is holding a pointer into storage that moved.
			const Entity entity = active->Entity;

			const auto now = static_cast<float>(store.Time().Elapsed);
			const float extent = store.Resource<WorldBounds>()->HalfExtent;

			// Far enough out that the whole scene fits, and drifting slowly so
			// that the depth buffer and the culling are visibly doing
			// something.
			const float distance = extent * 1.7f + 4.0f;
			const float angle = now * 0.12f;

			const Vector3 eye{
				std::cos(angle) * distance,
				5.0f + std::sin(now * 0.21f) * 3.5f,
				std::sin(angle) * distance,
			};

			store.Set(entity, Transform{CFrame::LookAt(eye, Vector3::Zero)});

			if (engine::scene::Camera *lens = store.GetMutable<engine::scene::Camera>(entity)) {
				// The far plane follows the orbit rather than being a constant,
				// so growing the scene does not clip its far side away.
				lens->FarPlane = distance * 3.0f;
			}
		}

		// Brings the render gate in step with the tree.
		//
		// **`PreRender`, and registered before `collect-instances` in the same
		// phase**, because what it produces is presentation state and this is
		// the phase that derives presentation state. Registration order carries
		// no ordering contract - `Scheduler` says so - but these two are
		// independent in the only way that matters: a gate one frame stale
		// would be a part that appears a frame after it was parented, and the
		// phase runs them in the order they were added.
		//
		// **It is the one thing here that is structural**, which is worth
		// naming rather than hiding: adding or removing `Rendered` moves a row
		// to another archetype. That is acceptable because nothing in the
		// simulation reads `Rendered` - it exists to be a query term for the
		// draw list and for nothing else - and because every host derives it
		// the same way, so two runs of one scene still agree. The alternative
		// was `PostSimulation`, and it fails a world that presents without
		// ticking: the studio edits a suspended world, and it would have shown
		// nothing until somebody pressed play.
		void SyncVisibility(Store &store) {
			(void)engine::scene::SyncRendered(store);
		}

		// Places every surface camera parented to a part. See
		// `scene/SurfaceCameras.hpp` for the reflection and why it lives in
		// `scene` rather than in a script.
		//
		// **Before the draw list is collected, and that ordering matters here.**
		// Aiming a camera also writes its part's `Visual::Surface`, so a pass
		// that ran afterwards would publish a draw list built from last frame's
		// answer - a mirror would be one frame late to start showing anything,
		// which is invisible in a still scene and a flicker in a moving one.
		void AimSurfaces(Store &store) {
			(void)engine::scene::AimSurfaceCameras(store);
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
				// is handed columns and no entity; a component that only some
				// rows had could not be read here at all without walking the
				// world a second time.
				written = store.EachBatchParallel<
					const Transform,
					const PreviousTransform,
					const Bounds,
					const Visual,
					const SurfaceAppearance,
					const Tags,
					const Rendered>(
					[out, capacity, alpha](
						size_t first,
						size_t rows,
						const Transform *transforms,
						const PreviousTransform *previous,
						const Bounds *bounds,
						const Visual *visuals,
						const SurfaceAppearance *appearances,
						const Tags *tags,
						const Rendered *
					) {
						// The count came from a different query than the one
						// being walked. They agree, and this is what happens if
						// they ever stop: instances go missing and the number on
						// the panel drops, rather than a worker writing past the
						// end of the buffer.
						if (first >= capacity) {
							return;
						}
						rows = std::min(rows, capacity - first);

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
							out[first + row] = engine::scene::MakeDrawInstance(
								previous[row].Frame.NLerp(transforms[row].Frame, alpha),
								bounds[row],
								visuals[row],
								&appearances[row],
								&tags[row]
							);
						}
					},
					DRAW_LIST_GRAIN
				);
			}

			{
				ENGINE_PROFILE_CAT("publish draw list", engine::core::ProfileCategory::Simulation);

				// Whatever the count said, this is how many there are. Shrinking
				// a vector writes nothing and keeps the capacity, so the frame
				// after an entity is destroyed still does not allocate.
				drawList->Instances.resize(std::min(written, drawList->Instances.size()));

				engine::core::Metrics::Count(
					"render.instances", static_cast<double>(drawList->Instances.size())
				);
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
	}

	// --- what the systems need, whoever built the entities --------------------
	//
	// Split out of `BuildDemoWorld` when the scene became loadable from a
	// script, and **`BuildDemoWorld` is gone now.** There is one path: a script
	// builds the world and a client installs the two systems it owns. Keeping
	// the C++ scene beside the Luau one would have been two ways to do one job,
	// which is the most expensive kind of debt in a monorepo because both
	// accumulate callers - and the scripted path is the one that proves the
	// bindings work.

	namespace {
		Entity InstallCamera(Store &store) {
			const Entity camera = store.Create();
			store.Set<Transform>(camera, Transform{});
			store.Set<engine::scene::Camera>(camera, engine::scene::Camera{});
			return camera;
		}

		// Built-in mesh counts are available before scripts load, without delivery.
		void RecordBuiltinMeshes(Store &store) {
			engine::scene::RegisterSceneComponents();

			struct Counted {
				engine::core::Name Name;
				uint32_t Triangles = 0;
			};

			static const std::vector<Counted> BUILTINS = [] {
				std::vector<Counted> counted;
				for (uint8_t index = 0; index < engine::assets::BUILTIN_MESH_COUNT; index++) {
					const auto builtin = static_cast<engine::assets::BuiltinMesh>(index);
					const engine::assets::MeshData mesh = engine::assets::MakeBuiltin(builtin);
					counted.push_back(
						{engine::core::Name(engine::assets::BuiltinName(builtin)),
						 static_cast<uint32_t>(mesh.Indices.size() / 3)}
					);
				}
				return counted;
			}();

			for (const Counted &builtin : BUILTINS) {
				engine::scene::RecordMesh(store, builtin.Name, builtin.Triangles);
			}
		}

		void InstallResources(Store &store, Entity camera, float extent, uint32_t reserve) {
			store.SetResource(WorldBounds{extent});

			ActiveCamera live;
			live.Entity = camera;
			store.SetResource(live);
			store.SetResource(DrawList{});

			store.ResourceMutable<DrawList>()->Instances.reserve(reserve);
		}

	}

	namespace {
		// A surface camera's view beside the entity id it came from, which is
		// the only thing the sort below needs and the one thing a `SurfaceView`
		// must not grow a field for.
		using OrderedView = std::pair<uint32_t, engine::render::SurfaceView>;

		// Scratch, kept between frames so a steady scene stops allocating. The
		// same argument `scene::SurfaceCameras.cpp` makes for its own.
		std::vector<OrderedView> &Ordered() {
			static thread_local std::vector<OrderedView> ordered;
			return ordered;
		}
	}

	size_t AttachForeignSurfaces(
		engine::world::Universe &universe,
		engine::world::WorldId world,
		std::vector<engine::scene::DrawInstance> &drawn,
		std::vector<engine::scene::DrawInstance> &foreign,
		std::vector<engine::render::SurfaceView> &views
	) {
		// **Cleared on every path, including the ones that attach nothing.** The
		// vector is kept between frames so a steady scene stops allocating, and
		// a host that stopped looking through a portal would otherwise keep
		// uploading last frame's copy of another world for ever.
		//
		// **`drawn` is not**, because it arrives holding this world's own rows.
		// This pass adds to it rather than owning it.
		foreign.clear();

		if (views.empty() || !world.IsValid()) {
			return 0;
		}

		// **This world's own rows, counted before anything is appended to
		// them.** The far world's straddlers land in `drawn` further down, and
		// handing those back to `AppendPortalClones` as a source would clone a
		// clone - a body that walked in from the far room would be copied
		// straight back into it. What crosses is what this world drew.
		const auto ownRows = drawn.size();

		// This world's name, which is how the far end recognises a pane that
		// leads back here. Resolved once: `NameOf` is a registry lookup and the
		// answer cannot change while the frame is being assembled.
		const engine::core::Name here = universe.NameOf(world);

		// Which surface index wants which world, gathered while inside the
		// source store and used entirely outside it - `Universe::Enter` is not
		// re-entrant, and the far world has to be entered to be read.
		struct Wanted {
			int8_t Surface = 0;
			engine::core::Name World;
		};

		std::vector<Wanted> wanted;

		universe.Enter(world, [&wanted](Store &store) {
			store.Each<const engine::scene::Portal, const engine::scene::SurfaceCamera>(
				[&wanted](
					engine::ecs::Entity,
					const engine::scene::Portal &portal,
					const engine::scene::SurfaceCamera &camera
				) {
					if (!portal.DestinationWorld.IsValid()) {
						return;
					}
					wanted.push_back(Wanted{camera.Surface, portal.DestinationWorld});
				}
			);
		});

		if (wanted.empty()) {
			return 0;
		}

		size_t attached = 0;

		// Which far worlds have already had their own straddlers brought back
		// here. Two panes onto one world is an ordinary arrangement - a room
		// with a window at each end - and the far world's bodies belong in this
		// world's list once, not once per pane.
		std::vector<engine::world::WorldId> paired;

		// The far world's panes that lead back here, by surface slot. Gathered
		// before anything is cloned because `AppendPortalClones` walks the store
		// itself, and a walk started inside another walk is a nesting the ECS
		// does not owe anybody.
		std::vector<int8_t> returning;

		for (const Wanted &entry : wanted) {
			// **The name resolved against the universe, every frame.** A world
			// created or destroyed between two frames is ordinary in an editor,
			// and caching the handle would outlive one of those.
			engine::world::WorldId found;
			for (const engine::world::WorldId candidate : universe.Worlds()) {
				if (universe.NameOf(candidate) == entry.World) {
					found = candidate;
					break;
				}
			}

			// A name matching nothing, or naming the world we are already in,
			// leaves the surface exactly as `CollectSurfaceViews` left it - this
			// world's own image, which is a mirror and is visible as one.
			if (!found.IsValid() || found == world) {
				continue;
			}

			// **Whether the far end still owes this one its own straddlers.** A
			// hole has two mouths and a body may be standing in either, so the
			// visit below does both halves of one pair in one entry - see the
			// `paired` note under it.
			const bool returns =
				here.IsValid() && std::find(paired.begin(), paired.end(), found) == paired.end();
			if (returns) {
				paired.push_back(found);
			}

			const auto first = static_cast<uint32_t>(foreign.size());
			universe.Enter(found, [&foreign, &drawn, &returning, here, returns](Store &store) {
				// **The far world's own panes back to here, gathered before its
				// rows are copied**, because they decide which of those rows may
				// be copied at all.
				returning.clear();
				store.Each<const engine::scene::Portal, const engine::scene::SurfaceCamera>(
					[&returning, here](
						engine::ecs::Entity,
						const engine::scene::Portal &portal,
						const engine::scene::SurfaceCamera &camera
					) {
						if (portal.DestinationWorld != here) {
							return;
						}
						if (std::find(returning.begin(), returning.end(), camera.Surface) ==
							returning.end()) {
							returning.push_back(camera.Surface);
						}
					}
				);

				// **A pane is not drawn into the picture of the hole that leads
				// back to it**, which is the rule a mirror has always had about
				// itself and which a cross-world pair had nowhere to state.
				//
				// **It blanked the feature outright, and looked like the pass
				// failing.** A pair is laid out the same way at both ends - that
				// is what makes a hole read as an opening rather than as a
				// painting - so the far world's own slab stands exactly where
				// this pane's camera is aimed, at about the distance the frustum
				// is fitted to, and it is the same rectangle that frustum covers.
				// It therefore fills the image edge to edge, in one flat colour,
				// and every room behind it is hidden. What that reads as is "the
				// other world does not render its objects": the floor shows
				// wherever the slab does not quite reach, and nothing else ever
				// does.
				//
				// Selected by **slot** rather than by entity, because a draw
				// instance carries a surface index and no identity - and the
				// slots wanted are exactly the ones gathered above.
				//
				// **An invisible row is left behind too, and this range is the
				// only draw path that has to say so.** Everywhere else a fully
				// transparent part *is* drawn - into the blended run, where an
				// alpha of nothing contributes nothing. The foreign range has no
				// runs: `SurfaceView` names one span and the surface pass submits
				// it as a single plain draw, bypassing the plan that would have
				// partitioned it. So a part authored invisible arrives at the
				// **opaque** pipeline and draws solid.
				//
				// **And every cross-world portal has exactly such a part in the
				// worst possible place.** `Portal::Destination` is a stand-in - a
				// transform and a size saying where the hole leads - authored
				// invisible and set at the pane, which is where this camera is
				// aimed and the size the frustum is fitted to. It filled most of
				// the picture with one flat colour, and how much depended on
				// where the viewer stood: the report was "certain angles produce
				// the artifact".
				//
				// Dropped here rather than teaching the range about blending,
				// which is the 80/20: nothing is lost, because there was no
				// picture in it. **A *partly* transparent part in the far world
				// is still drawn opaque** - that limit is stated in
				// `NON-EUCLIDEAN.md` rather than hidden here.
				if (const auto *list = store.Resource<DrawList>()) {
					for (const DrawInstance &instance : list->Instances) {
						if (instance.Surface >= 0 &&
							std::find(returning.begin(), returning.end(), instance.Surface) !=
								returning.end()) {
							continue;
						}
						if (instance.Transparency >= 1.0f) {
							continue;
						}
						foreign.push_back(instance);
					}
				}

				// **And whoever is standing in the far world's own pane, on
				// this side of it - which is the half that was missing and is
				// why a cross-world hole only worked one way round.** The two
				// mouths of a pair are not one job done twice: the clone below
				// this block carries *our* bodies into the picture the pane
				// shows, and this one carries *theirs* into the room the pane is
				// set in. Without it a body walking into the far mouth was whole
				// in the world it was leaving and absent from this one, so the
				// hole drew a body from A into B and nothing from B into A.
				//
				// **Into this world's own list rather than into `foreign`**,
				// because that is where the far body's near half actually is: it
				// stands in this room, in front of this world's pane, and is
				// culled, sorted and lit with everything else here. `foreign` is
				// the picture *inside* the glass, and the far body is already in
				// it - the far world drew itself.
				//
				// **Selected by name and never by `entry.Surface`.** A surface
				// slot numbers a camera within one store, so the near pane's
				// index says nothing about which of the far world's cameras
				// leads back - pointing it at that index picks whichever of the
				// far world's panes happens to share the number, which is any of
				// them or none.
				if (!returns) {
					return;
				}

				// The same slots the copy above filtered by, gathered once, and
				// the same rows the copy above read - so what arrives in this
				// room is the far half of exactly what the far world drew.
				if (const auto *list = store.Resource<DrawList>()) {
					for (const int8_t slot : returning) {
						(void)engine::scene::AppendPortalClones(store, slot, list->Instances, drawn);
					}
				}
			});

			// **And whoever is standing in the pane, on the far side of it.**
			// The far world holds no copy of a body that is still in this one, so
			// without this a character halfway through a cross-world portal is
			// whole in the room it is leaving and absent from the picture of the
			// room it is entering - which is the artefact `AppendPortalClones`
			// removes for a same-world pane, seen through the one boundary that
			// pass cannot answer for itself.
			//
			// **Appended after the far world's rows, so the two are one range.**
			// A `SurfaceView` names one span, and a second one would be a second
			// draw and a second reason for the two to fall out of order.
			const auto surface = entry.Surface;
			const std::span<const DrawInstance> own(drawn.data(), ownRows);
			universe.Enter(world, [&foreign, own, surface](Store &store) {
				(void)engine::scene::AppendPortalClones(store, surface, own, foreign);
			});

			const auto count = static_cast<uint32_t>(foreign.size() - first);
			if (count == 0) {
				// A world that has published nothing yet. Left showing this
				// world rather than pointed at an empty range, because an empty
				// range clears the surface to the pass's own colour and reads as
				// a hole into nothing.
				continue;
			}

			for (engine::render::SurfaceView &view : views) {
				if (view.Index != entry.Surface) {
					continue;
				}
				view.InstanceFirst = first;
				view.InstanceCount = count;
				attached++;
			}
		}

		return attached;
	}

	size_t CollectPortalViews(Store &store, std::vector<engine::render::PortalView> &portals) {
		portals.clear();

		// **`GatherPortalSeams`, and never a second measurement of the same
		// hole.** The rectangle and the map are what `CrossPortals` moves a body
		// through; a picture that derived them its own way would be a picture
		// that disagrees with where somebody comes out, which is the exact class
		// of bug that made the camera and the body pick different panes.
		static thread_local std::vector<engine::scene::PortalSeam> seams;
		if (engine::scene::GatherPortalSeams(store, seams) == 0) {
			return 0;
		}

		for (const engine::scene::PortalSeam &seam : seams) {
			// **A cross-world pane stays on the surface path**, because a warp
			// into another world's coordinate space is a stated frame rather than
			// a derived one - `Portal::DestinationWorld` and
			// `AttachForeignSurfaces` are the whole of that arrangement, and it
			// does not recurse.
			if (seam.Crosses || seam.Surface < 0) {
				continue;
			}

			engine::render::PortalView portal;
			portal.Index = seam.Surface;
			portal.Centre = seam.Centre;
			portal.Normal = seam.Normal;
			portal.First = seam.First;
			portal.Second = seam.Second;

			// **The same one map a body is carried by.** `SeamMapping` states it
			// once for the pane rather than once per side, so what the hole shows
			// and where walking into it puts you are the same arithmetic.
			portal.Warp = engine::scene::SeamMapping(seam);
			portal.TagFilter = seam.TagFilter;

			// The hole at the far end, so the level this one opens can skip it.
			// A pair is two seams whose `Far` and `Pane` cross over, which is the
			// only place that pairing is written down.
			for (const engine::scene::PortalSeam &other : seams) {
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
		Store &store,
		std::vector<engine::render::SurfaceView> &views,
		std::span<const engine::render::PortalView> portals
	) {
		views.clear();
		Ordered().clear();

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
		static thread_local std::vector<engine::scene::SurfacePane> panes;
		(void)engine::scene::GatherSurfacePanes(store, panes);

		store.Each<const engine::scene::SurfaceCamera, const engine::scene::Camera, const Transform>(
			// `panes` needs no capture: it has static storage duration, for the
			// reason every other scratch buffer in this file does - a per-frame
			// allocation in a walk that runs once per world per frame.
			[&views, &store, portals](
				Entity entity,
				const engine::scene::SurfaceCamera &target,
				const engine::scene::Camera &lens,
				const Transform &placement
			) {
				if (const engine::scene::Portal *portal = store.template Get<engine::scene::Portal>(entity);
					portal != nullptr && !portal->Enabled) {
					return;
				}

				// **A slot the recursive pass owns gets no surface camera.** Both
				// would draw the same pane - one from a camera derived from this
				// level and one from a camera placed off the eye - and the second
				// is the viewpoint error the pass exists to remove. Skipped here
				// rather than refused in the renderer so the cost of aiming it is
				// the only thing wasted.
				const auto claimed = [&](int8_t slot) {
					for (const engine::render::PortalView &portal : portals) {
						if (portal.Index == slot) {
							return true;
						}
					}
					return false;
				};

				// A negative slot is the scene pass's explicit "do not render"
				// value. Disabled portals and edge-on mirrors clear their old slots
				// this way, so collecting them would preserve the stale camera as an
				// invalid surface view instead of stopping its capture.
				if (target.Surface < 0) {
					return;
				}

				if (claimed(target.Surface)) {
					return;
				}

				engine::render::SurfaceView view;
				view.Index = target.Surface;
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
				for (const engine::scene::SurfacePane &pane : panes) {
					if (pane.Camera != entity) {
						continue;
					}

					view.PaneCentre = pane.Centre;
					view.PaneNormal = pane.Normal;
					view.PaneFirst = pane.First;
					view.PaneSecond = pane.Second;
					view.PaneNear = pane.NearPlane;
					view.PaneFar = pane.FarPlane;
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
				if (const engine::scene::SurfaceLens *fitted =
						store.template Get<engine::scene::SurfaceLens>(entity);
					fitted != nullptr) {
					view.Projection = engine::scene::SurfaceProjection(*fitted, placement.Frame);

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
					view.Mapping = engine::scene::SurfaceMapping(*fitted);
				} else {
					const float aspect = static_cast<float>(target.Width) /
										 static_cast<float>(std::max<uint16_t>(target.Height, 1));
					view.Projection = engine::scene::ResolveCamera(placement.Frame, lens, aspect).Projection;
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
				Ordered().push_back({entity.Id, view});
			}
		);

		std::sort(Ordered().begin(), Ordered().end(), [](const OrderedView &left, const OrderedView &right) {
			return left.first < right.first;
		});

		views.reserve(Ordered().size());
		for (const OrderedView &ordered : Ordered()) {
			views.push_back(ordered.second);
		}

		return views.size();
	}

	namespace {
		// Where a moved particle lives for the rest of the frame.
		//
		// **One buffer for every batch, and each batch takes a range of it.** The
		// spans handed to the renderer have to survive until it has drawn them, so
		// they cannot point at anything a loop iteration owns - and they cannot be
		// appended to while an earlier span is outstanding unless the buffer is
		// reserved. It is cleared once a frame and reserved to the pool's own size
		// the first time anything crosses, so no batch's span is ever invalidated
		// by a later batch's copy.
		std::vector<engine::effects::ParticleInstance> &Carried() {
			static thread_local std::vector<engine::effects::ParticleInstance> carried;
			return carried;
		}

		// Moves every particle in a batch that has gone through a hole.
		//
		// **Moved and not copied**, which is the whole difference from what a
		// straddling body needs. A body has a size and is cut by the plane; a
		// particle is a point, wholly on one side or the other, and drawing it in
		// both places would be two sparks where the author authored one.
		//
		// Returns the batch's own span when nothing crossed, so a scene with a
		// hole in it and no particles near it copies nothing at all.
		std::span<const engine::effects::ParticleInstance> CarryThroughSeams(
			const std::vector<engine::scene::PortalSeam> &seams,
			const std::vector<engine::scene::SeamTransform> &maps,
			std::span<const engine::effects::ParticleInstance> particles
		) {
			bool any = false;
			for (const engine::effects::ParticleInstance &particle : particles) {
				for (const engine::scene::PortalSeam &seam : seams) {
					if (!seam.Crosses && engine::scene::SeamCarries(seam, particle.Position)) {
						any = true;
						break;
					}
				}
				if (any) {
					break;
				}
			}

			if (!any) {
				return particles;
			}

			std::vector<engine::effects::ParticleInstance> &carried = Carried();
			const size_t first = carried.size();

			for (const engine::effects::ParticleInstance &particle : particles) {
				engine::effects::ParticleInstance moved = particle;
				for (size_t index = 0; index < seams.size(); index++) {
					if (seams[index].Crosses ||
						!engine::scene::SeamCarries(seams[index], particle.Position)) {
						continue;
					}

					// `Point`, because a position moves and scales. The packed
					// size is left alone: it is an unsigned normalised pair over a
					// sixty-four metre ceiling, and unpacking and repacking it per
					// particle to serve a mismatched pair of panes is a cost every
					// matched pair would also pay for nothing.
					moved.Position = maps[index].Point(particle.Position);
					break;
				}
				carried.push_back(moved);
			}

			return {carried.data() + first, particles.size()};
		}
	}

	size_t CollectParticleBatches(Store &store, std::vector<engine::render::ParticleBatch> &batches) {
		batches.clear();

		const auto *system = store.Resource<engine::effects::ParticleSystem>();
		if (system == nullptr || system->Blocks.empty()) {
			return 0;
		}

		// **The holes, so a spark that has gone through one is drawn in the room
		// it went into.** A particle is a point rather than a body: it is on one
		// side of a pane or the other and belongs wholly to whichever space that
		// is, so it is *moved* rather than cut and copied. A torch carried into a
		// doorway keeps the sparks this side of the plane where they are and the
		// ones past it arrive in the far room, which is what stops a flame dying
		// at the seam while the torch holding it does not.
		//
		// **A scratch copy, because the pool is the effects system's.** A batch is
		// normally a span straight into `ParticleSystem::Instances` and nothing is
		// copied at all; only an emitter with a particle actually through a hole
		// pays for one, and it pays once per frame.
		//
		// **Reserved to the whole pool before anything is written, and that is
		// load-bearing rather than tidy.** Every batch's span points into this
		// buffer and every span has to survive until the renderer has drawn it, so
		// a later batch that grew it would leave an earlier one pointing at freed
		// memory. The pool is the ceiling on how much can ever be carried, so one
		// reserve makes reallocation impossible rather than unlikely.
		static thread_local std::vector<engine::scene::PortalSeam> seams;
		const bool holed = engine::scene::GatherPortalSeams(store, seams) > 0;

		static thread_local std::vector<engine::scene::SeamTransform> maps;
		maps.clear();

		Carried().clear();
		if (holed) {
			Carried().reserve(system->Instances.size());
			for (const engine::scene::PortalSeam &seam : seams) {
				maps.push_back(engine::scene::SeamMapping(seam));
			}
		}

		// **Walked from the emitter column rather than from the block list**, and
		// the direction matters: a block knows how many particles it has and
		// nothing about what they look like, and the shared half - texture, blend
		// mode, flipbook - is on the emitter. Walking blocks would mean a lookup
		// from `EmitterBlock::Owner` back to a row per block, which is a random
		// access per emitter to avoid a sequential one.
		//
		// The order is therefore the emitter column's, which is stable within a
		// tick - so the batch list is the same every frame and the draw order does
		// not shuffle.
		store.Each<const engine::effects::ParticleEmitter, const engine::effects::EmitterSlot>(
			[&](engine::ecs::Entity,
				const engine::effects::ParticleEmitter &emitter,
				const engine::effects::EmitterSlot &slot) {
				if (slot.Index == engine::effects::NO_SLOT || slot.Index >= system->Blocks.size()) {
					return;
				}

				const engine::effects::EmitterBlock &block = system->Blocks[slot.Index];
				if (block.Live == 0) {
					// **Skipped rather than emitted as an empty batch.** An empty
					// batch is a uniform push and a pipeline bind for zero
					// vertices, and in a scene of a hundred thousand emitters most
					// of them are empty at any moment.
					return;
				}

				engine::render::ParticleBatch batch;
				batch.Particles = {system->Instances.data() + block.First, block.Live};

				if (holed) {
					batch.Particles = CarryThroughSeams(seams, maps, batch.Particles);
				}

				batch.Texture = emitter.Texture;
				batch.FlipbookSide = static_cast<float>(engine::effects::FlipbookSide(emitter.Flipbook));
				batch.ZOffset = emitter.ZOffset;
				batch.LightEmission = emitter.LightEmission;
				batch.LightInfluence = emitter.LightInfluence;
				batch.Additive = emitter.Additive;
				batch.WorldUp =
					emitter.Orientation == engine::effects::ParticleOrientation::FacingCameraWorldUp;
				batches.push_back(batch);
			}
		);

		return batches.size();
	}

	size_t CollectLights(Store &store, const Vector3 &eye, std::vector<engine::render::SceneLight> &lights) {
		lights.clear();

		// Gathered whole, then ordered, then cut. **Not cut during the walk**,
		// because "the sixteen nearest" cannot be decided until the far ones have
		// been seen - a partial sort over the whole set is the only form that is
		// correct, and a scene has tens of lights rather than thousands.
		store.Each<const engine::scene::Light>([&](engine::ecs::Entity entity,
												   const engine::scene::Light &bulb) {
			if (!bulb.Enabled || bulb.Brightness <= 0.0f || bulb.Range <= 0.0f) {
				return;
			}

			const engine::ecs::Entity parent = store.ParentOf(entity);
			if (parent == engine::ecs::NULL_ENTITY) {
				return;
			}

			engine::core::CFrame frame;
			if (const auto *point = store.Get<engine::scene::Attachment>(parent)) {
				frame = point->WorldFrame;
			} else if (const auto *placement = store.Get<Transform>(parent)) {
				frame = placement->Frame;
			} else {
				// No place to shine from. Skipped rather than placed at the
				// origin - see the header.
				return;
			}

			engine::render::SceneLight light;
			light.Position = frame.Position;
			light.Range = bulb.Range;

			// Brightness folded into the colour here, once per light per
			// frame, rather than in the shader once per light per fragment.
			light.Colour = engine::core::Color3{
				bulb.Colour.R * bulb.Brightness,
				bulb.Colour.G * bulb.Brightness,
				bulb.Colour.B * bulb.Brightness,
			};

			if (bulb.Kind == engine::scene::LightKind::Point) {
				// -1 is the value the shader reads as "never clip", which is
				// what a point light is, and it needs no branch of its own
				// there.
				light.ConeCosine = -1.0f;
			} else {
				// The face's normal, turned into world space by whatever the
				// light hangs off. A spot on an attachment points along the
				// attachment, which is what an attachment carries an
				// orientation for.
				light.Direction = frame.VectorToWorldSpace(engine::scene::NormalOf(bulb.Face));

				// Half the authored angle, as a cosine. Roblox's `Angle` is
				// the full cone width, and the dot product test is against the
				// half - halving in the shader would be doing it per fragment.
				light.ConeCosine = std::cos(
					std::clamp(bulb.Angle, 0.0f, 180.0f) * 0.5f * std::numbers::pi_v<float> / 180.0f
				);
			}

			lights.push_back(light);
		});

		// **And the same lamps again on the far side of every hole they reach.**
		// A torch carried up to a portal lights the room beyond it, which is what
		// "light works through a portal" means to somebody looking at one. The
		// copy is the lamp mapped by the seam: `Point` for where it is, `Length`
		// for how far it reaches, `Rotate` for which way a spot points - the same
		// four applications a body, a camera and a ray go through, and mixing two
		// of them up is a light that leads somewhere slightly wrong.
		//
		// **Where it lands is the same place the sub-camera stands**, which is
		// what makes this right rather than plausible: the map carries the front
		// of this pane to the *back* of the far one, so a lamp in front of a hole
		// arrives behind the far pane, shining forward into the room the hole
		// shows. A camera does exactly that and for exactly that reason.
		//
		// **It ignores the aperture, and is no less correct than the lamp it
		// copies.** A transported light spills into the whole far room rather
		// than the hole's beam - and a local light in this pipeline is unshadowed
		// and already spills through every wall in the world. When local shadows
		// arrive the copy inherits them for free, because it is an ordinary entry
		// in the same buffer. `NON-EUCLIDEAN.md` Part V.3.
		//
		// **One hop.** A copy is never itself copied through a second seam: two
		// hops is a geometric series inside a sixteen-entry budget, and a room
		// two holes away is not lit by a candle.
		{
			static thread_local std::vector<engine::scene::PortalSeam> seams;
			if (engine::scene::GatherPortalSeams(store, seams) > 0) {
				const size_t own = lights.size();
				for (size_t index = 0; index < own; index++) {
					for (const engine::scene::PortalSeam &seam : seams) {
						// A cross-world pane's destination is a camera stand-in
						// in *this* world, so a lamp through one would light a
						// spot a metre behind the pane rather than the world it
						// leads to. The same rule the copy pass has.
						if (seam.Crosses) {
							continue;
						}

						// Out of reach of the hole itself, so nothing of it gets
						// through - measured against the rectangle rather than
						// its plane, or every lamp in a building would transport
						// through every pane in it.
						if (engine::scene::SeamDistance(seam, lights[index].Position) >=
							lights[index].Range) {
							continue;
						}

						const engine::scene::SeamTransform through = engine::scene::SeamMapping(seam);

						engine::render::SceneLight copy = lights[index];
						copy.Position = through.Point(lights[index].Position);
						copy.Range = through.Length(lights[index].Range);
						copy.Direction = through.Rotate(lights[index].Direction);
						lights.push_back(copy);
					}
				}
			}
		}

		if (lights.size() > engine::render::MAX_SCENE_LIGHTS) {
			std::partial_sort(
				lights.begin(),
				lights.begin() + engine::render::MAX_SCENE_LIGHTS,
				lights.end(),
				[&eye](const engine::render::SceneLight &left, const engine::render::SceneLight &right) {
					// Squared, because the square root is monotonic and cannot
					// change an ordering - `scene::OrderForDrawing`'s reason.
					const Vector3 a = left.Position - eye;
					const Vector3 b = right.Position - eye;
					return a.Dot(a) < b.Dot(b);
				}
			);
			lights.resize(engine::render::MAX_SCENE_LIGHTS);
		}

		return lights.size();
	}

	namespace {
		// The three effects systems, installed together because they are one
		// dependency chain and installing two of the three is a scene where
		// nothing emits.
		//
		// **`ResolveAttachments` first and in `PreSimulation`**, because an
		// emitter parented to an attachment reads that attachment's world frame
		// when it spawns - and spawning happens in the same phase. Resolving after
		// would emit from where the attachment was last frame, which on a fast
		// projectile is a visible lag between the rocket and its exhaust.
		//
		// **`RecordTrails` in the simulation and not here**, which is the one that
		// does not follow the pattern: a trail is a record of where something has
		// been, so sampling it at frame rate would make its length depend on the
		// machine drawing it. `Ribbon.hpp` carries the argument.
		void InstallEffects(Store &store, Scheduler &scheduler, uint32_t poolCapacity) {
			engine::effects::RegisterEffectClasses();

			if (!store.HasResource<engine::effects::ParticleSystem>()) {
				engine::effects::InstallParticles(store, poolCapacity);
			}
			if (!store.HasResource<engine::effects::RibbonBuffer>()) {
				store.SetResource(engine::effects::RibbonBuffer{});
			}

			// **Registered in both phases, and that is not a duplicate system.**
			// `ResolveAttachments` is a pure recompute of a cache - one multiply
			// per attachment, from state it does not own - so running it twice
			// gives the same answer twice, which is what makes this safe where
			// two *different* systems writing one field would not be.
			//
			// It has a consumer in each phase and they need different things:
			//
			//   - `refresh-emitters`, below, reads `Attachment::WorldFrame` to
			//     place a spawn, and it runs in `PreSimulation`. Resolving only
			//     at `PreRender` would hand it the previous tick's frame, so a
			//     rocket's exhaust would trail its nozzle by a tick.
			//   - `client::CollectLights` reads it to place a lamp, and it runs
			//     at present time. A world that is being *authored* never ticks
			//     at all - `World::Present` runs `PreRender` alone - so
			//     resolving only at `PreSimulation` left every attachment at the
			//     identity and every lamp in the studio lighting the origin.
			//
			// `Attachments.hpp` says this pass runs in `PreRender`; it was
			// registered in `PreSimulation` alone, and the header was the half
			// that was right about the draw path. Both are true and both are
			// declared.
			scheduler.Add("resolve-attachments", Phase::PreSimulation, [](Store &world) {
				(void)engine::scene::ResolveAttachments(world);
			});
			scheduler.Add("refresh-emitters", Phase::PreSimulation, [](Store &world) {
				(void)engine::effects::RefreshEmitters(world);
			});
			scheduler.Add("step-particles", Phase::Simulation, [](Store &world) {
				(void)engine::effects::StepParticles(world, static_cast<float>(world.Time().Delta));
			});
			scheduler.Add("record-trails", Phase::Simulation, [](Store &world) {
				(void)engine::effects::RecordTrails(world, static_cast<float>(world.Time().Delta));
			});
			// The `PreRender` half of the pair above. First in this phase, so
			// `build-ribbons` and everything the host reads after `Present` -
			// `client::CollectLights`, `CollectParticleBatches` - see a frame
			// resolved against the transforms this frame is being drawn with.
			scheduler.Add("resolve-attachments", Phase::PreRender, [](Store &world) {
				(void)engine::scene::ResolveAttachments(world);
			});
			scheduler.Add("build-ribbons", Phase::PreRender, [](Store &world) {
				const auto *active = world.Resource<ActiveCamera>();
				const engine::scene::Transform *eye =
					active == nullptr ? nullptr : world.Get<engine::scene::Transform>(active->Entity);
				(void)engine::effects::BuildRibbons(
					world,
					eye == nullptr ? Vector3::Zero : eye->Frame.Position,
					static_cast<float>(world.Time().Elapsed)
				);
			});
		}

		// The camera and character systems, installed together.
		//
		// **`InputState` is created here and never by a script**, because a world
		// with no resource is one where every input query answers "nothing
		// pressed" - which is exactly right for a server and exactly wrong for a
		// client that forgot to install it. Creating it at install time makes the
		// presence of the resource mean "somebody is looking at this world".
		//
		// **The ground check is the client's rather than `scene`'s**, and that is
		// the tier doing its job: `scene` may not link `physics`, so
		// `StepCharacters` reads `Humanoid::Grounded` and this is what writes it.
		// The same split `replication::DistancePriority::Blocked` already has -
		// the arithmetic there, the query here.
		void InstallControls(Store &store, Scheduler &scheduler) {
			if (!store.HasResource<engine::scene::InputState>()) {
				store.SetResource(engine::scene::InputState{});
			}
			if (!store.HasResource<engine::scene::CameraController>()) {
				store.SetResource(engine::scene::CameraController{});
			}

			// **Camera control in `PreRender` and character control in
			// `Simulation`**, which is not an inconsistency. A camera is
			// presentation - it should turn at frame rate, because a mouse moves
			// at frame rate and a camera locked to the tick judders. A character
			// moves a body the physics step integrates, so it has to be on the
			// tick or two players at different frame rates would move at different
			// speeds.
			scheduler.Add("camera-control", Phase::PreRender, [](Store &world) {
				(void)engine::scene::UpdateCameraControl(world);
				(void)engine::scene::PlaceCamera(world);
			});

			scheduler.Add("character-control", Phase::PreSimulation, [](Store &world) {
				(void)engine::scene::UpdateCharacterControl(world);
			});

			// **The other three are `physics`', because grounding needs a
			// query.** They used to be a static `GroundCharacters` in this file
			// plus two lambdas beside it, which meant a dedicated server hosting
			// the same world had no grounding at all and its characters could
			// never jump. `physics::RegisterCharacterSystems` is the one
			// installation, and a client, a server and the studio share it.
			engine::physics::RegisterCharacterSystems(scheduler);
		}

		// How many particles a world's pool holds when nobody has said.
		//
		// **Half a million, because that is the number `ROADMAP.md` v0.10 asks
		// for by name** - a hundred thousand emitters at five particles each - and
		// a default below it makes the engine's stated target something every
		// scene has to opt into.
		//
		// **The cost is paid whether or not a world has an effect in it**, which
		// is what makes this a real decision rather than a generous one: the pool
		// is allocated up front and never grows (`ParticleSystem::Capacity` gives
		// the reason), so this is 524,288 slots times 68 bytes across the instance
		// and state arrays - **about 36 MB a world**, resident from the moment
		// presentation is installed.
		//
		// That is affordable for one world and is not for twenty. A host that
		// opens several at once - the studio with more than one place loaded -
		// should pass its own figure rather than take this one, and
		// `InstallParticles` takes the capacity for exactly that reason.
		//
		// **Measured before it was raised**: at 250,000 the stress scene's grid
		// starved after about 41,000 of its 102,400 emitters, and the symptom was
		// an effect that simply was not there rather than an error -
		// `ParticleStatistics::EmittersRefused` is the number that says so.
		constexpr uint32_t DEFAULT_PARTICLE_POOL = 524288;
	}

	bool BuildScriptedWorld(
		Store &store,
		Scheduler &scheduler,
		const std::string &path,
		uint32_t reserve,
		std::shared_ptr<engine::script::Runtime> *runtime
	) {
		// Before anything mints an automatic id for `DrawList`. See
		// `RegisterClientComponents`: `Components::Of<T>` caches its answer per
		// type per process, so an explicit registration that arrives second
		// aborts rather than quietly leaving two names for one thing.
		RegisterClientComponents();

		// **Before the script runs, not after.** `InstallEffects` below registers
		// the same classes and is too late: the script is what calls
		// `Instance.new("ParticleEmitter")`, and a class table that gains the name
		// afterwards is a scene whose emitters all failed to resolve.
		engine::effects::RegisterEffectClasses();

		// Register built-in metadata before the script can query it.
		RecordBuiltinMeshes(store);

		// The scene, the components and the systems that move it are the
		// engine's and every program's. What follows is the client's half.
		std::string error;
		if (!engine::examples::LoadScene(store, scheduler, path, error, runtime)) {
			ENGINE_ERROR("script '{}' failed:\n{}", path, error);
			return false;
		}

		const float extent = store.Resource<WorldBounds>()->HalfExtent;

		// **A scene that placed its own camera keeps it.** `MoveCamera` is this
		// client's placeholder - it orbits whatever `ActiveCamera` names so that
		// a scene with no camera of its own is still looked at from somewhere -
		// and running it beside a script that aimed one is two things writing
		// one `Transform`, the second winning silently every tick.
		//
		// That is not hypothetical: `Mirrors-1-world.luau` computes its
		// reflection camera from where the eye stands, so an orbiting eye makes
		// the reflection correct for a position the viewer is no longer at. The
		// mirror looked broken and the camera was the reason.
		const auto *existing = store.Resource<ActiveCamera>();
		const bool scripted = existing != nullptr && existing->Entity != engine::ecs::NULL_ENTITY &&
							  store.Alive(existing->Entity);

		const Entity camera = scripted ? existing->Entity : InstallCamera(store);
		InstallResources(store, camera, extent, std::max<uint32_t>(reserve, 1));

		if (!scripted) {
			scheduler.Add("move-camera", Phase::Simulation, MoveCamera);
		}

		// `PreRender`, ahead of the pass that reads it - `InstallPresentation`
		// carries the argument, and this is the same three lines.
		scheduler.Add("resolve-materials", Phase::PreRender, [](Store &world) {
			(void)engine::scene::ResolveMaterials(world);
		});
		scheduler.Add("sync-rendered", Phase::PreRender, SyncVisibility);
		scheduler.Add("aim-surface-cameras", Phase::PreRender, AimSurfaces);
		scheduler.Add("collect-instances", Phase::PreRender, CollectInstances);
		InstallEffects(store, scheduler, DEFAULT_PARTICLE_POOL);
		InstallControls(store, scheduler);
		return true;
	}

	bool InstallDefaultCamera(Store &store, Scheduler &scheduler) {
		const auto *existing = store.Resource<ActiveCamera>();
		if (existing != nullptr && existing->Entity != engine::ecs::NULL_ENTITY &&
			store.Alive(existing->Entity)) {
			return false;
		}

		ActiveCamera live;
		live.Entity = InstallCamera(store);
		store.SetResource(live);

		scheduler.Add("move-camera", Phase::Simulation, MoveCamera);
		return true;
	}

	engine::core::Name InstallRenderingProfiles(
		const engine::graph::PipelineSet &profiles,
		engine::render::Renderer &renderer,
		uint64_t world,
		engine::core::Name selectedProfile
	) {
		engine::graph::RegisterRenderNodeKinds();

		const std::string suffix = "#" + std::to_string(world);
		for (const engine::core::Name key : renderer.Pipelines()) {
			if (key.Text().ends_with(suffix)) {
				(void)renderer.RemovePipeline(key);
			}
		}

		engine::graph::PipelineSet defaults;
		const engine::graph::PipelineSet *set = &profiles;
		if (profiles.Count() == 0) {
			defaults.Set(engine::core::Name("Default PBR"), engine::graph::DefaultPbrDocument());
			set = &defaults;
		}

		std::vector<engine::core::Name> candidates;
		const auto addCandidate = [&](engine::core::Name name) {
			if (name.IsValid() && set->Find(name) != nullptr &&
				std::find(candidates.begin(), candidates.end(), name) == candidates.end()) {
				candidates.push_back(name);
			}
		};
		addCandidate(selectedProfile);
		addCandidate(engine::core::Name("Default PBR"));
		for (const engine::core::Name name : set->Names()) {
			addCandidate(name);
		}

		for (const engine::core::Name name : candidates) {
			const engine::graph::PipelineDocument *document = set->Find(name);
			assert(document != nullptr);

			engine::graph::RenderGraph graph;
			engine::core::Name offender;
			const engine::graph::PipelineDocumentStatus status =
				engine::graph::Build(*document, graph, offender);
			if (status != engine::graph::PipelineDocumentStatus::Ok) {
				ENGINE_ERROR(
					"pipeline '{}' does not build: {} at '{}'",
					name.Text(),
					engine::graph::Describe(status),
					offender.Text()
				);
				continue;
			}

			const engine::core::Name key(std::format("{}#{}", name.Text(), world));
			if (renderer.SetPipeline(key, graph)) {
				return key;
			}
		}
		return {};
	}

	void RegisterClientComponents() {
		// **A `DrawList` is derived state, and its serialisation says so by
		// writing nothing.**
		//
		// It had no registration at all before v0.7, which meant
		// `Store::SetResource` minted one under the compiler's spelling of the
		// type - rule 4's exact failure, sitting unnoticed because nothing had
		// ever tried to snapshot a world that had one. The studio's Stop does:
		// it saves the universe when Play is pressed and restores it when Stop
		// is, and `Store::Save` refuses a resource with no serialisation rather
		// than writing bytes that cannot be read back. That refusal is correct
		// and this is the fix for it.
		//
		// Nothing is written and nothing is read because the list is rebuilt by
		// `collect-instances` in `PreRender`, every frame, before anything
		// looks at it. Writing a frame's worth of interpolated cubes into every
		// save file would be storing an answer that is recomputed before it is
		// ever used.
		engine::ecs::Components::Register<DrawList>(
			"client.DrawList",
			[](engine::core::ByteWriter &, const void *, size_t) {},
			[](engine::core::ByteReader &, void *destination, size_t count) {
				auto *lists = static_cast<DrawList *>(destination);
				for (size_t index = 0; index < count; index++) {
					lists[index].Instances.clear();
				}
			}
		);
	}

	void InstallPresentation(Store &store, Scheduler &scheduler, uint32_t reserve) {
		RegisterClientComponents();

		// Game-file and studio worlds need the same built-in metadata.
		RecordBuiltinMeshes(store);

		if (!store.HasResource<DrawList>()) {
			store.SetResource(DrawList{});
			store.ResourceMutable<DrawList>()->Instances.reserve(reserve);
		}

		if (!store.HasResource<WorldBounds>()) {
			// A default rather than nothing. `WorldBounds` is what the
			// replication wire quantises against and what a camera would frame,
			// and a world opened in an editor has authored no such number - so
			// it gets the type's own default instead of a missing resource
			// somebody later reads through a null pointer.
			store.SetResource(WorldBounds{});
		}

		// **The same system `engine::examples` installs, from the same place.**
		// It moved into `scene` at v0.7 precisely so this call site could exist:
		// two copies of a system that writes `PreviousTransform` can both be
		// installed into one world, and the second wins silently every tick.
		scheduler.Add("capture-previous", Phase::PreSimulation, engine::scene::CapturePreviousTransforms);

		// **Who a teleport brings in, and it must not depend on scripts.** A
		// destination is chosen by a script in *another* world, so a world can be
		// somebody's destination without containing a line of code - and
		// admitting used to happen inside the Luau runtime's own delivery pump.
		// A world with no runtime took the payload into its inbox and left it
		// there: destroyed in the world you left, never built in the world you
		// went to. `script::RegisterTeleportAdmission` carries the argument.
		engine::script::RegisterTeleportAdmission(scheduler);

		// **`PreRender`, ahead of everything that reads a `SurfaceAppearance`.**
		// A `Material` instance names an asset and the part it hangs off is what
		// the draw-list pass reads, so resolving after collection would draw last
		// frame's texture for a frame every time somebody changed one.
		//
		// **It was `PreSimulation`, and that made it do nothing at all in an
		// edited world.** `World::Present` runs `PreRender` alone and the studio
		// never ticks while somebody is authoring, so a `Material` dropped onto
		// a part changed the part's appearance only after Play was pressed -
		// which reads as the material not being loaded rather than as a pass
		// that did not run. The same mistake put every part at the origin
		// through `PreviousTransform`; `studio::PresentationAlpha` is that one.
		//
		// Nothing in the tick reads a `SurfaceAppearance`, so the phase this
		// belongs in is the one its only consumer runs in. It now costs once per
		// *frame* rather than once per tick, which is a walk of the material
		// instances - a handful in a scene, against a draw list of thousands.
		scheduler.Add("resolve-materials", Phase::PreRender, [](Store &world) {
			(void)engine::scene::ResolveMaterials(world);
		});

		scheduler.Add("sync-rendered", Phase::PreRender, SyncVisibility);

		// **And the mirrors, which only `BuildScriptedWorld` was installing.**
		// That is why a mirror worked under `--scene` and was a plain white
		// rectangle everywhere else: the studio, `--game` and an imported world
		// all come through here, and none of them was aiming anything.
		//
		// The visible half of that failure is not the camera at all - it is
		// step 4 of `scene/SurfaceCameras.hpp`. Aiming a camera is also what
		// writes `Visual::Surface` on the pane it is parented to, so without
		// this system the pane keeps the component's default of -1, samples no
		// texture, and draws as its own flat `Tint`. `Mirrors-1-world.luau`
		// tints its pane white, so the symptom was a white part beside a frame
		// that was rendering perfectly - which reads as a broken surface pass
		// rather than as a missing system.
		//
		// **Between the two, not beside them.** `sync-rendered` decides what is
		// drawn at all and `collect-instances` reads the `Visual` this writes,
		// so a mirror aimed after collection would publish last frame's answer.
		scheduler.Add("aim-surface-cameras", Phase::PreRender, AimSurfaces);
		scheduler.Add("collect-instances", Phase::PreRender, CollectInstances);

		// **The same three systems `BuildScriptedWorld` installs, from the same
		// place.** This is the argument `aim-surface-cameras` already makes one
		// line up, arriving again: the studio, `--game` and an imported world all
		// come through here, and a world with emitters and no step is a world
		// whose effects are authored, saved, loaded and then motionless.
		InstallEffects(store, scheduler, DEFAULT_PARTICLE_POOL);
		InstallControls(store, scheduler);
	}
}
