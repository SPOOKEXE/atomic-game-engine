// Property descriptors for placement, physics, transparency, the humanoid root
// part and the camera: CFrame, Position, Size, Mass, CollisionGroup and the rest.

#include "PartInternal.hpp"

namespace engine::scene::part_detail {

	// Position: the translation of `Transform`, with the rotation kept.
	//
	// The read-modify-write is the entire point. An offset-shaped setter
	// would write twelve bytes over the front of a `CFrame` and leave a
	// quaternion that no longer matches - which is exactly what a member
	// pointer cannot express and why this is a conversion.
	// Writes a placement.
	//
	// **`PreviousTransform` is deliberately *not* written here, and that is
	// a decision this function exists to record.** The obvious reading is
	// that an authored write is a teleport and should move the frame it is
	// interpolated from - and it was written that way first. It is wrong:
	// `examples/Rings.luau` and every scripted animation in this engine set
	// `CFrame` once per tick and rely on the draw list interpolating between
	// ticks, which is what buys smooth motion at 300 frames a second over a
	// 60 Hz simulation. Clearing the previous frame on every write turns all
	// of it into stepped motion at the tick rate.
	//
	// `client.scene.tick`'s "rendering interpolates between the last two
	// ticks" is the case that caught it, and it is the case that matters.
	//
	// **The editor's problem was a different one and is fixed where it
	// belongs.** A world nothing ticks never runs `capture-previous` -
	// that is a `PreSimulation` system and `World::Present` runs
	// `PreRender` alone - so its `PreviousTransform` is wherever each part
	// was created. The fix is to present such a world at alpha *one*,
	// because there is no next tick to draw towards.
	//
	// **Which world that is cannot be read off `WorldState`**, and getting
	// that wrong put the bug back for the whole of Edit mode: the studio
	// leaves every world `Active` while ticking none of them, so a state
	// test said "interpolate" and every part drew at the origin.
	// `studio::PresentationAlpha` combines the two halves.
	//
	// @param store    The world.
	// @param instance The entity.
	// @param frame    Where it now is.
	bool PlaceInstance(ecs::Store &store, ecs::Entity instance, const core::CFrame &frame) {
		if (Transform *transform = store.GetMutable<Transform>(instance)) {
			transform->Frame = frame;
			return true;
		}
		return false;
	}

