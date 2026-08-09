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
#include <engine/physics/Query.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Attachments.hpp>
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

#include <algorithm>
#include <client/Scene.hpp>
#include <cmath>
#include <format>
#include <numbers>

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
		// engine::core::Random now — same reason, one copy, and a specified
		// algorithm rather than three constants nobody can check.

		// --- systems -------------------------------------------------------
		//
		// Every one of these is a plain function. None captures anything,
		// because there is nothing outside the world for it to capture — which
		// is what makes them registerable from bindings, replayable from a
		// recording, and reusable by a second world.

		// The camera is part of the scene, so it moves in the simulation on
		// simulated time. Driving it from wall time would slide it past
		// everything it is looking at whenever the frame rate changed.
		//
		// It is a row like anything else with a place in the world: a
		// `scene::Camera` and a `scene::Transform` on an entity, with the
		// `ActiveCamera` resource naming which of a world's cameras is live.
		// That is what makes a second one — a spectator, a mirror — a create
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
		// no ordering contract — `Scheduler` says so — but these two are
		// independent in the only way that matters: a gate one frame stale
		// would be a part that appears a frame after it was parented, and the
		// phase runs them in the order they were added.
		//
		// **It is the one thing here that is structural**, which is worth
		// naming rather than hiding: adding or removing `Rendered` moves a row
		// to another archetype. That is acceptable because nothing in the
		// simulation reads `Rendered` — it exists to be a query term for the
		// draw list and for nothing else — and because every host derives it
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
		// answer — a mirror would be one frame late to start showing anything,
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
		// `NLerp` — the same shape of arithmetic, the same reciprocal square
		// root, over roughly three times the bytes. `Integrate.hpp` measures its
		// crossover at 8,000 rows, and 1024 puts this loop's floor at the same
		// 8192.
		//
		// **What it replaces is the default, and the default was certainly
		// wrong.** `Jobs::DEFAULT_GRAIN` is calibrated for three float adds per
		// row; taking it put this loop's floor at 32,768 instances, so a scene of
		// twenty thousand parts ran the whole draw list on one thread — the exact
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
			// answers to "why is this system slow" — a cached query that is not
			// as cached as it looks, a vector reallocating every frame, or the
			// interpolation itself. One number covering all three cannot tell
			// them apart.
			//
			// It stops here. Going finer means a scope *inside* the row loop,
			// and a scope costs a clock read and a push — several times what a
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
				// The count is a floor rather than a contract — it comes from a
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

				// Taken once, outside. A worker cannot grow the vector — that is
				// a reallocation under every other worker's feet — so the buffer
				// is sized before the loop starts and the body writes into it.
				DrawInstance *const out = drawList->Instances.data();
				const size_t capacity = drawList->Instances.size();

				// **`Rendered` is in the signature and nothing reads it**, which
				// is the point of a tag: it is a term in the query, so the
				// archetype walk never reaches a row that has not been marked as
				// a visible descendant of `Workspace`. A branch here could not
				// have done the same job — this loop writes `out[first + row]`
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
							// each one five times and then jumps — which reads as
							// a frame-rate problem rather than as a tick-rate one.
							//
							// NLerp, not Lerp. The endpoints are one simulation
							// tick apart — a few degrees at most — and over an arc
							// that short the two agree to well inside a pixel.
							// Lerp's constant angular speed costs an acos and
							// three sin calls per entity, which on this loop was
							// the single most expensive thing in the frame.
							//
							// A `CFrame` and a half-extent, not a matrix: this is
							// what the world knows, and `render` is what turns it
							// into something a GPU binds.
							out[first + row] = DrawInstance{
								previous[row].Frame.NLerp(transforms[row].Frame, alpha),
								bounds[row].HalfExtent,
								visuals[row].Tint,
								visuals[row].Mesh,
								appearances[row].ColourMap,
								appearances[row].NormalMap,
								appearances[row].RoughnessMap,
								appearances[row].OcclusionMap,
								appearances[row].EmissiveMap,
								tags[row].Mask,

								// Copied rather than resolved here. Which pass this
								// instance lands in is the renderer's decision,
								// because it depends on where the camera is — and
								// this loop runs once for a world that may be drawn
								// from several views.
								visuals[row].Transparency,
								visuals[row].Surface,
								visuals[row].CastShadow,
								appearances[row].Mode,
							};
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
			// created and nothing after it — the capacity stays.
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
	// accumulate callers — and the scripted path is the one that proves the
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

	size_t CollectSurfaceViews(Store &store, std::vector<engine::render::SurfaceView> &views) {
		views.clear();
		Ordered().clear();

		store.Each<const engine::scene::SurfaceCamera, const engine::scene::Camera, const Transform>(
			[&views](
				Entity entity,
				const engine::scene::SurfaceCamera &target,
				const engine::scene::Camera &lens,
				const Transform &placement
			) {
				engine::render::SurfaceView view;
				view.Index = target.Surface;
				view.Frame = placement.Frame;
				view.Lens = lens;
				view.Width = target.Width;
				view.Height = target.Height;

				// **Opacity here, transparency in the component**, and the flip
				// happens once. `scene::SurfaceCamera::ImageTransparency` is
				// authored the way a script thinks — 0 is solid, like every
				// other transparency in this engine — and the shader multiplies
				// by the opposite, so converting at the boundary beats one
				// subtraction in a shader nobody can put a breakpoint in.
				// **Not clamped here.** The property setter is the authored gate
				// and `Renderer` clamps again at its own boundary because
				// `SurfaceView` is a public struct any host fills — a third copy
				// in between makes none of the three read as the authority, and
				// a future widening of the range has to find all of them.
				view.ImageOpacity = 1.0f - target.ImageTransparency;

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

	size_t CollectParticleBatches(Store &store, std::vector<engine::render::ParticleBatch> &batches) {
		batches.clear();

		const auto *system = store.Resource<engine::effects::ParticleSystem>();
		if (system == nullptr || system->Blocks.empty()) {
			return 0;
		}

		// **Walked from the emitter column rather than from the block list**, and
		// the direction matters: a block knows how many particles it has and
		// nothing about what they look like, and the shared half — texture, blend
		// mode, flipbook — is on the emitter. Walking blocks would mean a lookup
		// from `EmitterBlock::Owner` back to a row per block, which is a random
		// access per emitter to avoid a sequential one.
		//
		// The order is therefore the emitter column's, which is stable within a
		// tick — so the batch list is the same every frame and the draw order does
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
				batch.Texture = emitter.Texture;
				batch.FlipbookSide = static_cast<float>(engine::effects::FlipbookSide(emitter.Flipbook));
				batch.ZOffset = emitter.ZOffset;
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
		// been seen — a partial sort over the whole set is the only form that is
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
				// origin — see the header.
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
				// half — halving in the shader would be doing it per fragment.
				light.ConeCosine = std::cos(
					std::clamp(bulb.Angle, 0.0f, 180.0f) * 0.5f * std::numbers::pi_v<float> / 180.0f
				);
			}

			lights.push_back(light);
		});

		if (lights.size() > engine::render::MAX_SCENE_LIGHTS) {
			std::partial_sort(
				lights.begin(),
				lights.begin() + engine::render::MAX_SCENE_LIGHTS,
				lights.end(),
				[&eye](const engine::render::SceneLight &left, const engine::render::SceneLight &right) {
					// Squared, because the square root is monotonic and cannot
					// change an ordering — `scene::OrderForDrawing`'s reason.
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
		// Writes `Humanoid::Grounded` from a downward ray.
		//
		// **Here rather than in `scene`, because `scene` may not link
		// `physics`.** `scene::StepCharacters` reads the flag and never computes
		// it, which is the same shape `replication::DistancePriority` has: the
		// arithmetic in the shared module, the query in whatever can run one.
		void GroundCharacters(Store &store) {
			store.Each<engine::scene::Humanoid, const Transform>([&store](
																	 engine::ecs::Entity entity,
																	 engine::scene::Humanoid &humanoid,
																	 const Transform &placement
																 ) {
				if (!humanoid.Enabled) {
					return;
				}

				// From just inside the feet to just below them. **Starting
				// inside rather than at the surface**, because a ray that
				// begins exactly on a face is a coin flip about whether it hits
				// it — and the coin lands differently on two machines, which is
				// a desync arriving through a character controller.
				const Vector3 feet = placement.Frame.Position - Vector3{0.0f, humanoid.Height * 0.5f, 0.0f};

				const engine::core::Ray ray{feet + Vector3{0.0f, 0.1f, 0.0f}, Vector3{0.0f, -1.0f, 0.0f}};

				const auto hit = engine::physics::Raycast(store, ray, 0.1f + humanoid.GroundTolerance);

				// **The character's own collider is rejected here rather than
				// excluded from the query**, because `physics::Raycast` filters
				// by layer and not by entity. One compare against the nearest
				// hit is cheaper than a layer per character, and it is right
				// for the case that matters: a humanoid standing on itself.
				//
				// What it does not handle is a *multi-part* character standing
				// on its own leg. That wants an ignore list on the query, which
				// is `physics`' to add and not this function's to work around.
				humanoid.Grounded = hit.has_value() && hit->Owner != entity;
			});
		}

		// The three effects systems, installed together because they are one
		// dependency chain and installing two of the three is a scene where
		// nothing emits.
		//
		// **`ResolveAttachments` first and in `PreSimulation`**, because an
		// emitter parented to an attachment reads that attachment's world frame
		// when it spawns — and spawning happens in the same phase. Resolving after
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
			// `ResolveAttachments` is a pure recompute of a cache — one multiply
			// per attachment, from state it does not own — so running it twice
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
			//     at all — `World::Present` runs `PreRender` alone — so
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
			// `build-ribbons` and everything the host reads after `Present` —
			// `client::CollectLights`, `CollectParticleBatches` — see a frame
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
		// pressed" — which is exactly right for a server and exactly wrong for a
		// client that forgot to install it. Creating it at install time makes the
		// presence of the resource mean "somebody is looking at this world".
		//
		// **The ground check is the client's rather than `scene`'s**, and that is
		// the tier doing its job: `scene` may not link `physics`, so
		// `StepCharacters` reads `Humanoid::Grounded` and this is what writes it.
		// The same split `replication::DistancePriority::Blocked` already has —
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
			// presentation — it should turn at frame rate, because a mouse moves
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

			scheduler.Add("ground-characters", Phase::PreSimulation, GroundCharacters);

			scheduler.Add("step-characters", Phase::Simulation, [](Store &world) {
				(void)engine::scene::StepCharacters(world, static_cast<float>(world.Time().Delta));
			});
		}

		// How many particles a world's pool holds when nobody has said.
		//
		// **Half a million, because that is the number `ROADMAP.md` v0.10 asks
		// for by name** — a hundred thousand emitters at five particles each — and
		// a default below it makes the engine's stated target something every
		// scene has to opt into.
		//
		// **The cost is paid whether or not a world has an effect in it**, which
		// is what makes this a real decision rather than a generous one: the pool
		// is allocated up front and never grows (`ParticleSystem::Capacity` gives
		// the reason), so this is 524,288 slots times 68 bytes across the instance
		// and state arrays — **about 36 MB a world**, resident from the moment
		// presentation is installed.
		//
		// That is affordable for one world and is not for twenty. A host that
		// opens several at once — the studio with more than one place loaded —
		// should pass its own figure rather than take this one, and
		// `InstallParticles` takes the capacity for exactly that reason.
		//
		// **Measured before it was raised**: at 250,000 the stress scene's grid
		// starved after about 41,000 of its 102,400 emitters, and the symptom was
		// an effect that simply was not there rather than an error —
		// `ParticleStatistics::EmittersRefused` is the number that says so.
		constexpr uint32_t DEFAULT_PARTICLE_POOL = 524288;
	}

	bool BuildScriptedWorld(Store &store, Scheduler &scheduler, const std::string &path, uint32_t reserve) {
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
		if (!engine::examples::LoadScene(store, scheduler, path, error)) {
			ENGINE_ERROR("script '{}' failed:\n{}", path, error);
			return false;
		}

		const float extent = store.Resource<WorldBounds>()->HalfExtent;

		// **A scene that placed its own camera keeps it.** `MoveCamera` is this
		// client's placeholder — it orbits whatever `ActiveCamera` names so that
		// a scene with no camera of its own is still looked at from somewhere —
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

		// `PreRender`, ahead of the pass that reads it — `InstallPresentation`
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

	engine::core::Name
	InstallWorldPipelines(Store &store, engine::render::Renderer &renderer, uint64_t world) {
		// **Both registrations first, and neither is optional.** A `PipelineSet`
		// read out of a store that never registered the type would be minted
		// under the compiler's spelling and found by nobody, and a graph
		// compiled before the catalogue exists refuses every node in it — that
		// second one is not hypothetical, it is what made `RegisterPasses` fail
		// with "nothing can submit a 'ssao' node".
		engine::graph::RegisterPipelineComponents();
		engine::graph::RegisterStandardNodeKinds();

		const auto *set = store.Resource<engine::graph::PipelineSet>();
		if (set == nullptr) {
			return {};
		}

		engine::core::Name selected;
		for (const engine::core::Name &name : set->Names()) {
			const engine::graph::PipelineDocument *document = set->Find(name);
			if (document == nullptr) {
				continue;
			}

			engine::graph::RenderGraph graph;
			engine::core::Name offender;
			const engine::graph::PipelineDocumentStatus builds =
				engine::graph::Build(*document, graph, offender);
			if (builds != engine::graph::PipelineDocumentStatus::Ok) {
				ENGINE_ERROR(
					"pipeline '{}' does not build: {} at '{}'",
					name.Text(),
					engine::graph::Describe(builds),
					offender.Text()
				);
				continue;
			}

			// `#` because a name is interned text and nothing else in the engine
			// spells one with it, so a qualified key cannot collide with an
			// authored name however inventive somebody is.
			const engine::core::Name key(std::format("{}#{}", name.Text(), world));

			// **No message on failure.** `SetPipeline` compiles the graph and
			// reports exactly what it disliked and where; saying "and also it
			// failed" after that is a second line carrying nothing.
			if (!renderer.SetPipeline(key, graph)) {
				continue;
			}

			// **`main` is the one a view draws, and the rest are installed
			// anyway.** A world holding a pipeline for a reflection or a debug
			// viewport wants it available for something to select by name; only
			// the default choice is decided here.
			if (name == engine::core::Name("main")) {
				selected = key;
			}
		}

		return selected;
	}

	void RegisterClientComponents() {
		// **A `DrawList` is derived state, and its serialisation says so by
		// writing nothing.**
		//
		// It had no registration at all before v0.7, which meant
		// `Store::SetResource` minted one under the compiler's spelling of the
		// type — rule 4's exact failure, sitting unnoticed because nothing had
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
			// and a world opened in an editor has authored no such number — so
			// it gets the type's own default instead of a missing resource
			// somebody later reads through a null pointer.
			store.SetResource(WorldBounds{});
		}

		// **The same system `engine::examples` installs, from the same place.**
		// It moved into `scene` at v0.7 precisely so this call site could exist:
		// two copies of a system that writes `PreviousTransform` can both be
		// installed into one world, and the second wins silently every tick.
		scheduler.Add("capture-previous", Phase::PreSimulation, engine::scene::CapturePreviousTransforms);

		// **`PreRender`, ahead of everything that reads a `SurfaceAppearance`.**
		// A `Material` instance names an asset and the part it hangs off is what
		// the draw-list pass reads, so resolving after collection would draw last
		// frame's texture for a frame every time somebody changed one.
		//
		// **It was `PreSimulation`, and that made it do nothing at all in an
		// edited world.** `World::Present` runs `PreRender` alone and the studio
		// never ticks while somebody is authoring, so a `Material` dropped onto
		// a part changed the part's appearance only after Play was pressed —
		// which reads as the material not being loaded rather than as a pass
		// that did not run. The same mistake put every part at the origin
		// through `PreviousTransform`; `studio::PresentationAlpha` is that one.
		//
		// Nothing in the tick reads a `SurfaceAppearance`, so the phase this
		// belongs in is the one its only consumer runs in. It now costs once per
		// *frame* rather than once per tick, which is a walk of the material
		// instances — a handful in a scene, against a draw list of thousands.
		scheduler.Add("resolve-materials", Phase::PreRender, [](Store &world) {
			(void)engine::scene::ResolveMaterials(world);
		});

		scheduler.Add("sync-rendered", Phase::PreRender, SyncVisibility);

		// **And the mirrors, which only `BuildScriptedWorld` was installing.**
		// That is why a mirror worked under `--scene` and was a plain white
		// rectangle everywhere else: the studio, `--game` and an imported world
		// all come through here, and none of them was aiming anything.
		//
		// The visible half of that failure is not the camera at all — it is
		// step 4 of `scene/SurfaceCameras.hpp`. Aiming a camera is also what
		// writes `Visual::Surface` on the pane it is parented to, so without
		// this system the pane keeps the component's default of -1, samples no
		// texture, and draws as its own flat `Tint`. `Mirrors-1-world.luau`
		// tints its pane white, so the symptom was a white part beside a frame
		// that was rendering perfectly — which reads as a broken surface pass
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
