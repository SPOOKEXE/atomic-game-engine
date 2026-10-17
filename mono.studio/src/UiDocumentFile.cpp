#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/world/Universe.hpp>

#include <fstream>
#include <studio/Commands.hpp>
#include <studio/UiDocumentFile.hpp>
#include <vector>

namespace studio {
	namespace {
		void FileIssue(
			engine::gui::DocumentReport &report, const std::filesystem::path &path, const char *reason
		) {
			report.Issues.push_back({path.string(), reason, 0});
		}
	}

	bool ReadUiDocumentFile(
		const std::filesystem::path &path, engine::gui::UiDocument &out, engine::gui::DocumentReport &report
	) {
		report = {};
		std::ifstream input(path, std::ios::binary | std::ios::ate);
		if (!input) {
			FileIssue(report, path, "could not open UI document");
			return false;
		}
		const std::streampos length = input.tellg();
		if (length < 0 ||
			static_cast<uint64_t>(length) > engine::gui::DocumentLimits::HARD_MAXIMUM_BINARY_BYTES) {
			FileIssue(report, path, "UI document exceeds byte limit");
			return false;
		}
		std::vector<std::byte> bytes(static_cast<size_t>(length));
		input.seekg(0);
		if (!bytes.empty() && !input.read(reinterpret_cast<char *>(bytes.data()), length)) {
			FileIssue(report, path, "could not read UI document");
			return false;
		}
		engine::core::ByteReader reader(bytes);
		return engine::gui::DecodeDocument(reader, out, report);
	}

	bool WriteUiDocumentFile(
		const std::filesystem::path &path,
		const engine::gui::UiDocument &document,
		engine::gui::DocumentReport &report
	) {
		report = {};
		engine::core::ByteWriter writer(0, engine::gui::DocumentLimits::HARD_MAXIMUM_BINARY_BYTES);
		if (!engine::gui::EncodeDocument(document, writer, report)) return false;
		std::filesystem::path temporary = path;
		temporary += ".tmp";
		{
			std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
			if (!output ||
				!output.write(reinterpret_cast<const char *>(writer.Bytes().data()), writer.Size()) ||
				!output.flush()) {
				FileIssue(report, temporary, "could not write UI document");
				return false;
			}
		}
		std::error_code error;
		std::filesystem::rename(temporary, path, error);
		if (error) {
			std::filesystem::remove(temporary);
			FileIssue(report, path, "could not replace UI document");
			return false;
		}
		return true;
	}

	bool ImportUiDocumentEdit(
		engine::ecs::Store &store,
		engine::world::WorldId world,
		engine::ecs::Entity selectedParent,
		const engine::gui::UiDocument &document,
		CommandLog &commands,
		std::vector<engine::ecs::Entity> &roots,
		engine::gui::DocumentReport &report
	) {
		std::vector<engine::ecs::Entity> createdRoots;
		std::vector<engine::ecs::Entity> themes;
		if (!engine::gui::ImportDocument(store, document, createdRoots, report, {}, &themes)) return false;
		const engine::ecs::Entity starterGui =
			engine::scene::ServiceOf(store, engine::ecs::Classes::Find(engine::core::Name("StarterGui")));
		std::vector<engine::ecs::Entity> parents;
		parents.reserve(createdRoots.size());
		for (const engine::ecs::Entity root : createdRoots) {
			const bool collector = store.IsA(root, engine::gui::GuiClass("LayerCollector"));
			const bool screen = store.IsA(root, engine::gui::GuiClass("ScreenGui"));
			const bool selectedAlive = store.Alive(selectedParent);
			const engine::ecs::Entity parent =
				screen ? starterGui
				: selectedAlive && (collector ? !store.IsA(selectedParent, engine::gui::GuiClass("GuiBase"))
											  : store.IsA(selectedParent, engine::gui::GuiClass("GuiBase")))
					? selectedParent
					: engine::ecs::NULL_ENTITY;
			if (parent != engine::ecs::NULL_ENTITY && store.SetParent(root, parent)) {
				parents.push_back(parent);
				continue;
			}
			for (const engine::ecs::Entity created : createdRoots)
				if (store.Alive(created)) store.DestroyInstance(created);
			for (const engine::ecs::Entity theme : themes)
				if (store.Alive(theme)) store.DestroyInstance(theme);
			report.Issues.push_back({"$", "select a valid parent for the imported UI subtree", 0});
			return false;
		}
		if (!commands.RecordUiDocumentImport(
				store, world, document, createdRoots, themes, parents, "Import UI document"
			)) {
			for (const engine::ecs::Entity created : createdRoots)
				if (store.Alive(created)) store.DestroyInstance(created);
			for (const engine::ecs::Entity theme : themes)
				if (store.Alive(theme)) store.DestroyInstance(theme);
			report.Issues.push_back({"$", "could not record imported UI document", 0});
			return false;
		}
		roots = std::move(createdRoots);
		return true;
	}
}
