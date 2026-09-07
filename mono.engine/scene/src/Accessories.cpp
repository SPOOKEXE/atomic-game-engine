#include <engine/core/Profiling.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Accessories.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>

#include <cmath>
#include <cstring>
#include <vector>

namespace engine::scene {
	namespace {
		using ecs::Entity;
		using ecs::NULL_ENTITY;
		bool Valid(const core::CFrame &frame) {
			const auto q = frame.Rotation();
			const float norm = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
			return std::isfinite(frame.Position.X) && std::isfinite(frame.Position.Y) &&
				   std::isfinite(frame.Position.Z) && std::isfinite(norm) && std::abs(norm - 1) < .001f;
		}
		Entity Handle(const ecs::Store &store, Entity accessory) {
			const Entity handle = store.FindFirstChild(accessory, "Handle");
			return store.IsA(handle, ecs::Classes::Find(core::Name("BasePart"))) ? handle : NULL_ENTITY;
		}
		bool TargetOffset(const ecs::Store &store, Entity character, Entity point, core::CFrame &offset) {
			const auto *rig = store.Get<Character>(character);
			const auto *attachment = store.Get<Attachment>(point);
			if (!rig || !attachment || !Valid(attachment->Frame) || !store.Has<Transform>(rig->Root))
				return false;
			const Entity parent = store.ParentOf(point);
			if (parent == rig->Root) {
				offset = attachment->Frame;
				return true;
			}
			const auto *limb = store.Get<CharacterLimb>(parent);
			if (store.ParentOf(parent) != character || !limb || limb->Root != rig->Root ||
				!Valid(limb->Offset))
				return false;
			offset = limb->Offset * attachment->Frame;
			return true;
		}
		bool Bound(
			const ecs::Store &store,
			Entity character,
			Entity accessory,
			const Accessory &points,
			CharacterLimb &limb
		) {
			const Entity handle = Handle(store, accessory);
			const auto *point = store.Get<Attachment>(points.HandleAttachment);
			core::CFrame target;
			if (handle == NULL_ENTITY || store.ParentOf(points.HandleAttachment) != handle || !point ||
				!Valid(point->Frame) || !TargetOffset(store, character, points.CharacterAttachment, target) ||
				store.InstanceNameOf(points.HandleAttachment) !=
					store.InstanceNameOf(points.CharacterAttachment))
				return false;
			limb = CharacterLimb{store.Get<Character>(character)->Root, target * point->Frame.Inverse(), 0};
			return Valid(limb.Offset);
		}
		bool Match(
			const ecs::Store &store,
			Entity character,
			Entity accessory,
			Accessory &points,
			CharacterLimb &limb
		) {
			const Entity handle = Handle(store, accessory);
			if (handle == NULL_ENTITY || !store.Get<Character>(character)) return false;
			size_t matches = 0;
			store.EachChild(handle, [&](Entity point) {
				if (!store.Has<Attachment>(point)) return;
				store.EachChild(character, [&](Entity part) {
					if (!store.Has<Transform>(part) || store.Has<Accessory>(part)) return;
					store.EachChild(part, [&](Entity target) {
						if (store.InstanceNameOf(point) != store.InstanceNameOf(target)) return;
						CharacterLimb candidate;
						const Accessory pair{point, target};
						if (Bound(store, character, accessory, pair, candidate)) {
							++matches;
							points = pair;
							limb = candidate;
						}
					});
				});
			});
			return matches == 1;
		}
		void Carry(ecs::Store &store, Entity accessory, const Accessory &points, const CharacterLimb &limb) {
			const Entity handle = Handle(store, accessory);
			const auto *old = store.Get<Accessory>(accessory);
			if (!old || old->HandleAttachment != points.HandleAttachment ||
				old->CharacterAttachment != points.CharacterAttachment)
				store.Set(accessory, points);
			const auto *carried = store.Get<CharacterLimb>(handle);
			if (!carried || std::memcmp(carried, &limb, sizeof(limb)) != 0) store.Set(handle, limb);
			if (store.Has<Motion>(handle)) store.Remove<Motion>(handle);
		}
		void Release(ecs::Store &store, Entity accessory, const Accessory &points) {
			// A clone retains source refs until rebound. Never release a handle
			// owned by another accessory; moved or renamed owned handles still detach.
			Entity handle = store.ParentOf(points.HandleAttachment);
			if (!store.Has<Transform>(handle) ||
				(store.ParentOf(handle) != accessory && store.Has<Accessory>(store.ParentOf(handle))))
				handle = Handle(store, accessory);
			if (handle != NULL_ENTITY && store.Has<CharacterLimb>(handle)) {
				store.Remove<CharacterLimb>(handle);
				if (store.Has<Simulated>(handle) && !store.Has<Motion>(handle)) store.Set(handle, Motion{});
			}
			if (points.HandleAttachment != NULL_ENTITY || points.CharacterAttachment != NULL_ENTITY)
				store.Set(accessory, Accessory{});
		}
	}

	ecs::ClassId AccessoryClass() {
		static const ecs::ClassId value = (EnsureClassTree(), ecs::Classes::Find(core::Name("Accessory")));
		return value;
	}

	bool EquipAccessory(ecs::Store &store, ecs::Entity character, ecs::Entity accessory) {
		ENGINE_PROFILE("equip accessory");
		if (store.AdoptOnly() || !store.IsA(accessory, AccessoryClass()) || !store.Get<Character>(character))
			return false;
		Accessory points;
		CharacterLimb limb;
		if (!Match(store, character, accessory, points, limb) || !store.SetParent(accessory, character))
			return false;
		Carry(store, accessory, points, limb);
		return true;
	}

	size_t UpdateAccessoryAttachments(ecs::Store &store) {
		ENGINE_PROFILE("update accessory attachments");
		std::vector<Entity> accessories;
		store.Each<const Accessory>([&](Entity entity, const Accessory &) { accessories.push_back(entity); });
		size_t equipped = 0;
		for (const Entity entity : accessories) {
			const Accessory points = *store.Get<Accessory>(entity);
			const Entity character = store.ParentOf(entity);
			CharacterLimb limb;
			Accessory matched = points;
			const bool bound = Bound(store, character, entity, points, limb);
			// Established references stay exact. A destroyed target must not attach
			// to a coincident replacement in the same update.
			const bool fresh =
				points.HandleAttachment == NULL_ENTITY && points.CharacterAttachment == NULL_ENTITY;
			if (bound || (fresh && Match(store, character, entity, matched, limb))) {
				Carry(store, entity, matched, limb);
				++equipped;
			} else
				Release(store, entity, points);
		}
		return equipped;
	}
}
