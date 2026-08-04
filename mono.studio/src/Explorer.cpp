#include <engine/ecs/Classes.hpp>
#include <engine/script/Instances.hpp>
#include <engine/ui/Theme.hpp>

#include <algorithm>
#include <imgui.h>
#include <studio/Editor.hpp>
#include <studio/Widgets.hpp>
#include <vector>

namespace studio {

	using engine::core::Name;
	using engine::ecs::ClassId;
	using engine::ecs::Classes;
	using engine::ecs::NULL_ENTITY;
	using engine::ecs::Store;

	namespace {
		// The drag-and-drop payload: an entity, moved within one world.
		//
		// **Within one world, and the type name is what enforces it.** An
		// `Entity` is a handle inside one store and means nothing in another,
		// so a payload accepted across worlds would reparent a row onto a
		// number that happens to exist there. The world is carried alongside
		// and checked rather than assumed.
		constexpr const char *DRAG_TYPE = "studio.instance";

		struct DragPayload {
			uint32_t World = 0;
			engine::ecs::Entity Instance;
		};

		// A cached, filtered list of every class an author may insert.
		//
		// Rebuilt when the query changes rather than every frame: the class
		// table is process-wide and does not move, so re-scoring three hundred
		// names per frame would be work whose answer is already known.
		struct ClassSearch {
			std::string Query;
			std::vector<ClassId> Results;
			size_t KnownClasses = 0;

			void Refresh(std::string_view query) {
				if (query == Query && KnownClasses == Classes::Count()) {
					return;
				}

				Query.assign(query);
				KnownClasses = Classes::Count();
				Results.clear();

				const ClassId instanceClass = Classes::Find(Name("Instance"));

				std::vector<std::pair<int, ClassId>> scored;
				for (size_t index = 0; index < KnownClasses; index++) {
					const ClassId id{static_cast<uint32_t>(index)};
					const engine::ecs::ClassInfo &info = Classes::Describe(id);

					if (!info.Name.IsValid()) {
						continue;
					}

					// Everything under `Instance`, which is every class an
					// author can put in a tree. A class registered by some
					// other module for its own storage is not one of those and
					// is not offered.
					if (instanceClass.IsValid() && !Classes::IsA(id, instanceClass)) {
						continue;
					}

					// The abstract bases. Roblox does not let you insert an
					// `Instance` or a `BasePart` either, and offering them
					// would produce rows nothing knows how to draw.
					const std::string_view name = Label(info.Name);
					if (name == "Instance" || name == "PVInstance" || name == "BasePart" ||
						name == "LuaSourceContainer") {
						continue;
					}

					int score = 0;
					if (FuzzyMatch(Query, name, score)) {
						scored.emplace_back(score, id);
					}
				}

				std::sort(scored.begin(), scored.end(), [](const auto &left, const auto &right) {
					if (left.first != right.first) {
						return left.first > right.first;
					}
					// Ties broken by name rather than by registration index, so
					// the list is the same on two runs whatever order the
					// linker ran the registrations in.
					return std::string_view(Label(Classes::Describe(left.second).Name)) <
						   std::string_view(Label(Classes::Describe(right.second).Name));
				});

				for (const auto &entry : scored) {
					Results.push_back(entry.second);
				}
			}
		};
	}

