#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Property.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>

#include <array>
#include <numbers>

namespace engine::scene {

	namespace {
		// --- the property surface -------------------------------------------
		//
		// What a script sees, projected onto what the components hold. Roblox's
		// names, because v0.6 binds `Instance.new` to this table and a surface
		// that differs from the one scripts expect is a migration nobody asked
		// for — the argument `RegisterTree` already makes for the class tree.
		//
		// **Four of these are plain fields and the rest are conversions**, which
		// is why `PropertyDescriptor` stopped being a component and an offset at
		// v0.5. `Size` is a doubled half-extent, `Position` is part of a
		// `CFrame`, `Orientation` is a quaternion in degrees and `Anchored` is
		// not stored anywhere at all.

		using ecs::PropertyDescriptor;
		using ecs::PropertyKind;
		using ecs::PropertyType;

		constexpr float DEGREES_PER_RADIAN = 180.0f / std::numbers::pi_v<float>;
		constexpr float RADIANS_PER_DEGREE = std::numbers::pi_v<float> / 180.0f;

		// Position: the translation of `Transform`, with the rotation kept.
		//
		// The read-modify-write is the entire point. An offset-shaped setter
		// would write twelve bytes over the front of a `CFrame` and leave a
		// quaternion that no longer matches — which is exactly what a member
		// pointer cannot express and why this is a conversion.
		PropertyDescriptor PositionProperty() {
			PropertyDescriptor property;
			property.Name = core::Name("Position");
			property.Type = PropertyType::Vector3;
			property.Size = sizeof(core::Vector3);
			property.Kind = PropertyKind::Computed;
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Transform>()});
			property.Writes = property.Reads;

			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				const Transform *transform = store.Get<Transform>(instance);
				if (transform == nullptr) {
					return false;
				}
				*static_cast<core::Vector3 *>(out) = transform->Frame.Position;
				return true;
			};

			property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
				Transform *transform = store.GetMutable<Transform>(instance);
				if (transform == nullptr) {
					return false;
				}
				transform->Frame.Position = *static_cast<const core::Vector3 *>(value);
				return true;
			};

			return property;
		}

		// Orientation: the rotation of `Transform`, as intrinsic Y-X-Z turns.
		//
		// **Degrees, because Roblox's `.Orientation` is degrees** and this is
		// Roblox's surface — a script that works there should read the same
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
				Transform *transform = store.GetMutable<Transform>(instance);
				if (transform == nullptr) {
					return false;
				}
				const core::Vector3 degrees = *static_cast<const core::Vector3 *>(value);
				const core::CFrame rotation = core::CFrame::Angles(
					degrees.X * RADIANS_PER_DEGREE,
					degrees.Y * RADIANS_PER_DEGREE,
					degrees.Z * RADIANS_PER_DEGREE
				);

				const core::Vector3 kept = transform->Frame.Position;
				transform->Frame = rotation;
				transform->Frame.Position = kept;
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
		PropertyDescriptor SizeProperty() {
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
				// legal — so this is not a failure.
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
		// Roblox's `CanCollide = false` does — a part still fires `Touched`.
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

		// Parent: where the instance sits in the tree.
		//
		// A real property over the hierarchy `ecs` already keeps, not a
		// courtesy. **What it does not do is decide whether the part is
		// drawn** — `ecs/Instance.hpp` states the model: the tree is
		// organisational, exactly as Roblox's is, and parenting moves nothing
		// and re-resolves nothing. Drawing is decided by components, which is
		// what buys a world with no transform-hierarchy pass.
		//
		// That is a genuine divergence from Roblox, where an unparented part is
		// not rendered. It is written down here rather than left for somebody
		// to infer from a part that draws before it has a parent.
		PropertyDescriptor ParentProperty() {
			PropertyDescriptor property;
			property.Name = core::Name("Parent");
			property.Type = PropertyType::Reference;
			property.Size = sizeof(ecs::Entity);
			property.Kind = PropertyKind::Computed;
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<ecs::Hierarchy>()});
			property.Writes = property.Reads;

			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				*static_cast<ecs::Entity *>(out) = store.ParentOf(instance);
				return true;
			};

			property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
				return store.SetParent(instance, *static_cast<const ecs::Entity *>(value));
			};

			return property;
		}

		// Anchored: whether the world may move it — and the one property that
		// is not stored anywhere.
		//
		// `MakePart` says it in as many words: **anchored decides presence, not
		// a flag.** An anchored part carries neither `RigidBody` nor `Motion`,
		// so it sits in a different archetype and the dynamic queries never
		// visit it. Reading it is therefore a component test and writing it is
		// an archetype move — which is what `PropertyKind::Structural` exists to
		// announce, so a caller knows this one defers where the others do not.
		PropertyDescriptor AnchoredProperty() {
			PropertyDescriptor property;
			property.Name = core::Name("Anchored");
			property.Type = PropertyType::Bool;
			property.Size = sizeof(bool);
			property.Kind = PropertyKind::Structural;
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<RigidBody>()});
			property.Writes =
				&ecs::ComponentSet::Intern({ecs::Components::Of<RigidBody>(), ecs::Components::Of<Motion>()});

			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				*static_cast<bool *>(out) = store.Get<RigidBody>(instance) == nullptr;
				return true;
			};

			// Deferred by the store when this runs inside iteration, which is
			// the whole reason the kind is declared rather than inferred: a
			// structural change applied inline would move the row out from
			// under the loop walking it.
			property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
				if (*static_cast<const bool *>(value)) {
					store.Remove<RigidBody>(instance);
					store.Remove<Motion>(instance);
				} else {
					store.Set(instance, RigidBody{});
					store.Set(instance, Motion{});
				}
				return true;
			};

			return property;
		}

		// The class tree, built once for the process.
		//
		// A function-local static, so the tree exists before the first caller
		// reads an id from it and cannot be registered twice. Classes are
		// process-wide and never unregister, exactly as components are, so
		// there is nothing to tear down.
		ecs::ClassId RegisterTree() {
			RegisterSceneComponents();

			const ecs::ClassId instance = ecs::Classes::Register("Instance", {});

			// PVInstance is everything with a place in the world. Roblox's
			// split, kept because v0.6 binds `Instance.new` to this same table
			// and a tree that differs from the one scripts expect is a
			// migration nobody asked for.
			const std::array pv{ecs::Components::Of<Transform>()};
			const ecs::ClassId pvInstance = ecs::Classes::Register("PVInstance", instance, pv);

			const std::array base{
				ecs::Components::Of<Bounds>(),
				ecs::Components::Of<Visual>(),
				ecs::Components::Of<Collider>(),
				ecs::Components::Of<Surface>(),

				// **What is drawn is interpolated, so what is drawn carries the
				// component interpolation needs.** This used to be added by
				// whichever scene wanted it, which meant a part created from a
				// script — `Instance.new("Part")` and nothing else — was a
				// complete, correct part that the renderer silently skipped,
				// because `CollectInstances` matches on
				// `<Transform, PreviousTransform, Bounds, Visual>`.
				//
				// A class whose instances cannot be drawn without a component
				// the class does not have is a class with a footnote. This is
				// the footnote paid off.
				ecs::Components::Of<PreviousTransform>(),
			};
			const ecs::ClassId basePart = ecs::Classes::Register("BasePart", pvInstance, base);

			// Part adds nothing of its own: BasePart already holds the set
			// `v02v03v04.md` §3.3 names, and Part is the concrete leaf a script
			// asks for by name. `RigidBody` and `Motion` are deliberately
			// absent from every class here — whether a part has them is
			// `PartDesc::Anchored`'s decision, and putting them in the class
			// set would land static geometry in the dynamic archetype.
			const ecs::ClassId part = ecs::Classes::Register("Part", basePart, {});

			// --- properties, declared where the component arrives ------------
			//
			// Each on the class that first holds what it projects, so a derived
			// class inherits it and `Classes` merges base-first. Declaring them
			// all on `Part` would work today and would be wrong the moment a
			// second `BasePart` subclass exists.

			// Everything with a place in the world has these three, and all
			// three project one `Transform` — which is exactly the fan-out
			// v0.6's per-instance `.Changed` has to handle: one component write,
			// three property names observing it.
			// On `Instance`, because everything has a name and a place in the
			// tree.
			//
			// `Name` was special-cased in the Luau binding before this and
			// absent from the JavaScript one, which is exactly the drift a
			// declared property prevents: one declaration, both languages, and
			// it appears in the manifest like everything else. A Roblox script
			// sets `.Name`, so it has to be writable rather than readable.
			ecs::Classes::Property<&ecs::InstanceName::Value>(instance, "Name");
			ecs::Classes::Computed(instance, ParentProperty());

			ecs::Classes::Property<&Transform::Frame>(pvInstance, "CFrame");
			ecs::Classes::Computed(pvInstance, PositionProperty());
			ecs::Classes::Computed(pvInstance, OrientationProperty());

			ecs::Classes::Computed(basePart, SizeProperty());
			ecs::Classes::Computed(basePart, CanCollideProperty());
			ecs::Classes::Computed(basePart, AnchoredProperty());

			// The plain fields. `Color` and `Material` are renames rather than
			// conversions — `Visual::Tint` is what a script calls `Color`, and
			// `Visual::Material` is what it *looks* like. `Surface::Material`,
			// which is what it *feels* like, is deliberately not bound: the two
			// are separate facts that share a name, and `Visual::Material`'s own
			// comment gives the case — a mirror-finish floor and a rubber floor
			// may share a surface and never a material.
			ecs::Classes::Property<&Visual::Tint>(basePart, "Color");
			ecs::Classes::Property<&Visual::Visible>(basePart, "Visible");
			ecs::Classes::Property<&Visual::Mesh>(basePart, "Mesh");
			ecs::Classes::Property<&Visual::Material>(basePart, "Material");

			// Not declared, and each for a stated reason rather than an
			// oversight:
			//
			// - **`Transparency`** has no field to project onto. It becomes a
			//   renderer feature at v0.6 — `ROADMAP.md` carries it — because the
			//   component should move for rendering's sake and not for a
			//   binding's. A float nothing draws is a field that lies.
			// - **`CollisionGroup`** needs a name-to-layer registry that does
			//   not exist. `Collider::Layer` is 32 anonymous bits, and inventing
			//   a naming scheme here would be this module deciding a physics
			//   policy. Roblox has `PhysicsService` for it; that is the shape,
			//   and it wants its own design rather than a guess.
			return part;
		}
	}

	ecs::ClassId PartClass() {
		static const ecs::ClassId part = RegisterTree();
		return part;
	}

	ecs::Entity MakePart(ecs::Store &store, const PartDesc &desc) {
		// No adopt-only check here any more. `Store::CreateInstance` used to
		// walk straight past the flag while `Store::Create` honoured it, so this
		// module carried its own copy for the one minting path it owns; that
		// hole is closed in `ecs`, where every minting path is. A second check
		// here would be a second place to keep in step with the storage's rule.
		const ecs::Entity part = store.CreateInstance(PartClass());
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

		// **Anchored decides presence, not a flag.** An anchored part carries
		// neither of these, so it sits in a different archetype and the dynamic
		// queries never visit it — which beats testing a boolean per row per
		// tick and is the form the ECS is built for.
		if (!desc.Anchored) {
			store.Set(part, RigidBody{});
			store.Set(part, Motion{});
		}

		return part;
	}
}
