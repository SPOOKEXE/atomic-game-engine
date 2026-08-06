#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/SurfaceCameras.hpp>

#include <client/Replicated.hpp>
#include <client/Scene.hpp>
#include <cstring>
#include <optional>

namespace client {

	using engine::core::CFrame;
	using engine::core::Name;
	using engine::ecs::Entity;
	using engine::ecs::Phase;
	using engine::ecs::Scheduler;
	using engine::ecs::Store;
	using engine::replication::InterpolationSettings;
	using engine::replication::SnapshotBuffer;
	using engine::scene::AlphaMode;
	using engine::scene::Bounds;
	using engine::scene::DrawInstance;
	using engine::scene::SurfaceAppearance;
	using engine::scene::Tags;
	using engine::scene::Transform;
	using engine::scene::Visual;

	namespace {
		// What the server sent, turned into what a compositor takes.
		//
		// **Interpolated between two received ticks, not between two ticks of a
		// local simulation.** The demo world beside this one interpolates from
		// `PreviousTransform` to `Transform` because it owns both ends of its
		// own tick; a replica owns neither, and the two states worth
		// interpolating between arrived over a link that drops and reorders.
		// `engine::replication::SnapshotBuffer` is what holds them and decides
		// where between them the world is drawn; this only asks.
		//
		// **Nothing here writes a component.** The interpolated pose goes into a
		// `DrawInstance` and nowhere else — a render-rate quantity that reached
		// `Transform` would make the world this process replicates depend on the
		// frame rate of whoever was watching it.
		// Places every surface camera the wire brought, from this client's eye.
		// See `AimReplicaViewer` for why the aim is derived here and not
		// received.
		void AimReplicatedSurfaces(Store &store) {
			(void)engine::scene::AimSurfaceCameras(store);
		}

		void CollectReplicated(Store &store) {
			auto *drawList = store.ResourceMutable<DrawList>();
			auto *buffer = store.ResourceMutable<SnapshotBuffer>();
			if (drawList == nullptr || buffer == nullptr) {
				return;
			}

			// Once per frame, with the frame's own seconds. The buffer reads no
			// clock of its own, which is what makes a stall something a suite
			// states rather than waits for.
			buffer->Advance(store.Time().FrameDelta);

			// `Each` rather than the batched walk the demo uses, and the reason
			// is in `Store::EachBatch`'s own header: a batch is handed columns
			// and deliberately no entity, and the pose this needs is joined *by*
			// entity. Cleared and refilled into kept capacity, so a steady world
			// allocates nothing after the first frame.
			drawList->Instances.clear();
			drawList->Instances.reserve(store.CountMatching<Transform, Bounds, Visual>());

			// **No `Rendered` term here, and a replica is the one world where
			// its absence is right.** The gate is an ancestry test, and ancestry
			// is the one thing the wire does not carry: `Server.cpp` replicates
			// `Transform`, `Motion`, `Bounds` and `Visual`, and `Hierarchy`
			// holds `Entity` handles that would have to be remapped between two
			// processes' directories before they meant anything. A replica
			// therefore has no tree to test against, and testing an empty one
			// would draw nothing at all.
			//
			// It does not need one. **The authority already applied the
			// filter** — it replicates what is in its own scene — so for a
			// replica the wire *is* the visibility test, and re-deriving it here
			// would be re-deriving a conclusion from premises this process was
			// never sent. That is the same argument `RegisterSceneComponents`
			// makes for giving `Rendered` no wire form.
			//
			// `Visible` is a different matter and is honoured below: it rides
			// inside `Visual`, so this process genuinely was told.
			// **`SurfaceAppearance` and `Tags` are read through the store rather
			// than joined into the query**, and this is the one place that is
			// right: a replica adopts whatever archetypes the wire produced, and
			// `Server.cpp` decides which components those are. Requiring them in
			// the signature would silently drop every replicated part on a
			// server build that has not been taught to send them.
			store.Each<const Transform, const Bounds, const Visual>(
				[drawList, buffer, &store](
					Entity entity, const Transform &transform, const Bounds &bounds, const Visual &visual
				) {
					if (!visual.Visible) {
						return;
					}

					// Nothing for an entity the buffer has never seen, and
					// nothing for the predicted one — the first has only its
					// received pose to draw and the second must not be delayed
					// at all.
					const std::optional<CFrame> interpolated = buffer->Sample(entity);

					const SurfaceAppearance *appearance = store.Get<SurfaceAppearance>(entity);
					const Tags *tags = store.Get<Tags>(entity);

					// **Every field of the `Visual`, not the first five.** The
					// tail of this list used to be left at its defaults, so a
					// glass pane replicated as solid and a mirror replicated as
					// a plain part — the same class of silent loss the
					// `WriteVisuals` comment records, arriving through a
					// different door.
					drawList->Instances.push_back(
						DrawInstance{
							interpolated.value_or(transform.Frame),
							bounds.HalfExtent,
							visual.Tint,
							visual.Mesh,
							visual.Material,
							appearance != nullptr ? appearance->ColourMap : Name(),
							tags != nullptr ? tags->Mask : 0u,
							visual.Transparency,
							visual.Surface,
							visual.CastShadow,
							appearance != nullptr ? appearance->Mode : AlphaMode::Opaque,
						}
					);
				}
			);

			// Named apart from the demo's `render.instances` so a run with
			// `--connect` says which of the two worlds produced what. A replica
			// that joined and draws nothing reads exactly like a replica that
			// never joined unless this number exists.
			engine::core::Metrics::Count(
				"replica.instances", static_cast<double>(drawList->Instances.size())
			);

			// The two numbers that say whether the interpolation is working, and
			// they are worth having on the panel rather than in a test: `behind`
			// sits at the configured delay on a healthy link and falls toward
			// zero as a late packet eats the budget, and `stalls` is what it
			// costs when it runs out.
			engine::core::Metrics::Count("replica.behind.ticks", buffer->Behind());
			engine::core::Metrics::Count("replica.stalls", static_cast<double>(buffer->Stats().Stalls));

			// Measured, and worth showing because it is not the rate this client
			// was configured with — the server's default is 30 and this one's is
			// 60, and nothing on the wire says which is right.
			engine::core::Metrics::Count("replica.tickrate", buffer->MeasuredTickRate());
		}
	}

