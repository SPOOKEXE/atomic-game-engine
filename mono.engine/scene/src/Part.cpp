#include "PartInternal.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <numbers>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace engine::scene {

	core::CFrame PivotOf(const ecs::Store &store, ecs::Entity instance) {
		const Transform *transform = store.Get<Transform>(instance);
		if (transform == nullptr) {
			return {};
		}

		// **A missing `Pivot` is the identity rather than a refusal.** Every
		// `PVInstance` has the column, but a replica's row grows from a wire
		// delta and a headless world may hold an instance built by hand - and
		// "no offset" is a complete answer, not an error.
		const Pivot *pivot = store.Get<Pivot>(instance);
		return pivot == nullptr ? transform->Frame : transform->Frame * pivot->Offset;
	}

	bool PivotTo(ecs::Store &store, ecs::Entity instance, const core::CFrame &target) {
		const Pivot *pivot = store.Get<Pivot>(instance);
		const core::CFrame placement = pivot == nullptr ? target : target * pivot->Offset.Inverse();

		// Through the same helper a property write uses, so a script pivoting a
		// part and an author dragging one leave the world in the same state.
		return part_detail::PlaceInstance(store, instance, placement);
	}

	size_t BulkMoveTo(
		ecs::Store &store, std::span<const ecs::Entity> instances, std::span<const core::CFrame> frames
	) {
		const size_t count = std::min(instances.size(), frames.size());

		size_t moved = 0;
		for (size_t at = 0; at < count; at++) {
			// **The same helper a single write uses, deliberately.** The header
			// argues the case: a batch is a cheaper *boundary*, not a cheaper
			// write, and the moment the two paths differ they can disagree
			// about what a placement leaves behind.
			if (part_detail::PlaceInstance(store, instances[at], frames[at])) {
				moved++;
			}
		}
		return moved;
	}

	size_t BulkPivotTo(
		ecs::Store &store, std::span<const ecs::Entity> instances, std::span<const core::CFrame> targets
	) {
		const size_t count = std::min(instances.size(), targets.size());

		size_t moved = 0;
		for (size_t at = 0; at < count; at++) {
			if (PivotTo(store, instances[at], targets[at])) {
				moved++;
			}
		}
		return moved;
	}

	float LocalTransparencyOf(const ecs::Store &store, ecs::Entity instance) {
		const LocalTransparency *local = store.Get<LocalTransparency>(instance);
		return local == nullptr ? 0.0f : local->Value;
	}

	bool SetLocalTransparency(ecs::Store &store, ecs::Entity instance, float value) {
		LocalTransparency *local = store.GetMutable<LocalTransparency>(instance);
		if (local == nullptr) {
			return false;
		}
		local->Value = value;
		return true;
	}

	float VolumeOf(const Collider &collider) {
		// Half-extents, as everywhere else in this module - `InverseInertiaOf`
		// reads the same field the same way.
		const float x = std::max(collider.Extent.X, 0.0f);
		const float y = std::max(collider.Extent.Y, 0.0f);
		const float z = std::max(collider.Extent.Z, 0.0f);

		switch (collider.Shape) {
		case ShapeKind::Box:
			return 8.0f * x * y * z;
		case ShapeKind::Sphere:
			return (4.0f / 3.0f) * std::numbers::pi_v<float> * x * x * x;
		case ShapeKind::Cylinder:
			// Radius from X, half-height from Y - the axes `InverseInertiaOf`
			// puts the barrel along.
			return std::numbers::pi_v<float> * x * x * (2.0f * y);
		case ShapeKind::Capsule:
			// A cylinder whose half-height is Y plus the two hemispheres that
			// together make one sphere.
			return std::numbers::pi_v<float> * x * x * (2.0f * y) +
				   (4.0f / 3.0f) * std::numbers::pi_v<float> * x * x * x;

		case ShapeKind::Hull:
		case ShapeKind::Mesh:
			// **The part's bound, not the baked geometry's volume**, and the
			// difference is deliberate. This module has no edge to `collision`
			// and should not grow one to weigh a crate: a density times a volume
			// is what a *part* weighs, `scene::Bounds` is the extent a part is
			// drawn at, and a hull that is a good fit for its part differs from
			// that box by a factor a designer will not notice and cannot be
			// surprised by. A hull that is *not* a good fit for its part is a
			// scene mistake this would only hide.
			//
			// Read as half-extents, exactly as the box case above.
			return 8.0f * x * y * z;
		}
		return 0.0f;
	}

	float MassOf(const Collider &collider, const RigidBody &body, const PhysicsProperties *properties) {
		if (properties == nullptr || !properties->Custom) {
			return std::max(body.Mass, 0.0f);
		}

		// **A zero-volume shape keeps the authored mass rather than becoming
		// weightless.** A part with no extent is a scene being built rather than
		// a thing with no substance, and a mass of zero is what the solver reads
		// as immovable - the opposite of what a density of nearly nothing means.
		const float volume = VolumeOf(collider);
		if (!(volume > 0.0f) || !(properties->Density > 0.0f)) {
			return std::max(body.Mass, 0.0f);
		}
		return properties->Density * volume;
	}

	ecs::Entity MakePart(ecs::Store &store, const PartDesc &desc) {
		// No adopt-only check here any more. `Store::CreateInstance` used to
		// walk straight past the flag while `Store::Create` honoured it, so this
		// module carried its own copy for the one minting path it owns; that
		// hole is closed in `ecs`, where every minting path is. A second check
		// here would be a second place to keep in step with the storage's rule.
		// **The class is a parameter now, and `MakePart` is still the only
		// constructor.** A `SpawnLocation` is a `Part` plus one component, so
		// the alternative was a second builder that assembled the five
		// components itself - the duplicate `scene/AGENTS.md` refuses, and the
		// one that disagrees the first time `BasePart` gains a member.
		//
		// Anything that is not a `BasePart` is refused rather than half-built:
		// the writes below add `Bounds`, `Visual`, `Collider` and `Surface`, so
		// minting a `Model` here would produce a model with a part's components
		// bolted on and no class saying so.
		const ecs::ClassId plain = PartClass();
		const ecs::ClassId klass = desc.Class.IsValid() ? desc.Class : plain;
		if (!ecs::Classes::IsA(klass, ecs::Classes::Find(core::Name("BasePart")))) {
			return ecs::NULL_ENTITY;
		}

		const ecs::Entity part = store.CreateInstance(klass);
		if (part == ecs::NULL_ENTITY) {
			return part;
		}

		// Half, once, here. `Size` is the full extent because that is what a
		// person types and what the Roblox property is called; every consumer
		// downstream wants the half, so halving in two places is where the two
		// eventually disagree by a factor of two.
		//
		// The same value serves all three shapes: a box reads all of it, a
		// sphere reads X as its radius, a cylinder reads X and Y. See
		// `Collider::Extent`.
		const core::Vector3 halfExtent = desc.Size * 0.5f;

		store.Set(part, Transform{desc.Frame});
		store.Set(part, Bounds{halfExtent});
		store.Set(part, Surface{desc.Material});

		// Read-modify-write rather than a fresh value, for the two components
		// `PartDesc` only partly describes. A class prototype is where defaults
		// live, and constructing a `Collider{}` here would silently overwrite
		// a layer mask or a tint that the class had declared.
		Collider collider;
		if (const Collider *prototype = store.Get<Collider>(part)) {
			collider = *prototype;
		}
		collider.Extent = halfExtent;
		collider.Shape = desc.Shape;
		store.Set(part, collider);

		Visual visual;
		if (const Visual *prototype = store.Get<Visual>(part)) {
			visual = *prototype;
		}
		visual.Mesh = desc.Mesh;
		store.Set(part, visual);

		// **The decision is presence, not a flag.** A simulated part carries the
		// tag and a `Motion`, so it sits in a different archetype and the
		// dynamic queries name the tag rather than branching per row per tick -
		// which is the form the ECS is built for.
		//
		// Neither arrives with the class set, so this only has to put them on.
		// It is the whole of what `PartDesc::Simulated` does, and a static part
		// leaves this function having stored nothing to say so.
		//
		// `RigidBody` is on both and arrives from the class set: it is what the
		// part weighs and how it sheds speed, which a static part still has.
		// See `scene::Simulated`.
		if (desc.Simulated) {
			store.Set(part, Simulated{});
			store.Set(part, Motion{});
		}

		return part;
	}
}
