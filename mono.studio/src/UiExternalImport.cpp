#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Localization.hpp>
#include <engine/scene/Services.hpp>

#include <fstream>
#include <studio/Commands.hpp>
#include <studio/UiDocumentFile.hpp>
#include <studio/UiExternalImport.hpp>
#include <studio/UiImportAdapters.hpp>

namespace studio {
	using engine::core::Name;
	using engine::ecs::Entity;
	using engine::ecs::NULL_ENTITY;

	namespace {
		void ExternalImportFileIssue(
			engine::gui::DocumentReport &report, const std::filesystem::path &path, const char *reason
		) {
			report.Issues.push_back({path.string(), reason, 0});
		}

		bool
		ReadSource(const std::filesystem::path &path, std::string &out, engine::gui::DocumentReport &report) {
			std::ifstream input(path, std::ios::binary | std::ios::ate);
			if (!input) {
				ExternalImportFileIssue(report, path, "could not open UI import file");
				return false;
			}
			const std::streampos length = input.tellg();
			if (length < 0 || static_cast<uint64_t>(length) > UiDesignImportLimits::HARD_MAXIMUM_BYTES) {
				ExternalImportFileIssue(report, path, "UI import exceeds byte limit");
				return false;
			}
			std::string source(static_cast<size_t>(length), '\0');
			input.seekg(0);
			if (!source.empty() && !input.read(source.data(), length)) {
				ExternalImportFileIssue(report, path, "could not read UI import file");
				return false;
			}
			out = std::move(source);
			return true;
		}
	}

	bool ReadUiExternalImportFile(
		const std::filesystem::path &path, std::string &out, engine::gui::DocumentReport &report
	) {
		report = {};
		return ReadSource(path, out, report);
	}

	bool ImportUiDesignTokensFileEdit(
		engine::ecs::Store &store,
		engine::world::WorldId world,
		const Entity selectedParent,
		const std::filesystem::path &path,
		CommandLog &commands,
		std::vector<Entity> &roots,
		engine::gui::DocumentReport &report
	) {
		std::string source;
		if (!ReadUiExternalImportFile(path, source, report)) return false;
		engine::gui::UiDocument document;
		if (!ImportUiDesignTokensJson(source, document, report)) return false;
		return ImportUiDocumentEdit(store, world, selectedParent, document, commands, roots, report);
	}

	bool ImportUiLocalizationFileEdit(
		engine::ecs::Store &store,
		engine::world::WorldId world,
		const std::filesystem::path &path,
		CommandLog &commands,
		Entity &table,
		engine::gui::DocumentReport &report
	) {
		std::string source;
		if (!ReadUiExternalImportFile(path, source, report)) return false;
		engine::gui::LocalizationCatalogue catalogue;
		if (!ImportUiLocalizationCsv(source, catalogue, report)) return false;

		const auto tableClass = engine::ecs::Classes::Find(Name("LocalizationTable"));
		const Entity replicatedStorage =
			engine::scene::ServiceOf(store, engine::ecs::Classes::Find(Name("ReplicatedStorage")));
		if (!tableClass.IsValid() || replicatedStorage == NULL_ENTITY || !store.Alive(replicatedStorage)) {
			report.Issues.push_back({"$", "localization storage is unavailable", 0});
			return false;
		}

		const Entity created = store.CreateInstance(tableClass, "Localization");
		if (created == NULL_ENTITY || !store.SetProperty(created, Name("Value"), &source, sizeof(source)) ||
			!store.SetParent(created, replicatedStorage)) {
			if (created != NULL_ENTITY && store.Alive(created)) store.DestroyInstance(created);
			report.Issues.push_back({"$", "could not create localization table", 0});
			return false;
		}
		commands.RecordCreate(store, world, created, "Import localization table");
		table = created;
		return true;
	}
}
