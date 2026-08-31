#include "SourceEditor.hpp"

#include <engine/ecs/Classes.hpp>
#include <engine/game/Values.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Instances.hpp>
#include <engine/ui/Fonts.hpp>
#include <engine/ui/PerCallSite.hpp>
#include <engine/ui/Theme.hpp>

#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <studio/Complete.hpp>
#include <studio/Editor.hpp>
#include <studio/Keybinds.hpp>
#include <studio/Widgets.hpp>
#include <vector>

namespace studio {

	using engine::core::Name;
	using engine::ecs::Classes;
	using engine::ecs::ClassId;
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

				// **The filter is `InsertableClasses`, not a copy of it here.**
				// Which classes an author may name is now asked in two places -
				// this palette and the script editor's completion popup - and
				// `mono.tools/bindings` says why the answer may only live in
				// one: the abstract bases are excluded by name, the run time
				// would mint them perfectly happily, and two lists of names
				// that must agree are two lists that eventually do not.
				std::vector<std::pair<int, ClassId>> scored;
				for (const ClassId id : InsertableClasses()) {
					int score = 0;
					if (FuzzyMatch(Query, Label(Classes::Describe(id).Name), score)) {
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
		// One picker per call site, keyed by the id the caller gave. The
		// toolbar's popup and the tree's context menu are two different lists
		// with two different queries, and sharing one would make typing in one
		// filter the other.
		//
		// **The typed text and the search are one record and two fields.** One
		// record because they are keyed the same way, and keeping them in two
		// tables was two scans to answer one question. Two fields because
		// `Refresh` compares them: `Query` is what is in the box now and
		// `Search.Query` is what the results on screen are the answer to, and
		// collapsing those would either re-score three hundred names every
		// frame or never re-score them at all.
		struct Picker {
			std::string Query;
			ClassSearch Search;
		};

		Picker &picker = engine::ui::PerCallSite<Picker>(id);
		std::string *const query = &picker.Query;
		ClassSearch *const search = &picker.Search;

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
		// world's menu is drawn from outside it - the world row is above the
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

		const bool service = haveInstance && store.Get<engine::scene::ServiceComponent>(instance) != nullptr;
		if (ImGui::MenuItem(
				"Rename", Keybinds::Of(Action::Rename).Text().c_str(), false, haveInstance && !service
			)) {
			PendingRenameStart = instance;
		}
		if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, haveInstance)) {
			PendingDuplicate = true;
		}
		if (ImGui::MenuItem("Delete", "Del", false, haveInstance)) {
			PendingDelete = true;
		}

		if (haveInstance) {
			ImGui::Separator();

			if (SourceDocumentKindOf(store, instance).has_value()) {
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

			// **Only for something with a place.** An `EditableMesh`, a
			// `ModuleScript` or a service has no `Transform`, so there is
			// nowhere to point a camera at - the item is greyed rather than
			// hidden, because a menu whose length changes per row is one nobody
			// learns the shape of.
			const bool placed = store.Get<engine::scene::Transform>(instance) != nullptr;
			if (ImGui::MenuItem("Zoom To", nullptr, false, placed)) {
				PendingZoomWorld = world;
				PendingZoomTo = instance;
			}

			if (ImGui::MenuItem("Unparent")) {
				PendingReparent.World = world;
				PendingReparent.Parent = NULL_ENTITY;
				PendingReparent.Instances.clear();

				// The menu acts on the selection when the row it was opened
				// over is part of it, exactly as Delete and Duplicate do.
				if (world == SelectionWorld && IsSelected(instance)) {
					PendingReparent.Instances = Selection;
				} else {
					PendingReparent.Instances.push_back(instance);
				}
			}
		}
	}

