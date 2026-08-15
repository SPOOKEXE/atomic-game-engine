#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/EnumTable.hpp>
#include <engine/ecs/Property.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/effects/ParticleSystem.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/effects/Ribbon.hpp>
#include <engine/scene/Part.hpp>

#include <algorithm>
#include <array>
#include <string_view>
#include <type_traits>
#include <utility>

namespace engine::effects {

	namespace {
		using ecs::PropertyDescriptor;
		using ecs::PropertyKind;
		using ecs::PropertyType;

		// The enum names, interned once each.
		//
		// `scene::Part.cpp`'s `NormalIdEnum` states the rule: constructing a
		// `core::Name` from a literal takes the process-wide registry mutex and
		// hashes a string, and a property getter read every frame by a properties
		// panel is exactly the loop that must not do it.
		const core::Name &OrientationEnum() {
			static const core::Name name("ParticleOrientation");
			return name;
		}

		const core::Name &ShapeEnum() {
			static const core::Name name("ParticleEmitterShape");
			return name;
		}

		const core::Name &ShapeStyleEnum() {
			static const core::Name name("ParticleEmitterShapeStyle");
			return name;
		}

		const core::Name &ShapeDirectionEnum() {
			static const core::Name name("ParticleEmitterShapeInOut");
			return name;
		}

		const core::Name &FlipbookLayoutEnum() {
			static const core::Name name("ParticleFlipbookLayout");
			return name;
		}

		const core::Name &FlipbookModeEnum() {
			static const core::Name name("ParticleFlipbookMode");
			return name;
		}

		const core::Name &NormalIdEnum() {
			static const core::Name name("NormalId");
			return name;
		}

