// The properties `RegisterTree` declares on each class where its component
// arrives, and the descriptor templates only they use: authored affordances,
// render feature masks, levels of detail, PBR channels and render effects.

#include "PartInternal.hpp"

namespace engine::scene::part_detail {

	template <auto Member, PropertyType Type>
	PropertyDescriptor AuthoredAffordanceProperty(std::string_view name) {
		using Field = std::remove_cvref_t<decltype(AuthoredAffordance{}.*Member)>;
		using Value = std::conditional_t<Type == PropertyType::Enum, core::Name, Field>;
		PropertyDescriptor property;
		property.Name = core::Name(name);
		property.Type = Type;
		property.Size = sizeof(Value);
		property.Kind = PropertyKind::Structural;
		if constexpr (Type == PropertyType::Enum) property.EnumName = AuthoredAffordanceKindEnum();
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<AuthoredAffordance>()});
		property.Writes = property.Reads;
		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			if (!store.Alive(instance)) return false;
			const auto *existing = store.Get<AuthoredAffordance>(instance);
			const auto value = existing ? existing->*Member : AuthoredAffordance{}.*Member;
			if constexpr (Type == PropertyType::Enum)
				*static_cast<Value *>(out) =
					ecs::EnumTable::MemberAt(AuthoredAffordanceKindEnum(), static_cast<size_t>(value));
			else
				*static_cast<Value *>(out) = value;
			return true;
		};
		property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
			if (!store.Alive(instance)) return false;
			AuthoredAffordance authored;
			if (const auto *existing = store.Get<AuthoredAffordance>(instance)) authored = *existing;
			if constexpr (Type == PropertyType::Enum) {
				size_t ordinal = 0;
				if (!ecs::EnumTable::OrdinalOf(
						AuthoredAffordanceKindEnum(), *static_cast<const Value *>(value), ordinal
					))
					return false;
				authored.*Member = static_cast<Field>(ordinal);
			} else
				authored.*Member = *static_cast<const Value *>(value);
			if (!authored.Id.IsValid() && authored.Kind == AuthoredAffordanceKind::None && !authored.Enabled)
				store.Remove<AuthoredAffordance>(instance);
			else
				store.Set(instance, authored);
			return true;
		};
		return property;
	}

	template <class Component, auto Policy, auto Mask>
	PropertyDescriptor RenderFeatureMaskProperty(const char *name) {
		PropertyDescriptor property;
		property.Name = core::Name(name);
		property.Type = PropertyType::Int32;
		property.Size = sizeof(uint32_t);
		property.Kind = PropertyKind::Computed;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Component>()});
		property.Writes = property.Reads;
		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			const Component *component = store.Get<Component>(instance);
			if (component == nullptr) {
				return false;
			}
			*static_cast<uint32_t *>(out) = ((component->*Policy).*Mask) & ALL_RENDER_FEATURES;
			return true;
		};
		property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
			Component *component = store.GetMutable<Component>(instance);
			if (component == nullptr) {
				return false;
			}
			(component->*Policy).*Mask = *static_cast<const uint32_t *>(value) & ALL_RENDER_FEATURES;
			return true;
		};
		return property;
	}

	template <class Component, size_t Level> PropertyDescriptor LodMeshProperty(const char *name) {
		static_assert(Level > 0 && Level < LOD_LEVELS);
		PropertyDescriptor property;
		property.Name = core::Name(name);
		property.Type = PropertyType::Name;
		property.Size = sizeof(core::Name);
		property.Kind = PropertyKind::Structural;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Component>()});
		property.Writes = property.Reads;
		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			const Component *lod = store.Get<Component>(instance);
			*static_cast<core::Name *>(out) = lod == nullptr ? core::Name{} : lod->Meshes[Level - 1];
			return true;
		};
		property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
			Component lod;
			if (const Component *existing = store.Get<Component>(instance)) {
				lod = *existing;
			}
			lod.Meshes[Level - 1] = *static_cast<const core::Name *>(value);
			if (lod.Meshes[Level - 1].IsValid()) {
				lod.Levels = std::max<uint8_t>(lod.Levels, static_cast<uint8_t>(Level + 1));
			}
			store.Set(instance, lod);
			return true;
		};
		return property;
	}

	template <class Component, size_t Level> PropertyDescriptor LodRatioProperty(const char *name) {
		static_assert(Level > 0 && Level < LOD_LEVELS);
		// Automatic LOD uses this as the retained source-triangle fraction for
		// decimation. It is not a distance threshold.
		PropertyDescriptor property;
		property.Name = core::Name(name);
		property.Type = PropertyType::Float;
		property.Size = sizeof(float);
		property.Kind = PropertyKind::Structural;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Component>()});
		property.Writes = property.Reads;
		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			const Component *lod = store.Get<Component>(instance);
			*static_cast<float *>(out) = lod == nullptr ? 0.0f : lod->Ratios[Level - 1];
			return true;
		};
		property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
			Component lod;
			if (const Component *existing = store.Get<Component>(instance)) {
				lod = *existing;
			}
			lod.Ratios[Level - 1] = std::clamp(*static_cast<const float *>(value), 0.0f, 1.0f);
			store.Set(instance, lod);
			return true;
		};
		return property;
	}

	template <class Component> PropertyDescriptor LodTargetQuadAreaProperty(const char *name) {
		PropertyDescriptor property;
		property.Name = core::Name(name);
		property.Type = PropertyType::Float;
		property.Size = sizeof(float);
		property.Kind = PropertyKind::Structural;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Component>()});
		property.Writes = property.Reads;
		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			const Component *lod = store.Get<Component>(instance);
			*static_cast<float *>(out) = lod == nullptr ? 0.0f : lod->TargetQuadArea;
			return true;
		};
		property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
			Component lod;
			if (const Component *existing = store.Get<Component>(instance)) {
				lod = *existing;
			}
			lod.TargetQuadArea = std::max(*static_cast<const float *>(value), 0.0f);
			store.Set(instance, lod);
			return true;
		};
		return property;
	}

	template <class Component> PropertyDescriptor LodBillboardProperty(const char *name) {
		PropertyDescriptor property;
		property.Name = core::Name(name);
		property.Type = PropertyType::Name;
		property.Size = sizeof(core::Name);
		property.Kind = PropertyKind::Structural;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Component>()});
		property.Writes = property.Reads;
		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			const Component *lod = store.Get<Component>(instance);
			*static_cast<core::Name *>(out) = lod == nullptr ? core::Name{} : lod->Billboard;
			return true;
		};
		property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
			Component lod;
			if (const Component *existing = store.Get<Component>(instance)) {
				lod = *existing;
			}
			lod.Billboard = *static_cast<const core::Name *>(value);
			store.Set(instance, lod);
			return true;
		};
		return property;
	}

	template <size_t Level> PropertyDescriptor LodDistanceProperty(const char *name) {
		static_assert(Level > 0 && Level < LOD_LEVELS);
		PropertyDescriptor property;
		property.Name = core::Name(name);
		property.Type = PropertyType::Float;
		property.Size = sizeof(float);
		property.Kind = PropertyKind::Structural;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<LODSettings>()});
		property.Writes = property.Reads;
		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			const LODSettings *settings = store.Get<LODSettings>(instance);
			*static_cast<float *>(out) = settings == nullptr ? 0.0f : settings->MinimumDistances[Level - 1];
			return true;
		};
		property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
			LODSettings settings;
			if (const LODSettings *existing = store.Get<LODSettings>(instance)) {
				settings = *existing;
			}
			const float distance = *static_cast<const float *>(value);
			settings.MinimumDistances[Level - 1] = std::isfinite(distance) ? std::max(distance, 0.0f) : 0.0f;
			store.Set(instance, settings);
			return true;
		};
		return property;
	}

	template <class Component> PropertyDescriptor LodLevelsProperty(const char *name) {
		PropertyDescriptor property;
		property.Name = core::Name(name);
		property.Type = PropertyType::Int32;
		property.Size = sizeof(int32_t);
		property.Kind = PropertyKind::Structural;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Component>()});
		property.Writes = property.Reads;
		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			const Component *lod = store.Get<Component>(instance);
			*static_cast<int32_t *>(out) = lod == nullptr ? 1 : lod->Levels;
			return true;
		};
		property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
			Component lod;
			if (const Component *existing = store.Get<Component>(instance)) {
				lod = *existing;
			}
			lod.Levels = static_cast<uint8_t>(
				std::clamp(*static_cast<const int32_t *>(value), 1, static_cast<int32_t>(LOD_LEVELS))
			);
			store.Set(instance, lod);
			return true;
		};
		return property;
	}

	// `MeshPart.SubsurfaceProfile`: a skin shading profile id, 0 to 255.
	PropertyDescriptor SubsurfaceProfileProperty() {
		PropertyDescriptor property;
		property.Name = core::Name("SubsurfaceProfile");
		property.Type = PropertyType::Int32;
		property.Size = sizeof(int32_t);
		property.Kind = PropertyKind::Field;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<SurfaceAppearance>()});
		property.Writes = property.Reads;
		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			const SurfaceAppearance *appearance = store.Get<SurfaceAppearance>(instance);
			if (appearance == nullptr) return false;
			*static_cast<int32_t *>(out) = appearance->SubsurfaceProfile;
			return true;
		};
		property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
			const int32_t profile = *static_cast<const int32_t *>(value);
			if (profile < 0 || profile > 255) return false;
			SurfaceAppearance *appearance = store.GetMutable<SurfaceAppearance>(instance);
			if (appearance == nullptr) return false;
			appearance->SubsurfaceProfile = static_cast<uint8_t>(profile);
			return true;
		};
		return property;
	}

	// Packed PBR channels remain bytes in the draw column, but scripts carry
	// integers. Keeping that conversion here gives every binding the same
	// four-byte value and reserves 255 as the explicit "not packed" marker.
	template <uint8_t SurfaceAppearance::*Channel>
	PropertyDescriptor PackedPbrChannelProperty(const char *name) {
		PropertyDescriptor property;
		property.Name = core::Name(name);
		property.Type = PropertyType::Int32;
		property.Size = sizeof(int32_t);
		property.Kind = PropertyKind::Field;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<SurfaceAppearance>()});
		property.Writes = property.Reads;
		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			const SurfaceAppearance *appearance = store.Get<SurfaceAppearance>(instance);
			if (appearance == nullptr) {
				return false;
			}
			*static_cast<int32_t *>(out) = appearance->*Channel;
			return true;
		};
		property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
			const int32_t channel = *static_cast<const int32_t *>(value);
			if (channel < 0 || (channel > 3 && channel != 255)) {
				return false;
			}
			SurfaceAppearance *appearance = store.GetMutable<SurfaceAppearance>(instance);
			if (appearance == nullptr) {
				return false;
			}
			appearance->*Channel = static_cast<uint8_t>(channel);
			return true;
		};
		return property;
	}

	template <float SurfaceAppearance::*Factor, int Minimum = 0, int Maximum = 1>
	PropertyDescriptor BoundedSurfaceFactorProperty(const char *name) {
		PropertyDescriptor property;
		property.Name = core::Name(name);
		property.Type = PropertyType::Float;
		property.Size = sizeof(float);
		property.Kind = PropertyKind::Field;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<SurfaceAppearance>()});
		property.Writes = property.Reads;
		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			const SurfaceAppearance *appearance = store.Get<SurfaceAppearance>(instance);
			if (appearance == nullptr) return false;
			*static_cast<float *>(out) = appearance->*Factor;
			return true;
		};
		property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
			const float factor = *static_cast<const float *>(value);
			if (!std::isfinite(factor) || factor < float(Minimum) || factor > float(Maximum)) return false;
			SurfaceAppearance *appearance = store.GetMutable<SurfaceAppearance>(instance);
			if (appearance == nullptr) return false;
			appearance->*Factor = factor;
			return true;
		};
		return property;
	}

	PropertyDescriptor AutoLodStrategyProperty() {
		PropertyDescriptor property;
		property.Name = core::Name("AutoLodStrategy");
		property.Type = PropertyType::Enum;
		property.EnumName = AutoMeshLodStrategyEnum();
		property.Size = sizeof(core::Name);
		property.Kind = PropertyKind::Structural;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<LODAuto>()});
		property.Writes = property.Reads;
		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			const LODAuto *lod = store.Get<LODAuto>(instance);
			const size_t ordinal = lod != nullptr && lod->Strategy == LodStrategy::Reduced ? 1 : 0;
			*static_cast<core::Name *>(out) = ecs::EnumTable::MemberAt(AutoMeshLodStrategyEnum(), ordinal);
			return true;
		};
		property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
			size_t ordinal = 0;
			if (!ecs::EnumTable::OrdinalOf(
					AutoMeshLodStrategyEnum(), *static_cast<const core::Name *>(value), ordinal
				)) {
				return false;
			}
			LODAuto lod;
			if (const LODAuto *existing = store.Get<LODAuto>(instance)) {
				lod = *existing;
			}
			lod.Strategy = ordinal == 0 ? LodStrategy::Decimated : LodStrategy::Reduced;
			store.Set(instance, lod);
			return true;
		};
		return property;
	}

	template <size_t Slot, RenderEffectStage Stage>
	PropertyDescriptor RenderEffectNodeProperty(const char *name) {
		static_assert(Slot < MAX_RENDER_EFFECT_ATTACHMENTS);
		PropertyDescriptor property;
		property.Name = core::Name(name);
		property.Type = PropertyType::Name;
		property.Size = sizeof(core::Name);
		property.Kind = PropertyKind::Structural;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<RenderEffects>()});
		property.Writes = property.Reads;
		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			const RenderEffects *effects = store.Get<RenderEffects>(instance);
			*static_cast<core::Name *>(out) =
				effects == nullptr ? core::Name{} : effects->Attachments[Slot].Node;
			return true;
		};
		property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
			RenderEffects effects;
			if (const RenderEffects *existing = store.Get<RenderEffects>(instance)) {
				effects = *existing;
			}
			RenderEffectAttachment &attachment = effects.Attachments[Slot];
			attachment.Node = *static_cast<const core::Name *>(value);
			attachment.Stage = Stage;
			attachment.Enabled = attachment.Node.IsValid();
			effects.Count = std::max<uint8_t>(effects.Count, static_cast<uint8_t>(Slot + 1));
			store.Set(instance, effects);
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
		// unused and reads back as zero, which is the honest shape - a
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
				// **The component is removed, and the write reports success.**
				// A script shrinking a mirror below one texel loses the
				// mirror, which is right and is indistinguishable from a
				// mirror that never worked.
				ENGINE_DEBUG(
					"surface size {}x{} is below one texel; the SurfaceCamera on {} is removed",
					size.X,
					size.Y,
					instance.Id
				);
				store.Remove<SurfaceCamera>(instance);
				return true;
			}

			// Clamped rather than refused. A texture larger than any device
			// allows fails at creation with a driver message nobody can act
			// on; sixteen thousand is past every limit worth having and
			// small enough to allocate.
			//
			// **Read-modify-write, so a resize keeps the rest of the
			// component.** A fresh `SurfaceCamera` here silently reset
			// `Face`, `Effect`, `TagFilter` and `ImageTransparency` on
			// every size edit - a mirror that lost its thermal grade the
			// moment somebody dragged its resolution.
			SurfaceCamera surface;
			if (const SurfaceCamera *existing = store.Get<SurfaceCamera>(instance)) {
				surface = *existing;
			}
			surface.Width = static_cast<uint16_t>(std::min(size.X, 16384.0f));
			surface.Height = static_cast<uint16_t>(std::min(size.Y, 16384.0f));
			store.Set(instance, surface);
			return true;
		};
		return property;
	}

	// Declares the properties of every class `RegisterTree` registered.
	void RegisterClassProperties(const PartClasses &classes) {
		const ecs::ClassId pvInstance = classes.pvInstance;
		const ecs::ClassId vectorField2D = classes.vectorField2D;
		const ecs::ClassId vectorField3D = classes.vectorField3D;
		const ecs::ClassId gpuParticleFieldClass = classes.gpuParticleFieldClass;
		const ecs::ClassId basePart = classes.basePart;
		const ecs::ClassId spawnLocation = classes.spawnLocation;
		const ecs::ClassId toolClass = classes.toolClass;
		const ecs::ClassId meshPart = classes.meshPart;
		const ecs::ClassId cameraClass = classes.cameraClass;
		const ecs::ClassId surfaceCameraClass = classes.surfaceCameraClass;
		const ecs::ClassId portalClass = classes.portalClass;
		const ecs::ClassId soundClass = classes.soundClass;
		const ecs::ClassId attachmentClass = classes.attachmentClass;
		const ecs::ClassId materialClass = classes.materialClass;
		const ecs::ClassId humanoidClass = classes.humanoidClass;
		const ecs::ClassId lightClass = classes.lightClass;
		const ecs::ClassId spotLightClass = classes.spotLightClass;
		const ecs::ClassId surfaceLightClass = classes.surfaceLightClass;
		const ecs::ClassId stringValue = classes.stringValue;
		const ecs::ClassId localizationTable = classes.localizationTable;
		const ecs::ClassId boolValue = classes.boolValue;
		const ecs::ClassId cframeValue = classes.cframeValue;
		const ecs::ClassId color3Value = classes.color3Value;
		const ecs::ClassId intValue = classes.intValue;
		const ecs::ClassId numberValue = classes.numberValue;
		const ecs::ClassId objectValue = classes.objectValue;
		const ecs::ClassId vector3Value = classes.vector3Value;

		// --- properties, declared where the component arrives ------------
		//
		// Each on the class that first holds what it projects, so a derived
		// class inherits it and `Classes` merges base-first. Declaring them
		// all on `Part` would work today and would be wrong the moment a
		// second `BasePart` subclass exists.

		// Everything with a place in the world has these three, and all
		// three project one `Transform` - which is exactly the fan-out
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
		// was wrong - it breaks every scripted animation in the engine.
		// `PlaceInstance` carries the refutation and the fix.
		ecs::Classes::Computed(pvInstance, CFrameProperty());
		ecs::Classes::Computed(pvInstance, PositionProperty());
		ecs::Classes::Computed(pvInstance, OrientationProperty());

		// **Roblox's `PivotOffset`, on `PVInstance` rather than on
		// `BasePart`.** Roblox declares it on the latter; here the component
		// is on the former, and a `Model` - when there is one - wants the
		// same field for the same reason. Declaring it where the storage is
		// keeps one answer to "what has a pivot".
		ecs::Classes::Property<&Pivot::Offset>(pvInstance, "PivotOffset");

		ecs::Classes::Property<&GpuParticleField::Enabled>(gpuParticleFieldClass, "Enabled");
		ecs::Classes::Property<&GpuParticleSource::Source>(gpuParticleFieldClass, "Source");
		ecs::Classes::Property<&GpuParticleSource::Scale>(gpuParticleFieldClass, "SourceScale");
		ecs::Classes::Computed(gpuParticleFieldClass, GpuParticleLayersProperty());
		ecs::Classes::Property<&GpuParticleField::RequestedCount>(gpuParticleFieldClass, "RequestedCount");
		ecs::Classes::Property<&GpuParticleField::Seed>(gpuParticleFieldClass, "Seed");
		ecs::Classes::Property<&GpuParticleField::HalfExtent>(gpuParticleFieldClass, "Bounds");
		ecs::Classes::ClampedProperty<&GpuParticleField::VelocityResponse, 0.0f, 1000.0f>(
			gpuParticleFieldClass, "VelocityResponse"
		);
		ecs::PropertyDescriptor definition;
		definition.Name = core::Name("Definition");
		definition.Type = PropertyType::String;
		definition.Size = sizeof(std::string);
		definition.Scriptable = false;
		definition.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<GpuParticleField>()});
		definition.Writes = definition.Reads;
		definition.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) {
			const auto *field = store.Get<GpuParticleField>(instance);
			if (field == nullptr) return false;
			*static_cast<std::string *>(out) = GpuParticleDefinition(*field);
			return true;
		};
		definition.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) {
			const auto *field = store.Get<GpuParticleField>(instance);
			if (field == nullptr || (store.AdoptOnly() && !(ecs::Store::IsPredicted(instance) &&
															ecs::IsClientLocalInstance(store, instance))))
				return false;
			auto candidate = *field;
			if (!ReadGpuParticleDefinition(*static_cast<const std::string *>(value), candidate)) return false;
			store.Set(instance, std::move(candidate));
			return true;
		};
		ecs::Classes::Computed(gpuParticleFieldClass, definition);

		ecs::Classes::Computed(basePart, PartSizeProperty());
		ecs::Classes::Computed(basePart, CanCollideProperty());
		ecs::Classes::Computed(basePart, CanTouchProperty());
		ecs::Classes::Property<&Collider::CanQuery>(basePart, "CanQuery");
		ecs::Classes::Computed(
			basePart, AuthoredAffordanceProperty<&AuthoredAffordance::Id, PropertyType::Name>("AffordanceId")
		);
		ecs::Classes::Computed(
			basePart,
			AuthoredAffordanceProperty<&AuthoredAffordance::Kind, PropertyType::Enum>("AffordanceKind")
		);
		ecs::Classes::Computed(
			basePart,
			AuthoredAffordanceProperty<&AuthoredAffordance::Enabled, PropertyType::Bool>("AffordanceEnabled")
		);
		ecs::Classes::Computed(basePart, AnchoredProperty());
		ecs::Classes::Computed(basePart, KinematicProperty());

		// The plain fields. `Color` is a rename rather than a conversion -
		// `Visual::Tint` is what a script calls `Color`. `Surface::Material`,
		// which is what a part *feels* like, is deliberately not bound: it is
		// a separate fact that happens to share a word with the `Material`
		// instance below, and a mirror-finish floor and a rubber floor may
		// share a surface and never a material.
		ecs::Classes::Property<&Visual::Tint>(basePart, "Color");
		ecs::Classes::Property<&Visual::Visible>(basePart, "Visible");
		ecs::Classes::Computed(
			basePart,
			RenderFeatureMaskProperty<Visual, &Visual::RenderFeatures, &RenderFeaturePolicy::Enable>(
				"RenderFeatureEnableMask"
			)
		);
		ecs::Classes::Computed(
			basePart,
			RenderFeatureMaskProperty<Visual, &Visual::RenderFeatures, &RenderFeaturePolicy::Disable>(
				"RenderFeatureDisableMask"
			)
		);
		ecs::Classes::Computed(
			basePart, RenderEffectNodeProperty<0, RenderEffectStage::Compute>("ComputeEffectNode")
		);
		ecs::Classes::Computed(
			basePart, RenderEffectNodeProperty<1, RenderEffectStage::PostProcess>("PostProcessEffectNode")
		);

		// --- what it is made of ---------------------------------------
		//
		// **Roblox spells this as one `PhysicalProperties` value and this
		// spells it as four fields.** The value type would need a
		// `PropertyType` of its own, a Luau userdata, a wire form and a
		// panel row that edits four numbers inside one cell - for a thing
		// every caller immediately takes apart again. Four properties are
		// four rows a panel already knows how to draw and four names a
		// script already knows how to set.
		//
		// `CustomPhysicalProperties` keeps Roblox's name because it is the
		// one an author looks for, and it is the switch: without it the
		// three below are ignored and the part feels like its material.
		// **What the part collides as, which nothing else on it says.** A
		// part is drawn at its `Bounds` and collides at its `Collider`, and
		// until v0.17 the second of those was unreachable from a script or
		// the properties panel at all - a box was the only collider a part
		// could ever have without a C++ call.
		ecs::Classes::Computed(basePart, CollisionShapeProperty());
		ecs::Classes::Computed(basePart, CollisionGeometryProperty());

		ecs::Classes::Property<&PhysicsProperties::Custom>(basePart, "CustomPhysicalProperties");
		ecs::Classes::Property<&PhysicsProperties::Density>(basePart, "Density");
		ecs::Classes::Property<&PhysicsProperties::Friction>(basePart, "Friction");
		ecs::Classes::Property<&PhysicsProperties::Elasticity>(basePart, "Elasticity");

		// **Drag, under the names the integrator already reads.** A second
		// pair on `PhysicsProperties` would be two places to write one
		// number; these are `RigidBody`'s own fields.
		//
		// **Generated again since v0.15.** They were briefly hand-written to
		// answer a default for a part with no `RigidBody`, because an
		// anchored part had none and `print(part.LinearDamping)` raised
		// `could not read` on an ordinary part. `RigidBody` is on the class
		// now, so there is nothing to fall back to and the generated pair is
		// correct as it stands.
		ecs::Classes::Property<&RigidBody::LinearDamping>(basePart, "LinearDamping");
		ecs::Classes::Property<&RigidBody::AngularDamping>(basePart, "AngularDamping");

		ecs::Classes::Computed(basePart, MassProperty());

		// A zero bound axis is intentionally unbounded. The shared sampler
		// clamps outside a positive extent and turns `Falloff` into a fade
		// distance only when it is positive, keeping an authored hard edge exact.
		ecs::Classes::Property<&VectorField2D::Vector>(vectorField2D, "Vector");
		ecs::Classes::Property<&VectorField2D::HalfExtent>(vectorField2D, "Bounds");
		ecs::Classes::Property<&VectorField2D::Radial>(vectorField2D, "Radial");
		ecs::Classes::Property<&VectorField2D::Tangential>(vectorField2D, "Tangential");
		ecs::Classes::ClampedProperty<&VectorField2D::Falloff, 0.0f, 100000.0f>(vectorField2D, "Falloff");
		ecs::Classes::Property<&VectorField2D::LocalSpace>(vectorField2D, "LocalSpace");

		ecs::Classes::Property<&VectorField3D::Vector>(vectorField3D, "Vector");
		ecs::Classes::Property<&VectorField3D::HalfExtent>(vectorField3D, "Bounds");
		ecs::Classes::Property<&VectorField3D::Axis>(vectorField3D, "Axis");
		ecs::Classes::Property<&VectorField3D::Radial>(vectorField3D, "Radial");
		ecs::Classes::Property<&VectorField3D::Tangential>(vectorField3D, "Tangential");
		ecs::Classes::ClampedProperty<&VectorField3D::Falloff, 0.0f, 100000.0f>(vectorField3D, "Falloff");
		ecs::Classes::Property<&VectorField3D::LocalSpace>(vectorField3D, "LocalSpace");

		// **`Mesh` and `ColorMap` are not here, and that is v0.10's
		// correction.** `BasePart` is what `Part`, `MeshPart` and a future
		// `UnionOperation` *share*, and geometry loaded from a file is not
		// shared by any of them: a `Part` is one of six built-in shapes and
		// naming a mesh on it is a property that does nothing. Offering it
		// is worse than not having it - an author sets it, the part does not
		// change, and nothing says the class was the wrong one. Both are
		// declared on `MeshPart` below, under the names that class actually
		// shows.
		//
		// **The storage is unchanged and stays on `BasePart`.**
		// `SurfaceAppearance`'s own comment carries why: `client::
		// CollectInstances` is a batched parallel walk over a fixed
		// signature, and an optional column is precisely what that shape
		// cannot express. A dense column of mostly-invalid names is sixteen
		// bytes an entity and no branches - where a per-class component
		// would be a join per row per frame. What moved is the *vocabulary*,
		// which is what a properties panel and a script see.
		//
		// A `Part` is textured by a `Material` instance under it, which is
		// what `scene/Materials.hpp` is for.

		// **The alpha pair stays on `BasePart`, and the asymmetry is
		// deliberate.** These say how the alpha of whatever is being sampled
		// is treated, and a plain `Part` samples something the moment it has
		// a `Material` child - a cut-out material on a `Part` needs `Clip`
		// exactly as one on a `MeshPart` does. They are not *content*, which
		// is the line the two above fall on the far side of.
		ecs::Classes::ClampedProperty<&SurfaceAppearance::AlphaCutoff, 0.0f, 1.0f>(basePart, "AlphaCutoff");
		ecs::Classes::Computed(basePart, AlphaModeProperty());
		ecs::Classes::Property<&SurfaceAppearance::Colour>(basePart, "SurfaceColor");
		ecs::Classes::Property<&SurfaceAppearance::EmissiveTint>(basePart, "EmissiveTint");
		ecs::Classes::ClampedProperty<&SurfaceAppearance::EmissiveStrength, 0.0f, 16.0f>(
			basePart, "EmissiveStrength"
		);
		ecs::Classes::Computed(basePart, ResampleModeProperty());

		// **`Transparency` has a field to project onto now**, and it arrived
		// with the sorted pass that makes it mean something rather than
		// ahead of it. A float nothing draws is a field that lies, and it
		// would have sat in a snapshot and a delta being read by nobody.
		//
		// **Not clamped, deliberately.** It was, briefly. Roblox does not
		// clamp this either - `part.Transparency = 2` reads back as 2 - and
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

		// **Read-only through the table and never sent**, for
		// `LocalTransparencyProperty`'s and `scene::LocalTransparency`'s
		// own reasons.
		ecs::Classes::Computed(basePart, LocalTransparencyProperty());

		// **The third of the three, and they are three questions rather
		// than one.** `Visible` decides whether the part is submitted at
		// all, `Transparency` decides which pass it lands in, and this
		// decides whether it occludes the sun. `Visual::CastShadow` carries
		// the whole argument for why collapsing any two of them is wrong.
		ecs::Classes::Property<&Visual::CastShadow>(basePart, "CastShadow");

		// **The one property on a part that only an editor reads.** A
		// locked part still draws, still collides and is still reachable
		// from a script - what it refuses is a pointer pick, which is
		// `Visual::Locked`'s whole surface. Declared here rather than kept
		// in the editor because it is authoring data that has to survive a
		// save, which an editor-side set could not.
		ecs::Classes::Property<&Visual::Locked>(basePart, "Locked");

		// Every leaf spells the member `Value`, while the class table keeps
		// its script type exact. The two text leaves share one component
		// because both carry verbatim, non-interned text.
		for (const ecs::ClassId owner : {stringValue, localizationTable}) {
			ecs::Classes::Property<&TextContent::Value>(owner, "Value");
		}
		ecs::Classes::Property<&BoolValue::Value>(boolValue, "Value");
		ecs::Classes::Property<&CFrameValue::Value>(cframeValue, "Value");
		ecs::Classes::Property<&Color3Value::Value>(color3Value, "Value");
		ecs::Classes::Property<&IntValue::Value>(intValue, "Value");
		ecs::Classes::Property<&NumberValue::Value>(numberValue, "Value");
		ecs::Classes::Property<&ObjectValue::Value>(objectValue, "Value");
		ecs::Classes::Property<&Vector3Value::Value>(vector3Value, "Value");

		// Which surface texture this part shows, or -1 for none. An `int32`
		// rather than a reference to the camera: the renderer indexes a
		// small fixed set, and a handle would have to be resolved back to an
		// index every frame for every part.

		// The last of the two that were named as gaps at v0.5. `Material` was
		// the other and is no longer a property of a part at all: it is an
		// instance under one - see the `Material` class below.
		ecs::Classes::Computed(basePart, CollisionGroupProperty());

		// The camera's own three. `FieldOfView` is **degrees**, because
		// Roblox's is and because `Orientation` already reproduces that same
		// split - the component stores radians and the conversion is where
		// the unit changes, which is the whole shape of a property here.
		// **Roblox's names, and since v0.10 the only names.** These were
		// aliases of `BasePart.Mesh` and `BasePart.ColorMap`; those are gone
		// - see the note where they used to be declared - so a mesh
		// reference and its sheet are named on the one class that has
		// either. One spelling rather than two also ends the trap
		// `studio/Assets.hpp` warned about, where an alias missing from the
		// picker's table gave a plain text field on the name people use.
		//
		// They still project the components `BasePart` holds, which is a
		// storage decision and not a vocabulary one.
		ecs::Classes::Property<&Visual::Mesh>(meshPart, "MeshId");
		ecs::Classes::Property<&SurfaceAppearance::ColourMap>(meshPart, "TextureID");
		ecs::Classes::Property<&SurfaceAppearance::NormalMap>(meshPart, "NormalMap");
		ecs::Classes::Property<&SurfaceAppearance::RoughnessMap>(meshPart, "RoughnessMap");
		ecs::Classes::Property<&SurfaceAppearance::MetalnessMap>(meshPart, "MetalnessMap");
		ecs::Classes::Property<&SurfaceAppearance::OcclusionMap>(meshPart, "OcclusionMap");
		ecs::Classes::Property<&SurfaceAppearance::HeightMap>(meshPart, "HeightMap");
		ecs::Classes::Property<&SurfaceAppearance::EmissiveMap>(meshPart, "EmissiveMap");
		ecs::Classes::Property<&SurfaceAppearance::PackedPbrMap>(meshPart, "PackedPbrMap");
		ecs::Classes::Computed(
			meshPart, BoundedSurfaceFactorProperty<&SurfaceAppearance::SpecularFactor>("SpecularFactor")
		);
		ecs::Classes::Computed(
			meshPart,
			BoundedSurfaceFactorProperty<&SurfaceAppearance::TransmissionFactor>("TransmissionFactor")
		);
		ecs::Classes::Computed(
			meshPart,
			BoundedSurfaceFactorProperty<&SurfaceAppearance::IndexOfRefraction, 1, 3>("IndexOfRefraction")
		);
		ecs::Classes::Computed(
			meshPart, BoundedSurfaceFactorProperty<&SurfaceAppearance::Thickness, 0, 1000>("Thickness")
		);
		ecs::Classes::Property<&SurfaceAppearance::SkinShading>(meshPart, "SkinShading");
		ecs::Classes::Computed(meshPart, SubsurfaceProfileProperty());
		ecs::Classes::Computed(
			meshPart, BoundedSurfaceFactorProperty<&SurfaceAppearance::ScatterRadius, 0, 10>("ScatterRadius")
		);
		ecs::Classes::Property<&SurfaceAppearance::ThicknessMap>(meshPart, "ThicknessMap");
		ecs::Classes::Property<&SurfaceAppearance::SecondaryRoughnessMap>(meshPart, "SecondaryRoughnessMap");
		ecs::Classes::Computed(
			meshPart,
			BoundedSurfaceFactorProperty<&SurfaceAppearance::SecondaryRoughness>("SecondaryRoughness")
		);
		ecs::Classes::Computed(
			meshPart, BoundedSurfaceFactorProperty<&SurfaceAppearance::SecondarySpecular>("SecondarySpecular")
		);
		ecs::Classes::Computed(
			meshPart, PackedPbrChannelProperty<&SurfaceAppearance::RoughnessChannel>("PackedRoughnessChannel")
		);
		ecs::Classes::Computed(
			meshPart, PackedPbrChannelProperty<&SurfaceAppearance::OcclusionChannel>("PackedOcclusionChannel")
		);
		ecs::Classes::Computed(
			meshPart, PackedPbrChannelProperty<&SurfaceAppearance::HeightChannel>("PackedHeightChannel")
		);
		ecs::Classes::Computed(
			meshPart, PackedPbrChannelProperty<&SurfaceAppearance::MetalnessChannel>("PackedMetalnessChannel")
		);
		ecs::Classes::Computed(meshPart, LodMeshProperty<LODCustom, 1>("Lod1MeshId"));
		ecs::Classes::Computed(meshPart, LodMeshProperty<LODCustom, 2>("Lod2MeshId"));
		ecs::Classes::Computed(meshPart, LodMeshProperty<LODCustom, 3>("Lod3MeshId"));
		ecs::Classes::Computed(meshPart, LodRatioProperty<LODAuto, 1>("Lod1Ratio"));
		ecs::Classes::Computed(meshPart, LodRatioProperty<LODAuto, 2>("Lod2Ratio"));
		ecs::Classes::Computed(meshPart, LodRatioProperty<LODAuto, 3>("Lod3Ratio"));
		ecs::Classes::Computed(meshPart, LodTargetQuadAreaProperty<LODAuto>("LodTargetQuadArea"));
		ecs::Classes::Computed(meshPart, LodBillboardProperty<LODCustom>("LodBillboardTexture"));
		ecs::Classes::Computed(meshPart, LodDistanceProperty<1>("Lod1Distance"));
		ecs::Classes::Computed(meshPart, LodDistanceProperty<2>("Lod2Distance"));
		ecs::Classes::Computed(meshPart, LodDistanceProperty<3>("Lod3Distance"));

		for (const auto &property : {
				 LodMeshProperty<LODAuto, 1>("AutoLod1MeshId"),
				 LodMeshProperty<LODAuto, 2>("AutoLod2MeshId"),
				 LodMeshProperty<LODAuto, 3>("AutoLod3MeshId"),
				 LodRatioProperty<LODAuto, 1>("AutoLod1Ratio"),
				 LodRatioProperty<LODAuto, 2>("AutoLod2Ratio"),
				 LodRatioProperty<LODAuto, 3>("AutoLod3Ratio"),
				 LodTargetQuadAreaProperty<LODAuto>("AutoLodTargetQuadArea"),
				 LodBillboardProperty<LODAuto>("AutoLodBillboardTexture"),
				 LodLevelsProperty<LODAuto>("AutoLodLevels"),
				 AutoLodStrategyProperty(),
				 LodMeshProperty<LODCustom, 1>("CustomLod1MeshId"),
				 LodMeshProperty<LODCustom, 2>("CustomLod2MeshId"),
				 LodMeshProperty<LODCustom, 3>("CustomLod3MeshId"),
				 LodRatioProperty<LODCustom, 1>("CustomLod1Ratio"),
				 LodRatioProperty<LODCustom, 2>("CustomLod2Ratio"),
				 LodRatioProperty<LODCustom, 3>("CustomLod3Ratio"),
				 LodTargetQuadAreaProperty<LODCustom>("CustomLodTargetQuadArea"),
				 LodBillboardProperty<LODCustom>("CustomLodBillboardTexture"),
				 LodLevelsProperty<LODCustom>("CustomLodLevels"),
			 }) {
			ecs::Classes::Computed(meshPart, property);
		}

		// Mesh metadata belongs to MeshPart, not every BasePart.
		ecs::Classes::Computed(meshPart, TrianglesCountProperty());

		// The spawn's three, and every one of them has a reader in
		// `scene::FindSpawn`. That is the bar `docs/DEFERRED.md` D00119 set
		// for this class existing at all: a property nothing acts on reads
		// as decided, which is what kept `Player.Team` out of the tree for
		// eight versions.
		//
		// **`TeamColor` is Roblox's spelling and the storage is British**,
		// which is `Visual::Tint` reaching a script as `Color` - the
		// vocabulary crosses and the field name does not.
		ecs::Classes::Property<&SpawnLocation::TeamColour>(spawnLocation, "TeamColor");
		ecs::Classes::Property<&SpawnLocation::Neutral>(spawnLocation, "Neutral");
		ecs::Classes::Property<&SpawnLocation::Enabled>(spawnLocation, "Enabled");

		// **Not a Roblox property, and named so it reads as one anyway.**
		// Roblox has no way to say "spawn here specifically" - the closest
		// is turning every other pad off - so this is an addition rather
		// than a match. `SpawnLocation::Forced` carries the argument.
		ecs::Classes::Property<&SpawnLocation::Forced>(spawnLocation, "Forced");

		// Local cameras may be scripted without granting writes to replicated eyes.
		for (auto property :
			 {CFrameProperty(), PositionProperty(), OrientationProperty(), FieldOfViewProperty()}) {
			property.PredictedWritable = true;
			ecs::Classes::Computed(cameraClass, property);
		}
		ecs::Classes::Computed(cameraClass, CameraSubjectProperty());
		ecs::Classes::Computed(cameraClass, CameraTypeProperty());
		ecs::Classes::Property<&CameraSubject::Automatic>(cameraClass, "CameraSubjectAutomatic");
		ecs::Classes::Property<&Camera::NearPlane>(cameraClass, "NearPlaneZ");
		ecs::Classes::Property<&Camera::FarPlane>(cameraClass, "FarPlaneZ");
		ecs::Classes::Property<&Camera::MaxImageWidth>(cameraClass, "MaxImageWidth");
		ecs::Classes::Property<&Camera::MaxImageHeight>(cameraClass, "MaxImageHeight");
		ecs::Classes::Property<&Camera::ImageWidth>(cameraClass, "ImageWidth");
		ecs::Classes::Property<&Camera::ImageHeight>(cameraClass, "ImageHeight");
		ecs::Classes::Computed(
			cameraClass,
			RenderFeatureMaskProperty<Camera, &Camera::RenderFeatures, &RenderFeaturePolicy::Enable>(
				"RenderFeatureEnableMask"
			)
		);
		ecs::Classes::Computed(
			cameraClass,
			RenderFeatureMaskProperty<Camera, &Camera::RenderFeatures, &RenderFeaturePolicy::Disable>(
				"RenderFeatureDisableMask"
			)
		);
		ecs::Classes::Computed(cameraClass, SurfaceSizeProperty());

		// The surface camera's four. `SurfaceSize` above is inherited, so a
		// `SurfaceCamera` can still be resized like any other.
		ecs::Classes::ClampedProperty<&SurfaceCamera::ImageTransparency, 0.0f, 1.0f>(
			surfaceCameraClass, "ImageTransparency"
		);

		// **How often the surface may redraw, and zero is uncapped.** A
		// plain property rather than a clamped one: a ceiling here would be
		// a second opinion about what a display can do, and the floor is
		// already meaningful - see `SurfaceCamera::FPS`. A negative value
		// is refused where it is read rather than where it is written, so
		// a script that computes one gets the uncapped behaviour instead of
		// a surface that never draws again.
		ecs::Classes::Property<&SurfaceCamera::FPS>(surfaceCameraClass, "FPS");
		ecs::Classes::Computed(surfaceCameraClass, FaceProperty());
		ecs::Classes::Computed(surfaceCameraClass, SurfaceEffectProperty());
		ecs::Classes::Computed(surfaceCameraClass, TagFilterProperty());

		// The portal's authored link and activation state.
		ecs::Classes::Computed(portalClass, DestinationProperty());
		ecs::Classes::Computed(portalClass, DestinationWorldProperty());
		ecs::Classes::Property<&Portal::Enabled>(portalClass, "Enabled");
		ecs::Classes::Property<&Portal::Bidirectional>(portalClass, "Bidirectional");

		// The sound's seven. All plain fields, which is unusual enough here
		// to be worth saying: nothing about a sound is a doubled
		// half-extent or a quaternion in degrees, so there is no conversion
		// to write and no place for one to be wrong in one direction.
		//
		// `SoundId` names a published asset the way `MeshId` does -
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
		ecs::Classes::ClampedProperty<&Sound::Pan, -1.0f, 1.0f>(soundClass, "Pan");
		ecs::Classes::Property<&Sound::Looped>(soundClass, "Looped");
		ecs::Classes::Property<&Sound::Playing>(soundClass, "Playing");
		ecs::Classes::Property<&Sound::RollOffMinDistance>(soundClass, "RollOffMinDistance");
		ecs::Classes::Property<&Sound::RollOffMaxDistance>(soundClass, "RollOffMaxDistance");

		// The attachment's four. `CFrame` is the authored local offset and
		// carries the same name a `PVInstance`'s does - which is correct
		// rather than a collision: on both classes it means "where this thing
		// is, in the terms that thing is placed in", and an attachment is
		// placed relative to its parent.
		ecs::Classes::Property<&Attachment::Frame>(attachmentClass, "CFrame");
		ecs::Classes::Computed(attachmentClass, AttachmentPositionProperty());
		ecs::Classes::Computed(attachmentClass, WorldCFrameProperty());
		ecs::Classes::Computed(attachmentClass, WorldPositionProperty());

		// The material's one. **`MaterialId` rather than `Material`**, which
		// is this tree's spelling for "a name that is an asset" - `MeshId`,
		// `SoundId`, `TextureID` - and is what puts the studio's content
		// picker on it rather than a bare text field, through
		// `studio::ContentKindOfProperty`. Calling it `Material` on a class
		// called `Material` would also read as a self-reference at every call
		// site: `material.Material = ...`.
		ecs::Classes::Property<&MaterialRef::Asset>(materialClass, "MaterialId");

		// **Which shader draws it, and `Shader` rather than `ShaderId`.**
		// The `...Id` spelling above marks a name a *publisher* owns, which
		// is what puts the content picker on it - and this name is not one:
		// it may be a `ShaderScript` sitting in the same world, or a shader
		// the engine ships and no manifest lists. `scene/Shaders.hpp` is the
		// resolution order and why it is that way round.
		ecs::Classes::Property<&MaterialRef::Shader>(materialClass, "Shader");

		// The light's shared properties. Type-specific controls are declared on
		// the two classes that consume them, so a point light never offers an
		// angle or face that it cannot use.
		ecs::Classes::Property<&Light::Colour>(lightClass, "Color");
		ecs::Classes::Property<&Light::Enabled>(lightClass, "Enabled");
		ecs::Classes::Property<&Light::Shadows>(lightClass, "Shadows");

		// Clamped, because all three are quantities with an obvious nearest
		// meaning outside their range - `ClampedProperty`'s own argument. The
		// brightness ceiling is Roblox's; the range ceiling is the one past
		// which a forward renderer's light culling stops rejecting anything.
		ecs::Classes::ClampedProperty<&Light::Brightness, 0.0f, 10000.0f>(lightClass, "Brightness");
		ecs::Classes::ClampedProperty<&Light::Range, 0.0f, 60.0f>(lightClass, "Range");
		for (const ecs::ClassId shaped : {spotLightClass, surfaceLightClass}) {
			ecs::Classes::ClampedProperty<&Light::Angle, 0.0f, 180.0f>(shaped, "Angle");
			ecs::Classes::Computed(shaped, LightFaceProperty());
		}

		// The humanoid's movement fields. Nothing here is a doubled
		// half-extent or an angle in the wrong unit, so there is no conversion
		// to write and no place for one to be wrong in one direction.
		//
		// **`MoveDirection` is writable, which is what makes a scripted
		// character possible.** A game driving an NPC writes it directly and
		// never installs `UpdateCharacterControl`; a player's character has
		// that system writing it every frame instead. One field, two writers,
		// and only one of them installed per character - which is the same
		// shape `MoveCamera` and a scripted camera already have.
		// The root reference makes sibling humanoid rigs portable and cloneable.
		ecs::Classes::Computed(humanoidClass, HumanoidRootPartProperty());
		ecs::Classes::Property<&Humanoid::MoveDirection>(humanoidClass, "MoveDirection");
		ecs::Classes::ClampedProperty<&Humanoid::WalkSpeed, 0.0f, 1000.0f>(humanoidClass, "WalkSpeed");
		ecs::Classes::ClampedProperty<&Humanoid::JumpSpeed, 0.0f, 1000.0f>(humanoidClass, "JumpPower");
		ecs::Classes::ClampedProperty<&Humanoid::Height, 0.1f, 100.0f>(humanoidClass, "HipHeight");
		ecs::Classes::Property<&Humanoid::Enabled>(humanoidClass, "Enabled");
		ecs::Classes::Property<&Humanoid::AutoRotate>(humanoidClass, "AutoRotate");

		// **The two that decide whether a character is alive**, and both are
		// written conversions because each is clamped against the other. See
		// `HealthProperty` for why they are writable at all - the replica
		// refusal that answers "may a client set its own health" lives in
		// `Store::SetProperty` and covers every property here, so a second
		// answer declared on this one would be the copy that drifts.
		ecs::Classes::Computed(humanoidClass, HealthProperty());
		ecs::Classes::Computed(humanoidClass, MaxHealthProperty());

		// **Read-only, because it is what the world found rather than what an
		// author wants.** A script setting `Grounded` would be telling the
		// controller a lie it acts on immediately - the jump would fire in
		// mid-air, which is the exploit this flag exists to gate.
		ecs::Classes::Computed(humanoidClass, GroundedProperty());

		// The tool's one. **`Grip` and not `GripPos`/`GripForward`/the rest
		// of Roblox's six**, because those are the same `CFrame` taken apart
		// into a position and three basis vectors - six properties writing
		// one field, five of which are a rotation somebody has to keep
		// orthogonal by hand. A `CFrame` is a declared property type here and
		// `Attachment.CFrame` already crosses as one.
		ecs::Classes::Property<&Tool::Grip>(toolClass, "Grip");
	}

}