	void Editor::FrameViewportOn(WorldId world, const engine::core::Vector3 &centre, float radius) {
		// A part may be authored flat on an axis - a pane is - and one flat on
		// all three would put the camera inside it.
		radius = std::max(radius, 0.5f);

		// The viewport this applies to: the focused one when it is showing the
		// world, and the main one otherwise.
		//
		// **One panel rather than every panel showing the world.** Two viewports
		// both jumping because a row was right-clicked once is the surprise; the
		// second panel is usually the one being kept still on purpose.
		size_t panel = 0;
		if (FocusedIsViewport && ViewportWorld(FocusedViewport) == world) {
			panel = FocusedViewport;
		} else if (Active != world) {
			for (size_t index = 1; index <= Extras.size(); index++) {
				if (Extras[index - 1].Open && ViewportWorld(index) == world) {
					panel = index;
					break;
				}
			}
		}

		ViewportState *view = ExtraAt(panel);
		engine::core::CFrame &frame = view != nullptr ? view->Frame : CameraFrame;
		const float yaw = view != nullptr ? view->Yaw : CameraYaw;
		const float pitch = view != nullptr ? view->Pitch : CameraPitch;

		// **The same framing arithmetic `MeshPreview` uses**, and for the same
		// reason: the distance at which a sphere of this radius subtends the
		// whole of a vertical field of view is `radius / tan(fov / 2)`, and the
		// padding is what stops it touching the edges.
		//
		// A quarter rather than the four fifths a first attempt used. The radius
		// is already the *sphere* around the box, so rotation is covered before
		// the padding is applied at all - and doubling on top of that puts a
		// wide flat thing, which a terrain arena is, in the middle of a mostly
		// empty frame.
		constexpr float PADDING = 1.25f;
		const float halfFov = engine::scene::Camera{}.FieldOfViewRadians * 0.5f;
		const float distance = (radius / std::tan(halfFov)) * PADDING;

		const engine::core::Vector3 forward =
			engine::core::CFrame::Angles(pitch, yaw, 0.0f)
				.VectorToWorldSpace(engine::core::Vector3{0.0f, 0.0f, -1.0f});

		frame = engine::core::CFrame(centre - forward * distance, frame.Rotation());
	}

	bool Editor::FrameWorldContents(WorldId world) {
		if (!world.IsValid()) {
			return false;
		}

		engine::core::Vector3 lowest{};
		engine::core::Vector3 highest{};
		bool found = false;

		Universe->Enter(world, [&](Store &store) {
			// **Drawable rows only.** A world's services, its scripts and its
			// modules have no place, and an invisible part is one the author
			// does not want looked at - framing on either would aim the camera
			// at somewhere nothing is.
			store.Each<
				const engine::scene::Transform,
				const engine::scene::Bounds,
				const engine::scene::Visual>([&](Entity,
												 const engine::scene::Transform &placement,
												 const engine::scene::Bounds &bounds,
												 const engine::scene::Visual &visual) {
				if (!visual.Visible) {
					return;
				}

				const engine::core::Vector3 at = placement.Frame.Position;
				const engine::core::Vector3 half = bounds.HalfExtent;

				if (!found) {
					lowest = at - half;
					highest = at + half;
					found = true;
					return;
				}

				lowest = engine::core::Vector3{
					std::min(lowest.X, at.X - half.X),
					std::min(lowest.Y, at.Y - half.Y),
					std::min(lowest.Z, at.Z - half.Z),
				};
				highest = engine::core::Vector3{
					std::max(highest.X, at.X + half.X),
					std::max(highest.Y, at.Y + half.Y),
					std::max(highest.Z, at.Z + half.Z),
				};
			});
		});

		if (!found) {
			return false;
		}

		const engine::core::Vector3 centre = (lowest + highest) * 0.5f;
		FrameViewportOn(world, centre, ((highest - lowest) * 0.5f).Magnitude());
		return true;
	}

	void Editor::ZoomViewportTo(WorldId world, Entity instance) {
		if (!world.IsValid() || instance == NULL_ENTITY) {
			return;
		}

		engine::core::CFrame placement;
		float radius = 0.0f;
		bool found = false;

		Universe->Enter(world, [&](Store &store) {
			const auto *transform = store.Get<engine::scene::Transform>(instance);
			if (transform == nullptr) {
				return;
			}

			placement = transform->Frame;
			found = true;

			// **The bounding sphere of the box, not the box.** A camera pulled
			// back far enough for the half-extent alone leaves a part's corners
			// outside the frame whenever it is turned, and a part somebody has
			// just asked to look at is usually turned.
			const auto *bounds = store.Get<engine::scene::Bounds>(instance);
			const engine::core::Vector3 half =
				bounds != nullptr ? bounds->HalfExtent : engine::core::Vector3{0.5f, 0.5f, 0.5f};
			radius = half.Magnitude();
		});

		if (!found) {
			Say("that instance has no place in the world to look at");
			return;
		}

		FrameViewportOn(world, placement.Position, radius);
		Say("framed the selection");
	}

	Editor::WorldTree &Editor::TreeFor(WorldId world) {
		for (WorldTree &tree : Trees) {
			if (tree.World == world) {
				return tree;
			}
		}

		Trees.push_back(WorldTree{});
		Trees.back().World = world;
		return Trees.back();
	}

	void Editor::BeginRename(engine::ecs::Entity instance) {
		if (instance == NULL_ENTITY) {
			return;
		}

		bool service = false;
		if (SelectionWorld.IsValid()) {
			Universe->Enter(SelectionWorld, [&](Store &store) {
				service =
					store.Alive(instance) && store.Get<engine::scene::ServiceComponent>(instance) != nullptr;
			});
		}
		if (service) {
			return;
		}

		Renaming = instance;
		RenameFocus = true;
		RenameBuffer.clear();

		// Seeded with the name it has, because a rename is almost always an
		// edit of the current name rather than a replacement for it - and a
		// field that opened empty would make "Part" into "Part2" a retype.
		if (SelectionWorld.IsValid()) {
			Universe->Enter(SelectionWorld, [&](Store &store) {
				if (store.Alive(instance)) {
					RenameBuffer = Label(store.InstanceNameOf(instance));
				}
			});
		}
	}

