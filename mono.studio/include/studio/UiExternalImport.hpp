#pragma once

// File-backed UI imports. Parsing completes before these functions create an
// authored instance, so a malformed external file cannot leave a partial edit.

#include <engine/ecs/Entity.hpp>
#include <engine/gui/Document.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace engine::ecs {
	class Store;
}
namespace engine::world {
	struct WorldId;
}

namespace studio {
	class CommandLog;

	// Reads one bounded UTF-8 import source. On failure `out` remains unchanged
	// and `report` carries a file-local reason.
	bool ReadUiExternalImportFile(
		const std::filesystem::path &path, std::string &out, engine::gui::DocumentReport &report
	);

	// Parses a design-token JSON file, then attaches its canonical theme document
	// and records the complete transaction. Called inside Universe::Enter while a
	// Studio recording is open.
	bool ImportUiDesignTokensFileEdit(
		engine::ecs::Store &store,
		engine::world::WorldId world,
		engine::ecs::Entity selectedParent,
		const std::filesystem::path &path,
		CommandLog &commands,
		std::vector<engine::ecs::Entity> &roots,
		engine::gui::DocumentReport &report
	);

	// Parses a localization CSV file before saving its exact source under
	// ReplicatedStorage as a LocalizationTable. The created instance is recorded
	// as one normal Studio create edit, so undo and redo retain project data.
	bool ImportUiLocalizationFileEdit(
		engine::ecs::Store &store,
		engine::world::WorldId world,
		const std::filesystem::path &path,
		CommandLog &commands,
		engine::ecs::Entity &table,
		engine::gui::DocumentReport &report
	);
}