	// CFrame: the placement itself.
	//
	// A `Computed` over a field a member pointer could reach, which is the
	// one case in this file where that is not a smell - see `PlaceInstance`.
	PropertyDescriptor CFrameProperty() {
		PropertyDescriptor property;
		property.Name = core::Name("CFrame");
		property.Type = PropertyType::CFrame;
		property.Size = sizeof(core::CFrame);
		property.Kind = PropertyKind::Computed;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Transform>()});
		property.Writes = property.Reads;

		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			const Transform *transform = store.Get<Transform>(instance);
			if (transform == nullptr) {
				return false;
			}
			*static_cast<core::CFrame *>(out) = transform->Frame;
			return true;
		};

		property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
			return PlaceInstance(store, instance, *static_cast<const core::CFrame *>(value));
		};

		return property;
	}

	PropertyDescriptor PositionProperty() {
		PropertyDescriptor property;
		property.Name = core::Name("Position");
		property.Type = PropertyType::Vector3;
		property.Size = sizeof(core::Vector3);
		property.Kind = PropertyKind::Computed;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Transform>()});
		property.Writes = property.Reads;
		// A view of `CFrame`, which documents already carry. Older
		// documents that wrote it still load.
		property.DocumentWritable = false;

		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			const Transform *transform = store.Get<Transform>(instance);
			if (transform == nullptr) {
				return false;
			}
			*static_cast<core::Vector3 *>(out) = transform->Frame.Position;
			return true;
		};

		property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
			const Transform *transform = store.Get<Transform>(instance);
			if (transform == nullptr) {
				return false;
			}
			core::CFrame frame = transform->Frame;
			frame.Position = *static_cast<const core::Vector3 *>(value);
			PlaceInstance(store, instance, frame);
			return true;
		};

		return property;
	}

	// Orientation: the rotation of `Transform`, as intrinsic Y-X-Z turns.
	//
	// **Degrees, because Roblox's `.Orientation` is degrees** and this is
	// Roblox's surface - a script that works there should read the same
	// number here. `CFrame::Angles` keeps radians because that is the
	// engine's API in the engine's unit, so the factor lives here, in one
	// place, in both directions. A getter in degrees against a setter in
	// radians is a 57x error that looks like nothing until something spins.
	PropertyDescriptor OrientationProperty() {
		PropertyDescriptor property;
		property.Name = core::Name("Orientation");
		property.Type = PropertyType::Vector3;
		property.Size = sizeof(core::Vector3);
		property.Kind = PropertyKind::Computed;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Transform>()});
		property.Writes = property.Reads;
		// A view of `CFrame`, which documents already carry. Older
		// documents that wrote it still load.
		property.DocumentWritable = false;

		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			const Transform *transform = store.Get<Transform>(instance);
			if (transform == nullptr) {
				return false;
			}
			const core::Vector3 radians = transform->Frame.ToAngles();
			*static_cast<core::Vector3 *>(out) = radians * DEGREES_PER_RADIAN;
			return true;
		};

		// Position kept, for the same reason Position keeps the rotation.
		property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
			const Transform *transform = store.Get<Transform>(instance);
			if (transform == nullptr) {
				return false;
			}
			const core::Vector3 degrees = *static_cast<const core::Vector3 *>(value);
			core::CFrame frame = core::CFrame::Angles(
				degrees.X * RADIANS_PER_DEGREE, degrees.Y * RADIANS_PER_DEGREE, degrees.Z * RADIANS_PER_DEGREE
			);

			frame.Position = transform->Frame.Position;
			PlaceInstance(store, instance, frame);
			return true;
		};

		return property;
	}

	// Size: the full extent, where the storage keeps half of one.
	//
	// **Writes two components, and that is a correctness requirement rather
	// than a convenience.** `MakePart` sets `Bounds::HalfExtent` and
	// `Collider::Extent` from one number; a setter that moved only the
	// first would leave a part drawn at one size and collided at another,
	// and nothing would report it. `Writes` naming both is what tells the
	// manifest, and v0.6's `.Changed`, that this is what happened.
	PropertyDescriptor PartSizeProperty() {
		PropertyDescriptor property;
		property.Name = core::Name("Size");
		property.Type = PropertyType::Vector3;
		property.Size = sizeof(core::Vector3);
		property.Kind = PropertyKind::Computed;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Bounds>()});
		property.Writes =
			&ecs::ComponentSet::Intern({ecs::Components::Of<Bounds>(), ecs::Components::Of<Collider>()});

		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			const Bounds *bounds = store.Get<Bounds>(instance);
			if (bounds == nullptr) {
				return false;
			}
			*static_cast<core::Vector3 *>(out) = bounds->HalfExtent * 2.0f;
			return true;
		};

		property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
			Bounds *bounds = store.GetMutable<Bounds>(instance);
			if (bounds == nullptr) {
				return false;
			}

			// Halved once, here, for the reason `MakePart` gives: halving
			// in two places is where the two eventually disagree by a
			// factor of two.
			const core::Vector3 half = *static_cast<const core::Vector3 *>(value) * 0.5f;
			bounds->HalfExtent = half;

			// Absent on something that is drawn and not collided, which is
			// legal - so this is not a failure.
			if (Collider *collider = store.GetMutable<Collider>(instance)) {
				collider->Extent = half;
			}
			return true;
		};

		return property;
	}

	// CanCollide: the inverse of `Collider::Trigger`.
	//
	// **Not the layer mask**, which was the first mapping tried and is
	// lossy: clearing a mask to say "no" and restoring `All()` to say "yes"
	// destroys whatever the game had configured, so `part.CanCollide =
	// false` followed by `true` silently widens what it hits. `Trigger`
	// means "report the contact and apply no impulse", which is what
	// Roblox's `CanCollide = false` does - a part still fires `Touched`.
	PropertyDescriptor CanCollideProperty() {
		PropertyDescriptor property;
		property.Name = core::Name("CanCollide");
		property.Type = PropertyType::Bool;
		property.Size = sizeof(bool);
		property.Kind = PropertyKind::Computed;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Collider>()});
		property.Writes = property.Reads;

		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			const Collider *collider = store.Get<Collider>(instance);
			if (collider == nullptr) {
				return false;
			}
			*static_cast<bool *>(out) = !collider->Trigger;
			return true;
		};

		property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
			Collider *collider = store.GetMutable<Collider>(instance);
			if (collider == nullptr) {
				return false;
			}
			collider->Trigger = !*static_cast<const bool *>(value);
			return true;
		};

		return property;
	}

	// CanTouch: the inverse of `Collider::TouchDisabled`, stored inverted
	// so older raw saves keep reporting touches. See that field.
	PropertyDescriptor CanTouchProperty() {
		PropertyDescriptor property;
		property.Name = core::Name("CanTouch");
		property.Type = PropertyType::Bool;
		property.Size = sizeof(bool);
		property.Kind = PropertyKind::Computed;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Collider>()});
		property.Writes = property.Reads;

		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			const Collider *collider = store.Get<Collider>(instance);
			if (collider == nullptr) {
				return false;
			}
			*static_cast<bool *>(out) = !collider->TouchDisabled;
			return true;
		};

		property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
			Collider *collider = store.GetMutable<Collider>(instance);
			if (collider == nullptr) {
				return false;
			}
			collider->TouchDisabled = !*static_cast<const bool *>(value);
			return true;
		};

		return property;
	}

	// Mass: what the part weighs, and it is read-only for the reason
	// Roblox's is.
	//
	// **Derived, so there is nothing to write it to.** A part with
	// `CustomPhysicalProperties` weighs its density times its volume, and a
	// part without one weighs whatever `RigidBody::Mass` says - `MassOf` is
	// the rule and the solver asks the same function. Offering a setter
	// would mean deciding which of the two inputs an assignment moves, and
	// both answers are wrong: writing density makes a resize change what was
	// just set, and writing `RigidBody::Mass` makes it silently disagree
	// with the density beside it.
	//
	// An anchored part has no `RigidBody` and answers zero, which is what
	// the solver reads as immovable - the honest number for a thing the
	// world may not move.
	PropertyDescriptor MassProperty() {
		PropertyDescriptor property;
		property.Name = core::Name("Mass");
		property.Type = PropertyType::Float;
		property.Size = sizeof(float);
		property.Reads = &ecs::ComponentSet::Intern({
			ecs::Components::Of<RigidBody>(),
			ecs::Components::Of<Collider>(),
			ecs::Components::Of<PhysicsProperties>(),

			// Named because the answer depends on it: an anchored part
			// weighs zero here, and `.Changed` on `Mass` has to fire when a
			// part is anchored or let go.
			ecs::Components::Of<Simulated>(),
		});

		// **The empty set rather than nothing at all.** A read-only
		// property writes no component, and saying so with an interned
		// empty set is what lets a caller walk `Writes` without first
		// asking whether it is there - which is what per-instance
		// `.Changed` does for every property at once.
		property.Writes = &ecs::ComponentSet::Intern({});

		// **Declared rather than implied by the missing setter.** A panel
		// greys a row on this flag and a script's assignment is refused by
		// it; leaving it true with no `Set` would be a property that reads
		// as editable and silently does nothing.
		property.Writable = false;

		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			const RigidBody *body = store.Get<RigidBody>(instance);
			const Collider *collider = store.Get<Collider>(instance);

			// **Anchored still weighs zero**, which is what the solver reads
			// as immovable and the honest number for a thing the world may
			// not move. The test is `Simulated` rather than a missing
			// `RigidBody` because a part now keeps its body while anchored -
			// the number it would weigh is still there and is still not the
			// answer to this question.
			//
			// A *sleeping* part still reports its real mass, which is the
			// right answer: it is a body that happens to be resting, and the
			// panel should not blink to zero when a crate settles.
			if (body == nullptr || collider == nullptr || !store.Has<Simulated>(instance)) {
				*static_cast<float *>(out) = 0.0f;
				return true;
			}
			*static_cast<float *>(out) = MassOf(*collider, *body, store.Get<PhysicsProperties>(instance));
			return true;
		};

		// No `Set`. A descriptor with none is read-only, which is what a
		// panel greys out and what a script's assignment is refused by.
		return property;
	}

	// Anchored: whether the world may move it - and the one property that
	// is not stored anywhere.
	//
	// **This is the Roblox name and it is the negation of what the engine
	// stores.** The engine's tag is `scene::Simulated` and its presence
	// means the world may move it, so `Anchored` is `!Has<Simulated>`.
	// **This function is the only place that inversion is spelled**, which
	// is the whole reason the shim is a property descriptor rather than a
	// rename: a second `!` somewhere else is a part that reads as anchored
	// and falls.
	//
	// **Presence, not a flag**, and since v0.15 the presence it decides is a
	// tag's rather than `RigidBody`'s. A static part carries no tag and no
	// `Motion`, so it sits in a different archetype and the dynamic queries
	// name the tag positively. What changed at v0.15 is that it keeps its
	// `RigidBody`, so the mass and the drag an author typed are still there
	// when the part is let go again; what changed at v0.18 is which way
	// round the tag reads.
	//
	// Reading it is therefore a component test and writing it is an
	// archetype move - which is what `PropertyKind::Structural` exists to
	// announce, so a caller knows this one defers where the others do not.
	PropertyDescriptor AnchoredProperty() {
		PropertyDescriptor property;
		property.Name = core::Name("Anchored");
		property.Type = PropertyType::Bool;
		property.Size = sizeof(bool);
		property.Kind = PropertyKind::Structural;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Simulated>()});
		property.Writes =
			&ecs::ComponentSet::Intern({ecs::Components::Of<Simulated>(), ecs::Components::Of<Motion>()});

		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			*static_cast<bool *>(out) = !store.Has<Simulated>(instance);
			return true;
		};

		// Deferred by the store when this runs inside iteration, which is
		// the whole reason the kind is declared rather than inferred: a
		// structural change applied inline would move the row out from
		// under the loop walking it.
		//
		// **`RigidBody` is not touched either way.** Removing it here was
		// the bug: it is the author's numbers, and anchoring a part to move
		// it and unanchoring it again should not reset its mass.
		//
		// **`Motion` moves with the tag rather than being left to the
		// solver.** Unanchoring has to hand back a `Motion` or the part is
		// indistinguishable from a sleeping one and waits for a contact that
		// may never come; anchoring has to take it away or the integrator
		// keeps moving something the author just declared immovable.
		property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
			if (*static_cast<const bool *>(value)) {
				store.Remove<Simulated>(instance);
				store.Remove<Motion>(instance);
			} else {
				store.Set(instance, Simulated{});
				store.Set(instance, Motion{});
			}
			return true;
		};

		return property;
	}

	// Kinematic: whether a script or animation moves this part.
	//
	// A kinematic body has `Motion`, so the broad phase keeps its proxy out of
	// the static index, and `Simulated`, so it keeps the engine's established
	// awake-body archetype. `RigidBody::Kind` is the ownership boundary: the
	// solver gives it infinite mass and never writes a contact velocity back.
	// This is the right state for a moving platform, scripted projectile, or
	// editor gizmo. It can contact bodies and answer queries without forcing a
	// static-index rebuild after each script position write.
	//
	// `Anchored` is deliberately false in this state. That property is only
	// the inverse of `Simulated`; its one meaning must stay intact while
	// `Kinematic` names the separate question of who moves the body.
	PropertyDescriptor KinematicProperty() {
		PropertyDescriptor property;
		property.Name = core::Name("Kinematic");
		property.Type = PropertyType::Bool;
		property.Size = sizeof(bool);
		property.Kind = PropertyKind::Structural;
		property.Reads = &ecs::ComponentSet::Intern(
			{ecs::Components::Of<Simulated>(),
			 ecs::Components::Of<Motion>(),
			 ecs::Components::Of<RigidBody>()}
		);
		property.Writes = property.Reads;

		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			const RigidBody *body = store.Get<RigidBody>(instance);
			*static_cast<bool *>(out) = body != nullptr && store.Has<Simulated>(instance) &&
										store.Has<Motion>(instance) && body->Kind == BodyKind::Kinematic;
			return true;
		};

		property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
			RigidBody *body = store.GetMutable<RigidBody>(instance);
			if (body == nullptr) {
				return false;
			}
			if (*static_cast<const bool *>(value)) {
				// Changing ownership starts with no velocity. A script can explicitly
				// set one afterwards when it wants the integrator to advance it.
				body->Kind = BodyKind::Kinematic;
				store.Set(instance, Simulated{});
				store.Set(instance, Motion{});
			} else if (body->Kind == BodyKind::Kinematic) {
				body->Kind = BodyKind::Dynamic;
			}
			return true;
		};

		return property;
	}

	// CollisionGroup: a name over `Collider::Layer`.
	//
	// **The bits stay anonymous in `scene` and the naming lives in
	// `spatial`**, which is where `LayerMask` is. This module holding the
	// table would be a component module deciding a physics policy for every
	// game that uses it - the reason this property was named as a gap at
	// v0.5 rather than guessed at.
	//
	// A **name** crosses, never the index. Rule 4: which bit a group holds
	// depends on registration order, which is the order a game's files
	// linked in, and a save file carrying the number would point at a
	// different group after a build that reordered two registrations.
	PropertyDescriptor CollisionGroupProperty() {
		PropertyDescriptor property;
		property.Name = core::Name("CollisionGroup");
		property.Type = PropertyType::Name;
		property.Size = sizeof(core::Name);
		property.Kind = PropertyKind::Computed;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Collider>()});
		property.Writes = property.Reads;

		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			const Collider *collider = store.Get<Collider>(instance);
			if (collider == nullptr) {
				return false;
			}

			// The lowest set bit. A collider belongs to exactly one group
			// even though `Layer` could hold several - `MakePart` sets
			// `Only(0)` - so this reports the group it was put in rather
			// than inventing an answer for a mask nothing here produces.
			uint32_t index = 0;
			for (; index < spatial::LayerMask::LAYER_COUNT; index++) {
				if ((collider->Layer.Bits & (1u << index)) != 0) {
					break;
				}
			}

			*static_cast<core::Name *>(out) = spatial::CollisionGroups::NameOf(index);
			return true;
		};

		property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
			const auto name = *static_cast<const core::Name *>(value);
			const uint32_t index = spatial::CollisionGroups::IndexOf(name);
			if (index == spatial::NO_GROUP) {
				// Refused rather than defaulted. A typo that silently put a
				// part in `Default` would be a collision bug nobody could
				// see from the script that caused it - and the name is the
				// half a script author can act on.
				ENGINE_WARN("'{}' is not a registered collision group; the part is unchanged", name.Text());
				return false;
			}

			Collider *collider = store.GetMutable<Collider>(instance);
			if (collider == nullptr) {
				return false;
			}

			// **Both halves.** The layer says which group this is; the mask
			// says which groups it meets, and that comes from the matrix
			// rather than from the caller. Writing only the layer would put
			// the part in a group whose configuration it then ignored.
			collider->Layer = spatial::LayerMask::Only(index);
			collider->Mask = spatial::CollisionGroups::MaskFor(index);
			return true;
		};

		// A world document is authored content, so it must carry the group name
		// rather than the process-local bit. Recreate that name before this
		// property's strict setter runs, including for a port opened in a new
		// Studio process. Script writes still use the strict setter above.
		property.PrepareDocument = [](const void *value) -> bool {
			const auto name = *static_cast<const core::Name *>(value);
			return name.IsValid() && spatial::CollisionGroups::Register(name.Text()) != spatial::NO_GROUP;
		};
		return property;
	}

	// LocalTransparency: read-only through the property table, for
	// `scene::LocalTransparency`'s own reason.
	//
	// **Written through `scene::SetLocalTransparency` instead**, which is
	// not gated by `Store::AdoptOnly` the way `Store::SetProperty` is - see
	// that function's header. A script still reads `part.LocalTransparency`
	// like any other property; it calls a method to change it, the same
	// shape `Instance:SetAttribute` already asks an author to accept.
	PropertyDescriptor LocalTransparencyProperty() {
		PropertyDescriptor property;
		property.Name = core::Name("LocalTransparency");
		property.Type = PropertyType::Float;
		property.Size = sizeof(float);
		property.Kind = PropertyKind::Computed;

		property.Writable = false;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<LocalTransparency>()});
		property.Writes = &ecs::ComponentSet::Intern({});

		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			const LocalTransparency *local = store.Get<LocalTransparency>(instance);
			if (local == nullptr) {
				return false;
			}
			*static_cast<float *>(out) = local->Value;
			return true;
		};

		return property;
	}

	PropertyDescriptor HumanoidRootPartProperty() {
		PropertyDescriptor property;
		property.Name = core::Name("RootPart");
		property.Type = PropertyType::Reference;
		property.Kind = PropertyKind::Computed;
		property.Size = sizeof(ecs::Entity);
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Humanoid>()});
		property.Writes = property.Reads;
		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			const Humanoid *humanoid = store.Get<Humanoid>(instance);
			if (humanoid == nullptr) {
				return false;
			}
			*static_cast<ecs::Entity *>(out) =
				store.Alive(humanoid->RootPart) ? humanoid->RootPart : ecs::NULL_ENTITY;
			return true;
		};
		property.GetStoredReference = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			const Humanoid *humanoid = store.Get<Humanoid>(instance);
			if (humanoid == nullptr) return false;
			*static_cast<ecs::Entity *>(out) = humanoid->RootPart;
			return true;
		};
		property.PrepareStoredReference =
			[](ecs::Store &store, ecs::Entity instance, ecs::Entity target) -> ecs::Entity * {
			if (target != ecs::NULL_ENTITY && !store.Has<Transform>(target)) return nullptr;
			Humanoid *humanoid = store.GetMutable<Humanoid>(instance);
			return humanoid == nullptr ? nullptr : &humanoid->RootPart;
		};
		property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
			const ecs::Entity root = *static_cast<const ecs::Entity *>(value);
			if (root != ecs::NULL_ENTITY && !store.Has<Transform>(root)) {
				return false;
			}
			Humanoid *humanoid = store.GetMutable<Humanoid>(instance);
			if (humanoid == nullptr) {
				return false;
			}
			humanoid->RootPart = root;
			return true;
		};
		return property;
	}

	bool RestoreCameraSubject(ecs::Store &store, ecs::Entity camera, const void *value) {
		const ecs::Entity target = *static_cast<const ecs::Entity *>(value);
		if (target != ecs::NULL_ENTITY && !store.Has<Humanoid>(target) && !store.Has<Transform>(target)) {
			return false;
		}
		CameraSubject *selection = store.GetMutable<CameraSubject>(camera);
		if (selection == nullptr) {
			return false;
		}
		selection->Target = target;
		return true;
	}

	// Each camera owns its target, including before it becomes current.
	PropertyDescriptor CameraSubjectProperty() {
		PropertyDescriptor property;
		property.Name = core::Name("CameraSubject");
		property.PredictedWritable = true;
		property.Type = PropertyType::Reference;
		property.Nullable = true;
		property.Kind = PropertyKind::Computed;
		property.Size = sizeof(ecs::Entity);
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<CameraSubject>()});
		property.Writes = property.Reads;
		property.Get = [](const ecs::Store &store, ecs::Entity camera, void *out) -> bool {
			const CameraSubject *selection = store.Get<CameraSubject>(camera);
			if (selection == nullptr) {
				return false;
			}
			*static_cast<ecs::Entity *>(out) =
				store.Alive(selection->Target) ? selection->Target : ecs::NULL_ENTITY;
			return true;
		};
		property.GetStoredReference = [](const ecs::Store &store, ecs::Entity camera, void *out) -> bool {
			const CameraSubject *selection = store.Get<CameraSubject>(camera);
			if (selection == nullptr) return false;
			*static_cast<ecs::Entity *>(out) = selection->Target;
			return true;
		};
		property.PrepareStoredReference =
			[](ecs::Store &store, ecs::Entity camera, ecs::Entity target) -> ecs::Entity * {
			if (target != ecs::NULL_ENTITY && !store.Has<Humanoid>(target) && !store.Has<Transform>(target))
				return nullptr;
			CameraSubject *selection = store.GetMutable<CameraSubject>(camera);
			return selection == nullptr ? nullptr : &selection->Target;
		};
		property.RestoreReference = RestoreCameraSubject;
		property.Set = [](ecs::Store &store, ecs::Entity camera, const void *value) -> bool {
			if (!RestoreCameraSubject(store, camera, value)) {
				return false;
			}
			store.GetMutable<CameraSubject>(camera)->Automatic = false;
			return true;
		};
		return property;
	}

	constexpr std::array<std::string_view, 4> CAMERA_TYPE_NAMES{
		"Custom", "LockFirstPerson", "ShiftLock", "Scriptable"
	};

	// Only the current eye controls the keyboard camera's world-local mode.
	PropertyDescriptor CameraTypeProperty() {
		PropertyDescriptor property;
		property.Name = core::Name("CameraType");
		property.PredictedWritable = true;
		property.Type = PropertyType::Enum;
		property.EnumName = core::Name("CameraType");
		property.Kind = PropertyKind::Resource;
		property.Size = sizeof(core::Name);
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<CameraController>()});
		property.Writes = property.Reads;
		property.Get = [](const ecs::Store &store, ecs::Entity camera, void *out) -> bool {
			const auto *active = store.Resource<ActiveCamera>();
			const auto *controller = store.Resource<CameraController>();
			*static_cast<core::Name *>(out) = core::Name("Custom");
			if (active == nullptr || active->Entity != camera || controller == nullptr) return true;
			const size_t index = static_cast<size_t>(controller->Mode);
			if (index >= CAMERA_TYPE_NAMES.size()) return false;
			*static_cast<core::Name *>(out) = core::Name(CAMERA_TYPE_NAMES[index]);
			return true;
		};
		property.Set = [](ecs::Store &store, ecs::Entity camera, const void *value) -> bool {
			const auto *active = store.Resource<ActiveCamera>();
			const auto *controller = store.Resource<CameraController>();
			if (active == nullptr || active->Entity != camera || controller == nullptr) return false;
			const core::Name named = *static_cast<const core::Name *>(value);
			size_t index = 0;
			if (named != core::Name("Classic")) {
				for (; index < CAMERA_TYPE_NAMES.size(); ++index) {
					if (named == core::Name(CAMERA_TYPE_NAMES[index])) break;
				}
			}
			if (index >= CAMERA_TYPE_NAMES.size()) return false;
			const auto mode = static_cast<CameraMode>(index);
			if (controller->Mode == mode) return true;
			store.ResourceMutable<CameraController>()->Mode = mode;
			return true;
		};
		return property;
	}

	// FieldOfView: the camera's vertical angle, in degrees.
	//
	// Degrees out, radians stored - Roblox's `Camera.FieldOfView` is
	// degrees and every trigonometric consumer wants radians, so the
	// conversion is exactly what a computed property is for. `Orientation`
	// makes the same trade one file up.
	PropertyDescriptor FieldOfViewProperty() {
		PropertyDescriptor property;
		property.Name = core::Name("FieldOfView");
		property.Type = PropertyType::Float;
		property.Size = sizeof(float);
		property.Kind = PropertyKind::Computed;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Camera>()});
		property.Writes = property.Reads;

		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			const Camera *camera = store.Get<Camera>(instance);
			if (camera == nullptr) {
				return false;
			}
			*static_cast<float *>(out) = camera->FieldOfViewRadians * DEGREES_PER_RADIAN;
			return true;
		};

		property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
			Camera *camera = store.GetMutable<Camera>(instance);
			if (camera == nullptr) {
				return false;
			}
			camera->FieldOfViewRadians = *static_cast<const float *>(value) * RADIANS_PER_DEGREE;
			return true;
		};
		return property;
	}

}