	void Editor::OpenPathTo(WorldId world, engine::ecs::Entity instance) {
		if (!world.IsValid() || instance == NULL_ENTITY) {
			return;
		}

		WorldTree &tree = TreeFor(world);

		// **The store, not the compiled view.** This is reached from actions -
		// an insert, a paste, a Find result - and the view describing the world
		// as it was *before* that action has never heard of what was just made.
		Universe->Enter(world, [&](Store &store) {
			for (engine::ecs::Entity walk = store.ParentOf(instance);
				 walk != NULL_ENTITY && store.Alive(walk);
				 walk = store.ParentOf(walk)) {
				if (std::find(tree.Open.begin(), tree.Open.end(), walk) == tree.Open.end()) {
					tree.Open.push_back(walk);
				}
			}
		});
	}

	void Editor::SelectRange(
		WorldId world, const HierarchyView &view, engine::ecs::Entity anchor, engine::ecs::Entity to, bool add
	) {
		if (ViewportWorld(FocusedViewport) != world) {
			RetargetEditingViewport(FocusedViewport, world);
		}

		ClearRootSelection();

		// **The range itself is `RowsBetween`, which is a free function over the
		// compiled tree and is tested as one.** What is left here is the part
		// that needs the editor: which world the selection belongs to, and
		// whether this click replaces it or adds to it.
		const std::span<const HierarchyRow> range = RowsBetween(view, anchor, to);

		// An anchor that has been deleted, collapsed out of sight or filtered
		// away is no anchor. Falling back to a plain click is what every list
		// does, and is what an author reads the gesture as when the row they
		// remember shift-clicking from is no longer on screen.
		if (range.empty()) {
			Select(world, to, add);
			return;
		}

		if (world != SelectionWorld) {
			Selection.clear();
			SelectionWorld = world;
		}
		if (!add) {
			Selection.clear();
		}

		for (const HierarchyRow &row : range) {
			if (std::find(Selection.begin(), Selection.end(), row.Instance) == Selection.end()) {
				Selection.push_back(row.Instance);
			}
		}

		// **The anchor does not move.** Shift-clicking a second time has to
		// re-measure from the same place, or dragging the range back over
		// itself grows instead of shrinking.
	}

	void Editor::DrawInstanceRows(Store &store, WorldTree &tree) {
		const std::span<const HierarchyRow> rows = tree.View.Rows();
		if (rows.empty()) {
			ImGui::TextDisabled(tree.View.Filtering() ? "no instance matches" : "empty scene");
			return;
		}

		const WorldId world = tree.World;
		const float step = ImGui::GetStyle().IndentSpacing;

		// Resolved before the clipper, because a row it is going to skip cannot
		// be scrolled to - `IncludeItemByIndex` is what keeps it submitted.
		size_t reveal = HierarchyView::NO_ROW;
		if (RevealSelection && world == SelectionWorld && !Selection.empty()) {
			reveal = tree.View.RowOf(Selection.front());
		}

		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(rows.size()));
		if (reveal != HierarchyView::NO_ROW) {
			clipper.IncludeItemByIndex(static_cast<int>(reveal));
		}

