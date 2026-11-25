// Property descriptors for particles, surfaces, portals, decals, tag filters,
// attachments, collision shape, lights, humanoid health and triangle counts.

#include "PartInternal.hpp"

namespace engine::scene::part_detail {

	PropertyDescriptor GpuParticleLayersProperty() {
		PropertyDescriptor property;
		property.Name = core::Name("Layers");
		property.Type = PropertyType::Int32;
		property.Size = sizeof(int32_t);
		property.Kind = PropertyKind::Field;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<GpuParticleField>()});
		property.Writes = property.Reads;
		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			const GpuParticleField *field = store.Get<GpuParticleField>(instance);
			if (field == nullptr) return false;
			*static_cast<int32_t *>(out) = field->Layers;
			return true;
		};
		property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
			const int32_t layers = *static_cast<const int32_t *>(value);
			if (layers < 0 || (layers & ~static_cast<int32_t>(GPU_PARTICLE_ALL_LAYERS)) != 0) return false;
			GpuParticleField *field = store.GetMutable<GpuParticleField>(instance);
			if (field == nullptr) return false;
			field->Layers = static_cast<uint8_t>(layers);
			return true;
		};
		return property;
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

	// Destination: the part on the far side of a portal.
	//
	// **A reference, because both ends are in one world.** Rule 3 forbids a
	// handle that crosses a world boundary and this never does - a portal
	// pairs two parts of one store, which is the case `PropertyType::Reference`
	// exists for. `CurrentCameraProperty` in `Services.cpp` is the same shape.
	//
	// **Refused for anything that is not a part in space**, because that is
	// the whole of what the aim needs: a `Transform` to build the far face
	// frame from, and `Bounds` to find where that face is. Pointing a portal
	// at a `Folder` would be a portal that silently stayed a mirror, and a
	// mistake refused where it was made beats one discovered by looking at
	// the wall.
	//
	// **Clearing is allowed** - `portal.Destination = nil` turns it back
	// into a mirror, which is also what a deleted destination does.
	PropertyDescriptor DestinationProperty() {
		PropertyDescriptor property;
		property.Name = core::Name("Destination");
		property.Type = PropertyType::Reference;
		property.Size = sizeof(ecs::Entity);
		property.Kind = PropertyKind::Field;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Portal>()});
		property.Writes = property.Reads;
		property.PrepareStoredReference =
			[](ecs::Store &store, ecs::Entity instance, ecs::Entity target) -> ecs::Entity * {
			if (target != ecs::NULL_ENTITY && (!store.Has<Transform>(target) || !store.Has<Bounds>(target)))
				return nullptr;
			Portal *portal = store.GetMutable<Portal>(instance);
			return portal == nullptr ? nullptr : &portal->Destination;
		};

		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			const Portal *portal = store.Get<Portal>(instance);
			if (portal == nullptr) {
				return false;
			}
			*static_cast<ecs::Entity *>(out) = portal->Destination;
			return true;
		};
		property.GetStoredReference = property.Get;

		property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
			Portal *portal = store.GetMutable<Portal>(instance);
			if (portal == nullptr) {
				return false;
			}

			const auto destination = *static_cast<const ecs::Entity *>(value);
			if (destination != ecs::NULL_ENTITY &&
				(store.Get<Transform>(destination) == nullptr || store.Get<Bounds>(destination) == nullptr)) {
				return false;
			}

			portal->Destination = destination;
			return true;
		};

		return property;
	}

	// DestinationWorld: whose contents the pane shows.
	//
	// **A `PropertyType::Name` and not a `String`**, for the reason
	// `TagFilterProperty` records the hard way: `String` marshals a
	// `std::string` and this getter moves a `core::Name`, and declaring the
	// wrong one type-checks and fails at the first script that assigns to it.
	//
	// Unvalidated on purpose, unlike `Destination` above. A world is looked
	// up by name at draw time by whoever is drawing, and the destination
	// world may legitimately not exist yet - a script naming one that a
	// later `AddWorld` creates is ordinary, and refusing the assignment here
	// would make authoring order significant for no gain. A name matching
	// nothing shows this world, which is visible.
	PropertyDescriptor DestinationWorldProperty() {
		PropertyDescriptor property;
		property.Name = core::Name("DestinationWorld");
		property.Type = PropertyType::Name;
		property.Size = sizeof(core::Name);
		property.Kind = PropertyKind::Field;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Portal>()});
		property.Writes = property.Reads;

		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			const Portal *portal = store.Get<Portal>(instance);
			if (portal == nullptr) {
				return false;
			}
			*static_cast<core::Name *>(out) = portal->DestinationWorld;
			return true;
		};

		property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
			Portal *portal = store.GetMutable<Portal>(instance);
			if (portal == nullptr) {
				return false;
			}
			portal->DestinationWorld = *static_cast<const core::Name *>(value);
			return true;
		};

		return property;
	}

	// Which face of the parent part the surface camera projects off.
	//
	// An enum rather than a number, so `camera.Face = "Frnot"` is refused
	// where it was written instead of landing in the component as a face
	// nobody chose. Membership is `EnumTable`'s, and the storage is the
	// ordinal - Roblox's ordinal, so a game file carrying a number means the
	// same thing in both engines.
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

	PropertyDescriptor ResampleModeProperty() {
		PropertyDescriptor property;
		property.Name = core::Name("ResampleMode");
		property.Type = PropertyType::Enum;
		property.EnumName = ResamplerModeEnum();
		property.Size = sizeof(core::Name);
		property.Kind = PropertyKind::Field;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<SurfaceAppearance>()});
		property.Writes = property.Reads;
		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) {
			const auto *appearance = store.Get<SurfaceAppearance>(instance);
			if (appearance == nullptr) {
				return false;
			}
			*static_cast<core::Name *>(out) =
				ecs::EnumTable::MemberAt(ResamplerModeEnum(), static_cast<size_t>(appearance->Resample));
			return true;
		};
		property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) {
			auto *appearance = store.GetMutable<SurfaceAppearance>(instance);
			if (appearance == nullptr) {
				return false;
			}
			size_t ordinal = 0;
			if (!ecs::EnumTable::OrdinalOf(
					ResamplerModeEnum(), *static_cast<const core::Name *>(value), ordinal
				)) {
				return false;
			}
			appearance->Resample = static_cast<SurfaceResampleMode>(ordinal);
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
	// where the name is written - so the hot path never sees a string.
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
	// same way - so a filter written from a script round-trips exactly and
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
		// it - which is exactly how it was found.
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
			// behind when the table filled part way through - a redirected
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
					ENGINE_WARN(
						"tag '{}' does not fit the world's tag table; the filter is unchanged", entry
					);
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
	// an attachment, and the two would disagree the moment the parent moved -
	// which is exactly the state `GuiObject`'s absolutes are read-only to
	// prevent.
	//
	// **A getter that walked to the parent was tried and is what the derived
	// field replaced.** It is one `CFrame` product per read, and a beam reads
	// four of them per frame - but the cost is not the argument. The argument
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
	// `BasePart.CollisionShape`: which shape the collider is, as an enum.
	//
	// **Named `CollisionShape` rather than `Shape`**, because a part already
	// has a *drawn* shape in every engine a script author has used and the
	// two are not the same thing - `scene::Bounds` is what it is drawn at
	// and `scene::Collider` is what it collides as. A single `Shape` would
	// read as both and be neither.
	//
	// **The whole set, including the two that need geometry.** `Hull` and
	// `Mesh` are only meaningful with a `CollisionGeometry` beside them, and
	// a shape set to one with no geometry collides as the part's bound -
	// which `scene::Collider::Geometry` states and which is visible in the
	// studio's collider outlines. Hiding them here would leave the only way
	// to reach them a C++ call.
	//
	// @since v0.17
	PropertyDescriptor CollisionShapeProperty() {
		PropertyDescriptor property;
		property.Name = core::Name("CollisionShape");
		property.Type = PropertyType::Enum;
		property.EnumName = ShapeKindEnum();
		property.Size = sizeof(core::Name);
		property.Kind = PropertyKind::Field;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Collider>()});
		property.Writes = property.Reads;

		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			const Collider *collider = store.Get<Collider>(instance);
			if (collider == nullptr) {
				return false;
			}
			*static_cast<core::Name *>(out) =
				ecs::EnumTable::MemberAt(ShapeKindEnum(), static_cast<size_t>(collider->Shape));
			return true;
		};

		property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
			Collider *collider = store.GetMutable<Collider>(instance);
			if (collider == nullptr) {
				return false;
			}

			size_t ordinal = 0;
			if (!ecs::EnumTable::OrdinalOf(
					ShapeKindEnum(), *static_cast<const core::Name *>(value), ordinal
				)) {
				return false;
			}

			collider->Shape = static_cast<ShapeKind>(ordinal);
			return true;
		};

		return property;
	}

	// `BasePart.CollisionGeometry`: which baked shape the collider names.
	//
	// **A name, and the same name a `Visual::Mesh` is.** Whoever loads
	// content bakes a hull or a triangle soup into `scene::CollisionShapes`
	// under a name, and this is how a part says which one it is. Unread for
	// the three shapes an extent describes.
	//
	// **Generated rather than hand-written would have been enough** -
	// `Collider::Geometry` is a plain `core::Name` field - except that a
	// generated property is named after the field, and `Geometry` on a
	// `BasePart` reads as the thing it is drawn as. The one-word difference
	// is worth the twenty lines.
	//
	// @since v0.17
	PropertyDescriptor CollisionGeometryProperty() {
		PropertyDescriptor property;
		property.Name = core::Name("CollisionGeometry");
		property.Type = PropertyType::Name;
		property.Size = sizeof(core::Name);
		property.Kind = PropertyKind::Field;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Collider>()});
		property.Writes = property.Reads;

		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			const Collider *collider = store.Get<Collider>(instance);
			if (collider == nullptr) {
				return false;
			}
			*static_cast<core::Name *>(out) = collider->Geometry;
			return true;
		};

		property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
			Collider *collider = store.GetMutable<Collider>(instance);
			if (collider == nullptr) {
				return false;
			}

			// **Any name is accepted, including one nothing has baked.** A
			// scene is authored before its content finishes streaming and a
			// setter that refused an unresolved name would make the order
			// the two arrive in load-bearing. The part collides as its bound
			// until the shape appears.
			collider->Geometry = *static_cast<const core::Name *>(value);
			return true;
		};

		return property;
	}

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

	// How much life is left, clamped into the range the humanoid allows.
	//
	// **Written rather than a `ClampedProperty`, because the ceiling is
	// another field.** `ClampedProperty` bakes its bounds in as template
	// arguments - that is what keeps its setter captureless - and the bound
	// here is `MaxHealth`, which a game sets. So the conversion is written,
	// which is what `PropertyKind::Computed` is for.
	//
	// **Writable, and the authority question is not this descriptor's to
	// answer.** `Store::SetProperty` refuses every write in a replica, so a
	// client script asking for more health is told so by name; declaring this
	// read-only instead would also stop the *server* script that wants to kill
	// somebody, which is the ordinary way a game does it.
	PropertyDescriptor HealthProperty() {
		PropertyDescriptor property;
		property.Name = core::Name("Health");
		property.Type = PropertyType::Float;
		property.Size = sizeof(float);
		property.Kind = PropertyKind::Computed;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Humanoid>()});
		property.Writes = property.Reads;

		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			const Humanoid *humanoid = store.Get<Humanoid>(instance);
			if (humanoid == nullptr) {
				return false;
			}
			*static_cast<float *>(out) = humanoid->Health;
			return true;
		};

		property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
			Humanoid *humanoid = store.GetMutable<Humanoid>(instance);
			if (humanoid == nullptr) {
				return false;
			}

			// Clamped rather than refused, which is `ClampedProperty`'s own
			// argument: a health bar driven off a falling number overshoots
			// at the bottom, and refusing the write would leave a character
			// alive on the frame the game meant to kill them.
			humanoid->Health = std::clamp(*static_cast<const float *>(value), 0.0f, humanoid->MaxHealth);
			return true;
		};

		return property;
	}

	// The ceiling, which takes the current health down with it.
	//
	// **The engine's own reader for `MaxHealth`, and the reason it is not a
	// plain clamped field.** A property nothing in the engine consults is the
	// objection `docs/DEFERRED.md` D00119 held a whole class back for - so the
	// ceiling means something here rather than only in whatever bar a game
	// draws: `Health` clamps against it, and lowering it below the current
	// health pulls that down too. Roblox does the same, and the alternative
	// leaves a humanoid reporting more life than it is allowed to have with
	// nothing able to say which of the two numbers is wrong.
	PropertyDescriptor MaxHealthProperty() {
		PropertyDescriptor property;
		property.Name = core::Name("MaxHealth");
		property.Type = PropertyType::Float;
		property.Size = sizeof(float);
		property.Kind = PropertyKind::Computed;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Humanoid>()});
		property.Writes = property.Reads;

		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			const Humanoid *humanoid = store.Get<Humanoid>(instance);
			if (humanoid == nullptr) {
				return false;
			}
			*static_cast<float *>(out) = humanoid->MaxHealth;
			return true;
		};

		property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
			Humanoid *humanoid = store.GetMutable<Humanoid>(instance);
			if (humanoid == nullptr) {
				return false;
			}

			// **A zero ceiling is a dead character rather than a refusal**,
			// which follows from the clamp below rather than being a second
			// rule: `MaxHealth = 0` is how Roblox spells "cannot be alive",
			// and `IsDead` reads the health it produces.
			humanoid->MaxHealth = std::max(*static_cast<const float *>(value), 0.0f);
			humanoid->Health = std::min(humanoid->Health, humanoid->MaxHealth);
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

}
