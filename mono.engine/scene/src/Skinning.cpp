#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Skinning.hpp>

#include <algorithm>
#include <vector>

namespace engine::scene {

	namespace {
		// Whether two frames are the same value.
		//
		// **A file-local copy of `Attachments.cpp`'s, and deliberately not a
		// shared one.** The reason that function is not on `core::CFrame` is
		// that an equality on a rotation invites `q` and `-q` - the same
		// orientation, opposite quaternions - to compare unequal, and every
		// caller would inherit that surprise. Both places are comparing a
		// product against the previous run's product of the same inputs, so
		// bitwise is exactly the question; nowhere else should be able to reach
		// it.
		bool Same(const core::CFrame &left, const core::CFrame &right) {
			return left.Position == right.Position && left.QuaternionX == right.QuaternionX &&
				   left.QuaternionY == right.QuaternionY && left.QuaternionZ == right.QuaternionZ &&
				   left.QuaternionW == right.QuaternionW;
		}

		// Where a rig's roots hang from.
		//
		// A skeleton sits on a drawable, so its own placement is that drawable's
		// `Transform`. An entity carrying a `Skeleton` and no `Transform` is a
		// rig built out of bare instances, and the identity is the honest answer
		// for one: the same fallback `ResolveAttachments` makes for an attachment
		// under no part.
		core::CFrame RigFrame(const ecs::Store &store, ecs::Entity rig) {
			if (const Transform *placement = store.Get<Transform>(rig)) {
				return placement->Frame;
			}
			return core::CFrame{};
		}

		// One rig's bones, in palette order.
		struct Joint {
			ecs::Entity Instance;
			uint16_t Slot = 0;
			uint16_t Parent = NO_JOINT;
		};

		// Composes one rig and writes every `WorldFrame` that moved under it.
		//
		// The gather is a descendant walk and the compose is a forward pass over
		// the gathered order, which is what `Bone::ParentJoint` being lower than
		// `Bone::Joint` buys: no recursion, and every parent already written when
		// its child is reached.
		//
		// **A slot claimed twice keeps the first bone that claimed it.** A rig
		// cannot have two joint sevens, and taking the last one seen would make
		// the palette depend on the order the tree happened to be built in.
		size_t Compose(ecs::Store &store, ecs::Entity rig, std::vector<Joint> &joints) {
			joints.clear();

			store.EachDescendant(rig, [&](ecs::Entity descendant) {
				if (const Bone *bone = store.Get<Bone>(descendant)) {
					joints.push_back(Joint{descendant, bone->Joint, bone->ParentJoint});
				}
			});
			if (joints.empty()) {
				return 0;
			}

			// Stable, so two bones claiming one slot resolve in tree order rather
			// than in whatever order the sort felt like.
			std::stable_sort(joints.begin(), joints.end(), [](const Joint &left, const Joint &right) {
				return left.Slot < right.Slot;
			});

			const core::CFrame base = RigFrame(store, rig);

			// Resolved frames by slot, so a child reads its parent's answer with
			// no second search. Sized to the highest slot present rather than to
			// `MAX_JOINTS`, because a rig of four bones should cost four entries.
			std::vector<core::CFrame> resolved(static_cast<size_t>(joints.back().Slot) + 1, base);
			std::vector<bool> filled(resolved.size(), false);

			size_t written = 0;
			for (const Joint &joint : joints) {
				if (filled[joint.Slot]) {
					continue;
				}

				const Bone *bone = store.Get<Bone>(joint.Instance);
				if (bone == nullptr) {
					continue;
				}

				// A parent at or above this slot is the cycle `Bone::ParentJoint`
				// promises cannot happen; a slot nothing filled is a deleted bone.
				// Both fall back to the rig's own frame rather than to a
				// half-composed one, so a broken rig stands at its drawable
				// instead of collapsing to the origin.
				const bool linked =
					joint.Parent != NO_JOINT && joint.Parent < joint.Slot && filled[joint.Parent];
				const core::CFrame &parent = linked ? resolved[joint.Parent] : base;

				resolved[joint.Slot] = parent * bone->Rest * bone->Transform;
				filled[joint.Slot] = true;

				// **Only the rows that actually moved**, which is
				// `ResolveAttachments`' rule and is here for its reason: writing
				// every bone every frame advances the world's change counter for
				// ever, and the static broadphase and the gui compile are both
				// gated on an unchanged counter meaning nothing authored happened.
				if (Same(resolved[joint.Slot], bone->WorldFrame)) {
					continue;
				}
				if (Bone *row = store.GetMutable<Bone>(joint.Instance)) {
					row->WorldFrame = resolved[joint.Slot];
					written++;
				}
			}
			return written;
		}
	}

	size_t ResolveBones(ecs::Store &store) {
		// The rigs are gathered before any of them is composed, because
		// `Compose` walks descendants and reads rows outside the query's own
		// archetype. That is the shape `UpdateRespawns` uses for the same reason.
		std::vector<ecs::Entity> rigs;
		store.Each<const Skeleton>([&rigs](ecs::Entity entity, const Skeleton &) { rigs.push_back(entity); });

		std::vector<Joint> joints;
		size_t resolved = 0;
		for (const ecs::Entity rig : rigs) {
			resolved += Compose(store, rig, joints);
		}
		return resolved;
	}

	core::CFrame ResolveBone(const ecs::Store &store, ecs::Entity bone) {
		const Bone *row = store.Get<Bone>(bone);
		if (row == nullptr) {
			return core::CFrame{};
		}

		// Up through the instance parents rather than through the joint slots,
		// because a caller reaching this has not been through the pass and the
		// slot table only exists inside it. The two agree on a well-formed rig;
		// where they cannot, the tree is what a script has just built.
		core::CFrame frame = row->Rest * row->Transform;
		for (ecs::Entity walk = store.ParentOf(bone); walk != ecs::NULL_ENTITY; walk = store.ParentOf(walk)) {
			if (const Bone *parent = store.Get<Bone>(walk)) {
				frame = parent->Rest * parent->Transform * frame;
				continue;
			}
			if (store.Get<Skeleton>(walk) != nullptr) {
				return RigFrame(store, walk) * frame;
			}
		}
		return frame;
	}

	core::CFrame SkinningFrameOf(const Bone &bone) {
		return bone.WorldFrame * bone.InverseBind;
	}

	ecs::Entity SkeletonOf(const ecs::Store &store, ecs::Entity bone) {
		for (ecs::Entity walk = store.ParentOf(bone); walk != ecs::NULL_ENTITY; walk = store.ParentOf(walk)) {
			if (store.Get<Skeleton>(walk) != nullptr) {
				return walk;
			}
		}
		return ecs::NULL_ENTITY;
	}

	ecs::ClassId BoneClass() {
		// Through the one tree registration, for `AttachmentClass`'s reason:
		// whichever class a caller asks for first registers all of them.
		EnsureClassTree();
		return ecs::Classes::Find(core::Name("Bone"));
	}
}