		while (clipper.Step()) {
			for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; index++) {
				const HierarchyRow &row = rows[static_cast<size_t>(index)];

				// **Keyed by the instance, not by the row number.** A row's
				// index moves whenever anything above it opens, closes or is
				// filtered away, and an imgui id that moves takes its state
				// with it - most visibly the open context menu, which would
				// detach from the row it was opened on and reattach to whoever
				// inherited the number.
				ImGui::PushID(static_cast<int>(row.Instance.Id));
				if (row.Depth > 0) {
					// **`Indent` rather than `TreePush`.** A push has to be
					// matched by a pop in the same order, which is a recursion
					// - and a recursion is the one thing a clipper cannot skip
					// through. Indenting by depth costs the same pixels and
					// leaves every row independent of the ones above it.
					ImGui::Indent(static_cast<float>(row.Depth) * step);
				}

				ImGuiTreeNodeFlags flags =
					ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
					ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_NoTreePushOnOpen;
				if (!row.HasChildren) {
					flags |= ImGuiTreeNodeFlags_Leaf;
				}
				if (world == SelectionWorld && IsSelected(row.Instance)) {
					flags |= ImGuiTreeNodeFlags_Selected;
				}

				// Ours to assert every frame, because the row list was built
				// from it. imgui still toggles its own copy on a click, and
				// `IsItemToggledOpen` below is how that reaches us.
				const bool renaming = row.Instance == Renaming;

				ImGui::SetNextItemOpen(row.Open);

				// **The node is still submitted while renaming, with no
				// label.** It keeps the row's height, its arrow and its
				// indentation, so the tree does not jump as the field opens and
				// closes - and it keeps the clipper's rows uniform, which is
				// what lets it skip by arithmetic.
				ImGui::TreeNodeEx("##node", flags, "%s", renaming ? "" : row.Text);

				// **Everything that asks about "the last item" happens here,
				// before anything else is drawn.** imgui has exactly one last
				// item, and it is whatever was submitted most recently - so the
				// dimmed class name below would become it, and
				// `BeginDragDropSource` would then be asked to drag a `Text`
				// with no id. That is an assertion rather than a subtle bug,
				// which is the good version of this mistake.
				const bool toggled = ImGui::IsItemToggledOpen();
				const bool hovered = ImGui::IsItemHovered();

				if (toggled) {
					const auto found = std::find(tree.Open.begin(), tree.Open.end(), row.Instance);
					if (found != tree.Open.end()) {
						tree.Open.erase(found);
					} else {
						tree.Open.push_back(row.Instance);
					}
				}

				if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !toggled) {
					const ImGuiIO &io = ImGui::GetIO();
					if (io.KeyShift && SelectionAnchor != NULL_ENTITY) {
						SelectRange(world, tree.View, SelectionAnchor, row.Instance, io.KeyCtrl);
					} else {
						Select(world, row.Instance, io.KeyCtrl);
						SelectionAnchor = row.Instance;
					}
				}

				// **A right-click inside the selection keeps it.** It used to
				// collapse to the one row, so "select five parts, right-click,
				// Delete" deleted one - the gesture every author uses, doing
				// four fifths less than it looked like it would.
				if (ImGui::IsItemClicked(ImGuiMouseButton_Right) &&
					!(world == SelectionWorld && IsSelected(row.Instance))) {
					Select(world, row.Instance, false);
					SelectionAnchor = row.Instance;
				}

