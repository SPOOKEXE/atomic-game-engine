#include <engine/ecs/Classes.hpp>
#include <engine/game/Game.hpp>
#include <engine/ui/Theme.hpp>
#include <engine/world/Enums.hpp>

#include <client/Scene.hpp>
#include <imgui.h>
#include <studio/Editor.hpp>
#include <studio/Widgets.hpp>
#include <vector>

namespace studio {

	using engine::core::Name;
	using engine::ecs::Store;
	using engine::world::WorldId;

	void Editor::DrawWorlds() {
		if (!ShowWorlds) {
			return;
		}

		if (!ImGui::Begin("Worlds", &ShowWorlds)) {
			ImGui::End();
			return;
		}

		// **The universe is the container and a world is a scene**, which is the
		// sentence v0.7's roadmap opens with and this panel is where it becomes
		// actionable. The explorer shows the same worlds as a tree because that
		// is how a script reaches them; this shows them as a list because that
		// is how an author manages them. Same objects, not two models kept in
		// step.
		const bool editing = Mode == RunMode::Edit;

		ImGui::BeginDisabled(!editing);
		if (ImGui::Button("New")) {
			AskingNewWorld = true;
			NameBuffer = "World " + std::to_string(Universe->Count() + 1);
		}
		ImGui::SameLine();
		if (ImGui::Button("Import...")) {
			AskingImport = true;
			PathBuffer.clear();
		}
		ImGui::EndDisabled();

		if (!editing) {
			ImGui::SameLine();
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::WarningColour());
			// **Structural changes are refused while running, and it is not
			// timidity.** The snapshot Stop restores was taken before the run
			// began, so a world added during Play would vanish on Stop and one
			// removed would come back. Refusing is the honest answer; doing it
			// and then silently undoing it is not.
			ImGui::TextUnformatted("stop to change the scene list");
			ImGui::PopStyleColor();
		}

		ImGui::Separator();

		constexpr ImGuiTableFlags FLAGS =
			ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV;

		if (!ImGui::BeginTable("##worlds", 4, FLAGS)) {
			ImGui::End();
			return;
		}

