#include "Modal.hpp"

#include <engine/ecs/Components.hpp>

namespace engine::gui {
	void RegisterModalComponents() {
		ecs::Components::Register<ModalScope>("gui.ModalScope");
	}
}
