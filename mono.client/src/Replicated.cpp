#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/game/Game.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/gui/Services.hpp>
#include <engine/physics/Integrate.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Input.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/script/Instances.hpp>

#include <algorithm>
#include <client/Replicated.hpp>
#include <client/Scene.hpp>
#include <cstring>
#include <memory>
#include <optional>
#include <string>

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
		// Derives render poses from received ticks; interpolated poses never enter ECS.
		// Surface cameras are aimed from this client's viewer.
		void AimReplicatedSurfaces(Store &store) {
			(void)engine::scene::AimSurfaceCameras(store);
		}

		// How far a dead-reckoned body may be carried past where the authority
		// last put it, as a multiple of its own smallest half-extent.
		//
		// **The second bound, and it is the one that stands in for collision.**
		// Nothing here runs a broad phase, a narrow phase or a solver: a replica
		// holds whichever colliders interest management let it see, so a swept
		// stop would be right about the geometry that arrived and confidently
		// wrong about the geometry that did not - and building the index to ask
		// with is a per-tick pass over the whole replicated world, which is the
		// simulation `mono.client/AGENTS.md` says this process does not run.
		//
		// So the guess is bounded instead of tested. At one half-extent the
		// worst an unrun contact can cost is a body overlapping something it was
		// already touching, which is what a contact looks like anyway and what
		// the correction absorbs. Unbounded it is a crate two metres inside a
		// wall, which is the case `D00015(c)` says is worse than not guessing at
		// all.
		//
		// The smallest half-extent rather than the largest, because passing
		// *through* something is a question about a body's thinnest dimension.
		// Rotation is not bounded: a body spinning in place leaves nowhere.
		constexpr float RECKON_HALF_EXTENTS = 1.0f;

		// Where a body nobody owns would be `seconds` after the pose the
		// authority last described, or that pose unchanged.
		//
		// **Extrapolate what nobody owns.** A `scene::NetworkOwner` says some
		// machine already simulates this body authoritatively, so there is
		// nothing arriving for a guess to be reconciled against and guessing
		// as well would simulate it twice with one of the two wrong. A body
		// with no `scene::Motion` carries no function to evaluate, and holding
		// is D00010's answer for exactly that case.
		CFrame DeadReckon(
			const Store &store, Entity entity, const CFrame &frame, const Bounds &bounds, double seconds
		) {
			const auto *motion = store.Get<engine::scene::Motion>(entity);
			if (motion == nullptr || store.Has<engine::scene::NetworkOwner>(entity)) {
				return frame;
			}

			float travelSeconds = static_cast<float>(seconds);
			const float reachMetres =
				RECKON_HALF_EXTENTS *
				std::min({bounds.HalfExtent.X, bounds.HalfExtent.Y, bounds.HalfExtent.Z});
			const float speed = motion->Linear.Magnitude();
			if (speed * travelSeconds > reachMetres) {
				// Only reached with a positive speed, so there is no zero to
				// divide by.
				travelSeconds = reachMetres / speed;
			}

			return engine::physics::Advanced(frame, motion->Linear, motion->Angular, travelSeconds);
		}

		void CollectReplicated(Store &store) {
			auto *drawList = store.ResourceMutable<DrawList>();
			auto *buffer = store.ResourceMutable<SnapshotBuffer>();
			if (drawList == nullptr || buffer == nullptr) {
				return;
			}

			// Advance with the world's frame delta so stalls remain testable.
			buffer->Advance(store.Time().FrameDelta);

			// Zero unless the buffer has run out of ticks to interpolate
			// between, so the branch below costs a comparison on every ordinary
			// frame and the component lookups happen only while the link is
			// actually failing to deliver.
			const double reckonSeconds = buffer->DeadReckonSeconds();

			// Entity joins are required here; retain draw-list capacity.
			drawList->Instances.clear();
			drawList->Instances.reserve(store.CountMatching<Transform, Bounds, Visual>());

			// The authority owns ancestry filtering; the replica only honors `Visible`.
			// Optional appearance and tag components must not be query requirements.
			store.Each<const Transform, const Bounds, const Visual>(
				[drawList, buffer, &store, reckonSeconds](
					Entity entity, const Transform &transform, const Bounds &bounds, const Visual &visual
				) {
					if (!visual.Visible) {
						return;
					}

					std::optional<CFrame> interpolated = buffer->Sample(entity);

					// **Only a pose the buffer produced is guessed forward.**
					// Falling back to the live row already means this client has
					// no history for the entity - a row that arrived this frame,
					// or the predicted range - and neither is something to
					// extrapolate.
					if (reckonSeconds > 0.0 && interpolated.has_value()) {
						interpolated = DeadReckon(store, entity, *interpolated, bounds, reckonSeconds);
					}

					const SurfaceAppearance *appearance = store.Get<SurfaceAppearance>(entity);
					const Tags *tags = store.Get<Tags>(entity);

					// Every replicated visual field, through the builder both
					// collectors share - see `scene::MakeDrawInstance`. The
					// optional components stay optional here: a replicated row
					// may arrive without an appearance, which is the difference
					// from the local collector that made these two drift.
					drawList->Instances.push_back(
						engine::scene::MakeDrawInstance(
							interpolated.value_or(transform.Frame), bounds, visual, appearance, tags
						)
					);
				}
			);

			engine::core::Metrics::Count(
				"replica.instances", static_cast<double>(drawList->Instances.size())
			);

			// **A client sees itself in the hole too, and this is where.** The
			// ghost is built from the list above, which holds interpolated
			// frames - the ones this machine actually draws - so the far half of
			// a body lines up with the near half rather than trailing it by
			// however far the character walked since the last tick. After the
			// metric for the reason `client::CollectInstances` gives.
			(void)engine::scene::CutAndCloneSeams(store, drawList->Instances);

			engine::core::Metrics::Count("replica.behind.ticks", buffer->Behind());
			engine::core::Metrics::Count("replica.stalls", static_cast<double>(buffer->Stats().Stalls));

			// Against `replica.stalls`, this says how much of a stall was
			// covered by a guess rather than by a freeze.
			engine::core::Metrics::Count("replica.reckon.seconds", reckonSeconds);

			engine::core::Metrics::Count("replica.tickrate", buffer->MeasuredTickRate());
		}
	}

	std::shared_ptr<engine::script::Runtime>
	BuildReplicatedWorld(Store &store, Scheduler &scheduler, const InterpolationSettings &interpolation) {
		// Register snapshot component names before applying one.
		engine::scene::RegisterSceneComponents();

		// **The interface and the scripts too, and before rather than lazily.**
		// A snapshot naming a component this build has not registered is refused
		// whole - `Store::Apply` will not half-merge a world - so a replica that
		// waited for `InstallGuiServices` to register `gui.` on its first tick
		// would refuse the join that arrived before it. The *classes* have the
		// same deadline for a different reason: an instance whose class name
		// does not resolve here arrives untyped, and `ClientScriptsIn` decides
		// what to run by asking whether a row is a `LocalScript`.
		//
		// Both calls register their components first and both are idempotent.
		(void)engine::gui::RegisterGuiClasses();
		(void)engine::script::ScriptClass();

		// Register client resources before their component ids are minted.
		RegisterClientComponents();

		// **And the replication module's own, which nothing was doing.** A
		// `SnapshotBuffer` is a resource, a resource is keyed by a component id,
		// and one minted from the compiler's spelling is a world `Store::Save`
		// refuses - so a replica could not be snapshotted, which is what the
		// studio does every time Play is pressed. `client::DrawList` two lines
		// down is the same fix for the same reason, one version earlier.
		engine::replication::RegisterReplicationComponents();

		store.SetResource(DrawList{});

		// Per-world state belongs in the store.
		store.SetResource(SnapshotBuffer{interpolation});

		// **The two resources that make a replica somewhere a player stands
		// rather than somewhere they watch.** Both are on
		// `replication::LocalToTheClient`'s list, so nothing arriving from the
		// server ever overwrites them - which is precisely what makes it safe
		// to keep this machine's keyboard and this machine's camera in a world
		// whose every other row is somebody else's answer.
		store.SetResource(engine::scene::InputState{});
		store.SetResource(engine::scene::CameraController{});

		// PreRender derives draw data and mirror aim; the replica does not simulate.
		//
		// **The camera is the one thing here that is driven and not derived**,
		// and it is not a simulation: turning the view moves no row the server
		// owns. `FollowOwnCharacter` between the two halves is what points it at
		// the body that arrived over the wire - a client never calls
		// `LoadCharacter`, so there is no spawn moment for it to hook.
		scheduler.Add("replica-camera", Phase::PreRender, [](Store &store) {
			(void)engine::scene::UpdateCameraControl(store);
			(void)engine::scene::FollowOwnCharacter(store);
			(void)engine::scene::PlaceCamera(store);
		});

		// **Posed here and never stepped here.** A character's limbs hang off a
		// root the *server* moved and this machine interpolated, so the product
		// that places them has to run wherever the picture is made. The step and
		// the ground query deliberately do not: this world simulates nothing.
		scheduler.Add("pose-characters", Phase::PreRender, [](Store &store) {
			(void)engine::scene::PoseCharacters(store);
		});

		scheduler.Add("aim-surface-cameras", Phase::PreRender, AimReplicatedSurfaces);
		scheduler.Add("collect-replicated", Phase::PreRender, CollectReplicated);

		// **`GuiService` comes over the wire without the thing it is for**, and
		// that stayed true when the rest of `gui.` started crossing at v0.15.
		// `gui.GuiServiceState` holds `FocusedTextBox` - which box *this* person
		// is typing into - so it is deliberately client-local, and the row a
		// client is shown is a name and a class; `gui::Focus` and `gui::Select`
		// both read the state on it and both answer `false` without one, which
		// is a keyboard that never reaches a `TextBox` and nothing saying why.
		// `InstallGuiServices` mints nothing in a replica and completes whatever
		// arrived, so this is safe once a tick and does nothing on the ticks
		// before the join.
		scheduler.Add("replica-gui-services", Phase::PreSimulation, [](Store &world) {
			(void)engine::gui::InstallGuiServices(world);
		});

		// **A client's VM, over a world it does not own.** The role is what
		// decides which scripts it may run at all - a `Script` is the server's -
		// and `ClientScriptsIn` adds the container half below.
		engine::script::RuntimeLimits limits;
		limits.Role = engine::script::HostRole::OfClient();

		std::string failure;
		std::shared_ptr<engine::script::Runtime> runtime =
			engine::game::StartWorldScripts(store, scheduler, limits, failure);

		// Reported rather than returned. A replica is empty at this point, so
		// there is nothing here to fail - but the parameter is filled in by the
		// same call three other hosts make, and swallowing it would make this the
		// one that hides a start-up error.
		if (!failure.empty()) {
			ENGINE_ERROR("replica '{}': {}", store.Name(), failure);
		}

		// **The one thing about a replica's scripts that is not a host's.** A
		// host starts a world's scripts once because the world is already built;
		// this one fills from the wire, so what has to be asked every tick is
		// what arrived - and `RunNewScripts` is what makes asking repeatedly
		// cost one binary search per script rather than a second run of it.
		//
		// Before the heartbeat, because `StartWorldScripts` installs that in
		// `Phase::Simulation`: a script that arrived this tick connects to
		// `RunService.Heartbeat` in time to be beaten on the same tick, which is
		// the ordering every other loader already gives.
		scheduler.Add("replica-scripts", Phase::PreSimulation, [runtime](Store &world) {
			(void)runtime->RunNewScripts(engine::script::ClientScriptsIn(world));
		});

		return runtime;
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

		// **A replica with a body of its own places its own camera**, and this
		// must not fight it. `BuildReplicatedWorld` installs `replica-camera`,
		// which turns with the mouse and sits behind the character the server
		// gave this client; the frame passed in is where the *local* world is
		// looking, which is the right answer only while there is nothing here to
		// look at. Two writers and the last one wins, so the condition is stated
		// rather than left to phase order.
		if (const auto *controller = store.Resource<engine::scene::CameraController>();
			controller != nullptr && store.Alive(controller->Subject)) {
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
