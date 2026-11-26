#include <engine/core/Profiling.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/characters/Animation.hpp>
#include <engine/scene/characters/Skinning.hpp>

#include <algorithm>
#include <string>
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

		// Gathers a rig's bones in palette order.
		//
		// Stable, so two bones claiming one slot resolve in tree order rather
		// than in whatever order the sort felt like.
		void Gather(const ecs::Store &store, ecs::Entity rig, std::vector<Joint> &joints) {
			joints.clear();
			{
				ENGINE_PROFILE("scene skinning joint gather");
				store.EachDescendant(rig, [&](ecs::Entity descendant) {
					if (const Bone *bone = store.Get<Bone>(descendant)) {
						joints.push_back(Joint{descendant, bone->Joint, bone->ParentJoint});
					}
				});
			}
			ENGINE_PROFILE("scene skinning slot sort");
			std::stable_sort(joints.begin(), joints.end(), [](const Joint &left, const Joint &right) {
				return left.Slot < right.Slot;
			});
		}

		// One rig's working state for a pass: its joints in palette order and
		// what composing them gave.
		struct RigState {
			ecs::Entity Rig;
			std::vector<Joint> Joints;
			// Resolved frames by slot, so a child reads its parent's answer
			// with no second search.
			std::vector<core::CFrame> Resolved;
			std::vector<uint8_t> Filled;
			// Per joint (in `Joints` order): whether its row needs writing.
			std::vector<uint8_t> Moved;
			// A bone renumbered or removed since the gather.
			bool Stale = false;
		};

		// What `ResolveBones` keeps between passes over one store: the rigs and
		// their gathered joints, valid while the store's identity and layout
		// revision repeat (no bone or rig added, removed or reparented). A joint
		// renumbered in place (`SetBones`) is a value write the layout revision
		// misses, so `Compose` checks each bone's numbers as it reads it and a
		// mismatch regathers that rig.
		struct RigCache {
			uint64_t Identity = 0;
			uint64_t Layout = 0;
			bool Valid = false;
			std::vector<RigState> Rigs;
		};

		// Composes one rig into `state`, reading the store only, so rigs can
		// compose concurrently. The compose is a forward pass over the gathered
		// order, which is what `Bone::ParentJoint` being lower than
		// `Bone::Joint` buys: no recursion, and every parent already composed
		// when its child is reached. With `preview`, the previewed `Poses`
		// stand in for the bones' transforms.
		//
		// **A slot claimed twice keeps the first bone that claimed it.** A rig
		// cannot have two joint sevens, and taking the last one seen would make
		// the palette depend on the order the tree happened to be built in.
		void Compose(const ecs::Store &store, RigState &state, const AnimationPreview *preview = nullptr) {
			ENGINE_PROFILE("scene skinning rig");
			const std::vector<Joint> &joints = state.Joints;
			state.Stale = false;
			state.Moved.assign(joints.size(), 0);
			if (joints.empty()) return;
			const core::CFrame base = RigFrame(store, state.Rig);
			ENGINE_PROFILE("scene skinning pose compose");
			// Reset every used slot; the vectors keep their capacity between passes.
			state.Resolved.assign(static_cast<size_t>(joints.back().Slot) + 1, base);
			state.Filled.assign(state.Resolved.size(), 0);
			for (size_t index = 0; index < joints.size(); ++index) {
				const Joint &joint = joints[index];
				if (state.Filled[joint.Slot]) continue;
				const Bone *bone = store.Get<Bone>(joint.Instance);
				if (bone == nullptr || bone->Joint != joint.Slot || bone->ParentJoint != joint.Parent) {
					state.Stale = true;
					return;
				}
				// A parent at or above this slot is the cycle `Bone::ParentJoint`
				// promises cannot happen; a slot nothing filled is a deleted bone.
				// Both fall back to the rig's own frame rather than to a
				// half-composed one, so a broken rig stands at its drawable
				// instead of collapsing to the origin.
				const bool linked =
					joint.Parent != NO_JOINT && joint.Parent < joint.Slot && state.Filled[joint.Parent];
				const core::CFrame &parent = linked ? state.Resolved[joint.Parent] : base;
				const core::CFrame *transform = &bone->Transform;
				if (preview != nullptr) {
					const auto posed = preview->Poses.find(joint.Instance);
					if (posed != preview->Poses.end()) transform = &posed->second;
				}
				state.Resolved[joint.Slot] = parent * bone->Rest * *transform;
				state.Filled[joint.Slot] = 1;
				// **Only the rows that actually moved**, which is
				// `ResolveAttachments`' rule and is here for its reason: writing
				// every bone every frame advances the world's change counter for
				// ever, and the static broadphase and the gui compile are both
				// gated on an unchanged counter meaning nothing authored happened.
				state.Moved[index] = !Same(state.Resolved[joint.Slot], bone->WorldFrame);
			}
		}

		// Writes the moved rows of a composed rig on the owner thread.
		size_t Write(ecs::Store &store, const RigState &state) {
			size_t written = 0;
			for (size_t index = 0; index < state.Joints.size(); ++index) {
				if (!state.Moved[index]) continue;
				const Joint &joint = state.Joints[index];
				if (Bone *row = store.GetMutable<Bone>(joint.Instance)) {
					row->WorldFrame = state.Resolved[joint.Slot];
					written++;
				}
			}
			return written;
		}
	}

	size_t ResolveBones(ecs::Store &store) {
		ENGINE_PROFILE("scene resolve bones");
		// One cache per thread: a store is ticked by one thread at a time, and
		// a thread switching between stores just regathers.
		thread_local RigCache cache;
		if (!cache.Valid || cache.Identity != store.Identity() || cache.Layout != store.LayoutRevision()) {
			// The rigs are gathered before any of them is composed, because
			// `Gather` walks descendants and reads rows outside the query's own
			// archetype. That is the shape `UpdateRespawns` uses for the same reason.
			ENGINE_PROFILE("scene skinning rig gather");
			std::vector<ecs::Entity> rigs;
			store.Each<const Skeleton>([&rigs](ecs::Entity entity, const Skeleton &) { rigs.push_back(entity); });
			cache.Rigs.resize(rigs.size());
			for (size_t i = 0; i < rigs.size(); ++i) {
				cache.Rigs[i].Rig = rigs[i];
				Gather(store, rigs[i], cache.Rigs[i].Joints);
			}
			cache.Identity = store.Identity();
			cache.Layout = store.LayoutRevision();
			cache.Valid = true;
		}

		// Compose every rig concurrently: the store is read-only until the
		// fork-join finishes, and each rig writes only its own state.
		constexpr size_t RIGS_PER_RANGE = 8;
		constexpr size_t PARALLEL_MINIMUM = 32;
		// The rigs are captured by reference: `cache` is thread-local, so a
		// worker naming it would see its own empty one.
		const ecs::Store &reader = store;
		std::vector<RigState> &states = cache.Rigs;
		parallel::Jobs::For(
			states.size(),
			RIGS_PER_RANGE,
			[&reader, &states](size_t begin, size_t end) {
				for (size_t i = begin; i < end; ++i) Compose(reader, states[i]);
			},
			PARALLEL_MINIMUM
		);

		size_t resolved = 0;
		// A rig the animation preview posed composes once as authored and once
		// as previewed, so its rows stay authored and the palette sees the preview.
		AnimationPreview *preview = store.ResourceMutable<AnimationPreview>();
		for (RigState &state : cache.Rigs) {
			if (state.Stale) {
				Gather(store, state.Rig, state.Joints);
				Compose(store, state);
			}
			resolved += Write(store, state);
			if (preview == nullptr || preview->Poses.empty()) continue;
			bool posed = false;
			for (const Joint &joint : state.Joints)
				posed = posed || preview->Poses.contains(joint.Instance);
			if (!posed) continue;
			RigState previewed{state.Rig, state.Joints, {}, {}, {}, false};
			Compose(store, previewed, preview);
			for (const Joint &joint : previewed.Joints)
				if (previewed.Filled.size() > joint.Slot && previewed.Filled[joint.Slot])
					preview->WorldFrames.insert_or_assign(joint.Instance, previewed.Resolved[joint.Slot]);
		}
		// Writing `WorldFrame` is a value write, so the layout revision the
		// cache keys on is unchanged by this pass.
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

	std::string BindSkeletonBones(ecs::Store &store, ecs::Entity rig, std::span<const ecs::Entity> bones) {
		if (store.Get<Skeleton>(rig) == nullptr) return "the rig has no Skeleton";
		if (bones.size() > MAX_JOINTS) return "a rig holds at most 256 bones";
		size_t under = 0;
		store.EachDescendant(rig, [&](ecs::Entity descendant) {
			if (store.Get<Bone>(descendant) != nullptr) under++;
		});
		if (under != bones.size()) return "the list must name every Bone under the rig exactly once";

		std::vector<uint16_t> parents(bones.size(), NO_JOINT);
		for (size_t slot = 0; slot < bones.size(); slot++) {
			const ecs::Entity bone = bones[slot];
			if (store.Get<Bone>(bone) == nullptr || SkeletonOf(store, bone) != rig)
				return "entry " + std::to_string(slot + 1) + " is not a Bone under this rig";
			for (size_t earlier = 0; earlier < slot; earlier++)
				if (bones[earlier] == bone) return "entry " + std::to_string(slot + 1) + " is listed twice";
			for (ecs::Entity walk = store.ParentOf(bone); walk != rig && walk != ecs::NULL_ENTITY;
				 walk = store.ParentOf(walk)) {
				if (store.Get<Bone>(walk) == nullptr) continue;
				const auto found = std::find(bones.begin(), bones.end(), walk);
				const size_t parent = static_cast<size_t>(found - bones.begin());
				if (parent >= slot)
					return "entry " + std::to_string(slot + 1) + " is listed before its parent bone";
				parents[slot] = static_cast<uint16_t>(parent);
				break;
			}
		}

		for (size_t slot = 0; slot < bones.size(); slot++) {
			Bone *bone = store.GetMutable<Bone>(bones[slot]);
			bone->Joint = static_cast<uint16_t>(slot);
			bone->ParentJoint = parents[slot];
		}
		store.GetMutable<Skeleton>(rig)->JointCount = static_cast<uint16_t>(bones.size());
		return {};
	}

	ecs::ClassId BoneClass() {
		// Through the one tree registration, for `AttachmentClass`'s reason:
		// whichever class a caller asks for first registers all of them.
		EnsureClassTree();
		return ecs::Classes::Find(core::Name("Bone"));
	}

	ecs::ClassId RigKeypointClass() {
		EnsureClassTree();
		return ecs::Classes::Find(core::Name("RigKeypoint"));
	}
}