	void
	BuildReplicatedWorld(Store &store, Scheduler &scheduler, const InterpolationSettings &interpolation) {
		// The components a snapshot names have to exist before one arrives, or
		// every name resolves to nothing and the world applies empty. These are
		// the server's types because they are nobody's types — both programs
		// register the same set, which is the whole of what v0.4 bought here.
		engine::scene::RegisterSceneComponents();

		// **And this module's own, before the `SetResource` below reaches for
		// one.** `Components::Of<T>` caches its answer per type per process, so
		// the first mention of `DrawList` anywhere decides its name — and the
		// line below was that first mention. A world built this way then made
		// `client.DrawList` unregisterable, and the abort landed in whichever
		// *other* world was built next, naming a type this function never
		// mentions. `BuildScriptedWorld` and `InstallPresentation` both open
		// with this call for the same reason; this one was the gap.
		RegisterClientComponents();

		store.SetResource(DrawList{});

		// In the store rather than on `Client`, by the same test everything else
		// here answers: a second replicated world in this process would want its
		// own, and it is read and written by a system. `mono.client/AGENTS.md`.
		store.SetResource(SnapshotBuffer{interpolation});

		// `PreRender` only. Everything in this world arrived; the one thing this
		// process is allowed to derive from it is what to draw.
		//
		// **And the mirrors, which are derived and not received.** A reflection
		// depends on where the viewer stands, so the authority's answer is the
		// authority's — a client applying it would see the room reflected for
		// somebody else's eye, which slides across the glass as *this* client
		// moves and reads as a broken mirror rather than as the wrong camera.
		//
		// What arrives is the mirror itself: `scene.SurfaceCamera`, `scene.Camera`
		// and `ecs.Hierarchy`, which together say "this camera projects off that
		// pane's face". `AimSurfaceCameras` turns that into a placement here,
		// against `AimReplicaViewer`'s camera. It writes `Transform` onto rows the
		// authority owns and the next delta may overwrite them — which is
		// harmless and deliberate, because this runs in `PreRender` after the
		// delta was applied and before anything reads the result.
		scheduler.Add("aim-surface-cameras", Phase::PreRender, AimReplicatedSurfaces);
		scheduler.Add("collect-replicated", Phase::PreRender, CollectReplicated);
	}

	Entity AimReplicaViewer(Store &store, const CFrame &frame, const engine::scene::Camera &lens) {
		const auto *active = store.Resource<engine::scene::ActiveCamera>();
		Entity camera = active != nullptr ? active->Entity : engine::ecs::NULL_ENTITY;

		if (camera == engine::ecs::NULL_ENTITY || !store.Alive(camera)) {
			// **Predicted, not authoritative.** The high range is the client's
			// own and the authority never allocates from it, so this camera
			// cannot become the same entity as something the server made.
			camera = store.CreatePredicted("ReplicaViewer");
			if (camera == engine::ecs::NULL_ENTITY) {
				return camera;
			}

			store.Set(camera, engine::scene::Transform{frame});
			store.Set(camera, lens);

			engine::scene::ActiveCamera live;
			live.Entity = camera;
			store.SetResource(live);
			return camera;
		}

		// Guarded on the value differing, for `AimSurfaceCameras`' reason: a
		// `Set` marks the row dirty, and a viewer that has not moved is not a
		// write. A replica observes nothing today and that is not a reason to
		// emit changes it would have to.
		if (const auto *placement = store.Get<engine::scene::Transform>(camera);
			placement == nullptr || std::memcmp(&placement->Frame, &frame, sizeof(CFrame)) != 0) {
			store.Set(camera, engine::scene::Transform{frame});
		}

		if (const auto *current = store.Get<engine::scene::Camera>(camera);
			current == nullptr || current->FieldOfViewRadians != lens.FieldOfViewRadians ||
			current->NearPlane != lens.NearPlane || current->FarPlane != lens.FarPlane) {
			store.Set(camera, lens);
		}

		return camera;
	}

	void RecordReplicatedTick(Store &store, uint64_t tick) {
		auto *buffer = store.ResourceMutable<SnapshotBuffer>();
		if (buffer == nullptr || tick == 0 || buffer->Holds(tick)) {
			return;
		}

		store.Each<const Transform>([buffer, tick](Entity entity, const Transform &transform) {
			buffer->Record(tick, entity, transform.Frame);
		});
	}
}
