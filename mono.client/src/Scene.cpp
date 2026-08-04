#include <engine/core/Bytes.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/core/Random.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Property.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Interpolation.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/scene/Visibility.hpp>

#include <algorithm>
#include <client/Scene.hpp>
#include <cmath>
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
				matching = store.CountMatching<Transform, PreviousTransform, Bounds, Visual, Rendered>();
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
				written = store.EachBatchParallel<
					const Transform,
					const PreviousTransform,
					const Bounds,
					const Visual,
					const Rendered>([out, capacity, alpha](
										size_t first,
										size_t rows,
										const Transform *transforms,
										const PreviousTransform *previous,
										const Bounds *bounds,
										const Visual *visuals,
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
							visuals[row].Material,

							// Copied rather than resolved here. Which pass this
							// instance lands in is the renderer's decision,
							// because it depends on where the camera is — and
							// this loop runs once for a world that may be drawn
							// from several views.
							visuals[row].Transparency,
							visuals[row].Surface,
							visuals[row].CastShadow,
						};
					}
				});
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

		void InstallResources(Store &store, Entity camera, float extent, uint32_t reserve) {
			store.SetResource(WorldBounds{extent});

			ActiveCamera live;
			live.Entity = camera;
			store.SetResource(live);
			store.SetResource(DrawList{});

			store.ResourceMutable<DrawList>()->Instances.reserve(reserve);
		}

	}

	bool FindSurfaceCamera(Store &store, engine::render::SurfaceView &surface) {
		bool found = false;
		Entity chosen = engine::ecs::NULL_ENTITY;

		// By entity id, which is creation order, so a world loaded the same way
		// twice picks the same camera. An archetype walk would pick whichever
		// row happened to be first, and that moves when anything changes a
		// component set.
		store.Each<const engine::scene::SurfaceCamera, const engine::scene::Camera, const Transform>(
			[&](Entity entity,
				const engine::scene::SurfaceCamera &target,
				const engine::scene::Camera &lens,
				const Transform &placement) {
				if (found && entity.Id >= chosen.Id) {
					return;
				}

				chosen = entity;
				found = true;

				surface.Frame = placement.Frame;
				surface.Lens = lens;
				surface.Width = target.Width;
				surface.Height = target.Height;

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
				surface.ImageOpacity = 1.0f - target.ImageTransparency;
			}
		);
		return found;
	}

	bool BuildScriptedWorld(Store &store, Scheduler &scheduler, const std::string &path, uint32_t reserve) {
		// Before anything mints an automatic id for `DrawList`. See
		// `RegisterClientComponents`: `Components::Of<T>` caches its answer per
		// type per process, so an explicit registration that arrives second
		// aborts rather than quietly leaving two names for one thing.
		RegisterClientComponents();

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

		scheduler.Add("sync-rendered", Phase::PreRender, SyncVisibility);
		scheduler.Add("aim-surface-cameras", Phase::PreRender, AimSurfaces);
		scheduler.Add("collect-instances", Phase::PreRender, CollectInstances);
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
		scheduler.Add("sync-rendered", Phase::PreRender, SyncVisibility);
		scheduler.Add("collect-instances", Phase::PreRender, CollectInstances);
	}
}
