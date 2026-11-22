#pragma once

#include <engine/ecs/Classes.hpp>

#include <cassert>
#include <string_view>

namespace engine::scene {
	// Tags every property a class declares with one property group.
	inline void TagDeclaredProperties(ecs::ClassId owner, core::Name tag) {
		const auto properties = ecs::Classes::Describe(owner).Properties;
		for (const ecs::PropertyDescriptor &property : properties) {
			(void)ecs::Classes::SetPropertiesTag(owner, property.Spelling, tag);
		}
	}

	// Tags one declared property with a group. The property must exist on the owner.
	inline void TagProperty(ecs::ClassId owner, std::string_view property, std::string_view tag) {
		const bool tagged = ecs::Classes::SetPropertiesTag(owner, property, core::Name(tag));
		if (!tagged) {
			assert(tagged && "property group must name a declaration on its owner class");
		}
	}
}
