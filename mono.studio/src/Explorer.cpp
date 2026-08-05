#include <engine/ecs/Classes.hpp>
#include <engine/script/Instances.hpp>
#include <engine/ui/Fonts.hpp>
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

				// **Services are excluded as a category, not as nine names.** A
				// world has exactly one of each and `scene::InstallServices`
				// is what puts them there, so offering `ServerStorage` in the
				// palette offers a second one that nothing resolves and every
				// `GetService` ignores. Asking `IsA` rather than listing them
				// is what keeps a tenth service out of this function — the same
				// property the `Instance` filter above already has.
				const ClassId serviceClass = Classes::Find(Name("Service"));

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

					if (serviceClass.IsValid() && Classes::IsA(id, serviceClass)) {
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

	void Editor::DrawInsertMenu(const char *id, WorldId world, engine::ecs::Entity parent) {
		// **No `Store` parameter, and that is what lets three call sites share
		// it.** An instance's menu is drawn from inside `Universe::Enter` and a
		// world's menu is drawn from outside it — the world row is above the
		// `Enter` that walks its tree. A helper that took a store could only
		// ever be called from the first, which is how "Insert Object is on the
		// tree but not on the world" happens.
		if (!ImGui::BeginMenu("Insert Object")) {
			return;
		}

		if (const ClassId chosen = DrawClassPicker(id); chosen.IsValid()) {
			// Queued rather than applied here: `InsertInstance` enters the
			// world, and a tree's menu is already running inside `Enter` for
			// the world it is drawing. Entering twice is what
			// `Universe::Enter`'s affinity check exists to catch.
			PendingInsert.World = world;
			PendingInsert.Class = chosen;
			PendingInsert.Parent = parent;
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndMenu();
	}

	void Editor::DrawInstanceActions(Store &store, WorldId world, engine::ecs::Entity instance) {
		const bool haveInstance = instance != NULL_ENTITY && store.Alive(instance);

		DrawInsertMenu("insert-context", world, haveInstance ? instance : NULL_ENTITY);

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

			// **Only for a camera, and only for the world being viewed.**
			// Looking through a camera in a world the viewport is not showing
			// would move the eye to somewhere the panel cannot draw.
			const ClassId cameraClass = Classes::Find(Name("Camera"));
			if (cameraClass.IsValid() && store.IsA(instance, cameraClass) && world == Active) {
				const bool following = FollowCamera == instance;
				if (ImGui::MenuItem(following ? "Stop Looking Through" : "Look Through Camera")) {
					PendingLookThrough = following ? NULL_ENTITY : instance;
					PendingLookThroughSet = true;
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

		// **O(1), and it used to be a full walk of the sibling list.** This asks
		// whether to draw an expander, and answering it with `EachChild` cost two
		// hundred steps on the root of a two-hundred-part scene — every frame,
		// for a bool the first child settles. `bench_studio` measured the probe
		// alone at 2.40 us on that shape.
		const bool hasChildren = store.HasChildren(instance);

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

		// **Everything below this point is skipped for a row nobody can see**,
		// and on a long tree that is most of them. imgui already declines to
		// *render* a clipped item, but every question asked about it and every
		// scope opened around it still costs — the two drag-and-drop scopes, the
		// context popup, and the dimmed class name with its own font push, which
		// is glyph layout for text that is off screen.
		//
		// None of it can do anything for an invisible row: an item outside the
		// clip rect cannot be hovered, clicked or dragged, so every one of these
		// answers false anyway. The row still submits its node, so the scrollbar
		// and the layout are unchanged — what goes is only the work whose result
		// was already known.
		if (!ImGui::IsItemVisible()) {
			if (open && hasChildren) {
				const size_t first = ChildScratch.size();
				store.EachChild(instance, [&](engine::ecs::Entity child) {
					ChildScratch.push_back(child);
				});
				const size_t last = ChildScratch.size();

				for (size_t index = first; index < last; index++) {
					DrawTreeNode(store, world, ChildScratch[index]);
				}

				ChildScratch.resize(first);
				ImGui::TreePop();
			}

			ImGui::PopID();
			return;
		}

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

				// **Two different operations, and the payload's world is what
				// picks between them.** Within one store a move is a
				// `SetParent` and the instance keeps its handle, its id and
				// everything pointing at it. Across two stores none of that is
				// possible — an `Entity` means nothing in another world — so
				// the subtree is written out, rebuilt on the other side and the
				// original destroyed. Same gesture, and the cost is not the
				// same, which is why they are not one code path pretending to
				// be.
				PendingReparent.World = WorldId{};
				PendingMove.Source = WorldId{};

				if (moved->World == world.Index) {
					PendingReparent.World = world;
					PendingReparent.Instance = moved->Instance;
					PendingReparent.Parent = instance;
				} else {
					PendingMove.Source = WorldFor(moved->World);
					PendingMove.Instance = moved->Instance;
					PendingMove.Target = world;
					PendingMove.Parent = instance;
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
		// Smaller as well as dimmer. A class name is an annotation on the row
		// rather than a second name on it, and two things at the same size read
		// as two names however different their colours are.
		ImGui::SameLine();
		{
			const engine::ui::ScopedFont small(
				engine::ui::Typeface::Interface, engine::ui::TextSize::Small
			);
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
			ImGui::TextUnformatted(className);
			ImGui::PopStyleColor();
		}

		if (open && hasChildren) {
			// Collected first, then walked. `DrawTreeNode` can queue a
			// reparent, and reading the sibling list while something is about
			// to change it is the iteration hazard `EachChild` warns about from
			// the other side.
			//
			// **One buffer for the whole recursion rather than a vector per
			// node.** This was a `std::vector` constructed inside the body, so
			// an open tree of two hundred rows was two hundred heap allocations
			// and frees per frame, in a panel that rebuilds every frame because
			// imgui is immediate mode.
			//
			// The recursion shares it by range: each level appends its children,
			// remembers where its own run starts and ends, and truncates back to
			// that mark on the way out. Indices rather than iterators, because a
			// deeper level appending can reallocate — which is exactly the bug
			// this shape would have if it held references.
			const size_t first = ChildScratch.size();
			store.EachChild(instance, [&](engine::ecs::Entity child) {
				ChildScratch.push_back(child);
			});
			const size_t last = ChildScratch.size();

			for (size_t index = first; index < last; index++) {
				DrawTreeNode(store, world, ChildScratch[index]);
			}

			ChildScratch.resize(first);
			ImGui::TreePop();
		}

		ImGui::PopID();
	}

	void Editor::DrawExplorer() {
		if (!ShowExplorer) {
			return;
		}

		if (!ImGui::Begin("Explorer", &ShowExplorer)) {
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
		//
		// **Tagged "(universe)" rather than "(game)", and the worlds below are
		// each "(world)".** The two words were doing one job badly: the root
		// said "game" and a world said nothing, except the active one, which
		// said "(workspace)" — so the tag on a row changed as somebody clicked
		// around, and `Workspace` is now a real instance *inside* a world,
		// which made that word mean two things one line apart. What a row is
		// does not depend on which row is selected; selection is what the
		// highlight is for.
		const std::string universeLabel =
			std::string(GameName.IsValid() ? Label(GameName) : "Game") + "  (universe)";

		const bool universeOpen = ImGui::TreeNodeEx(
			universeLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth
		);

		// Immediately after the node, before anything else is submitted. See
		// the note in `DrawTreeNode`: imgui has exactly one "last item".
		if (ImGui::BeginPopupContextItem("##universe-actions")) {
			// Insert lands in the active world, because a universe holds worlds
			// and not instances — there is no other honest answer, and refusing
			// outright would make the root the one row with no menu.
			DrawInsertMenu("insert-universe", Active, NULL_ENTITY);

			ImGui::Separator();

			if (ImGui::MenuItem("New World...", nullptr, false, !AnyRunning())) {
				AskingNewWorld = true;
				NameBuffer = "World " + std::to_string(Universe->Count() + 1);
			}
			if (ImGui::MenuItem("Import World...", nullptr, false, !AnyRunning())) {
				AskingImport = true;
				PathBuffer.clear();
			}

			ImGui::Separator();

			if (ImGui::MenuItem("Export Universe...")) {
				AskingExportUniverse = true;
				PathBuffer = std::string(GameName.IsValid() ? Label(GameName) : "Game") +
							 std::string(engine::game::GAME_EXTENSION);
			}
			ImGui::EndPopup();
		}

		if (universeOpen) {
			for (const WorldId world : Universe->Worlds()) {
				const Name worldName = Universe->NameOf(world);

				ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow;
				if (world == Active) {
					flags |= ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Selected;
				}

				// The same consume-once request `DrawTreeNode` honours for an
				// instance, in its own set because a world index and an entity
				// id are different key spaces. See `ExpandedWorlds`.
				if (const auto found =
						std::find(ExpandedWorlds.begin(), ExpandedWorlds.end(), world.Index);
					found != ExpandedWorlds.end()) {
					ImGui::SetNextItemOpen(true);
					ExpandedWorlds.erase(found);
				}

				ImGui::PushID(static_cast<int>(world.Index));

				const std::string label =
					std::string(worldName.IsValid() ? Label(worldName) : "?") + "  (world)";

				const bool open = ImGui::TreeNodeEx("##world", flags, "%s", label.c_str());

				if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
					Active = world;
					SelectionWorld = world;
					ClearSelection();
				}

				// **A world row takes a drop too**, and it means "a root of
				// this world". Without it the only way to move something
				// between worlds would be to drop it onto an instance that
				// happened to already be there — which is impossible for the
				// case somebody reaches for first, an empty world.
				if (ImGui::BeginDragDropTarget()) {
					if (const ImGuiPayload *dropped = ImGui::AcceptDragDropPayload(DRAG_TYPE)) {
						const auto *moved = static_cast<const DragPayload *>(dropped->Data);
						if (moved->World != world.Index) {
							PendingMove.Source = WorldFor(moved->World);
							PendingMove.Instance = moved->Instance;
							PendingMove.Target = world;
							PendingMove.Parent = NULL_ENTITY;
						} else {
							// Same world, so it is an unparent rather than a
							// move: the instance becomes a root where it is.
							PendingReparent.World = world;
							PendingReparent.Instance = moved->Instance;
							PendingReparent.Parent = NULL_ENTITY;
						}
					}
					ImGui::EndDragDropTarget();
				}

				if (ImGui::BeginPopupContextItem("##world-actions")) {
					// **Insert at the world's root**, which is the parent a
					// right-click on the world itself means. This is the menu
					// that did not have it: the tree rows and the blank space
					// below them both offered Insert Object and the row
					// between them did not, so the way to make a root was to
					// find the empty part of a panel that is rarely empty.
					DrawInsertMenu("insert-world", world, NULL_ENTITY);

					ImGui::Separator();

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
	}

	WorldId Editor::WorldFor(uint32_t index) const {
		for (const WorldId id : Universe->Worlds()) {
			if (id.Index == index) {
				return id;
			}
		}
		return WorldId{};
	}

	bool Editor::MoveInstanceToWorld(
		WorldId source, engine::ecs::Entity instance, WorldId target, engine::ecs::Entity parent
	) {
		if (!source.IsValid() || !target.IsValid() || source == target) {
			return false;
		}

		// **Refused rather than half-done for a remote world.** A world held by
		// a host has no store in this process, so there is nothing to read the
		// subtree out of and nothing to build it into.
		if (Universe->IsRemote(source) || Universe->IsRemote(target)) {
			Say("a world held by another host cannot take or give instances",
				engine::core::LogLevel::Warning);
			return false;
		}

		// **Read first, in its own `Enter`.** Two worlds cannot be entered at
		// once — the affinity check aborts on re-entry — so the subtree becomes
		// a document while the source is open, and the document is what crosses.
		std::string document;
		Name moved;
		Universe->Enter(source, [&](Store &store) {
			if (!store.Alive(instance)) {
				return;
			}
			moved = store.InstanceNameOf(instance);
			document = engine::game::WriteInstanceDocument(store, instance);
		});

		if (document.empty()) {
			Say("that instance could not be written out, so it was not moved",
				engine::core::LogLevel::Error);
			return false;
		}

		std::string error;
		engine::ecs::Entity rebuilt = NULL_ENTITY;
		Universe->Enter(target, [&](Store &store) {
			// A parent that died between the drop and here leaves the subtree
			// at the world's root, which is where a drop onto a world row puts
			// it anyway.
			const engine::ecs::Entity into = parent != NULL_ENTITY && store.Alive(parent) ? parent
																						 : NULL_ENTITY;
			rebuilt = engine::game::ReadInstanceDocument(store, document, into, error);
		});

		if (rebuilt == NULL_ENTITY) {
			// **The original is still there**, which is the whole reason the
			// destroy is last. A move that deleted first and then failed to
			// rebuild would be a delete somebody did not ask for, with no undo
			// to reach for — see `mono.studio/AGENTS.md`.
			Say("could not move '" + std::string(Label(moved)) + "': " + error,
				engine::core::LogLevel::Error);
			return false;
		}

		// Script tabs first, for `RenameWorld`'s reason: a tab holds an entity
		// handle in the world being left, and one that saved afterwards would
		// write into storage that had been freed.
		for (size_t index = Scripts.size(); index > 0; index--) {
			if (Scripts[index - 1].World == source) {
				bool inside = false;
				Universe->Enter(source, [&](Store &store) {
					for (engine::ecs::Entity at = Scripts[index - 1].Instance;
						 at != NULL_ENTITY && store.Alive(at);
						 at = store.ParentOf(at)) {
						if (at == instance) {
							inside = true;
							break;
						}
					}
				});

				if (inside) {
					CloseScriptTab(index - 1);
				}
			}
		}

		Universe->Enter(source, [&](Store &store) {
			if (store.Alive(instance)) {
				// **The subtree, and unlinked from its parent.** `Destroy`
				// takes this one row and leaves the rest: the children stay
				// alive in a world nothing can reach them from, and the parent
				// keeps naming a freed handle as a child. A move that leaves
				// the source world holding both is not a move.
				store.DestroyInstance(instance);
			}
		});

		InstanceCounts.clear();

		// The selection follows the instance, which is what makes the move feel
		// like a move rather than like a delete and a paste somewhere else.
		SelectionWorld = target;
		Selection.assign(1, rebuilt);
		if (parent != NULL_ENTITY) {
			Expanded.push_back(parent.Id);
		}

		MarkModified();
		Say("moved '" + std::string(Label(moved)) + "' to '" +
			std::string(Label(Universe->NameOf(target))) + "'");
		return true;
	}

	void Editor::ApplyPendingActions() {
		// **Everything any panel queued, applied outside `Universe::Enter` and
		// outside every panel.**
		//
		// Two reasons, and the second was a bug. Drawing a world's tree happens
		// inside `Enter` and every action here enters a world itself, so it
		// cannot run where it was asked for — `Enter` aborts on re-entry rather
		// than allowing it, which is the affinity check doing its job.
		//
		// And it cannot run at the end of the panel that asked, either, because
		// panels are closable: this used to be called from `DrawExplorer` and
		// its world half from `DrawWorlds`, so closing the explorer made
		// "Remove" in the *Worlds* panel silently do nothing. One call, from
		// `DrawInterface`, every frame.

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

		if (PendingMove.Source.IsValid()) {
			const PendingMoveAction move = PendingMove;
			PendingMove = PendingMoveAction{};
			MoveInstanceToWorld(move.Source, move.Instance, move.Target, move.Parent);
		}

		if (PendingLookThroughSet) {
			PendingLookThroughSet = false;
			FollowCamera = PendingLookThrough;
			PendingLookThrough = NULL_ENTITY;

			Say(FollowCamera == NULL_ENTITY ? "back to the editor camera"
											: "looking through the scene's camera — right-drag to fly");
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

		ApplyPendingWorldActions();
	}
}