		ImGui::TableSetupColumn("scene", ImGuiTableColumnFlags_WidthStretch, 0.44f);
		ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthStretch, 0.20f);
		ImGui::TableSetupColumn("objects", ImGuiTableColumnFlags_WidthStretch, 0.20f);
		ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 0.16f);
		ImGui::TableHeadersRow();

		for (const WorldId world : Universe->Worlds()) {
			const Name name = Universe->NameOf(world);
			const bool active = world == Active;

			ImGui::PushID(static_cast<int>(world.Index));
			ImGui::TableNextRow();

			// --- the scene ----------------------------------------------------

			ImGui::TableSetColumnIndex(0);

			// `SpanAllColumns` so the whole row is the target, which is what
			// makes "click a scene to work on it" a row rather than a word.
			if (ImGui::Selectable(
					"##row", active, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap
				)) {
				PendingActivate = world;
			}

			const bool rowMenu = ImGui::BeginPopupContextItem("##world-menu");

			ImGui::SameLine(0.0f, 0.0f);
			if (active) {
				ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::AccentColour());
				ImGui::TextUnformatted(name.IsValid() ? Label(name) : "(unnamed)");
				ImGui::PopStyleColor();
			} else {
				ImGui::TextUnformatted(name.IsValid() ? Label(name) : "(unnamed)");
			}

			if (Universe->IsRemote(world)) {
				ImGui::SameLine();
				ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
				ImGui::Text("(on %s)", Label(Universe->HostOf(world)));
				ImGui::PopStyleColor();
			}

			// --- what it is doing ----------------------------------------------
			//
			// **State rather than tick rate.** A world's configured rate is a
			// `WorldSettings` field and `Universe` does not hand those back —
			// which is the right shape, because the interesting question about a
			// scene is what it is doing rather than what it was asked for.

			ImGui::TableSetColumnIndex(1);
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
			ImGui::TextUnformatted(engine::world::Describe(Universe->StateOf(world)));
			ImGui::PopStyleColor();

			ImGui::TableSetColumnIndex(2);
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());

			if (Universe->IsRemote(world)) {
				// A world held by a host has no store here, so counting its
				// instances is not this process's to do. Said, rather than shown
				// as zero — which would read as an empty scene.
				ImGui::TextUnformatted("-");
			} else {
				ImGui::Text("%zu", InstanceCountOf(world));
			}
			ImGui::PopStyleColor();

			// --- what can be done to it ------------------------------------------

			ImGui::TableSetColumnIndex(3);
			if (ImGui::SmallButton("...")) {
				ImGui::OpenPopup("##world-menu");
			}

			if (rowMenu || ImGui::BeginPopup("##world-menu")) {
				DrawWorldActions(world);
				ImGui::EndPopup();
			}

			ImGui::PopID();
		}

		ImGui::EndTable();
		ImGui::End();

		ApplyPendingWorldActions();
	}

	void Editor::DrawWorldActions(WorldId world) {
		const bool editing = Mode == RunMode::Edit;
		const bool local = !Universe->IsRemote(world);

		if (ImGui::MenuItem("Set Active", nullptr, false, world != Active)) {
			PendingActivate = world;
		}

		ImGui::Separator();

		if (ImGui::MenuItem("Rename...", nullptr, false, editing && local)) {
			// Queued, because the prompt has to outlive this popup — a modal
			// opened from inside one closes with it.
			RenamingWorld = world;
			NameBuffer = std::string(Label(Universe->NameOf(world)));
			AskingRenameWorld = true;
		}

		if (ImGui::MenuItem("Duplicate", nullptr, false, editing && local)) {
			PendingDuplicateWorld = world;
		}

		if (ImGui::MenuItem("Export...", nullptr, false, local)) {
			PendingActivate = world;
			AskingExport = true;
			PathBuffer =
				std::string(Label(Universe->NameOf(world))) + std::string(engine::game::WORLD_EXTENSION);
		}

		ImGui::Separator();

		// **The last world cannot be removed.** A universe with no worlds is a
		// game with nothing in it, and an editor showing an empty explorer with
		// no way back is worse than a menu item that is greyed out.
		if (ImGui::MenuItem("Remove", nullptr, false, editing && Universe->Count() > 1)) {
			PendingRemoveWorld = world;
		}
	}

	size_t Editor::InstanceCountOf(WorldId world) {
		// **Cached, because the honest version is a scan of every entity in the
		// world and this panel draws sixty times a second.** A world of a
		// hundred thousand parts would spend a frame counting them for a number
		// nobody reads that fast — so it is recounted on a clock, which is the
		// trade `client::Client` already makes for the debug panels and for the
		// same reason: a person cannot read a number that changes a thousand
		// times a second.
		constexpr double INTERVAL = 0.5;

		const double now = Clock.Now();
		for (InstanceCount &entry : InstanceCounts) {
			if (entry.World == world && now - entry.Taken < INTERVAL) {
				return entry.Count;
			}
		}

		size_t counted = 0;
		Universe->Enter(world, [&counted](Store &store) {
			store.EachEntity([&](engine::ecs::Entity entity) {
				// Instances, not entities. A world holds rows that are not
				// instances — the camera the client installs, a predicted entity
				// — and counting those would report a number the explorer does
				// not show.
				if (store.ClassOf(entity).IsValid()) {
					counted++;
				}
			});
		});

		for (InstanceCount &entry : InstanceCounts) {
			if (entry.World == world) {
				entry.Count = counted;
				entry.Taken = now;
				return counted;
			}
		}

		InstanceCounts.push_back(InstanceCount{world, counted, now});
		return counted;
	}

	void Editor::ApplyPendingWorldActions() {
		// Outside the table and outside any `Universe::Enter`, for the reason
		// `ApplyPendingActions` gives: the panel above enters worlds to count
		// their instances, and every action here enters one itself.

		if (PendingActivate.IsValid()) {
			const WorldId world = PendingActivate;
			PendingActivate = WorldId{};

			if (world != Active) {
				Active = world;
				SelectionWorld = world;
				ClearSelection();
			}
		}

		if (PendingDuplicateWorld.IsValid()) {
			const WorldId source = PendingDuplicateWorld;
			PendingDuplicateWorld = WorldId{};
			DuplicateWorld(source);
		}

		if (PendingRenameWorld.IsValid()) {
			const WorldId world = PendingRenameWorld;
			const std::string wanted = PendingRenameTo;
			PendingRenameWorld = WorldId{};
			PendingRenameTo.clear();
			RenameWorld(world, Name(wanted));
		}
	}

	bool Editor::DuplicateWorld(WorldId source) {
		std::string error;
		const std::string document = engine::game::WriteWorldDocument(*Universe, source, error);
		if (document.empty()) {
			Say("could not copy that scene: " + error, engine::core::LogLevel::Error);
			return false;
		}

		// A free name derived from the original, rather than refusing and making
		// somebody invent one. Two worlds cannot share a name, and being told so
		// is a worse answer than being given a name that works.
		const std::string base(Label(Universe->NameOf(source), "World"));
		WorldId copy;

		for (int attempt = 2; attempt < 100 && !copy.IsValid(); attempt++) {
			copy = engine::game::ReadWorldDocument(
				*Universe, document, Name(base + " " + std::to_string(attempt)), error
			);
		}

		if (!copy.IsValid()) {
			Say("could not copy that scene: " + error, engine::core::LogLevel::Error);
			return false;
		}

		Universe->Enter(copy, [](Store &store, engine::ecs::Scheduler &systems) {
			client::InstallPresentation(store, systems, 256);
		});

		InstanceCounts.clear();
		Active = copy;
		SelectionWorld = copy;
		ClearSelection();
		MarkModified();

		Say("copied '" + base + "' to '" + std::string(Label(Universe->NameOf(copy))) + "'");
		return true;
	}

	bool Editor::RenameWorld(WorldId world, Name wanted) {
		if (!wanted.IsValid() || !world.IsValid()) {
			return false;
		}

		const Name current = Universe->NameOf(world);
		if (wanted == current) {
			return true;
		}

		if (Universe->Find(wanted).IsValid()) {
			Say("a scene called '" + std::string(Label(wanted)) + "' already exists",
				engine::core::LogLevel::Warning);
			return false;
		}

		// **A world is renamed by being written out and read back, and the
		// reason is rule 4.** A world's name is what a bus envelope, a
		// subscription and a teleport all carry, so `world::Universe` has no
		// rename and should not grow one — a live world renamed underneath its
		// pending traffic is stranded, and nothing would report it.
		//
		// In Edit mode there is no traffic: nothing ticks and no script has
		// subscribed to anything. So the safe rename is the one that goes
		// through the save format, and that is why the menu item is disabled
		// while a game is running.
		//
		// **The handle survives.** `Universe::Adopt` reuses the hole a destroyed
		// world leaves, so recreating immediately takes the same slot — the
		// scene keeps its `WorldId` and its place in the list, which is what
		// stops a rename from quietly reordering the save file.
		std::string error;
		const std::string document = engine::game::WriteWorldDocument(*Universe, world, error);
		if (document.empty()) {
			Say("could not rename that scene: " + error, engine::core::LogLevel::Error);
			return false;
		}

		// Script tabs first: their entity handles do not survive the world being
		// rebuilt, and a tab that saved afterwards would write into storage that
		// had been freed.
		for (size_t index = Scripts.size(); index > 0; index--) {
			if (Scripts[index - 1].World == world) {
				CloseScriptTab(index - 1);
			}
		}

		const bool wasActive = world == Active;
		Universe->Destroy(world);

		const WorldId renamed = engine::game::ReadWorldDocument(*Universe, document, wanted, error);
		if (!renamed.IsValid()) {
			// The world is gone and the replacement was refused. Loud, because
			// an author whose scene disappeared needs to know it was this and
			// not something they did.
			Say("the scene was lost while renaming it: " + error, engine::core::LogLevel::Error);
			InstanceCounts.clear();
			ClearSelection();
			Active = Universe->Worlds().empty() ? WorldId{} : Universe->Worlds().front();
			SelectionWorld = Active;
			return false;
		}

		Universe->Enter(renamed, [](Store &store, engine::ecs::Scheduler &systems) {
			client::InstallPresentation(store, systems, 256);
		});

		InstanceCounts.clear();
		if (wasActive) {
			Active = renamed;
		}
		SelectionWorld = Active;
		ClearSelection();
		MarkModified();

		Say("renamed '" + std::string(Label(current)) + "' to '" + std::string(Label(wanted)) + "'");
		return true;
	}
}