				if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
					if (SourceDocumentKindOf(store, row.Instance).has_value()) {
						PendingOpenScript.World = world;
						PendingOpenScript.Instance = row.Instance;
					}
				}

				// --- drag and drop ------------------------------------------

				if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoDisableHover)) {
					const DragPayload payload{world.Index, row.Instance};
					ImGui::SetDragDropPayload(DRAG_TYPE, &payload, sizeof(payload));

					// Dragging a row that is part of the selection drags the
					// selection, which is what the highlight already promised.
					if (world == SelectionWorld && IsSelected(row.Instance) && Selection.size() > 1) {
						ImGui::Text("%zu instances", Selection.size());
					} else {
						ImGui::TextUnformatted(row.Text);
					}
					ImGui::EndDragDropSource();
				}

				if (ImGui::BeginDragDropTarget()) {
					if (const ImGuiPayload *dropped = ImGui::AcceptDragDropPayload(DRAG_TYPE)) {
						const auto *moved = static_cast<const DragPayload *>(dropped->Data);

						// **Two different operations, and the payload's world
						// is what picks between them.** Within one store a move
						// is a `SetParent` and the instance keeps its handle,
						// its id and everything pointing at it. Across two
						// stores none of that is possible - an `Entity` means
						// nothing in another world - so the subtree is written
						// out, rebuilt on the other side and the original
						// destroyed. Same gesture, and the cost is not the
						// same, which is why they are not one code path
						// pretending to be.
						PendingReparent.World = WorldId{};
						PendingMove.Source = WorldId{};

						if (moved->World == world.Index) {
							PendingReparent.World = world;
							PendingReparent.Parent = row.Instance;
							PendingReparent.Instances.clear();

							if (world == SelectionWorld && IsSelected(moved->Instance)) {
								PendingReparent.Instances = Selection;
							} else {
								PendingReparent.Instances.push_back(moved->Instance);
							}
						} else {
							PendingMove.Source = WorldFor(moved->World);
							PendingMove.Instance = moved->Instance;
							PendingMove.Target = world;
							PendingMove.Parent = row.Instance;
						}
					}
					ImGui::EndDragDropTarget();
				}

				if (ImGui::BeginPopupContextItem("##actions")) {
					DrawInstanceActions(store, world, row.Instance);
					ImGui::EndPopup();
				}

				// **After every consumer of the tree node's last-item state.** The
				// insertion button submits its own item, so drawing it any earlier makes
				// clicks, drags and context menus target this button instead of the row.
				if (hovered) {
					ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight());
					if (ImGui::SmallButton("+##insert-hover")) {
						ImGui::OpenPopup("##insert-hover-popup");
					}
				}
				if (ImGui::BeginPopup("##insert-hover-popup")) {
					if (const ClassId chosen = DrawClassPicker("insert-hover-picker"); chosen.IsValid()) {
						PendingInsert.World = world;
						PendingInsert.Class = chosen;
						PendingInsert.Parent = row.Instance;
						ImGui::CloseCurrentPopup();
					}
					ImGui::EndPopup();
				}

				if (renaming) {
					ImGui::SameLine();
					ImGui::SetNextItemWidth(-1.0f);
					if (RenameFocus) {
						ImGui::SetKeyboardFocusHere();
						RenameFocus = false;
					}

					TextField("##rename", RenameBuffer);

					// **Committed when the field is deactivated after an edit,
					// cancelled when it is deactivated without one.** That is
					// one test for three gestures: Enter and a click elsewhere
					// both commit what was typed, and Escape reverts the field
					// before deactivating it so nothing was edited at all.
					if (ImGui::IsItemDeactivatedAfterEdit()) {
						PendingRenameInstance.World = world;
						PendingRenameInstance.Instance = row.Instance;
						PendingRenameInstance.To = RenameBuffer;
						Renaming = NULL_ENTITY;
					} else if (ImGui::IsItemDeactivated()) {
						Renaming = NULL_ENTITY;
					}

					if (row.Depth > 0) {
						ImGui::Unindent(static_cast<float>(row.Depth) * step);
					}
					ImGui::PopID();
					continue;
				}

				// The class name, dimmed, on the same row. Roblox puts an icon
				// here; an icon set is a texture atlas and a lot of art, and
				// the class name carries the same information in the space
				// available. Smaller as well as dimmer: a class name is an
				// annotation on the row rather than a second name on it, and
				// two things at the same size read as two names however
				// different their colours are.
				ImGui::SameLine();
				{
					const engine::ui::ScopedFont small(
						engine::ui::Typeface::Interface, engine::ui::TextSize::Small
					);
					ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
					ImGui::TextUnformatted(row.ClassText);
					ImGui::PopStyleColor();
				}

				if (static_cast<size_t>(index) == reveal) {
					// The row somebody selected somewhere else, brought to the
					// middle rather than to whichever edge it happened to be
					// nearest - an author who has just clicked a part in the
					// viewport wants to see what is around it in the tree.
					ImGui::SetScrollHereY(0.5f);
				}

				if (row.Depth > 0) {
					ImGui::Unindent(static_cast<float>(row.Depth) * step);
				}
				ImGui::PopID();
			}
		}
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

		const char *worldLabel = "All Worlds";
		if (ExplorerWorld.IsValid()) {
			const Name name = Universe->NameOf(ExplorerWorld);
			worldLabel = name.IsValid() ? Label(name) : "?";
		}
		if (ImGui::BeginCombo("World", worldLabel)) {
			if (ImGui::Selectable("All Worlds", !ExplorerWorld.IsValid())) {
				ExplorerWorld = {};
			}
			for (const WorldId world : Universe->Worlds()) {
				const Name name = Universe->NameOf(world);
				const char *label = name.IsValid() ? Label(name) : "?";
				if (ImGui::Selectable(label, ExplorerWorld == world)) {
					ExplorerWorld = world;
				}
			}
			ImGui::EndCombo();
		}

		if (ImGui::CollapsingHeader("Property search")) {
			ImGui::SetNextItemWidth(-1.0f);
			TextField("##explorer-class", Find.Class, "class - Part, BasePart, Script");
			ImGui::SetNextItemWidth(-1.0f);
			TextField("##explorer-property", Find.Property, "property");
			ImGui::SetNextItemWidth(-1.0f);
			TextField("##explorer-value", Find.Value, "value");
			ImGui::Checkbox("exact value", &Find.Exact);

			Find.Name = ExplorerFilter;
			RunFind();
			ImGui::TextDisabled(
				"%zu matching instance%s", FindResults.size(), FindResults.size() == 1 ? "" : "s"
			);

			WorldId pickWorld;
			Entity pick = NULL_ENTITY;
			if (ImGui::BeginChild("##explorer-search-results", ImVec2(0.0f, 120.0f))) {
				for (size_t index = 0; index < FindResults.size(); index++) {
					const FindResult &result = FindResults[index];
					ImGui::PushID(static_cast<int>(index));
					const bool selected = SelectionWorld == result.World && IsSelected(result.Instance);
					if (ImGui::Selectable("##result", selected)) {
						pickWorld = result.World;
						pick = result.Instance;
					}
					ImGui::SameLine();
					ImGui::Text("%s (%s)", result.Name.c_str(), result.Class.c_str());
					ImGui::PopID();
				}
			}
			ImGui::EndChild();
			if (pick != NULL_ENTITY) {
				Select(pickWorld, pick, false);
			}
		}

		ImGui::Separator();
		const WorldId editingWorld = ViewportWorld(FocusedViewport);

		// **The universe is the root, and it is `game`.** `script`'s bindings
		// already map `game` to `world::Universe` and `workspace` to the world a
		// script runs on; drawing the same shape means what an author sees in
		// the tree and what a script reaches through `game` are one object
		// rather than two models kept in step.
		//
		// **Tagged "(universe)" rather than "(game)", and the worlds below are
		// each "(world)".** The two words were doing one job badly: the root
		// said "game" and a world said nothing, except the active one, which
		// said "(workspace)" - so the tag on a row changed as somebody clicked
		// around, and `Workspace` is now a real instance *inside* a world,
		// which made that word mean two things one line apart. What a row is
		// does not depend on which row is selected; selection is what the
		// highlight is for.
		const std::string universeLabel =
			std::string(GameName.IsValid() ? Label(GameName) : "Game") + "  (universe)";

		// **`OpenOnArrow`, and without it this row could not be selected at
		// all.** A tree node with no such flag toggles open on a click anywhere
		// along it, which sets imgui's `IsItemToggledOpen` for that frame - and
		// the guard below refuses a click that toggled, because a person opening
		// a node is not choosing it. The two together meant every click on the
		// universe row opened or closed it and none of them ever reached the
		// selection, so `UniverseSelected` was false for the life of the session
		// and the Properties panel showed "nothing selected" for a root whose
		// editable settings were sitting behind it.
		//
		// The world rows below have always had the flag, which is why they
		// select and this did not.
		const bool universeOpen = ImGui::TreeNodeEx(
			"##universe",
			ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth |
				ImGuiTreeNodeFlags_OpenOnArrow |
				(UniverseSelected ? ImGuiTreeNodeFlags_Selected : ImGuiTreeNodeFlags_None),
			"%s",
			universeLabel.c_str()
		);

		// Immediately after the node, before anything else is submitted. See
		// the note in `DrawTreeNode`: imgui has exactly one "last item".
		if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
			ClearSelection();
			SelectionWorld = {};
			UniverseSelected = true;
			UniverseNameDraft = std::string(GameName.IsValid() ? GameName.Text() : "Game");
		}

		if (ImGui::BeginPopupContextItem("##universe-actions")) {
			// Insert lands in the active world, because a universe holds worlds
			// and not instances - there is no other honest answer, and refusing
			// outright would make the root the one row with no menu.
			DrawInsertMenu("insert-universe", editingWorld, NULL_ENTITY);

			ImGui::Separator();

			// Enabled while a scene runs. `DrawWorlds` carries why: Stop
			// restores one world from its own document, so a scene added during
			// a run is in no snapshot and survives.
			if (ImGui::MenuItem("New World...")) {
				AskingNewWorld = true;
				NameBuffer = "World " + std::to_string(Universe->Count() + 1);
			}
			if (ImGui::MenuItem("Import World...", nullptr, false, !AnyRunning())) {
				AskingImport = true;
				PathBuffer.clear();
			}

			// Beside the other two ways of getting a scene, because this menu is
			// already the universe's own answer to "add a world" and a person who
			// found New World here should not have to go to the menu bar for the
			// shipped ones. Same function as the menu bar and the Worlds panel.
			DrawExampleSceneMenu();

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
				if (ExplorerWorld.IsValid() && ExplorerWorld != world) {
					continue;
				}
				const Name worldName = Universe->NameOf(world);

				ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow;
				if (world == SelectedWorldRow || (world == editingWorld && !UniverseSelected &&
												  Selection.empty() && !SelectedWorldRow.IsValid())) {
					flags |= ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Selected;
				}

				// The same consume-once request `DrawTreeNode` honours for an
				// instance, in its own set because a world index and an entity
				// id are different key spaces. See `ExpandedWorlds`.
				if (const auto found = std::find(ExpandedWorlds.begin(), ExpandedWorlds.end(), world.Index);
					found != ExpandedWorlds.end()) {
					ImGui::SetNextItemOpen(true);
					ExpandedWorlds.erase(found);
				}

				ImGui::PushID(static_cast<int>(world.Index));

				// **A replica says so on its own row**, because the tag is the
				// only thing that distinguishes it. A client view holds the same
				// instances under the same names as the scene it is a view of,
				// so an author who scrolled to the wrong one has no other cue -
				// and an edit made there reaches one client and is overwritten
				// by the next delta. See `EditAuthority`.
				const bool clientView = AuthorityOf(world) == EditAuthority::ClientLocal;

				const std::string label = std::string(worldName.IsValid() ? Label(worldName) : "?") +
										  (clientView ? "  (client view)" : "  (world)");

				const bool open = ImGui::TreeNodeEx("##world", flags, "%s", label.c_str());

				if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
					RetargetEditingViewport(FocusedViewport, world);

					// **Selecting the row as well as retargeting the viewport**,
					// which is the rule the universe row above already follows:
					// clicking a row in this tree selects that row. Until v0.17
					// a world row was the one exception - it moved the viewport
					// and left the Properties panel showing whichever instance
					// was selected before, so the world's own tick rates had
					// nowhere to be shown even after they became editable.
					//
					// `ClearSelection` first, so the instance selection goes
					// with it: an explorer highlighting an instance while
					// Properties describes a world is two answers to "what am I
					// looking at".
					ClearSelection();
					SelectionWorld = {};
					SelectedWorldRow = world;
				}

				// **A world row takes a drop too**, and it means "a root of
				// this world". Without it the only way to move something
				// between worlds would be to drop it onto an instance that
				// happened to already be there - which is impossible for the
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
							PendingReparent.Parent = NULL_ENTITY;
							PendingReparent.Instances.clear();

							if (world == SelectionWorld && IsSelected(moved->Instance)) {
								PendingReparent.Instances = Selection;
							} else {
								PendingReparent.Instances.push_back(moved->Instance);
							}
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

					if (ImGui::MenuItem("Show in Editing Viewport")) {
						RetargetEditingViewport(FocusedViewport, world);
					}
					if (ImGui::MenuItem("Export World...")) {
						Active = world;
						AskingExport = true;
						PathBuffer =
							std::string(Label(worldName)) + std::string(engine::game::WORLD_EXTENSION);
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
						WorldTree &tree = TreeFor(world);

						Universe->Enter(world, [&](Store &store) {
							// **The whole tree, worked out in one pass over the
							// store and only when the store has moved.** See
							// `studio/Hierarchy.hpp`: the scan is a signature,
							// and the rows behind it are re-compiled only when
							// that signature does not match the last one.
							HierarchyRequest request;
							request.Filter = ExplorerFilter;
							request.Open = tree.Open;

							// The selection reveals its own path, and only on
							// the frame something outside this panel asked for
							// it. Folded into the signature, so asking is a
							// re-compile and not asking is not.
							std::span<const engine::ecs::Entity> reveal;
							if (RevealSelection && world == SelectionWorld && !Selection.empty()) {
								reveal = Selection;
							}
							request.Reveal = reveal;

							tree.View.Rebuild(store, request);
							DrawInstanceRows(store, tree);

							if (ImGui::BeginPopupContextWindow(
									"##world-blank",
									ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems
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

		// **Consumed here, whatever happened to it.** A reveal that could not
		// land - the world is collapsed, the selection was emptied, the panel
		// is showing something else - must not stay pending, or the tree
		// scrolls itself back to the selection on some later frame for a reason
		// nobody can connect to anything they did.
		RevealSelection = false;

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
		// once - the affinity check aborts on re-entry - so the subtree becomes
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
			Say("that instance could not be written out, so it was not moved", engine::core::LogLevel::Error);
			return false;
		}

		std::string error;
		engine::ecs::Entity rebuilt = NULL_ENTITY;
		Universe->Enter(target, [&](Store &store) {
			// A parent that died between the drop and here leaves the subtree
			// at the world's root, which is where a drop onto a world row puts
			// it anyway.
			const engine::ecs::Entity into =
				parent != NULL_ENTITY && store.Alive(parent) ? parent : NULL_ENTITY;
			rebuilt = engine::game::ReadInstanceDocument(store, document, into, error);
		});

		if (rebuilt == NULL_ENTITY) {
			// **The original is still there**, which is the whole reason the
			// destroy is last. A move that deleted first and then failed to
			// rebuild would be a delete somebody did not ask for, with no undo
			// to reach for - see `mono.studio/AGENTS.md`.
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
				// Authored: the Delete key is a person asking, and a fixture
				// refuses it exactly as a script's `Destroy()` does.
				(void)store.DestroyAuthored(instance);
			}
		});

		InstanceCounts.clear();

		// The selection follows the instance, which is what makes the move feel
		// like a move rather than like a delete and a paste somewhere else.
		SelectionWorld = target;
		Selection.assign(1, rebuilt);
		SelectionAnchor = rebuilt;
		OpenPathTo(target, rebuilt);
		RevealSelection = true;

		MarkModified();
		Say("moved '" + std::string(Label(moved)) + "' to '" + std::string(Label(Universe->NameOf(target))) +
			"'");
		return true;
	}

	void Editor::ApplyPendingActions() {
		// **Everything any panel queued, applied outside `Universe::Enter` and
		// outside every panel.**
		//
		// Two reasons, and the second was a bug. Drawing a world's tree happens
		// inside `Enter` and every action here enters a world itself, so it
		// cannot run where it was asked for - `Enter` aborts on re-entry rather
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
			const bool authoritative = AuthorityOf(world) == EditAuthority::Authoritative;
			const engine::ecs::Entity parent = PendingReparent.Parent;
			const std::vector<engine::ecs::Entity> moving = PendingReparent.Instances;
			PendingReparent = PendingReparentAction{};

			// One record per instance, taken while the world is open, and
			// applied there too. `RecordReparent` is outside the `Enter`
			// because the command log enters the world itself.
			struct Applied {
				engine::ecs::Entity Instance;
				engine::ecs::Entity Was;
				std::string Named;
			};
			std::vector<Applied> applied;
			size_t refused = 0;

			// **The nested members dropped before the world is entered**, by
			// `TopMost` - a free function over the compiled tree, tested as
			// one. Dragging a model and one of its own parts together means
			// "move the model", and moving both would take the part out of the
			// model on the way.
			std::vector<engine::ecs::Entity> topMost;
			TopMost(TreeFor(world).View, moving, topMost);

			Universe->Enter(world, [&](Store &store) {
				for (const engine::ecs::Entity instance : topMost) {
					if (!store.Alive(instance)) {
						continue;
					}
					if (parent != NULL_ENTITY && !store.Alive(parent)) {
						return;
					}

					// Read before the write, because the parent it had is the
					// whole of what undo needs and there is no way to ask
					// afterwards.
					const engine::ecs::Entity was = store.ParentOf(instance);
					std::string named(Label(store.InstanceNameOf(instance)));

					// **Refused rather than allowed to cycle.** `SetParent`
					// already refuses to make an instance its own ancestor - a
					// cycle in the tree is a hang in every walk of it rather
					// than a wrong answer - and this reports the refusal
					// instead of leaving a drag that silently did nothing.
					if (store.SetParentAuthored(instance, parent)) {
						applied.push_back(Applied{instance, was, std::move(named)});
					} else {
						refused++;
					}
				}
			});

			// **Recorded only for the ones that landed.** A refused reparent is
			// not an edit, and logging one would put an entry on the stack
			// whose undo restores the parent it never left.
			if (authoritative && Commands != nullptr) {
				for (const Applied &entry : applied) {
					Commands->RecordReparent(world, entry.Instance, entry.Was, parent, "Move " + entry.Named);
				}
			}

			if (!applied.empty()) {
				OpenPathTo(world, applied.front().Instance);
				if (authoritative) {
					MarkModified();
				}
			}

			if (refused > 0) {
				Say("that would put an instance inside itself", engine::core::LogLevel::Warning);
			}
		}

		if (PendingRenameStart != NULL_ENTITY) {
			const engine::ecs::Entity starting = PendingRenameStart;
			PendingRenameStart = NULL_ENTITY;
			BeginRename(starting);
		}

		if (PendingRenameInstance.World.IsValid()) {
			const PendingRenameInstanceAction rename = PendingRenameInstance;
			PendingRenameInstance = PendingRenameInstanceAction{};
			const bool authoritative = AuthorityOf(rename.World) == EditAuthority::Authoritative;

			bool renamed = false;
			engine::game::PropertyValue before;
			engine::game::PropertyValue after;

			Universe->Enter(rename.World, [&](Store &store) {
				if (!store.Alive(rename.Instance) ||
					store.Get<engine::scene::ServiceComponent>(rename.Instance) != nullptr) {
					return;
				}

				const Name was = store.InstanceNameOf(rename.Instance);
				if (Label(was) == rename.To) {
					// Nothing changed, so nothing goes on the undo stack. A
					// rename field that was opened and closed is not an edit.
					return;
				}

				// **Recorded as a write to the `Name` property**, which is a
				// real registered property here - so undo, the properties panel
				// and a script all reverse this the same way rather than the
				// tree having a private path to the same field.
				before.Type = engine::ecs::PropertyType::Name;
				before.Name = was;
				after.Type = engine::ecs::PropertyType::Name;
				after.Name = rename.To.empty() ? Name{} : Name(rename.To);

				renamed = store.SetInstanceName(rename.Instance, rename.To);
			});

			if (renamed) {
				if (authoritative && Commands != nullptr) {
					Commands->RecordProperty(
						rename.World, rename.Instance, Name("Name"), before, after, "Rename to " + rename.To
					);
				}
				if (authoritative) {
					MarkModified();
				}
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
											: "looking through the scene's camera - right-drag to fly");
		}

		if (PendingZoomWorld.IsValid()) {
			const WorldId world = PendingZoomWorld;
			const Entity instance = PendingZoomTo;
			PendingZoomWorld = WorldId{};
			PendingZoomTo = NULL_ENTITY;
			ZoomViewportTo(world, instance);
		}

		if (PendingOpenScript.World.IsValid()) {
			OpenScriptTab(PendingOpenScript.World, PendingOpenScript.Instance);
			ShowScripts = true;
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