	ClassId Editor::DrawClassPicker(const char *id) {
		// One search per call site, keyed by the id the caller gave. The
		// toolbar's popup and the tree's context menu are two different lists
		// with two different queries, and sharing one would make typing in one
		// filter the other.
		static std::vector<std::pair<std::string, ClassSearch>> searches;

		ClassSearch *search = nullptr;
		for (auto &entry : searches) {
			if (entry.first == id) {
				search = &entry.second;
				break;
			}
		}
		if (search == nullptr) {
			searches.emplace_back(id, ClassSearch{});
			search = &searches.back().second;
		}

		static std::vector<std::pair<std::string, std::string>> queries;
		std::string *query = nullptr;
		for (auto &entry : queries) {
			if (entry.first == id) {
				query = &entry.second;
				break;
			}
		}
		if (query == nullptr) {
			queries.emplace_back(id, std::string{});
			query = &queries.back().second;
		}

		ImGui::SetNextItemWidth(220.0f * Settings.Scale);
		if (ImGui::IsWindowAppearing()) {
			ImGui::SetKeyboardFocusHere();
		}
		TextField("##class-filter", *query, "search classes");

		search->Refresh(*query);

		ClassId chosen;

		if (ImGui::BeginChild("##class-list", ImVec2(220.0f * Settings.Scale, 260.0f * Settings.Scale))) {
			for (const ClassId candidate : search->Results) {
				const engine::ecs::ClassInfo &info = Classes::Describe(candidate);
				if (ImGui::Selectable(Label(info.Name))) {
					chosen = candidate;
				}
			}

			if (search->Results.empty()) {
				ImGui::TextDisabled("no class matches");
			}
		}
		ImGui::EndChild();

		return chosen;
	}

	void Editor::DrawInstanceActions(Store &store, WorldId world, engine::ecs::Entity instance) {
		const bool haveInstance = instance != NULL_ENTITY && store.Alive(instance);

		if (ImGui::BeginMenu("Insert Object")) {
			if (const ClassId chosen = DrawClassPicker("insert-context"); chosen.IsValid()) {
				// Queued rather than applied here: `InsertInstance` enters the
				// world, and this code is already running inside `Enter` for
				// the world the tree is drawing. Entering twice is what
				// `Universe::Enter`'s affinity check exists to catch.
				PendingInsert.World = world;
				PendingInsert.Class = chosen;
				PendingInsert.Parent = haveInstance ? instance : NULL_ENTITY;
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndMenu();
		}

		ImGui::Separator();

		if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, haveInstance)) {
			PendingDuplicate = true;
		}
		if (ImGui::MenuItem("Delete", "Del", false, haveInstance)) {
			PendingDelete = true;
		}