		// One enum property over one `uint8_t` field of `ParticleEmitter`.
		//
		// **A template where `scene::Part.cpp` writes each one out**, and the
		// difference is the count: there are seven here against two there, and
		// seven copies of the same thirty lines differing only in a member pointer
		// is the duplication `AGENTS.md` calls the most expensive kind of debt.
		// What is given up is that `Reads` and `Writes` no longer name a component
		// per property in readable text - they are the same component for all
		// seven, which is the fact that makes the template safe.
		//
		// **The enum's name arrives as a second template argument and not as a
		// parameter**, and that is forced rather than stylistic:
		// `PropertyDescriptor::Get` and `Set` are raw function pointers, so the
		// generated conversions have to be captureless - the same constraint that
		// makes `Classes::Property` take its member as a template argument. A
		// pointer to the interning accessor is a compile-time constant, so it
		// bakes into the generated function and the descriptor stays a pointer.
		template <auto Member, const core::Name &(*EnumOf)()>
		PropertyDescriptor EnumProperty(std::string_view name) {
			using Enum = std::remove_reference_t<decltype(std::declval<ParticleEmitter &>().*Member)>;

			PropertyDescriptor property;
			property.Name = core::Name(name);
			property.Type = PropertyType::Enum;
			property.EnumName = EnumOf();
			property.Size = sizeof(core::Name);
			property.Kind = PropertyKind::Field;
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<ParticleEmitter>()});
			property.Writes = property.Reads;

			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				const ParticleEmitter *emitter = store.Get<ParticleEmitter>(instance);
				if (emitter == nullptr) {
					return false;
				}
				*static_cast<core::Name *>(out) =
					ecs::EnumTable::MemberAt(EnumOf(), static_cast<size_t>(emitter->*Member));
				return true;
			};

			property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
				ParticleEmitter *emitter = store.GetMutable<ParticleEmitter>(instance);
				if (emitter == nullptr) {
					return false;
				}
				size_t ordinal = 0;
				if (!ecs::EnumTable::OrdinalOf(EnumOf(), *static_cast<const core::Name *>(value), ordinal)) {
					// Refused where it was written, rather than landing in the
					// component as a value nobody chose.
					return false;
				}
				emitter->*Member = static_cast<Enum>(ordinal);
				return true;
			};

			return property;
		}
	}

	namespace {
		// How many of the grid's cells hold a frame, as a number a script writes.
		//
		// **A conversion, because `uint8_t` is not a `PropertyType`.** The storage
		// is one byte - the ceiling is 64 and it fits the padding after the flags -
		// and `Classes::TypeOf` has cases for 32- and 64-bit integers and none for
		// a byte, so a generated property over this field is `Opaque` and its
		// setter refuses every write. That is not a compile error and not a load
		// error: it surfaces as `'FlipbookFrames' cannot take that value` the first
		// time a scene sets it, which is exactly how it was found.
		//
		// So the value that crosses is an `int32` and the storage stays a byte,
		// which is the same shape `Size` has against `Bounds::HalfExtent`: a
		// property is a conversion, and this is one.
		//
		// **Clamped rather than refused**, which is `ClampedProperty`'s own
		// argument - a count has an obvious nearest meaning outside its range, and
		// a scene computing one off a decoded frame count should be corrected
		// rather than stopped.
		PropertyDescriptor FlipbookFramesProperty() {
			PropertyDescriptor property;
			property.Name = core::Name("FlipbookFrames");
			property.Type = PropertyType::Int32;
			property.Size = sizeof(int32_t);
			property.Kind = PropertyKind::Computed;
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<ParticleEmitter>()});
			property.Writes = property.Reads;

			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				const ParticleEmitter *emitter = store.Get<ParticleEmitter>(instance);
				if (emitter == nullptr) {
					return false;
				}
				*static_cast<int32_t *>(out) = emitter->FlipbookFrames;
				return true;
			};

			property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
				ParticleEmitter *emitter = store.GetMutable<ParticleEmitter>(instance);
				if (emitter == nullptr) {
					return false;
				}

				// Zero is "the whole grid" and is the default, so the low bound is
				// zero rather than one. Sixty-four is the widest grid drawn.
				const int32_t wanted = *static_cast<const int32_t *>(value);
				emitter->FlipbookFrames = static_cast<uint8_t>(std::clamp(wanted, 0, 64));
				return true;
			};

			return property;
		}

		ecs::ClassId RegisterTree() {
			RegisterEffectComponents();

			// The class tree these hang off. `scene` owns `Instance`, and a second
			// root would be a tree scripts cannot walk.
			scene::EnsureClassTree();
			const ecs::ClassId instance = ecs::Classes::RegisterInstanceRoot();

			// The member lists, registered once so a script setting one is checked
			// against them. Order is the storage - every one of these is stored as
			// its ordinal in a trivially-copied component - so a member may be
			// appended and never reordered.
			ecs::EnumTable::Register(
				OrientationEnum().Text(),
				std::array<std::string_view, 4>{
					"FacingCamera", "FacingCameraWorldUp", "VelocityParallel", "VelocityPerpendicular"
				}
			);
			ecs::EnumTable::Register(
				ShapeEnum().Text(), std::array<std::string_view, 4>{"Box", "Sphere", "Cylinder", "Disc"}
			);
			ecs::EnumTable::Register(
				ShapeStyleEnum().Text(), std::array<std::string_view, 2>{"Volume", "Surface"}
			);
			ecs::EnumTable::Register(
				ShapeDirectionEnum().Text(), std::array<std::string_view, 3>{"Outward", "Inward", "InAndOut"}
			);
			ecs::EnumTable::Register(
				FlipbookLayoutEnum().Text(),
				std::array<std::string_view, 4>{"None", "Grid2x2", "Grid4x4", "Grid8x8"}
			);
			ecs::EnumTable::Register(
				FlipbookModeEnum().Text(),
				std::array<std::string_view, 4>{"Loop", "OneShot", "PingPong", "Random"}
			);

			// **`EmitterSlot` is in the class set and carries no property**, which
			// is the arrangement that lets `RefreshEmitters` write a block index
			// without ever adding a component during iteration. A structural change
			// inside the walk would move the row out from under it -
			// `AnchoredProperty` is the same hazard declared rather than avoided.
			const std::array emitterSet{
				ecs::Components::Of<ParticleEmitter>(), ecs::Components::Of<EmitterSlot>()
			};
			const ecs::ClassId emitter = ecs::Classes::Register("ParticleEmitter", instance, emitterSet);

			const std::array beamSet{ecs::Components::Of<Beam>()};
			const ecs::ClassId beam = ecs::Classes::Register("Beam", instance, beamSet);

			const std::array trailSet{ecs::Components::Of<Trail>()};
			const ecs::ClassId trail = ecs::Classes::Register("Trail", instance, trailSet);

			// --- the emitter's property surface -----------------------------
			//
			// **Plain fields wherever the storage is what a script writes**, which
			// is most of it: nothing about a particle emitter is a doubled
			// half-extent or a quaternion in degrees, so there is no conversion to
			// write and no place for one to be wrong in one direction. That is
			// unusual enough here to be worth saying, and it is why this list
			// reads as declarations rather than as code.

			ecs::Classes::Property<&ParticleEmitter::Size>(emitter, "Size");
			ecs::Classes::Property<&ParticleEmitter::Transparency>(emitter, "Transparency");
			ecs::Classes::Property<&ParticleEmitter::Squash>(emitter, "Squash");
			ecs::Classes::Property<&ParticleEmitter::Colour>(emitter, "Color");

			ecs::Classes::Property<&ParticleEmitter::Lifetime>(emitter, "Lifetime");
			ecs::Classes::Property<&ParticleEmitter::Speed>(emitter, "Speed");
			ecs::Classes::Property<&ParticleEmitter::Rotation>(emitter, "Rotation");
			ecs::Classes::Property<&ParticleEmitter::RotationSpeed>(emitter, "RotSpeed");
			ecs::Classes::Property<&ParticleEmitter::FlipbookFramerate>(emitter, "FlipbookFramerate");

			ecs::Classes::Property<&ParticleEmitter::Acceleration>(emitter, "Acceleration");
			ecs::Classes::Property<&ParticleEmitter::SpreadAngle>(emitter, "SpreadAngle");
			ecs::Classes::Property<&ParticleEmitter::Texture>(emitter, "Texture");

			// **Rate is clamped and Roblox's ceiling is 500.** Kept here because
			// the ceiling is what stops one emitter taking the whole pool: a block
			// is `Rate * Lifetime` slots, so an unclamped rate with a long life is
			// an emitter that refuses every other emitter in the world.
			ecs::Classes::ClampedProperty<&ParticleEmitter::Rate, 0.0f, 500.0f>(emitter, "Rate");
			ecs::Classes::ClampedProperty<&ParticleEmitter::Drag, -100.0f, 100.0f>(emitter, "Drag");
			ecs::Classes::ClampedProperty<&ParticleEmitter::VelocityInheritance, -100.0f, 100.0f>(
				emitter, "VelocityInheritance"
			);
			ecs::Classes::ClampedProperty<&ParticleEmitter::LightEmission, 0.0f, 1.0f>(
				emitter, "LightEmission"
			);
			ecs::Classes::ClampedProperty<&ParticleEmitter::LightInfluence, 0.0f, 1.0f>(
				emitter, "LightInfluence"
			);
			ecs::Classes::ClampedProperty<&ParticleEmitter::Brightness, 0.0f, 10000.0f>(
				emitter, "Brightness"
			);
			ecs::Classes::ClampedProperty<&ParticleEmitter::ShapePartial, 0.0f, 1.0f>(
				emitter, "ShapePartial"
			);
			ecs::Classes::ClampedProperty<&ParticleEmitter::TimeScale, 0.0f, 10.0f>(emitter, "TimeScale");
			ecs::Classes::Property<&ParticleEmitter::ZOffset>(emitter, "ZOffset");

			ecs::Classes::Computed(emitter, FlipbookFramesProperty());
			ecs::Classes::Property<&ParticleEmitter::FlipbookStartRandom>(emitter, "FlipbookStartRandom");
			ecs::Classes::Property<&ParticleEmitter::LockedToPart>(emitter, "LockedToPart");
			ecs::Classes::Property<&ParticleEmitter::Enabled>(emitter, "Enabled");
			ecs::Classes::Property<&ParticleEmitter::Additive>(emitter, "Additive");

			ecs::Classes::Computed(
				emitter, EnumProperty<&ParticleEmitter::EmissionDirection, NormalIdEnum>("EmissionDirection")
			);
			ecs::Classes::Computed(
				emitter, EnumProperty<&ParticleEmitter::Orientation, OrientationEnum>("Orientation")
			);
			ecs::Classes::Computed(emitter, EnumProperty<&ParticleEmitter::Shape, ShapeEnum>("Shape"));
			ecs::Classes::Computed(
				emitter, EnumProperty<&ParticleEmitter::ShapeStyle, ShapeStyleEnum>("ShapeStyle")
			);
			ecs::Classes::Computed(
				emitter, EnumProperty<&ParticleEmitter::ShapeDirection, ShapeDirectionEnum>("ShapeInOut")
			);
			ecs::Classes::Computed(
				emitter, EnumProperty<&ParticleEmitter::Flipbook, FlipbookLayoutEnum>("FlipbookLayout")
			);
			ecs::Classes::Computed(
				emitter, EnumProperty<&ParticleEmitter::FlipbookPlayback, FlipbookModeEnum>("FlipbookMode")
			);

			// --- the beam's ---------------------------------------------------

			ecs::Classes::Property<&Beam::Colour>(beam, "Color");
			ecs::Classes::Property<&Beam::Transparency>(beam, "Transparency");
			ecs::Classes::Property<&Beam::Texture>(beam, "Texture");
			ecs::Classes::Property<&Beam::Attachment0>(beam, "Attachment0");
			ecs::Classes::Property<&Beam::Attachment1>(beam, "Attachment1");
			ecs::Classes::Property<&Beam::CurveSize0>(beam, "CurveSize0");
			ecs::Classes::Property<&Beam::CurveSize1>(beam, "CurveSize1");
			ecs::Classes::Property<&Beam::Width0>(beam, "Width0");
			ecs::Classes::Property<&Beam::Width1>(beam, "Width1");
			ecs::Classes::Property<&Beam::TextureSpeed>(beam, "TextureSpeed");
			ecs::Classes::Property<&Beam::TextureLength>(beam, "TextureLength");
			ecs::Classes::Property<&Beam::ZOffset>(beam, "ZOffset");
			ecs::Classes::Property<&Beam::FaceCamera>(beam, "FaceCamera");
			ecs::Classes::Property<&Beam::Additive>(beam, "Additive");
			ecs::Classes::Property<&Beam::Enabled>(beam, "Enabled");

			// --- the trail's --------------------------------------------------

			ecs::Classes::Property<&Trail::Colour>(trail, "Color");
			ecs::Classes::Property<&Trail::Transparency>(trail, "Transparency");
			ecs::Classes::Property<&Trail::Texture>(trail, "Texture");
			ecs::Classes::Property<&Trail::Attachment0>(trail, "Attachment0");
			ecs::Classes::Property<&Trail::Attachment1>(trail, "Attachment1");
			ecs::Classes::ClampedProperty<&Trail::Lifetime, 0.0f, 60.0f>(trail, "Lifetime");
			ecs::Classes::ClampedProperty<&Trail::MinimumAngle, 0.0f, 180.0f>(trail, "MinLength");
			ecs::Classes::Property<&Trail::TextureLength>(trail, "TextureLength");
			ecs::Classes::Property<&Trail::Enabled>(trail, "Enabled");
			ecs::Classes::Property<&Trail::Additive>(trail, "Additive");

			return emitter;
		}
	}

	void RegisterEffectClasses() {
		static const ecs::ClassId emitter = RegisterTree();
		(void)emitter;
	}
}
