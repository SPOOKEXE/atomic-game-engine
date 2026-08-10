#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/EnumTable.hpp>
#include <engine/ecs/Property.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Attachments.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/scene/Enums.hpp>
#include <engine/scene/Input.hpp>
#include <engine/scene/MeshCatalogue.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Tagging.hpp>
#include <engine/spatial/CollisionGroups.hpp>

#include <algorithm>
#include <array>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

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
		// Writes a placement.
		//
		// **`PreviousTransform` is deliberately *not* written here, and that is
		// a decision this function exists to record.** The obvious reading is
		// that an authored write is a teleport and should move the frame it is
		// interpolated from — and it was written that way first. It is wrong:
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
		// belongs.** A world nothing ticks never runs `capture-previous` —
		// that is a `PreSimulation` system and `World::Present` runs
		// `PreRender` alone — so its `PreviousTransform` is wherever each part
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
		void PlaceInstance(ecs::Store &store, ecs::Entity instance, const core::CFrame &frame) {
			if (Transform *transform = store.GetMutable<Transform>(instance)) {
				transform->Frame = frame;
			}
		}

		// CFrame: the placement itself.
		//
		// A `Computed` over a field a member pointer could reach, which is the
		// one case in this file where that is not a smell — see `PlaceInstance`.
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
				if (store.Get<Transform>(instance) == nullptr) {
					return false;
				}
				PlaceInstance(store, instance, *static_cast<const core::CFrame *>(value));
				return true;
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
				const Transform *transform = store.Get<Transform>(instance);
				if (transform == nullptr) {
					return false;
				}
				const core::Vector3 degrees = *static_cast<const core::Vector3 *>(value);
				core::CFrame frame = core::CFrame::Angles(
					degrees.X * RADIANS_PER_DEGREE,
					degrees.Y * RADIANS_PER_DEGREE,
					degrees.Z * RADIANS_PER_DEGREE
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

		// The surface effect's enum name, interned for `NormalIdEnum`'s reason.
		const core::Name &SurfaceEffectEnum() {
			static const core::Name name("SurfaceEffect");
			return name;
		}

		// What a surface camera's image is put through, as an enum member.
		//
		// The same shape as `FaceProperty` below and deliberately not shared
		// with it: both read a `SurfaceCamera`, but a descriptor is a pair of
		// function pointers over one field, and a template over the field would
		// save nine lines and cost the reader the ability to see what the
		// property touches.
		PropertyDescriptor SurfaceEffectProperty() {
			PropertyDescriptor property;
			property.Name = core::Name("Effect");
			property.Type = PropertyType::Enum;
			property.EnumName = SurfaceEffectEnum();
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
					ecs::EnumTable::MemberAt(SurfaceEffectEnum(), static_cast<size_t>(surface->Effect));
				return true;
			};

			property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
				SurfaceCamera *surface = store.GetMutable<SurfaceCamera>(instance);
				if (surface == nullptr) {
					return false;
				}

				size_t ordinal = 0;
				if (!ecs::EnumTable::OrdinalOf(
						SurfaceEffectEnum(), *static_cast<const core::Name *>(value), ordinal
					)) {
					return false;
				}

				surface->Effect = static_cast<SurfaceEffect>(ordinal);
				return true;
			};

			return property;
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

		// How a surface's texture alpha is treated, as an enum member.
		//
		// Interned once, for `NormalIdEnum`'s reason: a properties panel reads
		// this every frame and constructing from a literal takes the global
		// name-registry mutex each time.
		const core::Name &AlphaModeEnum() {
			static const core::Name name("AlphaMode");
			return name;
		}

		PropertyDescriptor AlphaModeProperty() {
			PropertyDescriptor property;
			property.Name = core::Name("AlphaMode");
			property.Type = PropertyType::Enum;
			property.EnumName = AlphaModeEnum();
			property.Size = sizeof(core::Name);
			property.Kind = PropertyKind::Field;
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<SurfaceAppearance>()});
			property.Writes = property.Reads;

			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				const SurfaceAppearance *appearance = store.Get<SurfaceAppearance>(instance);
				if (appearance == nullptr) {
					return false;
				}
				*static_cast<core::Name *>(out) =
					ecs::EnumTable::MemberAt(AlphaModeEnum(), static_cast<size_t>(appearance->Mode));
				return true;
			};

			property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
				SurfaceAppearance *appearance = store.GetMutable<SurfaceAppearance>(instance);
				if (appearance == nullptr) {
					return false;
				}

				size_t ordinal = 0;
				if (!ecs::EnumTable::OrdinalOf(
						AlphaModeEnum(), *static_cast<const core::Name *>(value), ordinal
					)) {
					// Refused where it was written rather than landing in the
					// component as a mode nobody chose.
					return false;
				}

				appearance->Mode = static_cast<AlphaMode>(ordinal);
				return true;
			};

			return property;
		}

		// Which tags a surface camera draws, as a name a script writes.
		//
		// **The property is a name and the storage is a bit**, which is the
		// whole of `AGENTS.md` rule 4 applied to a filter: a script says
		// `camera.TagFilter = "Reflective"` and the component holds a mask that
		// a draw loop can `and` against. The registration happens here, once,
		// where the name is written — so the hot path never sees a string.
		//
		// **Several tags, comma-separated**, because a mask holds thirty-two and
		// a property holds one value. `"Imported, Reflective"` is the whole
		// syntax: a list in a string, which is what a property type of `Name`
		// can carry without inventing a list type in `ecs::PropertyType` that
		// exactly one property would use.
		//
		// Spaces around a comma are ignored, so the separator reads the way
		// somebody would type it. An empty entry is skipped rather than
		// registering a blank tag.
		//
		// Reading it back gives every name in the mask, in bit order, joined the
		// same way — so a filter written from a script round-trips exactly and
		// one assembled another way reads as the tags it includes rather than as
		// a number.
		PropertyDescriptor TagFilterProperty() {
			PropertyDescriptor property;
			property.Name = core::Name("TagFilter");

			// **`Name` and not `String`, and the difference is the payload.**
			// `PropertyType::String` marshals a `std::string`, which the script
			// binding hands to a setter as an owning object; this getter and
			// setter move a `core::Name`, which is what `PropertyType::Name`
			// means. Declaring the wrong one type-checks, passes a test that
			// writes raw bytes, and fails at the first script that assigns to
			// it — which is exactly how it was found.
			property.Type = PropertyType::Name;
			property.Size = sizeof(core::Name);
			property.Kind = PropertyKind::Field;
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<SurfaceCamera>()});
			property.Writes = property.Reads;

			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				const SurfaceCamera *surface = store.Get<SurfaceCamera>(instance);
				const TagTable *table = store.Resource<TagTable>();
				if (surface == nullptr) {
					return false;
				}

				const std::vector<core::Name> named =
					table == nullptr ? std::vector<core::Name>{} : table->Describe(surface->TagFilter);

				std::string joined;
				for (const core::Name &name : named) {
					if (!joined.empty()) {
						joined += ", ";
					}
					joined += name.Text();
				}

				// An empty mask reads back as an invalid name rather than as an
				// empty string, so "no filter" is the same value clearing it
				// takes.
				*static_cast<core::Name *>(out) = joined.empty() ? core::Name() : core::Name(joined);
				return true;
			};

			property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
				SurfaceCamera *surface = store.GetMutable<SurfaceCamera>(instance);
				if (surface == nullptr) {
					return false;
				}

				const core::Name &name = *static_cast<const core::Name *>(value);
				if (!name.IsValid()) {
					// An empty filter is a camera that draws the world, which is
					// what every mirror is and what clearing the property has to
					// mean.
					surface->TagFilter = 0;
					return true;
				}

				// **Assembled into a local and assigned once.** A loop writing
				// straight into the component would leave a half-built filter
				// behind when the table filled part way through — a redirected
				// pass drawing some of its group, which is harder to notice than
				// one drawing none of it.
				uint32_t mask = 0;
				const std::string_view text = name.Text();

				for (size_t start = 0; start <= text.size();) {
					const size_t comma = std::min(text.find(',', start), text.size());
					std::string_view entry = text.substr(start, comma - start);
					start = comma + 1;

					while (!entry.empty() && entry.front() == ' ') {
						entry.remove_prefix(1);
					}
					while (!entry.empty() && entry.back() == ' ') {
						entry.remove_suffix(1);
					}
					if (entry.empty()) {
						continue;
					}

					const uint32_t bit = TagsOf(store).Register(core::Name(entry));
					if (bit == 0) {
						// The table is full. Refused rather than left partly
						// applied: a filter that silently became "everything" is
						// a redirected pass quietly drawing the whole world.
						return false;
					}
					mask |= bit;
				}

				surface->TagFilter = mask;
				return true;
			};

			return property;
		}

		// --- the attachment's four ------------------------------------------
		//
		// **Two writable and two read-only, and the split is the design.**
		// `CFrame` and `Position` are the local offset an author writes;
		// `WorldCFrame` and `WorldPosition` are what `ResolveAttachments`
		// computed from it. A writable world frame would be a second way to place
		// an attachment, and the two would disagree the moment the parent moved —
		// which is exactly the state `GuiObject`'s absolutes are read-only to
		// prevent.
		//
		// **A getter that walked to the parent was tried and is what the derived
		// field replaced.** It is one `CFrame` product per read, and a beam reads
		// four of them per frame — but the cost is not the argument. The argument
		// is that a read on the frame an attachment is created, before the pass
		// has run, would answer a stale identity through the field and the right
		// value through the walk, and a property that answers differently
		// depending on when in the frame it is asked is a property nobody can
		// reason about. So the getter resolves on the spot and the *draw path*
		// reads the field: one authoritative answer for a script, one cheap one
		// for a loop.
		PropertyDescriptor AttachmentPositionProperty() {
			PropertyDescriptor property;
			property.Name = core::Name("Position");
			property.Type = PropertyType::Vector3;
			property.Size = sizeof(core::Vector3);
			property.Kind = PropertyKind::Computed;
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Attachment>()});
			property.Writes = property.Reads;

			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				const Attachment *point = store.Get<Attachment>(instance);
				if (point == nullptr) {
					return false;
				}
				*static_cast<core::Vector3 *>(out) = point->Frame.Position;
				return true;
			};

			// The rotation is kept, for `PositionProperty`'s reason one file up:
			// an attachment carries a direction as well as a place, and a setter
			// that wrote twelve bytes over the front of a `CFrame` would leave a
			// quaternion that no longer matches.
			property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
				Attachment *point = store.GetMutable<Attachment>(instance);
				if (point == nullptr) {
					return false;
				}
				point->Frame.Position = *static_cast<const core::Vector3 *>(value);
				return true;
			};

			return property;
		}

		PropertyDescriptor WorldCFrameProperty() {
			PropertyDescriptor property;
			property.Name = core::Name("WorldCFrame");
			property.Type = PropertyType::CFrame;
			property.Size = sizeof(core::CFrame);
			property.Kind = PropertyKind::Computed;
			property.Writable = false;
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Attachment>()});

			// **Nothing, because nothing is written.** `Writes` reaches the
			// binding manifest, so a read-only property naming a component was
			// telling every script author that setting it moves storage it
			// cannot even be given a value for.
			property.Writes = &ecs::ComponentSet::Intern({});

			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				if (store.Get<Attachment>(instance) == nullptr) {
					return false;
				}
				*static_cast<core::CFrame *>(out) = ResolveAttachment(store, instance);
				return true;
			};

			return property;
		}

		PropertyDescriptor WorldPositionProperty() {
			PropertyDescriptor property;
			property.Name = core::Name("WorldPosition");
			property.Type = PropertyType::Vector3;
			property.Size = sizeof(core::Vector3);
			property.Kind = PropertyKind::Computed;
			property.Writable = false;
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Attachment>()});

			// **Nothing, because nothing is written.** `Writes` reaches the
			// binding manifest, so a read-only property naming a component was
			// telling every script author that setting it moves storage it
			// cannot even be given a value for.
			property.Writes = &ecs::ComponentSet::Intern({});

			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				if (store.Get<Attachment>(instance) == nullptr) {
					return false;
				}
				*static_cast<core::Vector3 *>(out) = ResolveAttachment(store, instance).Position;
				return true;
			};

			return property;
		}

		// Which face a spot or surface light points out of.
		//
		// The same shape as `FaceProperty` above and deliberately not shared with
		// it: that one reads a `SurfaceCamera` and this reads a `Light`, and a
		// descriptor is a pair of function pointers over a specific component. A
		// template over the component would save nine lines and cost the reader
		// the ability to see what a property touches, which is what `Reads` and
		// `Writes` exist to state.
		PropertyDescriptor LightFaceProperty() {
			PropertyDescriptor property;
			property.Name = core::Name("Face");
			property.Type = PropertyType::Enum;
			property.EnumName = NormalIdEnum();
			property.Size = sizeof(core::Name);
			property.Kind = PropertyKind::Field;
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Light>()});
			property.Writes = property.Reads;

			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				const Light *bulb = store.Get<Light>(instance);
				if (bulb == nullptr) {
					return false;
				}
				*static_cast<core::Name *>(out) =
					ecs::EnumTable::MemberAt(NormalIdEnum(), static_cast<size_t>(bulb->Face));
				return true;
			};

			property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
				Light *bulb = store.GetMutable<Light>(instance);
				if (bulb == nullptr) {
					return false;
				}

				size_t ordinal = 0;
				if (!ecs::EnumTable::OrdinalOf(
						NormalIdEnum(), *static_cast<const core::Name *>(value), ordinal
					)) {
					return false;
				}

				bulb->Face = static_cast<NormalId>(ordinal);
				return true;
			};

			return property;
		}

		// Whether the world found something under the character's feet.
		//
		// Read-only, for the reason the declaration site gives.
		PropertyDescriptor GroundedProperty() {
			PropertyDescriptor property;
			property.Name = core::Name("Grounded");
			property.Type = PropertyType::Bool;
			property.Size = sizeof(bool);
			property.Kind = PropertyKind::Computed;
			property.Writable = false;
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Humanoid>()});

			// **Nothing, because nothing is written.** `Writes` reaches the
			// binding manifest, so a read-only property naming a component was
			// telling every script author that setting it moves storage it
			// cannot even be given a value for.
			property.Writes = &ecs::ComponentSet::Intern({});

			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				const Humanoid *humanoid = store.Get<Humanoid>(instance);
				if (humanoid == nullptr) {
					return false;
				}
				*static_cast<bool *>(out) = humanoid->Grounded;
				return true;
			};

			return property;
		}

		// Read-only mesh metadata; zero means the catalogue has no entry.
		PropertyDescriptor TrianglesCountProperty() {
			PropertyDescriptor property;
			property.Name = core::Name("TrianglesCount");

			property.Type = PropertyType::Int32;
			property.Size = sizeof(int32_t);
			property.Kind = PropertyKind::Computed;

			property.Writable = false;

			// Visual is the dependency for change notifications.
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Visual>()});

			// **Nothing, because nothing is written.** `Writes` reaches the
			// binding manifest, so a read-only property naming a component was
			// telling every script author that setting it moves storage it
			// cannot even be given a value for.
			property.Writes = &ecs::ComponentSet::Intern({});

			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				const Visual *visual = store.Get<Visual>(instance);
				if (visual == nullptr) {
					return false;
				}

				// Do not create the catalogue from a getter.
				*static_cast<int32_t *>(out) = static_cast<int32_t>(TrianglesOf(store, visual->Mesh));
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

			// **`Material` is not registered as an enum, and its absence is the
			// point.** It held seventeen names — `Plastic`, `Wood`, `Metal` —
			// and the membership check was the only thing it did: no renderer
			// sampled anything different because a part said `Wood`. A material
			// is content now, named by a `Material` instance and resolved
			// against what a publisher published — `scene/Materials.hpp`. A game
			// that wants its own named set still registers one with
			// `ecs::EnumTable::Register`; the engine no longer ships one that
			// promises something it cannot draw.

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

			// The surface effects, generated from `Describe` for the same reason
			// the faces are: a member list written out twice is a member list
			// that gains an entry in one place.
			std::array<std::string_view, SURFACE_EFFECT_COUNT> effects{};
			for (size_t index = 0; index < effects.size(); index++) {
				effects[index] = Describe(static_cast<SurfaceEffect>(index));
			}
			ecs::EnumTable::Register(SurfaceEffectEnum().Text(), effects);

			// The alpha modes, by the same rule: a member list registered once
			// so a script setting `.AlphaMode = "Clip"` is checked against it.
			ecs::EnumTable::Register(
				AlphaModeEnum().Text(), std::array<std::string_view, 3>{"Opaque", "Clip", "Blend"}
			);

			// The default collision group, so `CollisionGroup` reads back
			// something a script can compare rather than an invalid name on a
			// part nobody configured.
			spatial::CollisionGroups::Register(spatial::CollisionGroups::DEFAULT);

			// **`ecs`'s, not this module's.** `Instance`, `Name` and `Parent`
			// all project components `ecs` owns, and they were declared here
			// only because `scene` happened to be the first module with a class
			// tree. v0.8 added a second — `gui`, which is `shared` and may not
			// link this one — so the root moved down to where its components
			// already live. Same correction `ecs.Hierarchy`'s registration went
			// through, and the `ParentProperty` this file used to hold is gone with it.
			const ecs::ClassId instance = ecs::Classes::RegisterInstanceRoot();

			// PVInstance is everything with a place in the world. Roblox's
			// split, kept because v0.6 binds `Instance.new` to this same table
			// and a tree that differs from the one scripts expect is a
			// migration nobody asked for.
			const std::array pv{ecs::Components::Of<Transform>(), ecs::Components::Of<Pivot>()};
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

				// **What a surface is made of, and what groups it belongs to.**
				// Both are on `BasePart` rather than on `MeshPart`, which costs
				// sixteen bytes on every part in the world and buys a draw-list
				// pass with no optional join — `SurfaceAppearance`'s own header
				// carries the whole argument.
				ecs::Components::Of<SurfaceAppearance>(),
				ecs::Components::Of<Tags>(),
			};
			const ecs::ClassId basePart = ecs::Classes::Register("BasePart", pvInstance, base);

			// Part adds nothing of its own: BasePart already holds the set
			// `v02v03v04.md` §3.3 names, and Part is the concrete leaf a script
			// asks for by name. `RigidBody` and `Motion` are deliberately
			// absent from every class here — whether a part has them is
			// `PartDesc::Anchored`'s decision, and putting them in the class
			// set would land static geometry in the dynamic archetype.
			const ecs::ClassId part = ecs::Classes::Register("Part", basePart, {});

			// **A `MeshPart` is a `BasePart` whose mesh came from somewhere
			// else**, and that is the whole of the difference. It adds no
			// component: `Visual::Mesh` already names a mesh and
			// `SurfaceAppearance::ColourMap` already names a texture, both on
			// `BasePart`, because a plain `Part` may name a built-in and a
			// texture just as legitimately.
			//
			// So what the class is *for* is the vocabulary. A script written
			// against Roblox says `Instance.new("MeshPart")` and reads
			// `.MeshId` and `.TextureID`, and a class tree that made it say
			// `Part` and `.Mesh` would be a migration nobody asked for —
			// `scene/AGENTS.md`'s argument for keeping the tree Roblox's,
			// applied to the class v0.9 exists to add.
			const ecs::ClassId meshPart = ecs::Classes::Register("MeshPart", basePart, {});

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

			// **A sound is an `Instance`, not a `PVInstance`, and the omission
			// is the design.** It has no place of its own: under `Workspace` it
			// is heard everywhere at one level, and inside a part it is heard
			// from that part and falls off with distance. Giving it a
			// `Transform` would be a second opinion about where a thing is —
			// rule 2 with a speaker attached — and would make "attach a sound
			// to a thing" a field to keep in step with a parent that already
			// says it.
			//
			// Nothing in this module plays one. `scene` is `shared` and a
			// server has no mixer; the client walks these rows and drives
			// `engine::audio`, which is the same split `Visual::Mesh` has
			// against the renderer.
			const std::array emitter{ecs::Components::Of<Sound>()};
			const ecs::ClassId soundClass = ecs::Classes::Register("Sound", instance, emitter);

			// **An `Attachment` is an `Instance` and not a `PVInstance`**, which
			// is `Sound`'s omission for a related reason. A `PVInstance` carries a
			// `Transform` — a world-space placement — and an attachment already
			// holds a `CFrame` relative to its parent. Two of those on one row is
			// two opinions about where a point is, and `SetParent` would silently
			// decide which one won.
			//
			// The whole reason this class exists before anything that uses it is
			// that beams, trails and particle emitters are all parented to one.
			// Building any of those first means building them twice.
			const std::array point{ecs::Components::Of<Attachment>()};
			const ecs::ClassId attachmentClass = ecs::Classes::Register("Attachment", instance, point);

			// **A `Material` is an `Instance` under a drawable**, which is
			// Roblox's `SurfaceAppearance` arrangement and is what replaces the
			// seventeen-name `Material` enum this tree used to register. Not a
			// `PVInstance`, for `Attachment`'s reason: it has no place in the
			// world of its own, and a `Transform` on this row would be a second
			// opinion about where its parent is.
			//
			// **An instance rather than a property**, because `ROADMAP.md` v0.11
			// grows this into something carrying several texture references, and
			// a `core::Name` field on `BasePart` could never grow children.
			// `scene/Materials.hpp` carries the whole argument, including what
			// `ResolveMaterials` writes and what it costs.
			const std::array binding{ecs::Components::Of<MaterialRef>()};
			const ecs::ClassId materialClass = ecs::Classes::Register("Material", instance, binding);

			// **One component, three classes**, which is `Collider`'s trade across
			// three shapes. The three lights differ by two fields; three
			// components would be three columns, three queries and three upload
			// paths for something a renderer packs into one array either way.
			//
			// Each class sets its own `Kind` as a *prototype default*, so
			// `Instance.new("SpotLight")` is a spot light without a script saying
			// so — which is the whole point of the prototype row. A `Kind`
			// property is deliberately not declared: the class is the answer, and
			// a second way to say it is the duplicate `AGENTS.md` warns about.
			// **A `Humanoid` is an `Instance` under a character model**, which is
			// Roblox's arrangement exactly: the humanoid is a sibling of the parts
			// it drives rather than a component on one of them. That is what lets
			// a character be a model of several parts with one thing steering it,
			// and it is why `StepCharacters` walks `<Humanoid, Motion>` rather
			// than looking for a humanoid on a part.
			const std::array body{ecs::Components::Of<Humanoid>()};
			const ecs::ClassId humanoidClass = ecs::Classes::Register("Humanoid", instance, body);

			// The key names, so a script can say `Enum.KeyCode.Space` and be told
			// when it is wrong. **Generated from `Describe` rather than typed out
			// beside the enum**, which is the rule `NormalId` already follows here
			// — one declaration, and adding a key means adding it in one place.
			std::array<std::string_view, static_cast<size_t>(KeyCode::Count)> keys{};
			for (size_t index = 0; index < keys.size(); index++) {
				keys[index] = Describe(static_cast<KeyCode>(index));
			}
			ecs::EnumTable::Register("KeyCode", keys);

			std::array<std::string_view, static_cast<size_t>(MouseButton::Count)> buttons{};
			for (size_t index = 0; index < buttons.size(); index++) {
				buttons[index] = Describe(static_cast<MouseButton>(index));
			}
			ecs::EnumTable::Register("UserInputType", buttons);

			// **The states a bound action's handler is told about.** Registered
			// here beside the other input enums rather than in
			// `script::OpenInputServices`, because the bindings generator does not
			// open a VM — an enum registered at VM-open time is one the manifest
			// never sees, and `Enum_UserInputState` came out of the declaration
			// file as an unknown type.
			ecs::EnumTable::Register(
				"UserInputState", std::array<std::string_view, 3>{"Begin", "Change", "End"}
			);

			ecs::EnumTable::Register(
				"MouseBehavior",
				std::array<std::string_view, 3>{"Default", "LockCenter", "LockCurrentPosition"}
			);
			ecs::EnumTable::Register(
				"CameraType",
				std::array<std::string_view, 4>{"Classic", "LockFirstPerson", "ShiftLock", "Scriptable"}
			);

			const std::array bulb{ecs::Components::Of<Light>()};
			const ecs::ClassId lightClass = ecs::Classes::Register("Light", instance, bulb);
			// **Registered and not kept**, because a point light declares no property of
			// its own: everything it has is on `Light`, and the class exists so that
			// `Instance.new("PointLight")` resolves and so that `:IsA("PointLight")`
			// answers. The two below are kept only to hang a prototype default off.
			(void)ecs::Classes::Register("PointLight", lightClass, {});
			const ecs::ClassId spotLightClass = ecs::Classes::Register("SpotLight", lightClass, {});
			const ecs::ClassId surfaceLightClass = ecs::Classes::Register("SurfaceLight", lightClass, {});

			Light spot;
			spot.Kind = LightKind::Spot;
			ecs::Classes::Default(spotLightClass, spot);

			Light surfaceLight;
			surfaceLight.Kind = LightKind::Surface;
			ecs::Classes::Default(surfaceLightClass, surfaceLight);

			// **The two instances whose whole content is a string**, and the
			// only members of Roblox's `ValueBase` family this engine has. Rojo
			// maps `*.txt` onto the first and `*.csv` onto the second, and a
			// folder sync that could not build either would silently drop files
			// — see `scene::TextContent` for why the rest of the family is
			// deliberately absent.
			//
			// `ValueBase` is registered as the base so `:IsA("ValueBase")`
			// answers the question a script would actually ask, exactly as
			// `LuaSourceContainer` does one module over.
			const std::array text{ecs::Components::Of<TextContent>()};
			const ecs::ClassId valueBase = ecs::Classes::Register("ValueBase", instance, text);
			ecs::Classes::Register("StringValue", valueBase, {});
			ecs::Classes::Register("LocalizationTable", valueBase, {});

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
			// `Name` and `Parent` are declared by `RegisterInstanceRoot` above.

			// **Computed rather than a member projection**, because `Position`
			// and `Orientation` are read-modify-writes over one `CFrame` and a
			// member pointer cannot express that: writing twelve bytes over the
			// front of a frame leaves a quaternion that no longer matches it.
			//
			// The reason this comment used to give was that an authored
			// placement has to move `PreviousTransform` with it, and that reason
			// was wrong — it breaks every scripted animation in the engine.
			// `PlaceInstance` carries the refutation and the fix.
			ecs::Classes::Computed(pvInstance, CFrameProperty());
			ecs::Classes::Computed(pvInstance, PositionProperty());
			ecs::Classes::Computed(pvInstance, OrientationProperty());

			// **Roblox's `PivotOffset`, on `PVInstance` rather than on
			// `BasePart`.** Roblox declares it on the latter; here the component
			// is on the former, and a `Model` — when there is one — wants the
			// same field for the same reason. Declaring it where the storage is
			// keeps one answer to "what has a pivot".
			ecs::Classes::Property<&Pivot::Offset>(pvInstance, "PivotOffset");

			ecs::Classes::Computed(basePart, SizeProperty());
			ecs::Classes::Computed(basePart, CanCollideProperty());
			ecs::Classes::Computed(basePart, AnchoredProperty());

			// The plain fields. `Color` is a rename rather than a conversion —
			// `Visual::Tint` is what a script calls `Color`. `Surface::Material`,
			// which is what a part *feels* like, is deliberately not bound: it is
			// a separate fact that happens to share a word with the `Material`
			// instance below, and a mirror-finish floor and a rubber floor may
			// share a surface and never a material.
			ecs::Classes::Property<&Visual::Tint>(basePart, "Color");
			ecs::Classes::Property<&Visual::Visible>(basePart, "Visible");

			// **`Mesh` and `ColorMap` are not here, and that is v0.10's
			// correction.** `BasePart` is what `Part`, `MeshPart` and a future
			// `UnionOperation` *share*, and geometry loaded from a file is not
			// shared by any of them: a `Part` is one of six built-in shapes and
			// naming a mesh on it is a property that does nothing. Offering it
			// is worse than not having it — an author sets it, the part does not
			// change, and nothing says the class was the wrong one. Both are
			// declared on `MeshPart` below, under the names that class actually
			// shows.
			//
			// **The storage is unchanged and stays on `BasePart`.**
			// `SurfaceAppearance`'s own comment carries why: `client::
			// CollectInstances` is a batched parallel walk over a fixed
			// signature, and an optional column is precisely what that shape
			// cannot express. A dense column of mostly-invalid names is sixteen
			// bytes an entity and no branches — where a per-class component
			// would be a join per row per frame. What moved is the *vocabulary*,
			// which is what a properties panel and a script see.
			//
			// A `Part` is textured by a `Material` instance under it, which is
			// what `scene/Materials.hpp` is for.

			// **The alpha pair stays on `BasePart`, and the asymmetry is
			// deliberate.** These say how the alpha of whatever is being sampled
			// is treated, and a plain `Part` samples something the moment it has
			// a `Material` child — a cut-out material on a `Part` needs `Clip`
			// exactly as one on a `MeshPart` does. They are not *content*, which
			// is the line the two above fall on the far side of.
			ecs::Classes::ClampedProperty<&SurfaceAppearance::AlphaCutoff, 0.0f, 1.0f>(
				basePart, "AlphaCutoff"
			);
			ecs::Classes::Computed(basePart, AlphaModeProperty());

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

			// **The one property on a part that only an editor reads.** A
			// locked part still draws, still collides and is still reachable
			// from a script — what it refuses is a pointer pick, which is
			// `Visual::Locked`'s whole surface. Declared here rather than kept
			// in the editor because it is authoring data that has to survive a
			// save, which an editor-side set could not.
			ecs::Classes::Property<&Visual::Locked>(basePart, "Locked");

			// **`Value` on the base, so both classes have it once.** A
			// `LocalizationTable` holds its CSV here and resolves nothing —
			// translation lookup is a service with a locale and a fallback
			// chain, and none of that is a file mapping.
			ecs::Classes::Property<&TextContent::Value>(valueBase, "Value");

			// Which surface texture this part shows, or -1 for none. An `int32`
			// rather than a reference to the camera: the renderer indexes a
			// small fixed set, and a handle would have to be resolved back to an
			// index every frame for every part.

			// The last of the two that were named as gaps at v0.5. `Material` was
			// the other and is no longer a property of a part at all: it is an
			// instance under one — see the `Material` class below.
			ecs::Classes::Computed(basePart, CollisionGroupProperty());

			// The camera's own three. `FieldOfView` is **degrees**, because
			// Roblox's is and because `Orientation` already reproduces that same
			// split — the component stores radians and the conversion is where
			// the unit changes, which is the whole shape of a property here.
			// **Roblox's names, and since v0.10 the only names.** These were
			// aliases of `BasePart.Mesh` and `BasePart.ColorMap`; those are gone
			// — see the note where they used to be declared — so a mesh
			// reference and its sheet are named on the one class that has
			// either. One spelling rather than two also ends the trap
			// `studio/Assets.hpp` warned about, where an alias missing from the
			// picker's table gave a plain text field on the name people use.
			//
			// They still project the components `BasePart` holds, which is a
			// storage decision and not a vocabulary one.
			ecs::Classes::Property<&Visual::Mesh>(meshPart, "MeshId");
			ecs::Classes::Property<&SurfaceAppearance::ColourMap>(meshPart, "TextureID");

			// Mesh metadata belongs to MeshPart, not every BasePart.
			ecs::Classes::Computed(meshPart, TrianglesCountProperty());

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
			ecs::Classes::Computed(surfaceCameraClass, SurfaceEffectProperty());
			ecs::Classes::Computed(surfaceCameraClass, TagFilterProperty());

			// The sound's six. All plain fields, which is unusual enough here
			// to be worth saying: nothing about a sound is a doubled
			// half-extent or a quaternion in degrees, so there is no conversion
			// to write and no place for one to be wrong in one direction.
			//
			// `SoundId` names a published asset the way `MeshId` does —
			// extension included, exactly as the manifest carries it, because
			// the lookup is a string compare and the one place the two could
			// diverge is a spelling.
			ecs::Classes::Property<&Sound::SoundId>(soundClass, "SoundId");

			// **Clamped at 10 rather than at 1**, which is Roblox's ceiling and
			// is also the honest one: the graph works in floats precisely so a
			// value over full scale passes through harmlessly and is clamped
			// once at the device. Refusing at 1 would make a sound authored
			// quiet unable to be brought up.
			ecs::Classes::ClampedProperty<&Sound::Volume, 0.0f, 10.0f>(soundClass, "Volume");
			ecs::Classes::Property<&Sound::Looped>(soundClass, "Looped");
			ecs::Classes::Property<&Sound::Playing>(soundClass, "Playing");
			ecs::Classes::Property<&Sound::RollOffMinDistance>(soundClass, "RollOffMinDistance");
			ecs::Classes::Property<&Sound::RollOffMaxDistance>(soundClass, "RollOffMaxDistance");

			// The attachment's four. `CFrame` is the authored local offset and
			// carries the same name a `PVInstance`'s does — which is correct
			// rather than a collision: on both classes it means "where this thing
			// is, in the terms that thing is placed in", and an attachment is
			// placed relative to its parent.
			ecs::Classes::Property<&Attachment::Frame>(attachmentClass, "CFrame");
			ecs::Classes::Computed(attachmentClass, AttachmentPositionProperty());
			ecs::Classes::Computed(attachmentClass, WorldCFrameProperty());
			ecs::Classes::Computed(attachmentClass, WorldPositionProperty());

			// The material's one. **`MaterialId` rather than `Material`**, which
			// is this tree's spelling for "a name that is an asset" — `MeshId`,
			// `SoundId`, `TextureID` — and is what puts the studio's content
			// picker on it rather than a bare text field, through
			// `studio::ContentKindOfProperty`. Calling it `Material` on a class
			// called `Material` would also read as a self-reference at every call
			// site: `material.Material = ...`.
			ecs::Classes::Property<&MaterialRef::Asset>(materialClass, "MaterialId");

			// The light's, declared on the base so all three inherit them.
			//
			// **`Angle` and `Face` are on `Light` rather than on the two classes
			// that read them**, which is the shape the single component forces and
			// is honest about it: a `PointLight` has an `Angle` property that does
			// nothing. The alternative is declaring the same two descriptors on
			// two classes, which is two declarations of one projection — and the
			// component is shared either way, so the storage does not change. A
			// point light's angle reads back what it was set to and is ignored,
			// which is the same contract `SurfaceCamera::Face` has on a camera
			// parented to the world.
			ecs::Classes::Property<&Light::Colour>(lightClass, "Color");
			ecs::Classes::Property<&Light::Enabled>(lightClass, "Enabled");
			ecs::Classes::Property<&Light::Shadows>(lightClass, "Shadows");

			// Clamped, because all three are quantities with an obvious nearest
			// meaning outside their range — `ClampedProperty`'s own argument. The
			// brightness ceiling is Roblox's; the range ceiling is the one past
			// which a forward renderer's light culling stops rejecting anything.
			ecs::Classes::ClampedProperty<&Light::Brightness, 0.0f, 10000.0f>(lightClass, "Brightness");
			ecs::Classes::ClampedProperty<&Light::Range, 0.0f, 60.0f>(lightClass, "Range");
			ecs::Classes::ClampedProperty<&Light::Angle, 0.0f, 180.0f>(lightClass, "Angle");
			ecs::Classes::Computed(lightClass, LightFaceProperty());

			// The humanoid's. All plain fields — nothing here is a doubled
			// half-extent or an angle in the wrong unit, so there is no conversion
			// to write and no place for one to be wrong in one direction.
			//
			// **`MoveDirection` is writable, which is what makes a scripted
			// character possible.** A game driving an NPC writes it directly and
			// never installs `UpdateCharacterControl`; a player's character has
			// that system writing it every frame instead. One field, two writers,
			// and only one of them installed per character — which is the same
			// shape `MoveCamera` and a scripted camera already have.
			ecs::Classes::Property<&Humanoid::MoveDirection>(humanoidClass, "MoveDirection");
			ecs::Classes::ClampedProperty<&Humanoid::WalkSpeed, 0.0f, 1000.0f>(humanoidClass, "WalkSpeed");
			ecs::Classes::ClampedProperty<&Humanoid::JumpSpeed, 0.0f, 1000.0f>(humanoidClass, "JumpPower");
			ecs::Classes::ClampedProperty<&Humanoid::Height, 0.1f, 100.0f>(humanoidClass, "HipHeight");
			ecs::Classes::Property<&Humanoid::Enabled>(humanoidClass, "Enabled");

			// **Read-only, because it is what the world found rather than what an
			// author wants.** A script setting `Grounded` would be telling the
			// controller a lie it acts on immediately — the jump would fire in
			// mid-air, which is the exploit this flag exists to gate.
			ecs::Classes::Computed(humanoidClass, GroundedProperty());

			// Still not declared, and for a reason rather than an oversight:
			// **`Surface::Material`**, which is what a part *feels* like. The
			// `Material` class above is what it looks like, and the two are
			// separate facts that share a word — a mirror-finish floor and a
			// rubber floor may share a surface and never a material. Binding both
			// under one script name would make one of them unreachable and the
			// other ambiguous.
			return part;
		}
	}

	core::CFrame PivotOf(const ecs::Store &store, ecs::Entity instance) {
		const Transform *transform = store.Get<Transform>(instance);
		if (transform == nullptr) {
			return {};
		}

		// **A missing `Pivot` is the identity rather than a refusal.** Every
		// `PVInstance` has the column, but a replica's row grows from a wire
		// delta and a headless world may hold an instance built by hand — and
		// "no offset" is a complete answer, not an error.
		const Pivot *pivot = store.Get<Pivot>(instance);
		return pivot == nullptr ? transform->Frame : transform->Frame * pivot->Offset;
	}

	bool PivotTo(ecs::Store &store, ecs::Entity instance, const core::CFrame &target) {
		if (store.Get<Transform>(instance) == nullptr) {
			return false;
		}

		const Pivot *pivot = store.Get<Pivot>(instance);
		const core::CFrame placement = pivot == nullptr ? target : target * pivot->Offset.Inverse();

		// Through the same helper a property write uses, so a script pivoting a
		// part and an author dragging one leave the world in the same state.
		PlaceInstance(store, instance, placement);
		return true;
	}

	ecs::ClassId PartClass() {
		static const ecs::ClassId part = (EnsureClassTree(), ecs::Classes::Find(core::Name("Part")));
		return part;
	}

	// **One registration, and the static that holds it lives here.** Two
	// function-local statics each calling `RegisterTree` would have raced to
	// register the same names, which `Classes::Register` tolerates — and then
	// the *order* of the two would decide which id each class got, which is
	// rule 4's hazard arriving inside one process. So there is one static, and
	// every accessor goes through it.
	void EnsureClassTree() {
		static const ecs::ClassId root = RegisterTree();
		(void)root;
	}

	// The rest of the tree's accessors, and they share one shape.
	//
	// **The id is cached, and that is safe because of the above.** Only
	// `EnsureClassTree` calls `RegisterTree`, so a static here calls a static
	// there rather than racing beside it — and a class id is fixed for the life
	// of the process once registered. Without this each call was a `core::Name`
	// intern plus a lookup, paid every time somebody asked.
	ecs::ClassId SoundClass() {
		static const ecs::ClassId sound = (EnsureClassTree(), ecs::Classes::Find(core::Name("Sound")));
		return sound;
	}

	ecs::ClassId CameraClass() {
		static const ecs::ClassId camera = (EnsureClassTree(), ecs::Classes::Find(core::Name("Camera")));
		return camera;
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
