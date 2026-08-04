#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/EnumTable.hpp>
#include <engine/ecs/Property.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Enums.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/spatial/CollisionGroups.hpp>

#include <algorithm>
#include <array>
#include <numbers>
#include <string_view>

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

		// CollisionGroup: a name over `Collider::Layer`.
		//
		// **The bits stay anonymous in `scene` and the naming lives in
		// `spatial`**, which is where `LayerMask` is. This module holding the
		// table would be a component module deciding a physics policy for every
		// game that uses it — the reason this property was named as a gap at
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
				// even though `Layer` could hold several — `MakePart` sets
				// `Only(0)` — so this reports the group it was put in rather
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
					// see from the script that caused it.
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
			return property;
		}

		// Material: the name, checked against a registered set.
		//
		// The storage is `Visual::Material`, a `core::Name`, exactly as it was.
		// What `PropertyType::Enum` adds is that `part.Material = "Plsatic"` is
		// refused where it was written instead of landing in the component and
		// surfacing as a part drawn with the default for reasons nobody can see.
		PropertyDescriptor MaterialProperty() {
			PropertyDescriptor property;
			property.Name = core::Name("Material");
			property.Type = PropertyType::Enum;
			property.EnumName = core::Name("Material");
			property.Size = sizeof(core::Name);
			property.Kind = PropertyKind::Field;
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Visual>()});
			property.Writes = property.Reads;

			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				const Visual *visual = store.Get<Visual>(instance);
				if (visual == nullptr) {
					return false;
				}
				*static_cast<core::Name *>(out) = visual->Material;
				return true;
			};

			property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
				Visual *visual = store.GetMutable<Visual>(instance);
				if (visual == nullptr) {
					return false;
				}
				visual->Material = *static_cast<const core::Name *>(value);
				return true;
			};
			return property;
		}

		// FieldOfView: the camera's vertical angle, in degrees.
		//
		// Degrees out, radians stored — Roblox's `Camera.FieldOfView` is
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

		// Surface: which surface texture a part shows, as a script integer.
		//
		// **A conversion rather than a plain field, and the reason is a real
		// hazard.** `Visual::Surface` is an `int8_t` — one byte, because it fits
		// in padding the struct already had — and no `PropertyType` describes
		// one. Declaring it with `Classes::Property` would type it `Opaque` and
		// size it at one byte, and a binding marshalling an `Int32` into that
		// would write four bytes over three neighbouring fields.
		//
		// So the width changes here, once, where it can be seen. Neither Luau
		// nor JavaScript has an eight-bit integer to hand back anyway.
		PropertyDescriptor SurfaceProperty() {
			PropertyDescriptor property;
			property.Name = core::Name("Surface");
			property.Type = PropertyType::Int32;
			property.Size = sizeof(int32_t);
			property.Kind = PropertyKind::Computed;
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Visual>()});
			property.Writes = property.Reads;

			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				const Visual *visual = store.Get<Visual>(instance);
				if (visual == nullptr) {
					return false;
				}
				*static_cast<int32_t *>(out) = visual->Surface;
				return true;
			};

			property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
				const auto index = *static_cast<const int32_t *>(value);

				// Anything out of range is "no surface" rather than a refusal.
				// A script computing an index that came out wrong gets a part
				// that draws normally, which is visible; a refusal mid-loop
				// would stop the rest of the scene being built.
				Visual *visual = store.GetMutable<Visual>(instance);
				if (visual == nullptr) {
					return false;
				}

				visual->Surface = index >= 0 && index < 127 ? static_cast<int8_t>(index) : int8_t{-1};
				return true;
			};
			return property;
		}

		// SurfaceSize: the texture a surface camera renders into.
		//
		// **Structural, because the component's presence is the query.** A
		// camera with no `SurfaceCamera` is an ordinary camera and a consumer
		// walks past it; setting a non-zero size is what makes it one that
		// renders to a texture, and setting a zero size takes it back.
		//
		// A `Vector2` rather than two properties: a width without a height is
		// half a target, and two writes means a frame where the two disagree.
		// Which face of the parent part the surface camera projects off.
		//
		// An enum rather than a number, so `camera.Face = "Frnot"` is refused
		// where it was written instead of landing in the component as a face
		// nobody chose. Membership is `EnumTable`'s, and the storage is the
		// ordinal — Roblox's ordinal, so a game file carrying a number means the
		// same thing in both engines.
		// Interned once. `Name.hpp` states the rule this was breaking in as many
		// words — "not free, so do it once and keep the result rather than
		// constructing from a literal inside a loop" — and a property getter read
		// every frame by an immediate-mode properties panel is that loop. Each
		// construction took the global name-registry mutex and hashed a string,
		// on top of the `EnumTable` mutex the lookup already takes.
		const core::Name &NormalIdEnum() {
			static const core::Name name("NormalId");
			return name;
		}

		PropertyDescriptor FaceProperty() {
			PropertyDescriptor property;
			property.Name = core::Name("Face");
			property.Type = PropertyType::Enum;
			property.EnumName = NormalIdEnum();
			property.Size = sizeof(core::Name);
			property.Kind = PropertyKind::Field;
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<SurfaceCamera>()});
			property.Writes = property.Reads;

			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				const SurfaceCamera *surface = store.Get<SurfaceCamera>(instance);
				if (surface == nullptr) {
					return false;
				}
				*static_cast<core::Name *>(out) =
					ecs::EnumTable::MemberAt(NormalIdEnum(), static_cast<size_t>(surface->Face));
				return true;
			};

			property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
				SurfaceCamera *surface = store.GetMutable<SurfaceCamera>(instance);
				if (surface == nullptr) {
					return false;
				}

				size_t ordinal = 0;
				if (!ecs::EnumTable::OrdinalOf(
						NormalIdEnum(), *static_cast<const core::Name *>(value), ordinal
					)) {
					return false;
				}

				surface->Face = static_cast<NormalId>(ordinal);
				return true;
			};

			return property;
		}

		// Which surface index a camera renders into.
		//
		// **The camera's, and distinct from `BasePart::Surface` above even
		// though they hold the same number.** One says "this pane samples that
		// texture" and the other says "this camera writes it"; they agree
		// because `AimSurfaceCameras` copies the camera's onto the pane it is
		// parented to, and that copy is the whole reason a mirror is a camera
		// parented to a part and nothing else.
		//
		// Authored rather than handed out by the engine, because the number is
		// what pairs the two across a wire that carries no tree — and a derived
		// one would move whenever creation order did.
		PropertyDescriptor SurfaceIndexProperty() {
			PropertyDescriptor property;
			property.Name = core::Name("Surface");
			property.Type = PropertyType::Int32;
			property.Size = sizeof(int32_t);
			property.Kind = PropertyKind::Computed;
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<SurfaceCamera>()});
			property.Writes = property.Reads;

			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				const SurfaceCamera *target = store.Get<SurfaceCamera>(instance);
				if (target == nullptr) {
					return false;
				}
				*static_cast<int32_t *>(out) = target->Surface;
				return true;
			};

			property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
				const auto index = *static_cast<const int32_t *>(value);

				SurfaceCamera *target = store.GetMutable<SurfaceCamera>(instance);
				if (target == nullptr) {
					return false;
				}

				// **Clamped into the renderer's range rather than refused**, for
				// `BasePart::Surface`'s reason: a script computing an index that
				// came out wrong should get a mirror that visibly renders
				// nothing, not a scene that stopped building half way through.
				// The renderer says so in the log when it drops the view.
				target->Surface = index >= 0 && index < 127 ? static_cast<int8_t>(index) : int8_t{0};
				return true;
			};

			return property;
		}

		PropertyDescriptor SurfaceSizeProperty() {
			PropertyDescriptor property;
			property.Name = core::Name("SurfaceSize");
			property.Type = PropertyType::Vector3;
			property.Size = sizeof(core::Vector3);
			property.Kind = PropertyKind::Structural;
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<SurfaceCamera>()});
			property.Writes = property.Reads;

			// **A `Vector3` carrying two numbers**, because `PropertyType` has
			// no `Vector2` case and adding one is a decision about what userland
			// can hold rather than a detail this property gets to take. Z is
			// unused and reads back as zero, which is the honest shape — a
			// script that set it would find it ignored rather than silently
			// meaning something.
			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				const SurfaceCamera *surface = store.Get<SurfaceCamera>(instance);
				*static_cast<core::Vector3 *>(out) =
					surface == nullptr
						? core::Vector3::Zero
						: core::Vector3{
							  static_cast<float>(surface->Width), static_cast<float>(surface->Height), 0.0f
						  };
				return true;
			};

			property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
				const auto size = *static_cast<const core::Vector3 *>(value);

				if (size.X < 1.0f || size.Y < 1.0f) {
					store.Remove<SurfaceCamera>(instance);
					return true;
				}

				// Clamped rather than refused. A texture larger than any device
				// allows fails at creation with a driver message nobody can act
				// on; sixteen thousand is past every limit worth having and
				// small enough to allocate.
				SurfaceCamera surface;
				surface.Width = static_cast<uint16_t>(std::min(size.X, 16384.0f));
				surface.Height = static_cast<uint16_t>(std::min(size.Y, 16384.0f));
				store.Set(instance, surface);
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

			// The materials `Visual::Material` may name.
			//
			// **Roblox's set, and a subset of it**, which is the honest shape:
			// every name here is one a renderer could plausibly be asked to
			// draw, and adding a name the renderer ignores would be offering an
			// author completion for something that does nothing. A game
			// registers its own with `ecs::EnumTable::Register`, and the
			// registry takes a second declaration of an existing member as
			// agreement rather than conflict.
			static const std::string_view MATERIALS[] = {
				"Plastic",
				"SmoothPlastic",
				"Wood",
				"WoodPlanks",
				"Metal",
				"CorrodedMetal",
				"DiamondPlate",
				"Concrete",
				"Brick",
				"Cobblestone",
				"Grass",
				"Sand",
				"Slate",
				"Ice",
				"Glass",
				"Neon",
				"ForceField",
			};
			ecs::EnumTable::Register("Material", MATERIALS);

			// The faces of a box, for `SurfaceCamera::Face`.
			//
			// **Generated from the enum rather than typed out beside it**, which
			// is the difference between one declaration and two that agree until
			// they do not. `NormalId`'s ordinals are the storage — a `Face` of 1
			// is `Top` in a game file — so a literal list here would be a second
			// place the order lives, and getting it wrong would load every saved
			// mirror pointing at the wrong side of its pane. Silently: nothing
			// about a face is checkable at load time.
			//
			// The loop walks the enum's own range, so adding a seventh face means
			// adding it in one place. `Describe(NormalId)` is what makes it
			// possible, and `scene/Enums.hpp` says why that one round-trips where
			// its neighbours are only for logs.
			std::array<std::string_view, 6> normals{};
			for (size_t index = 0; index < normals.size(); index++) {
				normals[index] = Describe(static_cast<NormalId>(index));
			}
			ecs::EnumTable::Register(NormalIdEnum().Text(), normals);

			// The default collision group, so `CollisionGroup` reads back
			// something a script can compare rather than an invalid name on a
			// part nobody configured.
			spatial::CollisionGroups::Register(spatial::CollisionGroups::DEFAULT);

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

			// **A camera is an instance, because a camera is a row.**
			// `scene::Camera` has been a component since v0.4 precisely so a
			// world can hold several — a spectator, a cutscene, a security
			// monitor — and `ActiveCamera` names the live one. What was missing
			// was a class, so `Instance.new("Camera")` had nothing to resolve to
			// and a script could not make one, aim one, or ask which was live.
			//
			// Derives from `PVInstance` rather than from `BasePart`: a camera
			// has a place in the world and is not drawn, collided or bounded.
			const std::array camera{ecs::Components::Of<Camera>()};
			const ecs::ClassId cameraClass = ecs::Classes::Register("Camera", pvInstance, camera);

			// **A surface camera is a camera you parent to a part**, and that is
			// the whole of the class.
			//
			// It derives from `Camera` rather than standing beside it, because
			// it *is* one — it has a field of view, clip planes and a place in
			// the world, and `workspace.CurrentCamera` is a question anybody may
			// ask of it. What it adds is a texture to render into and a face to
			// project off.
			//
			// **The component is in the class set, so `Instance.new` makes a
			// working one.** On `Camera` the surface component is structural —
			// `SurfaceSize` adds and removes it — which is right there, because
			// an ordinary camera that acquired a render target by being asked
			// its size would be a surprise. Here it is what the class is for, so
			// a `SurfaceCamera` that had to be given a size before it became one
			// would be a class with a footnote. Same argument
			// `PreviousTransform` on `BasePart` settles.
			const std::array surface{ecs::Components::Of<SurfaceCamera>()};
			const ecs::ClassId surfaceCameraClass =
				ecs::Classes::Register("SurfaceCamera", cameraClass, surface);

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

			// **`Transparency` has a field to project onto now**, and it arrived
			// with the sorted pass that makes it mean something rather than
			// ahead of it. A float nothing draws is a field that lies, and it
			// would have sat in a snapshot and a delta being read by nobody.
			//
			// **Not clamped, deliberately.** It was, briefly. Roblox does not
			// clamp this either — `part.Transparency = 2` reads back as 2 — and
			// the reason to match is not fidelity for its own sake: a script that
			// drives a fade by arithmetic and reads the value back expects what
			// it wrote, and a property that silently rewrites its input is one an
			// author debugs by disbelieving their own assignment.
			//
			// The renderer is where the range has to hold, and that is a
			// different place from where it is authored. `SurfaceCamera::
			// ImageTransparency` below is still clamped because it is not
			// Roblox's property and has no such expectation to honour.
			ecs::Classes::Property<&Visual::Transparency>(basePart, "Transparency");

			// **The third of the three, and they are three questions rather
			// than one.** `Visible` decides whether the part is submitted at
			// all, `Transparency` decides which pass it lands in, and this
			// decides whether it occludes the sun. `Visual::CastShadow` carries
			// the whole argument for why collapsing any two of them is wrong.
			ecs::Classes::Property<&Visual::CastShadow>(basePart, "CastShadow");

			// Which surface texture this part shows, or -1 for none. An `int32`
			// rather than a reference to the camera: the renderer indexes a
			// small fixed set, and a handle would have to be resolved back to an
			// index every frame for every part.
			ecs::Classes::Computed(basePart, SurfaceProperty());

			// The two that were named as gaps at v0.5, both now closed with the
			// thing they were waiting for rather than with a guess.
			ecs::Classes::Computed(basePart, MaterialProperty());
			ecs::Classes::Computed(basePart, CollisionGroupProperty());

			// The camera's own three. `FieldOfView` is **degrees**, because
			// Roblox's is and because `Orientation` already reproduces that same
			// split — the component stores radians and the conversion is where
			// the unit changes, which is the whole shape of a property here.
			ecs::Classes::Computed(cameraClass, FieldOfViewProperty());
			ecs::Classes::Property<&Camera::NearPlane>(cameraClass, "NearPlaneZ");
			ecs::Classes::Property<&Camera::FarPlane>(cameraClass, "FarPlaneZ");
			ecs::Classes::Computed(cameraClass, SurfaceSizeProperty());

			// The surface camera's three. `SurfaceSize` above is inherited, so a
			// `SurfaceCamera` can still be resized like any other.
			ecs::Classes::ClampedProperty<&SurfaceCamera::ImageTransparency, 0.0f, 1.0f>(
				surfaceCameraClass, "ImageTransparency"
			);
			ecs::Classes::Computed(surfaceCameraClass, FaceProperty());
			ecs::Classes::Computed(surfaceCameraClass, SurfaceIndexProperty());

			// Still not declared, and for a reason rather than an oversight:
			// **`Surface::Material`**, which is what a part *feels* like.
			// `Visual::Material` above is what it looks like, and the two are
			// separate facts that share a name — a mirror-finish floor and a
			// rubber floor may share a surface and never a material. Binding
			// both under one script name would make one of them unreachable and
			// the other ambiguous.
			return part;
		}
	}

	ecs::ClassId PartClass() {
		static const ecs::ClassId part = RegisterTree();
		return part;
	}

	ecs::ClassId CameraClass() {
		// Through `PartClass` rather than through a second static, so the whole
		// tree is registered exactly once whichever class a caller asks for
		// first. Two function-local statics each calling `RegisterTree` would
		// have raced to register the same names, which `Classes::Register`
		// tolerates — and then the *order* of the two would decide which id each
		// class got, which is rule 4's hazard arriving inside one process.
		PartClass();
		return ecs::Classes::Find(core::Name("Camera"));
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
