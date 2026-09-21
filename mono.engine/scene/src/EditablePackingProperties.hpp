#pragma once

#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Property.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/EditablePacking.hpp>

#include <array>
#include <cstdint>

namespace engine::scene::detail {
	enum class PackingField : uint8_t { Format, Attributes, Minimum, Maximum };

	template <
		class Component,
		bool (*SetPacking)(ecs::Store &, ecs::Entity, const EditablePacking &),
		PackingField Field>
	ecs::PropertyDescriptor PackingProperty(const char *name) {
		ecs::PropertyDescriptor property;
		property.Name = core::Name(name);
		property.Kind = ecs::PropertyKind::Field;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Component>()});
		property.Writes = property.Reads;
		if constexpr (Field == PackingField::Format) {
			property.Type = ecs::PropertyType::Name;
			property.Size = sizeof(core::Name);
		} else if constexpr (Field == PackingField::Attributes) {
			property.Type = ecs::PropertyType::Int32;
			property.Size = sizeof(int32_t);
		} else {
			property.Type = ecs::PropertyType::Float;
			property.Size = sizeof(float);
		}
		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) {
			const Component *component = store.Get<Component>(instance);
			if (component == nullptr) return false;
			if constexpr (Field == PackingField::Format)
				*static_cast<core::Name *>(out) =
					core::Name(EditablePackingFormatName(component->Packing.Format));
			else if constexpr (Field == PackingField::Attributes)
				*static_cast<int32_t *>(out) = component->Packing.Attributes;
			else if constexpr (Field == PackingField::Minimum)
				*static_cast<float *>(out) = component->Packing.Minimum;
			else
				*static_cast<float *>(out) = component->Packing.Maximum;
			return true;
		};
		property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) {
			const Component *component = store.Get<Component>(instance);
			if (component == nullptr) return false;
			EditablePacking packing = component->Packing;
			if constexpr (Field == PackingField::Format) {
				if (!ParseEditablePackingFormat(
						static_cast<const core::Name *>(value)->Text(), packing.Format
					))
					return false;
			} else if constexpr (Field == PackingField::Attributes) {
				const int32_t attributes = *static_cast<const int32_t *>(value);
				if (attributes < 0 || attributes > UINT8_MAX) return false;
				packing.Attributes = static_cast<uint8_t>(attributes);
			} else if constexpr (Field == PackingField::Minimum)
				packing.Minimum = *static_cast<const float *>(value);
			else
				packing.Maximum = *static_cast<const float *>(value);
			return SetPacking(store, instance, packing);
		};
		return property;
	}

	template <class Component, bool (*SetPacking)(ecs::Store &, ecs::Entity, const EditablePacking &)>
	std::array<ecs::PropertyDescriptor, 4> PackingProperties() {
		return {
			PackingProperty<Component, SetPacking, PackingField::Format>("PackingFormat"),
			PackingProperty<Component, SetPacking, PackingField::Attributes>("PackingAttributes"),
			PackingProperty<Component, SetPacking, PackingField::Minimum>("PackingMinimum"),
			PackingProperty<Component, SetPacking, PackingField::Maximum>("PackingMaximum"),
		};
	}
}
