// Property descriptors for the forward-declared classes: bones, skeletons, rig
// keypoints, animation buffers and tracks, constraint anchors and joint limits.

#include "PartInternal.hpp"

namespace engine::scene::part_detail {

	// --- the forward-declared property surface ---------------------------
	//
	// **What a script may name on a bone, a track and a joint.** Each of
	// these is storage nothing reads yet - decision 16 - and every one of
	// them is authored, so leaving them undeclared would ship a class whose
	// only door is C++.
	//
	// **Three components deliberately declare no property at all**, and the
	// reason is `AGENTS.md`'s "do not declare a property for scripts alone"
	// read the other way round. `Skeleton` and `LevelOfDetail` are optional
	// columns on a `MeshPart`: a property on that class would fail on every
	// mesh part that is not skinned and has no level ladder, which is almost
	// all of them, and a property that usually fails is worse than one that
	// does not exist. Both are written by the publisher, which is C++.
	// `Bone::Joint` and `Bone::ParentJoint` are refused for a sharper reason:
	// a script renumbering a palette slot is a script that makes a vertex's
	// joint index name a slot nothing filled.

	// The frame a bone ended up in, once `ResolveBones` has run.
	//
	// Roblox's `Bone.TransformedWorldCFrame`, and read-only for
	// `WorldCFrameProperty`'s reason: a writable one would be a second way to
	// place a joint, and the two would disagree the moment the rig moved.
	//
	// **The getter resolves on the spot rather than reading the field**,
	// which is the same split the attachment's four make: a script reading
	// this on the frame it built a rig would otherwise get an identity
	// through the field and the right answer through the walk, and a property
	// whose answer depends on when in the frame it is asked is one nobody can
	// reason about. The draw path reads `Bone::WorldFrame`.
	PropertyDescriptor BoneWorldProperty() {
		PropertyDescriptor property;
		property.Name = core::Name("TransformedWorldCFrame");
		property.Type = PropertyType::CFrame;
		property.Size = sizeof(core::CFrame);
		property.Kind = PropertyKind::Computed;
		property.Writable = false;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Bone>()});

		// Nothing, because nothing is written. `Writes` reaches the binding
		// manifest, and naming a component there tells a script author that
		// setting this moves storage.
		property.Writes = &ecs::ComponentSet::Intern({});

		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			if (store.Get<Bone>(instance) == nullptr) {
				return false;
			}
			*static_cast<core::CFrame *>(out) = ResolveBone(store, instance);
			return true;
		};
		return property;
	}

	// The palette count is stored compactly and authored as an ordinary script
	// integer. The conversion also keeps a malformed rig from sizing a palette
	// beyond the limit the renderer and mesh format share.
	PropertyDescriptor SkeletonJointCountProperty() {
		PropertyDescriptor property;
		property.Name = core::Name("JointCount");
		property.Type = PropertyType::Int32;
		property.Size = sizeof(int32_t);
		property.Kind = PropertyKind::Computed;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Skeleton>()});
		property.Writes = property.Reads;

		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			const Skeleton *skeleton = store.Get<Skeleton>(instance);
			if (skeleton == nullptr) {
				return false;
			}
			*static_cast<int32_t *>(out) = static_cast<int32_t>(skeleton->JointCount);
			return true;
		};

		property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
			const int32_t count = *static_cast<const int32_t *>(value);
			if (count < 0 || count > static_cast<int32_t>(MAX_JOINTS)) {
				return false;
			}
			Skeleton *skeleton = store.GetMutable<Skeleton>(instance);
			if (skeleton == nullptr) {
				return false;
			}
			skeleton->JointCount = static_cast<uint16_t>(count);
			return true;
		};
		return property;
	}

	PropertyDescriptor RigKeypointJointProperty() {
		PropertyDescriptor property;
		property.Name = core::Name("Joint");
		property.Type = PropertyType::Int32;
		property.Size = sizeof(int32_t);
		property.Kind = PropertyKind::Computed;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<RigKeypoint>()});
		property.Writes = property.Reads;

		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			const RigKeypoint *keypoint = store.Get<RigKeypoint>(instance);
			if (keypoint == nullptr) return false;
			*static_cast<int32_t *>(out) =
				keypoint->Joint == NO_JOINT ? -1 : static_cast<int32_t>(keypoint->Joint);
			return true;
		};
		property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
			const int32_t joint = *static_cast<const int32_t *>(value);
			if (store.Get<RigKeypoint>(instance) == nullptr) return false;
			if (joint == -1) {
				RigKeypoint *keypoint = store.GetMutable<RigKeypoint>(instance);
				if (keypoint == nullptr) return false;
				keypoint->Joint = NO_JOINT;
				return true;
			}
			if (joint < 0) return false;
			const Skeleton *skeleton = nullptr;
			for (ecs::Entity walk = store.ParentOf(instance); walk != ecs::NULL_ENTITY;
				 walk = store.ParentOf(walk)) {
				if ((skeleton = store.Get<Skeleton>(walk)) != nullptr) break;
			}
			// A detached keypoint is a normal intermediate state while a script
			// builds a rig. Bound its slot by the shared palette limit until an
			// ancestor skeleton exists; once parented, use that rig's tighter size.
			if (skeleton != nullptr ? joint >= skeleton->JointCount : joint >= MAX_JOINTS) return false;
			RigKeypoint *keypoint = store.GetMutable<RigKeypoint>(instance);
			if (keypoint == nullptr) return false;
			keypoint->Joint = static_cast<uint16_t>(joint);
			return true;
		};
		return property;
	}

	PropertyDescriptor AnimationBufferSizeProperty() {
		PropertyDescriptor property;
		property.Name = core::Name("DataSize");
		property.Type = PropertyType::Int32;
		property.Size = sizeof(int32_t);
		property.Kind = PropertyKind::Computed;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<AnimationBuffer>()});
		property.Writes = &ecs::ComponentSet::Intern({});
		property.Writable = false;
		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			const AnimationBuffer *buffer = store.Get<AnimationBuffer>(instance);
			if (buffer == nullptr) {
				return false;
			}
			*static_cast<int32_t *>(out) = static_cast<int32_t>(buffer->Data.size());
			return true;
		};
		return property;
	}

	PropertyDescriptor AnimationBufferKeyframeBytesProperty() {
		PropertyDescriptor property;
		property.Name = core::Name("KeyframeBytes");
		property.Type = PropertyType::Int32;
		property.Size = sizeof(int32_t);
		property.Kind = PropertyKind::Computed;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<AnimationBuffer>()});
		property.Writes = &ecs::ComponentSet::Intern({});
		property.Writable = false;
		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			if (store.Get<AnimationBuffer>(instance) == nullptr) {
				return false;
			}
			*static_cast<int32_t *>(out) = static_cast<int32_t>(AnimationBuffer::KEYFRAME_BYTES);
			return true;
		};
		return property;
	}

	// Interned once, for `SurfaceEffectEnum`'s reason: a `core::Name`
	// constructed from a literal inside a getter is a registry lookup per
	// property read.
	const core::Name &AnimationPriorityEnum() {
		static const core::Name name("AnimationPriority");
		return name;
	}

	// Which layer a track's channels land on.
	//
	// An `Enum` and not an `Int32`, for `CollisionShapeProperty`'s reason: a
	// number out of range would be a priority the blend has no case for, and
	// an enum is checked against `EnumTable` where the mistake was made.
	PropertyDescriptor TrackPriorityProperty() {
		PropertyDescriptor property;
		property.Name = core::Name("Priority");
		property.Type = PropertyType::Enum;
		property.EnumName = AnimationPriorityEnum();
		property.Size = sizeof(core::Name);
		property.Kind = PropertyKind::Field;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<AnimationTrack>()});
		property.Writes = property.Reads;

		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			const AnimationTrack *track = store.Get<AnimationTrack>(instance);
			if (track == nullptr) {
				return false;
			}
			*static_cast<core::Name *>(out) =
				ecs::EnumTable::MemberAt(AnimationPriorityEnum(), static_cast<size_t>(track->Priority));
			return true;
		};

		property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
			AnimationTrack *track = store.GetMutable<AnimationTrack>(instance);
			if (track == nullptr) {
				return false;
			}

			size_t ordinal = 0;
			if (!ecs::EnumTable::OrdinalOf(
					AnimationPriorityEnum(), *static_cast<const core::Name *>(value), ordinal
				)) {
				return false;
			}

			track->Priority = static_cast<AnimationPriority>(ordinal);
			return true;
		};
		return property;
	}

	// Whether an entity is usable as a constraint's anchor.
	//
	// **An `Attachment` and nothing else**, which is `DestinationProperty`'s
	// refusal one type along: a joint is anchored at a point with an
	// orientation, and pointing one at a `Folder` would be a constraint that
	// silently held nothing. Clearing is allowed, because an unattached second
	// end is how a door hangs off a wall that is not a body.
	bool UsableAsAnchor(const ecs::Store &store, ecs::Entity candidate) {
		return candidate == ecs::NULL_ENTITY || store.Get<Attachment>(candidate) != nullptr;
	}

	PropertyDescriptor Attachment0Property() {
		PropertyDescriptor property;
		property.Name = core::Name("Attachment0");
		property.Type = PropertyType::Reference;
		property.Size = sizeof(ecs::Entity);
		property.Kind = PropertyKind::Field;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Constraint>()});
		property.Writes = property.Reads;
		property.PrepareStoredReference =
			[](ecs::Store &store, ecs::Entity instance, ecs::Entity target) -> ecs::Entity * {
			if (!UsableAsAnchor(store, target)) return nullptr;
			Constraint *joint = store.GetMutable<Constraint>(instance);
			return joint == nullptr ? nullptr : &joint->Attachment0;
		};

		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			const Constraint *joint = store.Get<Constraint>(instance);
			if (joint == nullptr) {
				return false;
			}
			*static_cast<ecs::Entity *>(out) = joint->Attachment0;
			return true;
		};
		property.GetStoredReference = property.Get;

		property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
			const auto anchor = *static_cast<const ecs::Entity *>(value);
			if (!UsableAsAnchor(store, anchor)) {
				return false;
			}
			Constraint *joint = store.GetMutable<Constraint>(instance);
			if (joint == nullptr) {
				return false;
			}
			joint->Attachment0 = anchor;
			return true;
		};
		return property;
	}

	PropertyDescriptor Attachment1Property() {
		PropertyDescriptor property;
		property.Name = core::Name("Attachment1");
		property.Type = PropertyType::Reference;
		property.Size = sizeof(ecs::Entity);
		property.Kind = PropertyKind::Field;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Constraint>()});
		property.Writes = property.Reads;
		property.PrepareStoredReference =
			[](ecs::Store &store, ecs::Entity instance, ecs::Entity target) -> ecs::Entity * {
			if (!UsableAsAnchor(store, target)) return nullptr;
			Constraint *joint = store.GetMutable<Constraint>(instance);
			return joint == nullptr ? nullptr : &joint->Attachment1;
		};

		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			const Constraint *joint = store.Get<Constraint>(instance);
			if (joint == nullptr) {
				return false;
			}
			*static_cast<ecs::Entity *>(out) = joint->Attachment1;
			return true;
		};
		property.GetStoredReference = property.Get;

		property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
			const auto anchor = *static_cast<const ecs::Entity *>(value);
			if (!UsableAsAnchor(store, anchor)) {
				return false;
			}
			Constraint *joint = store.GetMutable<Constraint>(instance);
			if (joint == nullptr) {
				return false;
			}
			joint->Attachment1 = anchor;
			return true;
		};
		return property;
	}

	// --- a joint's limits, as one number each ---------------------------
	//
	// **Written to every axis and read from the first, which is what makes
	// one number describe both a slot and a sphere.** A prismatic joint is
	// limited on one linear axis and a rope is limited on all three, and a
	// `Locked` axis ignores its bounds entirely - so writing all three is
	// correct for both and lets an author say "this rope is four metres"
	// without knowing which axes the class chose.
	//
	// **Degrees on the way in and radians in the row**, which is
	// `Orientation`'s arrangement: the solver works in radians and Roblox's
	// constraint angles are degrees, so the conversion lives at the one place
	// a human types a number.

	PropertyDescriptor LinearLimitProperty(bool upper) {
		PropertyDescriptor property;
		property.Name = core::Name(upper ? "UpperLimit" : "LowerLimit");
		property.Type = PropertyType::Float;
		property.Size = sizeof(float);
		property.Kind = PropertyKind::Computed;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Constraint>()});
		property.Writes = property.Reads;

		if (upper) {
			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				const Constraint *joint = store.Get<Constraint>(instance);
				if (joint == nullptr) {
					return false;
				}
				*static_cast<float *>(out) = joint->LinearUpper[0];
				return true;
			};
			property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
				Constraint *joint = store.GetMutable<Constraint>(instance);
				if (joint == nullptr) {
					return false;
				}
				for (size_t axis = 0; axis < CONSTRAINT_AXES; axis++) {
					joint->LinearUpper[axis] = *static_cast<const float *>(value);
				}
				return true;
			};
		} else {
			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				const Constraint *joint = store.Get<Constraint>(instance);
				if (joint == nullptr) {
					return false;
				}
				*static_cast<float *>(out) = joint->LinearLower[0];
				return true;
			};
			property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
				Constraint *joint = store.GetMutable<Constraint>(instance);
				if (joint == nullptr) {
					return false;
				}
				for (size_t axis = 0; axis < CONSTRAINT_AXES; axis++) {
					joint->LinearLower[axis] = *static_cast<const float *>(value);
				}
				return true;
			};
		}
		return property;
	}

	PropertyDescriptor AngularLimitProperty(bool upper) {
		PropertyDescriptor property;
		property.Name = core::Name(upper ? "UpperAngle" : "LowerAngle");
		property.Type = PropertyType::Float;
		property.Size = sizeof(float);
		property.Kind = PropertyKind::Computed;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Constraint>()});
		property.Writes = property.Reads;

		if (upper) {
			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				const Constraint *joint = store.Get<Constraint>(instance);
				if (joint == nullptr) {
					return false;
				}
				*static_cast<float *>(out) = joint->AngularUpper[0] * DEGREES_PER_RADIAN;
				return true;
			};
			property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
				Constraint *joint = store.GetMutable<Constraint>(instance);
				if (joint == nullptr) {
					return false;
				}
				const float radians = *static_cast<const float *>(value) * RADIANS_PER_DEGREE;
				for (size_t axis = 0; axis < CONSTRAINT_AXES; axis++) {
					joint->AngularUpper[axis] = radians;
				}
				return true;
			};
		} else {
			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				const Constraint *joint = store.Get<Constraint>(instance);
				if (joint == nullptr) {
					return false;
				}
				*static_cast<float *>(out) = joint->AngularLower[0] * DEGREES_PER_RADIAN;
				return true;
			};
			property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
				Constraint *joint = store.GetMutable<Constraint>(instance);
				if (joint == nullptr) {
					return false;
				}
				const float radians = *static_cast<const float *>(value) * RADIANS_PER_DEGREE;
				for (size_t axis = 0; axis < CONSTRAINT_AXES; axis++) {
					joint->AngularLower[axis] = radians;
				}
				return true;
			};
		}
		return property;
	}

}