		if (haveInstance) {
			ImGui::Separator();

			const ClassId scriptClass = Classes::Find(Name("LuaSourceContainer"));
			if (scriptClass.IsValid() && store.IsA(instance, scriptClass)) {
				if (ImGui::MenuItem("Edit Script")) {
					PendingOpenScript.World = world;
					PendingOpenScript.Instance = instance;
				}
			}

			if (ImGui::MenuItem("Unparent")) {
				PendingReparent.World = world;
				PendingReparent.Instance = instance;
				PendingReparent.Parent = NULL_ENTITY;
			}
		}
	}

	void Editor::DrawTreeNode(Store &store, WorldId world, engine::ecs::Entity instance) {
		if (!store.Alive(instance)) {
			return;
		}

		const Name name = store.InstanceNameOf(instance);
		const ClassId klass = store.ClassOf(instance);
		const char *className = klass.IsValid() ? Label(Classes::Describe(klass).Name) : "Entity";

		bool hasChildren = false;
		store.EachChild(instance, [&](engine::ecs::Entity) { hasChildren = true; });

		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
								   ImGuiTreeNodeFlags_OpenOnDoubleClick;
		if (!hasChildren) {
			flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
		}
		if (IsSelected(instance) && world == SelectionWorld) {
			flags |= ImGuiTreeNodeFlags_Selected;
		}

		const bool wanted =
			std::find(Expanded.begin(), Expanded.end(), instance.Id) != Expanded.end();
		if (wanted) {
			ImGui::SetNextItemOpen(true);
			// Consumed. The set is a request from an action — "I just made a
			// child in here" — and not a mirror of what is open, which imgui
			// already tracks. Keeping it would fight every attempt to collapse.
			Expanded.erase(std::remove(Expanded.begin(), Expanded.end(), instance.Id), Expanded.end());
		}

		ImGui::PushID(static_cast<int>(instance.Id));
		const bool open = ImGui::TreeNodeEx(
			"##node", flags, "%s", name.IsValid() ? Label(name) : "(unnamed)"
		);

		// **Everything that asks about "the last item" happens here, before
		// anything else is drawn.** imgui has exactly one last item, and it is
		// whatever was submitted most recently — so the dimmed class name below
		// would become it, and `BeginDragDropSource` would then be asked to drag
		// a `Text` with no id. That is an assertion rather than a subtle bug,
		// which is the good version of this mistake, and it cost one run to
		// find.

		const bool toggled = ImGui::IsItemToggledOpen();
		const bool hovered = ImGui::IsItemHovered();

		if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !toggled) {
			Select(world, instance, ImGui::GetIO().KeyCtrl);
		}

		if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
			Select(world, instance, false);
		}

		if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
			const ClassId scriptClass = Classes::Find(Name("LuaSourceContainer"));
			if (scriptClass.IsValid() && store.IsA(instance, scriptClass)) {
				PendingOpenScript.World = world;
				PendingOpenScript.Instance = instance;
			}
		}

		// --- drag and drop --------------------------------------------------

		if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoDisableHover)) {
			const DragPayload payload{world.Index, instance};
			ImGui::SetDragDropPayload(DRAG_TYPE, &payload, sizeof(payload));
			ImGui::Text("%s", name.IsValid() ? Label(name) : "(unnamed)");
			ImGui::EndDragDropSource();
		}

		if (ImGui::BeginDragDropTarget()) {
			if (const ImGuiPayload *dropped = ImGui::AcceptDragDropPayload(DRAG_TYPE)) {
				const auto *moved = static_cast<const DragPayload *>(dropped->Data);

				// **Same world only.** An `Entity` is a handle inside one store
				// and means nothing in another, so accepting one across worlds
				// would reparent whatever row happens to hold that number.
				if (moved->World == world.Index) {
					PendingReparent.World = world;
					PendingReparent.Instance = moved->Instance;
					PendingReparent.Parent = instance;
				}
			}
			ImGui::EndDragDropTarget();
		}

		if (ImGui::BeginPopupContextItem("##actions")) {
			DrawInstanceActions(store, world, instance);
			ImGui::EndPopup();
		}

		// The class name, dimmed, on the same row. Roblox puts an icon here; an
		// icon set is a texture atlas and a lot of art, and the class name
		// carries the same information in the space available.
		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
		ImGui::TextUnformatted(className);
		ImGui::PopStyleColor();

		if (open && hasChildren) {
			// Collected first, then walked. `DrawTreeNode` can queue a
			// reparent, and reading the sibling list while something is about
			// to change it is the iteration hazard `EachChild` warns about from
			// the other side.
			std::vector<engine::ecs::Entity> children;
			store.EachChild(instance, [&](engine::ecs::Entity child) { children.push_back(child); });

			for (const engine::ecs::Entity child : children) {
				DrawTreeNode(store, world, child);
			}
			ImGui::TreePop();
		}

		ImGui::PopID();
	}

	void Editor::DrawExplorer() {
		if (!ImGui::Begin("Explorer")) {
			ImGui::End();
			return;
		}

		ImGui::SetNextItemWidth(-1.0f);
		TextField("##explorer-filter", ExplorerFilter, "filter by name");

		ImGui::Separator();

		// **The universe is the root, and it is `game`.** `script`'s bindings
		// already map `game` to `world::Universe` and `workspace` to the world a
		// script runs on; drawing the same shape means what an author sees in
		// the tree and what a script reaches through `game` are one object
		// rather than two models kept in step.
		const std::string universeLabel =
			std::string(GameName.IsValid() ? Label(GameName) : "Game") + "  (game)";

		if (ImGui::TreeNodeEx(
				universeLabel.c_str(),
				ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth
			)) {
			for (const WorldId world : Universe->Worlds()) {
				const Name worldName = Universe->NameOf(world);

				ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow;
				if (world == Active) {
					flags |= ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Selected;
				}

				ImGui::PushID(static_cast<int>(world.Index));

				const std::string label =
					std::string(worldName.IsValid() ? Label(worldName) : "?") +
					(world == Active ? "  (workspace)" : "");

				const bool open = ImGui::TreeNodeEx("##world", flags, "%s", label.c_str());

				if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
					Active = world;
					SelectionWorld = world;
					ClearSelection();
				}

				if (ImGui::BeginPopupContextItem("##world-actions")) {
					if (ImGui::MenuItem("Set Active")) {
						Active = world;
						SelectionWorld = world;
						ClearSelection();
					}
					if (ImGui::MenuItem("Export World...")) {
						Active = world;
						AskingExport = true;
						PathBuffer = std::string(Label(worldName)) +
									 std::string(engine::game::WORLD_EXTENSION);
					}
					if (ImGui::MenuItem("Remove World", nullptr, false, Universe->Count() > 1)) {
						PendingRemoveWorld = world;
					}
					ImGui::EndPopup();
				}

				if (open) {
					if (Universe->IsRemote(world)) {
						ImGui::TextDisabled("held by host '%s'", Label(Universe->HostOf(world)));
					} else {
						Universe->Enter(world, [&](Store &store) {
							std::vector<engine::ecs::Entity> roots;
							store.EachRoot([&](engine::ecs::Entity root) { roots.push_back(root); });

							for (const engine::ecs::Entity root : roots) {
								if (!ExplorerFilter.empty()) {
									// Filtered at the root, which is the honest
									// version of what this does: a subtree
									// search that hid parents would show
									// children with no path to them, and a tree
									// with no parents is a list.
									const Name name = store.InstanceNameOf(root);
									int score = 0;
									if (!name.IsValid() ||
										!FuzzyMatch(ExplorerFilter, Label(name), score)) {
										continue;
									}
								}
								DrawTreeNode(store, world, root);
							}

							if (ImGui::BeginPopupContextWindow(
									"##world-blank", ImGuiPopupFlags_MouseButtonRight |
														 ImGuiPopupFlags_NoOpenOverItems
								)) {
								DrawInstanceActions(store, world, NULL_ENTITY);
								ImGui::EndPopup();
							}
						});
					}
					ImGui::TreePop();
				}

				ImGui::PopID();
			}

			ImGui::TreePop();
		}

		ImGui::End();

		ApplyPendingActions();
	}

	void Editor::ApplyPendingActions() {
		// **Everything the tree queued, applied outside `Universe::Enter`.**
		// Drawing a world's tree happens inside `Enter`, and every action here
		// enters a world itself — `Enter` aborts on re-entry rather than
		// silently allowing it, which is the affinity check doing its job. So
		// the panel records what was asked for and this applies it, once, from
		// the outside.

		if (PendingInsert.Class.IsValid()) {
			InsertInstance(PendingInsert.World, PendingInsert.Class, PendingInsert.Parent);
			PendingInsert = PendingInsertAction{};
		}

		if (PendingReparent.World.IsValid()) {
			const WorldId world = PendingReparent.World;
			const engine::ecs::Entity instance = PendingReparent.Instance;
			const engine::ecs::Entity parent = PendingReparent.Parent;
			PendingReparent = PendingReparentAction{};

			bool moved = false;
			Universe->Enter(world, [&](Store &store) {
				if (!store.Alive(instance)) {
					return;
				}
				if (parent != NULL_ENTITY && !store.Alive(parent)) {
					return;
				}

				// **Refused rather than allowed to cycle.** `SetParent` already
				// refuses to make an instance its own ancestor — a cycle in the
				// tree is a hang in every walk of it rather than a wrong answer
				// — and this reports the refusal instead of leaving a drag that
				// silently did nothing.
				moved = store.SetParent(instance, parent);
			});

			if (moved) {
				if (parent != NULL_ENTITY) {
					Expanded.push_back(parent.Id);
				}
				MarkModified();
			} else {
				Say("that would put an instance inside itself", engine::core::LogLevel::Warning);
			}
		}

		if (PendingOpenScript.World.IsValid()) {
			OpenScriptTab(PendingOpenScript.World, PendingOpenScript.Instance);
			PendingOpenScript = PendingScriptAction{};
		}

		if (PendingDuplicate) {
			PendingDuplicate = false;
			DuplicateSelection();
		}

		if (PendingDelete) {
			PendingDelete = false;
			DeleteSelection();
		}

		if (PendingRemoveWorld.IsValid()) {
			const WorldId world = PendingRemoveWorld;
			PendingRemoveWorld = WorldId{};
			RemoveWorld(world);
		}
	}
}
