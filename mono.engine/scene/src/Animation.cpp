#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Animation.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Skinning.hpp>

#include <algorithm>
#include <cmath>

namespace engine::scene {

	ecs::Entity RigFor(const ecs::Store &store, ecs::Entity animator) {
		const Animator *driver = store.Get<Animator>(animator);
		if (driver == nullptr) {
			return ecs::NULL_ENTITY;
		}

		// A named rig wins even when it is nowhere near the animator, because
		// naming one is the explicit case and the walk below is the convenience.
		if (driver->Rig != ecs::NULL_ENTITY && store.Get<Skeleton>(driver->Rig) != nullptr) {
			return driver->Rig;
		}

		for (ecs::Entity walk = store.ParentOf(animator); walk != ecs::NULL_ENTITY;
			 walk = store.ParentOf(walk)) {
			if (store.Get<Skeleton>(walk) != nullptr) {
				return walk;
			}
		}
		return ecs::NULL_ENTITY;
	}

	ecs::Entity AnimatorFor(const ecs::Store &store, ecs::Entity rig) {
		if (store.Get<Skeleton>(rig) == nullptr) {
			return ecs::NULL_ENTITY;
		}

		// **Two subtrees and never the whole world.** An animator either sits
		// under the rig it poses or beside it - Roblox's arrangement is an
		// `Animator` under a `Humanoid` under the character `Model`, with the
		// skinned mesh a sibling of that humanoid - so the rig's own subtree and
		// its parent's cover every rig anybody authors. A scan of every
		// `Animator` in the world would answer for the third case too and would
		// cost a walk of the world per call to do it; that case is what
		// `Animator::Rig` is for, and it is reached from the animator's side.
		const auto search = [&store, rig](ecs::Entity root) {
			ecs::Entity found = ecs::NULL_ENTITY;
			store.EachDescendant(root, [&](ecs::Entity descendant) {
				if (found == ecs::NULL_ENTITY && store.Get<Animator>(descendant) != nullptr &&
					RigFor(store, descendant) == rig) {
					found = descendant;
				}
			});
			return found;
		};

		if (const ecs::Entity under = search(rig); under != ecs::NULL_ENTITY) {
			return under;
		}

		const ecs::Entity parent = store.ParentOf(rig);
		return parent == ecs::NULL_ENTITY ? ecs::NULL_ENTITY : search(parent);
	}

	bool ClipFitsRig(const AnimationClip &clip, const Skeleton &skeleton) {
		// A clip that names no rig plays anywhere, which is the permissive case
		// and the one an author gets before anybody has said otherwise. Stated
		// once here rather than at each caller, because two statements of it
		// would disagree about exactly this branch.
		if (!clip.Rig.IsValid()) {
			return true;
		}
		return clip.Rig == skeleton.Rig;
	}

	size_t AdvanceAnimationTracks(ecs::Store &store) {
		const float delta = store.Time().Delta;
		size_t changed = 0;
		store.Each<const AnimationTrack>([&](ecs::Entity entity, const AnimationTrack &track) {
			AnimationTrack next = track;
			if (track.Playing && std::isfinite(track.Speed) && std::isfinite(track.TimePosition)) {
				const float advanced = track.TimePosition + delta * track.Speed;
				if (std::isfinite(advanced)) {
					next.TimePosition = advanced;
				}
			}

			const float target = std::clamp(track.WeightTarget, 0.0f, 1.0f);
			if (track.FadeTime <= 0.0f || !std::isfinite(track.FadeTime)) {
				next.Weight = target;
			} else {
				const float step = delta / track.FadeTime;
				if (track.Weight < target) {
					next.Weight = std::min(track.Weight + step, target);
				} else {
					next.Weight = std::max(track.Weight - step, target);
				}
			}
			next.Weight = std::clamp(next.Weight, 0.0f, 1.0f);

			if (next.TimePosition != track.TimePosition || next.Weight != track.Weight) {
				store.Set(entity, next);
				changed++;
			}
		});
		return changed;
	}

	ecs::ClassId AnimatorClass() {
		// Through the one tree registration, for `AttachmentClass`'s reason.
		EnsureClassTree();
		return ecs::Classes::Find(core::Name("Animator"));
	}
}
