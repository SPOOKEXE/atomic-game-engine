#pragma once

// Shared value controls for the property and component inspectors.
#include <engine/ecs/Schema.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/game/Values.hpp>

namespace studio {
	float StepFor(float value);
	bool DrawReference(const engine::ecs::Store &store, engine::ecs::Entity &reference);
	bool DrawTransform(engine::core::CFrame &frame);
	bool DrawSchemaValue(
		const engine::ecs::Store &store,
		const engine::ecs::FieldDescriptor &field,
		engine::game::PropertyValue &value,
		std::string &draft
	);
}
