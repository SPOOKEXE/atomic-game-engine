#pragma once

// Bounded file adapter for the canonical authored game UI document.

#include <engine/gui/Document.hpp>

#include <filesystem>

namespace engine::ecs {
	class Store;
}
namespace engine::world {
	struct WorldId;
}

namespace studio {
	class CommandLog;

	// Reads and validates a complete document before replacing `out`.
	bool ReadUiDocumentFile(
		const std::filesystem::path &path, engine::gui::UiDocument &out, engine::gui::DocumentReport &report
	);

	// Encodes first, then replaces the destination through a temporary file.
	bool WriteUiDocumentFile(
		const std::filesystem::path &path,
		const engine::gui::UiDocument &document,
		engine::gui::DocumentReport &report
	);

	// Called inside Universe::Enter while one Studio recording is open. Attaches
	// imported roots and records the complete canonical document for atomic undo.
	bool ImportUiDocumentEdit(
		engine::ecs::Store &store,
		engine::world::WorldId world,
		engine::ecs::Entity selectedParent,
		const engine::gui::UiDocument &document,
		CommandLog &commands,
		std::vector<engine::ecs::Entity> &roots,
		engine::gui::DocumentReport &report
	);
}
