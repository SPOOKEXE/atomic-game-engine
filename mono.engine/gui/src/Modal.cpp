#include <engine/ecs/Components.hpp>
#include <engine/gui/Modal.hpp>

namespace engine::gui {
	void RegisterModalComponents() {
		ecs::Components::Register<ModalScope>("gui.ModalScope");
	}
}
