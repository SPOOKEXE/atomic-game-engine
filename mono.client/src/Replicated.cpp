#include <engine/core/Bytes.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/game/Game.hpp>
#include <engine/game/Play.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/gui/Services.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/Characters.hpp>
#include <engine/physics/Integrate.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Attachments.hpp>
#include <engine/scene/CameraContinuation.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Input.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/script/Instances.hpp>

#include <algorithm>
#include <client/Replicated.hpp>
#include <client/Scene.hpp>
#include <cstring>
#include <limits>
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
	using engine::ecs::SystemOrder;
	using engine::render::DrawList;
	using engine::replication::InterpolationSettings;
	using engine::replication::SnapshotBuffer;
	using engine::scene::AlphaMode;
	using engine::scene::Bounds;
	using engine::scene::DrawInstance;
	using engine::scene::LocalTransparency;
	using engine::scene::SurfaceAppearance;
	using engine::scene::Tags;
	using engine::scene::Transform;
	using engine::scene::Visual;

	namespace {
		// Local prediction is transient client state. A saved replica must
		// restart from an authority tick instead of resuming a stale input run.
		void WriteLocalPlayerPredictions(engine::core::ByteWriter &, const void *, size_t) {}

		void ReadLocalPlayerPredictions(engine::core::ByteReader &, void *destination, size_t count) {
			auto *predictions = static_cast<LocalPlayerPrediction *>(destination);
			for (size_t index = 0; index < count; index++) {
				predictions[index] = {};
			}
		}

		void WritePortalInputHistories(engine::core::ByteWriter &, const void *, size_t) {}
		void WriteNativePredictions(engine::core::ByteWriter &, const void *, size_t) {}
		void ReadNativePredictions(engine::core::ByteReader &, void *destination, size_t count) {
			auto *predictions = static_cast<NativePlayerPrediction *>(destination);
			for (size_t index = 0; index < count; ++index)
				predictions[index] = {};
		}
		void ReadPortalInputHistories(engine::core::ByteReader &, void *destination, size_t count) {
			auto *histories = static_cast<PortalInputHistory *>(destination);
			for (size_t index = 0; index < count; ++index)
				histories[index] = {};
		}

		void AdvanceLocalPlayerPrediction(
			LocalPlayerPrediction &prediction, const engine::game::MoveInput &move, float delta
		) {
			prediction.Humanoid.MoveDirection = move.Direction;
			prediction.Linear.X = move.Direction.X * prediction.Humanoid.WalkSpeed;
			prediction.Linear.Z = move.Direction.Z * prediction.Humanoid.WalkSpeed;

			if (move.Jump && prediction.Humanoid.Grounded) {
				prediction.Linear.Y = prediction.Humanoid.JumpSpeed;
				prediction.Humanoid.Grounded = false;
			}

			prediction.Frame =
				engine::physics::Advanced(prediction.Frame, prediction.Linear, prediction.Angular, delta);
		}

		bool ReplayLocalPlayerInput(
			LocalPlayerPrediction &prediction, const engine::game::MoveInput &move, float fallbackStep
		) {
			if (move.StepSeconds > std::numeric_limits<float>::max()) return false;
			// Timed input retains its source clock when replay moves to another world.
			const float step = move.StepSeconds > 0 ? static_cast<float>(move.StepSeconds) : fallbackStep;
			AdvanceLocalPlayerPrediction(prediction, move, step);
			return true;
		}

		std::optional<PredictionReplayClock> ReplayClock(
			PredictionReplayClock previous,
			const engine::script::PortalTransferMotion &motion,
			double acknowledgedSeconds,
			uint64_t acknowledgedThrough
		) {
			if (motion.SimulationSeconds == 0) {
				if (previous.SimulationSeconds != 0) return {};
				return PredictionReplayClock{};
			}
			if (previous.SimulationSeconds == 0)
				return PredictionReplayClock{motion.SimulationSeconds, 0, motion.InputTick};
			if (motion.SimulationSeconds <= previous.SimulationSeconds ||
				motion.InputTick < previous.InputTick || acknowledgedThrough != motion.InputTick)
				return {};
			const double lead = previous.InputLeadSeconds + acknowledgedSeconds -
								(motion.SimulationSeconds - previous.SimulationSeconds);
			if (!std::isfinite(lead) || std::abs(lead) > std::numeric_limits<float>::max()) return {};
			return PredictionReplayClock{motion.SimulationSeconds, lead, motion.InputTick};
		}

		void BeginTimedReplay(LocalPlayerPrediction &prediction, double lead) {
			// The pose can cover less time than its consumed input numbers suggest.
			// Preserve that elapsed interval with the completed motion, without
			// triggering an acknowledged jump or changing its held direction.
			if (lead > 0)
				prediction.Frame = engine::physics::Advanced(
					prediction.Frame, prediction.Linear, prediction.Angular, static_cast<float>(lead)
				);
		}

		void ReplayTimedInput(
			LocalPlayerPrediction &prediction, engine::game::MoveInput move, float delta, double &skip
		) {
			const double skipped = std::min(skip, static_cast<double>(delta));
			skip -= skipped;
			// Unacknowledged control edges still apply when their elapsed interval
			// overlaps the completed pose. Only the integration time is skipped.
			AdvanceLocalPlayerPrediction(prediction, move, static_cast<float>(delta - skipped));
		}

		constexpr float MAX_POSITION_CORRECTION_SECONDS = 1.f;

		void RetainPositionCorrection(const LocalPlayerPrediction &previous, LocalPlayerPrediction &next) {
			if (!previous.Active || previous.Player != next.Player || previous.Root != next.Root) return;
			const auto difference = previous.Frame.Position - next.Frame.Position;
			next.PositionCorrection = difference + previous.PositionCorrection;
			const float distance = next.PositionCorrection.Magnitude();
			if (!std::isfinite(distance) || distance == 0) {
				next.PositionCorrection = {};
				next.CorrectionSeconds = 0;
				return;
			}
			if (difference.Magnitude() == 0) {
				next.CorrectionSeconds = previous.CorrectionSeconds;
				return;
			}
			// Fixed-duration large corrections can cancel walking for the whole blend.
			// Limit their speed to half walking speed; snap corrections beyond one second.
			const float speed = std::max(.01f, next.Humanoid.WalkSpeed * .5f);
			next.CorrectionSeconds = std::max(.1f, distance / speed);
			if (next.CorrectionSeconds > MAX_POSITION_CORRECTION_SECONDS) {
				next.PositionCorrection = {};
				next.CorrectionSeconds = 0;
			}
		}

		void AdvancePositionCorrection(Store &store) {
			auto *prediction = store.ResourceMutable<LocalPlayerPrediction>();
			if (!prediction || prediction->CorrectionSeconds <= 0) return;
			const float delta = store.Time().FrameDelta;
			if (!std::isfinite(delta) || delta <= 0) return;
			const float remaining = std::max(0.f, prediction->CorrectionSeconds - delta);
			prediction->PositionCorrection =
				prediction->PositionCorrection * (remaining / prediction->CorrectionSeconds);
			prediction->CorrectionSeconds = remaining;
		}

		CFrame PredictionPresentationFrame(const Store &store, const LocalPlayerPrediction &prediction) {
			const auto time = store.Time();
			const double seconds =
				prediction.PresentationOffsetSeconds + static_cast<double>(time.Alpha) * time.Delta;
			auto frame = prediction.Frame;
			if (seconds != 0 && std::isfinite(seconds) &&
				std::abs(seconds) <= std::numeric_limits<float>::max())
				frame = engine::physics::Advanced(
					frame, prediction.Linear, prediction.Angular, static_cast<float>(seconds)
				);
			frame.Position = frame.Position + prediction.PositionCorrection;
			return frame;
		}

		std::optional<CFrame> PredictedFrame(const Store &store, Entity entity, const CFrame &presented) {
			const auto *prediction = store.Resource<LocalPlayerPrediction>();
			if (prediction == nullptr || !prediction->Active) {
				return std::nullopt;
			}
			if (entity == prediction->Root) {
				return presented;
			}
			const auto *limb = store.Get<engine::scene::CharacterLimb>(entity);
			if (limb != nullptr && limb->Root == prediction->Root) {
				return presented * limb->Offset;
			}
			return std::nullopt;
		}

		void OffsetReplicaCameraForPrediction(Store &store) {
			const auto *prediction = store.Resource<LocalPlayerPrediction>();
			const auto *active = store.Resource<engine::scene::ActiveCamera>();
			const auto *control = store.Resource<engine::scene::CameraController>();
			if (!prediction || !prediction->Active || !active || !control ||
				control->Mode == engine::scene::CameraMode::Scriptable)
				return;
			const Entity subject = engine::scene::CameraSubjectRoot(store, active->Entity);
			const auto presented =
				PredictedFrame(store, subject, PredictionPresentationFrame(store, *prediction));
			if (!presented) return;
			const auto *authoritative = store.Get<Transform>(subject);
			auto *camera = store.GetMutable<Transform>(active->Entity);
			if (!authoritative || !camera) return;
			const engine::core::Vector3 offset = presented->Position - authoritative->Frame.Position;
			camera->Frame = CFrame{camera->Frame.Position + offset, camera->Frame.Rotation()};
		}

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
			// **Named, because it was the one collector with no span.**
			// `collect-instances` has had one since v0.5 and this is its twin
			// for a replicated world, so a `--connect` run's `pre-render` bar
			// had a hole in it exactly the size of the loop below - which is
			// also the number `docs/ARCH_REVIEW.md` F3 asks for and nobody
			// could read.
			ENGINE_PROFILE_CAT("collect-replicated", engine::core::ProfileCategory::Simulation);

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
			//
			// **Serial, and the reason is not the crossover.** Rule 5 asks for
			// the number, so: measured in `release` against a server holding
			// 20,000 replicated rows, this loop is **1.542 ms at p50, 77 ns a
			// row** - read off the `collect-replicated` span above. A pool
			// handover is 7.74 us for eight ranges (`parallel/Jobs.hpp`,
			// re-measured at `-O3`), so at this row cost the handover is a
			// quarter of the serial work at about **400 rows**. That is a very
			// low crossover and the loop clears it by fifty times: at 20,000
			// rows a perfect split across 23 workers would be about 75 us
			// against 1.542 ms. `docs/ARCH_REVIEW.md` F3 is right that the
			// blocker is not the loop shape, and it is right that the
			// crossover is not the argument either - so the argument has to be
			// written down rather than implied by leaving it serial.
			//
			// **Two things stop it, both outside this loop and neither cheap.**
			//
			//  * `SnapshotBuffer::Sample` is not thread-safe and its header says
			//    why it never will be quietly: it counts `Statistics::Held`
			//    against `Statistics::Interpolated` on every call, which is what
			//    says whether the buffer is smoothing anything at all, and that
			//    header refuses to hide those behind a `mutable`. Twenty-three
			//    workers incrementing two plain counters is a race, and making
			//    them atomic puts a contended write on the per-row path this
			//    loop is trying to make cheaper.
			//  * The output is a *filtered* `push_back`. `visual.Visible` is a
			//    field test rather than a query term, so a row's position in the
			//    walk does not decide its position in the draw list - which is
			//    exactly what `engine::render::CollectInstances` relies on to let
			//    workers write `out[base + first + row]` with no atomic and no
			//    reshuffling. Making this list dense needs `scene::Rendered` in
			//    the query, and marking it needs a visibility system running in
			//    a world that `mono.client/AGENTS.md` says advances nothing.
			//
			// The four optional joins below are *not* a third reason. They rule
			// out `EachBatchParallel`, which is handed columns and no entity, but
			// `Store::EachParallel` hands out entities and could express them.
			//
			// So this is a measured decision to leave it, not an unmeasured one:
			// the win is real and it is bought with a change to `replication`'s
			// public contract and a new system in the replica. Neither belongs
			// in a loop rewrite.
			const auto *prediction = store.Resource<LocalPlayerPrediction>();
			const CFrame presented =
				prediction && prediction->Active ? PredictionPresentationFrame(store, *prediction) : CFrame{};
			store.Each<const Transform, const Bounds, const Visual>(
				[drawList, buffer, &store, reckonSeconds, &presented](
					Entity entity, const Transform &transform, const Bounds &bounds, const Visual &visual
				) {
					if (!visual.Visible) {
						return;
					}

					std::optional<CFrame> interpolated = buffer->Sample(entity);
					const std::optional<CFrame> predicted = PredictedFrame(store, entity, presented);
					if (predicted.has_value()) {
						interpolated = predicted;
					}

					// **Only a pose the buffer produced is guessed forward.**
					// Falling back to the live row already means this client has
					// no history for the entity - a row that arrived this frame,
					// or the predicted range - and neither is something to
					// extrapolate.
					if (reckonSeconds > 0.0 && !predicted.has_value() && interpolated.has_value()) {
						interpolated = DeadReckon(store, entity, *interpolated, bounds, reckonSeconds);
					}

					const SurfaceAppearance *appearance = store.Get<SurfaceAppearance>(entity);
					const Tags *tags = store.Get<Tags>(entity);

					// **Local, and never over the wire.** A replica's own copy
					// of `scene::LocalTransparency` is this machine's alone -
					// nothing arrived to fill the row, and nothing sends what a
					// script or a camera pass writes into it later. `Get`
					// rather than a required column, for the reason `appearance`
					// and `tags` already are here: an optional join is exactly
					// what a plain `Each` can express and a batched parallel
					// walk cannot.
					const LocalTransparency *local = store.Get<LocalTransparency>(entity);

					// Every replicated visual field, through the builder both
					// collectors share. Fully transparent rows stop here, before
					// residency, sorting, shadows or portal cloning can charge for
					// geometry that contributes no pixel.
					const engine::scene::DrawInstance instance = engine::scene::MakeDrawInstance(
						interpolated.value_or(transform.Frame),
						bounds,
						visual,
						appearance,
						tags,
						entity.Id,
						local,
						// Which rig this row belongs to, so a portal cuts a
						// replicated character in one piece. Optional like
						// the two above it, and for the same reason: most
						// rows are not a limb of anything.
						store.Get<engine::scene::CharacterLimb>(entity)
					);
					if (!(instance.Transparency >= 1.0f)) {
						drawList->Instances.push_back(instance);
					}
				}
			);

			engine::render::CollectSkinPalettes(store, *drawList);
			if (!engine::scene::ContinueCameraBodyPose(
					store, presented, drawList->Instances, drawList->JointFrames
				))
				engine::core::Metrics::Count("replica.body-pose.refused", 1);
			const auto *heldBody = store.Resource<engine::scene::CameraCharacterHold>();
			engine::scene::UpdatePortalBodyView(
				store,
				heldBody && heldBody->Active	   ? heldBody->SourceRoot
				: prediction && prediction->Active ? prediction->Root
												   : engine::ecs::NULL_ENTITY,
				presented.Position,
				drawList->Instances
			);
			engine::core::Metrics::Count(
				"replica.instances", static_cast<double>(drawList->Instances.size())
			);

			// **A client sees itself in the hole too, and this is where.** The
			// ghost is built from the list above, which holds interpolated
			// frames - the ones this machine actually draws - so the far half of
			// a body lines up with the near half rather than trailing it by
			// however far the character walked since the last tick. After the
			// metric for the reason `engine::render::CollectInstances` gives.
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
		//
		// **And `scene`'s classes, which were the ones actually missing.** The
		// paragraph above is exactly right about why an unresolved class name
		// matters and then registers `gui` and `script` and stops - so every
		// `Part`, `SpawnLocation` and `Model` a snapshot named arrived untyped,
		// with `ecs: 'Workspace' is not a class registered here` in the log to
		// say so.
		//
		// **It cost 81% of a replica's traffic**, and the path is worth stating
		// because nothing about it looks like bandwidth. `ecs.InstanceClass`
		// crosses as a class *name*; a replica that cannot resolve the name
		// stores an empty one, so the row reads 13 bytes on the authority and 4
		// here. The anti-entropy audit compares the two, disputes the group,
		// and a dispute re-arms the recovery walk - which then re-sends every
		// row of every entity every few ticks, for ever, over a difference no
		// amount of re-sending can fix. Measured on `loadtest` with four
		// clients and nothing moving: 91,507 B/s before, 16,964 after.
		//
		// `mono.studio` calls this and the registry is process-wide, which is
		// exactly why the editor never showed it.
		engine::scene::RegisterSceneClasses();

		(void)engine::gui::RegisterGuiClasses();
		(void)engine::script::ScriptClass();

		// Register client resources before their component ids are minted.
		RegisterClientComponents();
		engine::ecs::Components::Register<LocalPlayerPrediction>(
			"client.LocalPlayerPrediction", WriteLocalPlayerPredictions, ReadLocalPlayerPredictions
		);

		engine::ecs::Components::Register<PortalInputHistory>(
			"client.PortalInputHistory", WritePortalInputHistories, ReadPortalInputHistories
		);
		engine::ecs::Components::Register<NativePlayerPrediction>(
			"client.NativePlayerPrediction", WriteNativePredictions, ReadNativePredictions
		);

		// **And the replication module's own, which nothing was doing.** A
		// `SnapshotBuffer` is a resource, a resource is keyed by a component id,
		// and one minted from the compiler's spelling is a world `Store::Save`
		// refuses - so a replica could not be snapshotted, which is what the
		// studio does every time Play is pressed. `engine::render::DrawList` two lines
		// down is the same fix for the same reason, one version earlier.
		engine::replication::RegisterReplicationComponents();

		store.SetResource(DrawList{});

		// Per-world state belongs in the store.
		store.SetResource(SnapshotBuffer{interpolation});

		// A replica advances no physics, but its local camera still has to query
		// the walls the authority sent. The broadphase is derived presentation
		// state here: it indexes received colliders and never moves a body.
		engine::physics::PreparePhysicsWorld(store);

		// **The two resources that make a replica somewhere a player stands
		// rather than somewhere they watch.** Both are on
		// `replication::LocalToTheClient`'s list, so nothing arriving from the
		// server ever overwrites them - which is precisely what makes it safe
		// to keep this machine's keyboard and this machine's camera in a world
		// whose every other row is somebody else's answer.
		store.SetResource(engine::scene::InputState{});
		store.SetResource(engine::scene::ControllerState{});
		store.SetResource(engine::scene::CameraController{});

		// **First in the phase, because everything below is derived from it.**
		// A replica never ticks a simulation, so the `PreSimulation` copy every
		// other host installs has nothing to hang off - this is the replica's
		// only resolve, and without it `Attachment::WorldFrame` stayed at the
		// identity for the whole session. What that looked like was a
		// `PointLight` parented to an attachment lighting the world origin
		// rather than the lamp it hangs from: `engine::render::CollectLights` reads the
		// cache and there was nobody to fill it. The script surface was never
		// affected - `Attachment.WorldCFrame` is a computed property that
		// resolves on the spot - but its *change signal* was, for the reason
		// `server::PrepareSimulation` gives.
		//
		// **Resolved again here rather than trusted from the wire.** The
		// authority's answer arrives a tick old and against uninterpolated
		// transforms; this world draws from interpolated ones, so a lamp placed
		// from the wire would sit where its part was at the last snapshot.
		scheduler.Add("resolve-attachments", Phase::PreRender, [](Store &store) {
			(void)engine::scene::ResolveAttachments(store);
		});

		// PreRender derives draw data and mirror aim; the replica does not simulate.
		//
		// **The camera is the one thing here that is driven and not derived**,
		// and it is not a simulation: turning the view moves no row the server
		// owns. `FollowOwnCharacter` between the two halves is what points it at
		// the body that arrived over the wire - a client never calls
		// `LoadCharacter`, so there is no spawn moment for it to hook.
		scheduler.Add("replica-camera", Phase::PreRender, [](Store &store) {
			AdvancePositionCorrection(store);
			(void)engine::scene::UpdateCameraControl(store);
			(void)engine::scene::FollowOwnCharacter(store);
			(void)engine::physics::UpdatePoppercam(store);
			(void)engine::scene::PlaceCamera(store);
			OffsetReplicaCameraForPrediction(store);
		});

		// **Posed here and never stepped here.** A character's limbs hang off a
		// root the *server* moved and this machine interpolated, so the product
		// that places them has to run wherever the picture is made. The step and
		// the ground query deliberately do not: this world simulates nothing.
		scheduler.Add("pose-characters", Phase::PreRender, [](Store &store) {
			(void)engine::scene::PoseCharacters(store);
		});

		scheduler.Add(
			"aim-surface-cameras",
			Phase::PreRender,
			AimReplicatedSurfaces,
			SystemOrder{{}, {"replica-camera"}}
		);
		scheduler.Add(
			"collect-replicated",
			Phase::PreRender,
			CollectReplicated,
			SystemOrder{{}, {"resolve-attachments", "pose-characters", "aim-surface-cameras"}}
		);

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
			camera = store.CreatePredictedInstance(engine::scene::CameraClass(), "ReplicaViewer");
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

		// The local world's pose is only a fallback while automatic follow waits
		// for a subject. Explicit selections and scripted cameras own their pose.
		const auto *selection = store.Get<engine::scene::CameraSubject>(camera);
		const auto *control = store.Resource<engine::scene::CameraController>();
		if ((selection && !selection->Automatic) ||
			(control && control->Mode == engine::scene::CameraMode::Scriptable) ||
			engine::scene::CameraSubjectRoot(store, camera) != engine::ecs::NULL_ENTITY)
			return camera;

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
		buffer->RecordTick(tick);

		store.Each<const Transform>([buffer, tick](Entity entity, const Transform &transform) {
			buffer->Record(tick, entity, transform.Frame);
		});

		// The rows now hold one complete received tick. Index them once here,
		// rather than rebuilding moving colliders at the presentation frame rate,
		// so the local poppercam queries the same geometry the replica draws.
		// Portal openings derive from the received links before camera queries use them.
		(void)engine::scene::OpenPortals(store);
		engine::physics::SyncBroadphase(store);
	}

	void ReconcileLocalPlayerPrediction(
		Store &store, uint64_t tick, std::span<const engine::replication::Input> unconfirmed
	) {
		if (tick == 0) {
			return;
		}

		if (!store.HasResource<LocalPlayerPrediction>()) {
			store.SetResource(LocalPlayerPrediction{});
		}
		LocalPlayerPrediction &prediction = *store.ResourceMutable<LocalPlayerPrediction>();
		const auto *local = store.Resource<engine::scene::LocalPlayer>();
		if (local == nullptr) {
			prediction = {};
			return;
		}

		const Entity character = engine::scene::CharacterOf(store, local->Instance);
		const auto *rig = store.Get<engine::scene::Character>(character);
		if (rig == nullptr) {
			prediction = {};
			return;
		}
		if (const auto *held = store.Resource<engine::scene::CameraCharacterHold>();
			held && held->Active && local->Instance == held->Player && rig->Root == held->Root)
			return;
		const auto *frame = store.Get<Transform>(rig->Root);
		const auto *humanoid = store.Get<engine::scene::Humanoid>(rig->Humanoid);
		if (frame == nullptr || humanoid == nullptr) {
			prediction = {};
			return;
		}

		if (prediction.Active && prediction.AuthorityTick >= tick && prediction.Player == local->Instance &&
			prediction.Root == rig->Root) {
			return;
		}

		const auto previousPrediction = prediction;
		const double presentationOffset =
			prediction.Active && prediction.Player == local->Instance && prediction.Root == rig->Root
				? prediction.PresentationOffsetSeconds
				: 0;
		prediction = {};
		prediction.PresentationOffsetSeconds = presentationOffset;
		prediction.Player = local->Instance;
		prediction.Root = rig->Root;
		prediction.Frame = frame->Frame;
		prediction.Humanoid = *humanoid;
		prediction.AuthorityTick = tick;
		prediction.Active = true;
		if (const auto *motion = store.Get<engine::scene::Motion>(rig->Root); motion != nullptr) {
			prediction.Linear = motion->Linear;
			prediction.Angular = motion->Angular;
		}

		if (auto *buffer = store.ResourceMutable<SnapshotBuffer>(); buffer != nullptr) {
			buffer->Predict(prediction.Root);
		}

		const float delta = store.Time().Delta;
		for (const engine::replication::Input &input : unconfirmed) {
			engine::game::MoveInput move;
			if (engine::game::DecodeMoveInput(input.Bytes, move)) {
				(void)ReplayLocalPlayerInput(prediction, move, delta);
			}
		}
		RetainPositionCorrection(previousPrediction, prediction);
	}

	std::optional<CFrame> PresentedPlayerPrediction(const Store &store) {
		const auto *prediction = store.Resource<LocalPlayerPrediction>();
		if (!prediction || !prediction->Active) return {};
		return PredictionPresentationFrame(store, *prediction);
	}

	void PredictLocalPlayerMove(Store &store, const engine::game::MoveInput &move, float delta) {
		auto *prediction = store.ResourceMutable<LocalPlayerPrediction>();
		if (prediction == nullptr || !prediction->Active) {
			return;
		}
		AdvanceLocalPlayerPrediction(*prediction, move, delta);
	}
	bool BeginPortalInputHistory(
		Store &store,
		const engine::game::PortalResume &claim,
		const engine::scene::SeamTransform &through,
		uint64_t submittedTick
	) {
		const auto finite = [](const engine::core::Vector3 &v) {
			return std::isfinite(v.X) && std::isfinite(v.Y) && std::isfinite(v.Z);
		};
		const auto q = through.Frame.Rotation();
		if (claim.Transfer.SourceIncarnation == 0 || claim.Transfer.Sequence == 0 ||
			claim.DestinationIncarnation == 0 || claim.Destination.empty() || !std::isfinite(through.Scale) ||
			through.Scale <= 0 || !finite(through.Origin) || !finite(through.Frame.Position) ||
			!std::isfinite(glm::dot(q, q)) || std::abs(glm::dot(q, q) - 1) > .001f)
			return false;
		if (const auto *history = store.Resource<PortalInputHistory>(); history && history->Claim == claim)
			return true;
		ENGINE_PROFILE("portal input history begin");
		PortalInputHistory history;
		history.Claim = claim;
		history.Through = through;
		history.CoveredThrough = submittedTick;
		history.LastRecordedTick = submittedTick;
		store.SetResource(history);
		return true;
	}

	std::optional<PortalPredictionContinuation>
	CapturePortalPrediction(const Store &store, const engine::game::PortalResume &claim, float alpha) {
		if (!std::isfinite(alpha) || alpha < 0 || alpha > 1) return {};
		const auto *local = store.Resource<engine::scene::LocalPlayer>();
		const auto *history = store.Resource<PortalInputHistory>();
		const auto *prediction = store.Resource<LocalPlayerPrediction>();
		const auto *held = store.Resource<engine::scene::CameraCharacterHold>();
		if (!local || !history || history->Claim != claim || history->AppliedDestinationTick == 0 ||
			!prediction || !prediction->Active || !held || !held->Active ||
			prediction->Player != held->Player || prediction->Root != held->Root ||
			local->Instance != held->Player)
			return {};
		PortalPredictionContinuation continuation;
		continuation.CoveredThrough = history->CoveredThrough;
		continuation.Clock = history->Clock;
		continuation.PositionCorrection = history->Through.Carry(prediction->PositionCorrection);
		continuation.CorrectionSeconds = prediction->CorrectionSeconds;
		continuation.PresentationSeconds =
			prediction->PresentationOffsetSeconds + static_cast<double>(alpha) * store.Time().Delta;
		continuation.Inputs.reserve(history->Count);
		for (size_t offset = 0; offset < history->Count; ++offset) {
			const auto &input = history->Inputs[(history->Begin + offset) % PortalInputHistory::CAPACITY];
			auto move = input.Move;
			move.Direction = history->Through.Rotate(move.Direction);
			move.StepSeconds = input.Delta;
			continuation.Inputs.push_back({input.Tick, engine::game::EncodeMoveInput(move)});
		}
		auto &motion = continuation.Motion;
		motion.DestinationIncarnation = claim.DestinationIncarnation;
		motion.DestinationTick = history->AppliedDestinationTick;
		motion.InputTick = history->LastRecordedTick;
		motion.Frame = history->Through.Place(prediction->Frame).Orthonormalize();
		motion.Linear = history->Through.Carry(prediction->Linear);
		motion.Angular = history->Through.Rotate(prediction->Angular);
		motion.WalkSpeed = history->Through.Length(prediction->Humanoid.WalkSpeed);
		motion.JumpSpeed = history->Through.Length(prediction->Humanoid.JumpSpeed);
		motion.Grounded = prediction->Humanoid.Grounded;
		continuation.MoveDirection = history->Through.Rotate(prediction->Humanoid.MoveDirection);
		if (!engine::script::ValidPortalTransferMotion(motion) ||
			!std::isfinite(continuation.MoveDirection.Magnitude()))
			return {};
		return continuation;
	}

	bool AdoptPortalPrediction(
		Store &store, Entity player, const PortalPredictionContinuation &continuation, float alpha
	) {
		if (!std::isfinite(alpha) || alpha < 0 || alpha > 1 ||
			!std::isfinite(continuation.PositionCorrection.Magnitude()) ||
			!std::isfinite(continuation.CorrectionSeconds) || continuation.CorrectionSeconds < 0 ||
			continuation.CorrectionSeconds > MAX_POSITION_CORRECTION_SECONDS ||
			!std::isfinite(continuation.PresentationSeconds) ||
			!std::isfinite(continuation.Clock.SimulationSeconds) ||
			continuation.Clock.SimulationSeconds < 0 || !std::isfinite(continuation.Clock.InputLeadSeconds) ||
			continuation.Clock.InputTick > continuation.CoveredThrough)
			return false;
		const auto *local = store.Resource<engine::scene::LocalPlayer>();
		const auto *rig = store.Get<engine::scene::Character>(engine::scene::CharacterOf(store, player));
		if (!store.AdoptOnly() || !local || local->Instance != player || !rig ||
			!store.Has<Transform>(rig->Root) ||
			!engine::script::ValidPortalTransferMotion(continuation.Motion) ||
			!std::isfinite(continuation.MoveDirection.Magnitude()))
			return false;
		const auto *humanoid = store.Get<engine::scene::Humanoid>(rig->Humanoid);
		if (!humanoid) return false;
		ENGINE_PROFILE("portal prediction adopt");
		LocalPlayerPrediction prediction;
		prediction.Player = player;
		prediction.Root = rig->Root;
		prediction.Frame = continuation.Motion.Frame;
		prediction.PositionCorrection = continuation.PositionCorrection;
		prediction.CorrectionSeconds = continuation.CorrectionSeconds;
		prediction.PresentationOffsetSeconds =
			continuation.PresentationSeconds - static_cast<double>(alpha) * store.Time().Delta;
		prediction.Linear = continuation.Motion.Linear;
		prediction.Angular = continuation.Motion.Angular;
		prediction.AuthorityTick = continuation.Motion.DestinationTick;
		prediction.Humanoid = *humanoid;
		prediction.Humanoid.MoveDirection = continuation.MoveDirection;
		prediction.Humanoid.WalkSpeed = continuation.Motion.WalkSpeed;
		prediction.Humanoid.JumpSpeed = continuation.Motion.JumpSpeed;
		prediction.Humanoid.Grounded = continuation.Motion.Grounded;
		prediction.Active = true;
		store.SetResource(prediction);
		if (auto *buffer = store.ResourceMutable<SnapshotBuffer>()) buffer->Predict(prediction.Root);
		store.SetResource(
			NativePlayerPrediction{
				.Incarnation = continuation.Motion.DestinationIncarnation, .Clock = continuation.Clock
			}
		);
		return true;
	}

	bool
	AcceptNativePlayerMotion(Store &store, const engine::game::PlayerMotion &sample, uint64_t submittedTick) {
		auto *native = store.ResourceMutable<NativePlayerPrediction>();
		const auto *local = store.Resource<engine::scene::LocalPlayer>();
		if (!native || !local || local->Instance != sample.Player ||
			sample.Root == engine::ecs::NULL_ENTITY || sample.Root == sample.Player ||
			sample.Motion.DestinationIncarnation != native->Incarnation || sample.Motion.InputTick == 0 ||
			sample.Motion.InputTick > submittedTick ||
			!engine::script::ValidPortalTransferMotion(sample.Motion))
			return false;
		if (native->Sample && (sample.Motion.DestinationTick <= native->Sample->Motion.DestinationTick ||
							   sample.Motion.InputTick < native->Sample->Motion.InputTick))
			return false;
		native->Sample = sample;
		return true;
	}

	std::optional<uint64_t> ReconcileNativePlayerPrediction(
		Store &store, std::span<const engine::replication::Input> inputs, uint64_t coveredThrough
	) {
		auto *native = store.ResourceMutable<NativePlayerPrediction>();
		if (!native || !native->Sample) return {};
		const auto &sample = *native->Sample;
		const auto *local = store.Resource<engine::scene::LocalPlayer>();
		const auto *rig =
			store.Get<engine::scene::Character>(engine::scene::CharacterOf(store, sample.Player));
		if (!local || local->Instance != sample.Player || !rig || rig->Root != sample.Root ||
			!store.Has<Transform>(sample.Root) || sample.Motion.InputTick < coveredThrough ||
			sample.Motion.DestinationTick <= native->AppliedPoseTick)
			return {};
		const auto *humanoid = store.Get<engine::scene::Humanoid>(rig->Humanoid);
		if (!humanoid) return {};
		if (const auto *current = store.Resource<LocalPlayerPrediction>();
			current && current->Active && current->Player == sample.Player && current->Root == sample.Root &&
			current->AuthorityTick > sample.Motion.DestinationTick)
			return {};
		ENGINE_PROFILE("native player prediction replay");
		LocalPlayerPrediction prediction;
		if (const auto *current = store.Resource<LocalPlayerPrediction>();
			current && current->Active && current->Player == sample.Player && current->Root == sample.Root)
			prediction.PresentationOffsetSeconds = current->PresentationOffsetSeconds;
		prediction.Player = sample.Player;
		prediction.Root = sample.Root;
		prediction.Frame = sample.Motion.Frame;
		prediction.Linear = sample.Motion.Linear;
		prediction.Angular = sample.Motion.Angular;
		prediction.Humanoid = *humanoid;
		prediction.Humanoid.WalkSpeed = sample.Motion.WalkSpeed;
		prediction.Humanoid.JumpSpeed = sample.Motion.JumpSpeed;
		prediction.Humanoid.Grounded = sample.Motion.Grounded;
		prediction.AuthorityTick = sample.Motion.DestinationTick;
		prediction.Active = true;
		double acknowledgedSeconds = 0;
		uint64_t acknowledgedThrough = native->Clock.InputTick;
		for (const auto &input : inputs) {
			if (input.Tick <= native->Clock.InputTick || input.Tick > sample.Motion.InputTick) continue;
			engine::game::MoveInput move;
			if (!engine::game::DecodeMoveInput(input.Bytes, move)) continue;
			acknowledgedSeconds += move.StepSeconds > 0 ? move.StepSeconds : store.Time().Delta;
			acknowledgedThrough = input.Tick;
		}
		auto clock = ReplayClock(native->Clock, sample.Motion, acknowledgedSeconds, acknowledgedThrough);
		if (!clock) return {};
		BeginTimedReplay(prediction, clock->InputLeadSeconds);
		double skip = std::max(0.0, -clock->InputLeadSeconds);
		for (const auto &input : inputs) {
			if (input.Tick <= sample.Motion.InputTick) continue;
			engine::game::MoveInput move;
			if (!engine::game::DecodeMoveInput(input.Bytes, move)) continue;
			if (move.StepSeconds > std::numeric_limits<float>::max()) return {};
			const float delta =
				move.StepSeconds > 0 ? static_cast<float>(move.StepSeconds) : store.Time().Delta;
			ReplayTimedInput(prediction, move, delta, skip);
		}
		// A slow client can remain behind authority indefinitely. Rebase the
		// uncovered time onto this completed pose after preserving pending controls.
		clock->InputLeadSeconds += skip;
		auto validated = sample.Motion;
		validated.Frame = prediction.Frame;
		validated.Linear = prediction.Linear;
		if (!engine::script::ValidPortalTransferMotion(validated)) return {};
		if (const auto *previous = store.Resource<LocalPlayerPrediction>())
			RetainPositionCorrection(*previous, prediction);
		store.SetResource(prediction);
		if (auto *buffer = store.ResourceMutable<SnapshotBuffer>()) buffer->Predict(prediction.Root);
		native->AppliedPoseTick = sample.Motion.DestinationTick;
		native->AppliedInputTick = sample.Motion.InputTick;
		native->Clock = *clock;
		return sample.Motion.InputTick;
	}

	bool RecordPortalPredictionInput(
		Store &store, uint64_t tick, const engine::game::MoveInput &move, float delta
	) {
		auto *history = store.ResourceMutable<PortalInputHistory>();
		if (!history || history->Claim.Destination.empty() || tick <= history->LastRecordedTick ||
			!std::isfinite(delta) || delta < 0 || !std::isfinite(move.Direction.Magnitude()) ||
			move.Direction.Magnitude() > 1.001f)
			return false;
		if (history->Count == PortalInputHistory::CAPACITY) {
			history->CoveredThrough = history->Inputs[history->Begin].Tick;
			history->Begin = (history->Begin + 1) % PortalInputHistory::CAPACITY;
			--history->Count;
			++history->DiscardedInputs;
		}
		const auto at = (history->Begin + history->Count) % PortalInputHistory::CAPACITY;
		history->Inputs[at] = {tick, move, delta};
		++history->Count;
		history->LastRecordedTick = tick;
		engine::core::Metrics::Count("client.portal.input.copied.bytes", sizeof(PortalPredictionInput));
		return true;
	}

	bool ReconcilePortalInputHistory(
		Store &store,
		const engine::game::PortalResume &claim,
		const engine::script::PortalTransferMotion &motion
	) {
		auto *history = store.ResourceMutable<PortalInputHistory>();
		auto *prediction = store.ResourceMutable<LocalPlayerPrediction>();
		const auto *held = store.Resource<engine::scene::CameraCharacterHold>();
		if (!history || history->Claim != claim || !prediction || !prediction->Active || !held ||
			!held->Active || prediction->Root != held->Root || prediction->Player != held->Player ||
			!engine::script::ValidPortalTransferMotion(motion) ||
			motion.DestinationIncarnation != claim.DestinationIncarnation ||
			motion.DestinationTick <= history->AppliedDestinationTick ||
			motion.InputTick < history->CoveredThrough || motion.InputTick > history->LastRecordedTick)
			return false;
		ENGINE_PROFILE("portal input replay");
		auto replayed = *prediction;
		replayed.Frame = motion.Frame;
		replayed.AuthorityTick = motion.DestinationTick;
		replayed.Linear = motion.Linear;
		replayed.Angular = motion.Angular;
		// With no remaining inputs, the held direction still needs the same
		// coordinate round trip as the state that replay would otherwise replace.
		replayed.Humanoid.MoveDirection = history->Through.Rotate(replayed.Humanoid.MoveDirection);
		replayed.Humanoid.WalkSpeed = motion.WalkSpeed;
		replayed.Humanoid.JumpSpeed = motion.JumpSpeed;
		replayed.Humanoid.Grounded = motion.Grounded;
		double acknowledgedSeconds = 0;
		uint64_t acknowledgedThrough = history->Clock.InputTick;
		for (size_t index = 0; index < history->Count; ++index) {
			const auto &entry = history->Inputs[(history->Begin + index) % PortalInputHistory::CAPACITY];
			if (entry.Tick <= history->Clock.InputTick || entry.Tick > motion.InputTick) continue;
			acknowledgedSeconds += entry.Delta;
			acknowledgedThrough = entry.Tick;
		}
		auto clock = ReplayClock(history->Clock, motion, acknowledgedSeconds, acknowledgedThrough);
		if (!clock) return false;
		BeginTimedReplay(replayed, clock->InputLeadSeconds);
		double skip = std::max(0.0, -clock->InputLeadSeconds);
		for (size_t index = 0; index < history->Count; ++index) {
			const auto &entry = history->Inputs[(history->Begin + index) % PortalInputHistory::CAPACITY];
			if (entry.Tick <= motion.InputTick) continue;
			auto move = entry.Move;
			move.Direction = history->Through.Rotate(move.Direction);
			ReplayTimedInput(replayed, move, entry.Delta, skip);
		}
		// A slow client can remain behind authority indefinitely. Rebase the
		// uncovered time onto this completed pose after preserving pending controls.
		clock->InputLeadSeconds += skip;
		const engine::scene::SeamTransform reverse{
			history->Through.Frame.Inverse(),
			history->Through.Point(history->Through.Origin),
			1 / history->Through.Scale
		};
		replayed.Frame = reverse.Place(replayed.Frame).Orthonormalize();
		replayed.Linear = reverse.Carry(replayed.Linear);
		replayed.Angular = reverse.Rotate(replayed.Angular);
		replayed.Humanoid.MoveDirection = reverse.Rotate(replayed.Humanoid.MoveDirection);
		replayed.Humanoid.WalkSpeed = reverse.Length(replayed.Humanoid.WalkSpeed);
		replayed.Humanoid.JumpSpeed = reverse.Length(replayed.Humanoid.JumpSpeed);
		auto validation = motion;
		validation.Frame = replayed.Frame;
		validation.Linear = replayed.Linear;
		validation.Angular = replayed.Angular;
		validation.WalkSpeed = replayed.Humanoid.WalkSpeed;
		validation.JumpSpeed = replayed.Humanoid.JumpSpeed;
		if (!engine::script::ValidPortalTransferMotion(validation)) return false;
		RetainPositionCorrection(*prediction, replayed);
		*prediction = replayed;
		while (history->Count && history->Inputs[history->Begin].Tick <= motion.InputTick) {
			history->Begin = (history->Begin + 1) % PortalInputHistory::CAPACITY;
			--history->Count;
		}
		history->CoveredThrough = motion.InputTick;
		history->AppliedInputTick = motion.InputTick;
		history->AppliedDestinationTick = motion.DestinationTick;
		history->Clock = *clock;
		return true;
	}

}
