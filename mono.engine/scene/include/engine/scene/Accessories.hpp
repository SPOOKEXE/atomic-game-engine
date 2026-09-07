#pragma once

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Entity.hpp>

#include <cstddef>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {
	// The matched points of a single-handle accessory. The hierarchy records
	// whether it is equipped; the handle is carried by CharacterLimb, not a joint.
	struct Accessory {
		ecs::Entity HandleAttachment = ecs::NULL_ENTITY;
		ecs::Entity CharacterAttachment = ecs::NULL_ENTITY;
	};

	// The Model-derived class with a BasePart child named Handle.
	ecs::ClassId AccessoryClass();

	// Matches one uniquely named handle attachment to a point on a character's
	// root or direct limb. Refuses ambiguous matches and replica writes without
	// reparenting or changing physical state. Existing local offsets are retained.
	bool EquipAccessory(ecs::Store &store, ecs::Entity character, ecs::Entity accessory);

	// Rebuilds carried offsets before PoseCharacters, including script reparents.
	// Detached handles regain Motion only when Simulated; dead references release
	// the handle at its last pose. Does not reparent or destroy instances.
	size_t UpdateAccessoryAttachments(ecs::Store &store);
}
