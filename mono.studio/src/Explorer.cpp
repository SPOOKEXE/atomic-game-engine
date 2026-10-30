#include "SourceEditor.hpp"

#include <engine/core/Paths.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/game/Values.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/ui/Fonts.hpp>
#include <engine/ui/PerCallSite.hpp>
#include <engine/ui/Theme.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <filesystem>
#include <imgui.h>
#include <imgui_internal.h>
#include <studio/Complete.hpp>
#include <studio/Editor.hpp>
#include <studio/Keybinds.hpp>
#include <studio/Widgets.hpp>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace studio {

	using engine::core::Name;
	using engine::ecs::Classes;
	using engine::ecs::ClassId;
	using engine::ecs::NULL_ENTITY;
	using engine::ecs::Store;

	namespace {
		// A drag retains its source store incarnation because world and entity
		// handles can both be reused before the mouse is released.
		constexpr const char *DRAG_TYPE = "studio.instance";

		struct DragPayload {
			uint32_t World = 0;
			engine::ecs::Entity Instance;
			uint64_t StoreIdentity = 0;
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
			PendingRenameStartWorld = world;
			PendingRenameStart = instance;
		}
		if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, haveInstance)) {
			PendingDuplicate.World = world;
			PendingDuplicate.Instances =
				world == SelectionWorld && IsSelected(instance) ? Selection : std::vector<Entity>{instance};
		}
		if (ImGui::MenuItem("Delete", "Del", false, haveInstance)) {
			PendingDelete.World = world;
			PendingDelete.Instances =
				world == SelectionWorld && IsSelected(instance) ? Selection : std::vector<Entity>{instance};
		}

		if (haveInstance) {
			ImGui::Separator();

			if (SourceDocumentKindOf(store, instance).has_value()) {
				if (ImGui::MenuItem("Edit Script")) {
					PendingOpenScript.World = world;
					PendingOpenScript.Instance = instance;
				}
			}

			// **Only for a camera, and only for the focused viewport's world.**
			// Looking through a camera in a world the viewport is not showing
			// would move the eye to somewhere the panel cannot draw.
			const ClassId cameraClass = Classes::Find(Name("Camera"));
			if (cameraClass.IsValid() && store.IsA(instance, cameraClass) &&
				world == ViewportWorld(FocusedViewport)) {
				ViewportState *viewport = ExtraAt(FocusedViewport);
				Entity &follow = viewport != nullptr ? viewport->Follow : FollowCamera;
				const bool following = follow == instance;
				if (ImGui::MenuItem(following ? "Stop Looking Through" : "Look Through Camera")) {
					PendingLookThrough = following ? NULL_ENTITY : instance;
					PendingLookThroughPrevious = follow;
					PendingLookThroughWorld = world;
					PendingLookThroughViewport = FocusedViewport;
					PendingLookThroughTitle = ViewportTitle(FocusedViewport);
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
				PendingZoomSelectionWorld = SelectionWorld;
				PendingZoomSelection = Selection;
			}

			if (ImGui::MenuItem("Unparent")) {
				PendingReparent.World = world;
				PendingReparent.StoreIdentity = store.Identity();
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

	bool Editor::FrameViewportOn(
		WorldId world, const engine::core::Vector3 &centre, float radius, bool stopFollowing
	) {
		// A part may be authored flat on an axis - a pane is - and one flat on
		// all three would put the camera inside it.
		radius = std::max(radius, 0.5f);

		// Replica viewports use their runtime camera, not the editor camera.
		const auto matches = [&](size_t index) {
			const bool open = index == 0 ? ShowViewport : index <= Extras.size() && Extras[index - 1].Open;
			if (!open) {
				return false;
			}
			const WorldId shown = ViewportWorld(index);
			const ViewportState *view = ExtraAt(index);
			const Entity follow = view != nullptr ? view->Follow : FollowCamera;
			return shown == world && !IsReplicaWorld(shown) &&
				   (stopFollowing || follow == NULL_ENTITY || IsRunning(shown));
		};
		size_t panel = Extras.size() + 1;
		if (FocusedIsViewport && matches(FocusedViewport)) {
			panel = FocusedViewport;
		} else if (matches(0)) {
			panel = 0;
		} else {
			for (size_t index = 1; index <= Extras.size(); index++) {
				if (matches(index)) {
					panel = index;
					break;
				}
			}
		}
		if (panel > Extras.size()) {
			return false;
		}

		ViewportState *view = ExtraAt(panel);
		if (stopFollowing) {
			(view != nullptr ? view->Follow : FollowCamera) = NULL_ENTITY;
		}
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
		return true;
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
		return FrameViewportOn(world, centre, ((highest - lowest) * 0.5f).Magnitude());
	}

	void Editor::ZoomViewportTo(WorldId world, Entity instance) {
		if (!world.IsValid() || instance == NULL_ENTITY) {
			return;
		}
		if (IsReplicaWorld(world)) {
			Say("the client camera cannot be framed by the editor", engine::core::LogLevel::Warning);
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

		if (!FrameViewportOn(world, placement.Position, radius, true)) {
			const auto open = [&](size_t index) {
				return index == 0 ? ShowViewport : index <= Extras.size() && Extras[index - 1].Open;
			};
			size_t panel = FocusedViewport;
			if (!open(panel)) {
				panel = 0;
				while (panel <= Extras.size() && !open(panel)) {
					panel++;
				}
			}
			if (panel > Extras.size()) {
				Say("show a viewport to frame the selection", engine::core::LogLevel::Warning);
				return;
			}
			RetargetEditingViewport(panel, world);
			if (!FrameViewportOn(world, placement.Position, radius, true)) {
				return;
			}
		}
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

	void Editor::BeginRename(WorldId world, engine::ecs::Entity instance) {
		if (!world.IsValid() || instance == NULL_ENTITY) {
			return;
		}

		bool allowed = false;
		std::string name;
		Universe->Enter(world, [&](Store &store) {
			if (store.Alive(instance) && store.Get<engine::scene::ServiceComponent>(instance) == nullptr) {
				allowed = true;
				name = Label(store.InstanceNameOf(instance));
			}
		});
		if (!allowed) {
			return;
		}

		RenameInstanceWorld = world;
		Renaming = instance;
		RenameFocus = true;
		RenameBuffer = std::move(name);
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
		// An entity handle only identifies a row within its own world.
		if (world != SelectionWorld) {
			Select(world, to, add);
			SelectionAnchor = to;
			return;
		}
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
			SelectionAnchor = to;
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

				ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
										   ImGuiTreeNodeFlags_SpanAvailWidth |
										   ImGuiTreeNodeFlags_NoTreePushOnOpen;
				if (!row.HasChildren) {
					flags |= ImGuiTreeNodeFlags_Leaf;
				}
				if (world == SelectionWorld && IsSelected(row.Instance)) {
					flags |= ImGuiTreeNodeFlags_Selected;
				}

				// Ours to assert every frame, because the row list was built
				// from it. imgui still toggles its own copy on a click, and
				// `IsItemToggledOpen` below is how that reaches us.
				const bool renaming = world == RenameInstanceWorld && row.Instance == Renaming;

				ImGui::SetNextItemOpen(row.Open);

				// **The node is still submitted while renaming, with no
				// label.** It keeps the row's height, its arrow and its
				// indentation, so the tree does not jump as the field opens and
				// closes - and it keeps the clipper's rows uniform, which is
				// what lets it skip by arithmetic.
				// The trailing insert control is submitted later on this row. The
				// tree node must release its full-width hit box so that control can
				// own a click in the overlap.
				ImGui::SetNextItemAllowOverlap();
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
				const ImVec2 rowMin = ImGui::GetItemRectMin();
				const float insertButtonSize = ImGui::GetFrameHeight();
				const float insertButtonX =
					ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x - insertButtonSize;
				const bool insertButtonHovered = ImGui::IsMouseHoveringRect(
					ImVec2(insertButtonX, rowMin.y),
					ImVec2(insertButtonX + insertButtonSize, rowMin.y + insertButtonSize)
				);

				if (toggled) {
					const auto found = std::find(tree.Open.begin(), tree.Open.end(), row.Instance);
					if (found != tree.Open.end()) {
						tree.Open.erase(found);
					} else {
						tree.Open.push_back(row.Instance);
					}
				}

				ImGuiStorage *storage = ImGui::GetStateStorage();
				const ImGuiID pendingClick = ImGui::GetID("##pending-selection-click");
				if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !toggled) {
					const ImGuiIO &io = ImGui::GetIO();
					const bool selectedGroup = !io.KeyCtrl && !io.KeyShift && world == SelectionWorld &&
											   Selection.size() > 1 && IsSelected(row.Instance);
					storage->SetBool(pendingClick, selectedGroup);
					if (io.KeyShift && SelectionAnchor != NULL_ENTITY) {
						SelectRange(world, tree.View, SelectionAnchor, row.Instance, io.KeyCtrl);
					} else if (!selectedGroup) {
						Select(world, row.Instance, io.KeyCtrl);
						SelectionAnchor = row.Instance;
					} else {
						SelectionAnchor = row.Instance;
					}
				}
				if (ImGui::IsItemDeactivated() && storage->GetBool(pendingClick, false)) {
					storage->SetBool(pendingClick, false);
					if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && hovered &&
						!ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Left)) {
						Select(world, row.Instance, false);
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
					const DragPayload payload{world.Index, row.Instance, store.Identity()};
					ImGui::SetDragDropPayload(DRAG_TYPE, &payload, sizeof(payload), ImGuiCond_Once);

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
							PendingReparent.StoreIdentity = moved->StoreIdentity;
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
							PendingMove.SourceIdentity = moved->StoreIdentity;
							PendingMove.TargetIdentity = store.Identity();
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
				// Its popup id is scoped to the instance. Without that scope, a row
				// that happened to reuse an ImGui tree id could open the wrong picker
				// and leave the tree node handling the click as an expand/collapse.
				ImGui::PushID(static_cast<int>(row.Instance.Id));
				if (hovered || insertButtonHovered) {
					ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - insertButtonSize);
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
				ImGui::PopID();

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
						RenameInstanceWorld = {};
					} else if (ImGui::IsItemDeactivated()) {
						Renaming = NULL_ENTITY;
						RenameInstanceWorld = {};
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
				auto &metrics = FindRows;
				const auto *font = ImGui::GetFont();
				const uint32_t fontBake = ImGui::GetFontBaked()->BakedId;
				const float fontSize = ImGui::GetFontSize();
				if (metrics.Dirty || metrics.Font != font || metrics.FontBake != fontBake ||
					metrics.FontSize != fontSize) {
					metrics.MaximumTextWidth = 0;
					std::string text;
					for (const auto &result : FindResults) {
						text = result.Name.c_str();
						text += " (";
						text += result.Class.c_str();
						text += ')';
						metrics.MaximumTextWidth =
							std::max(metrics.MaximumTextWidth, ImGui::CalcTextSize(text.c_str()).x);
					}
					metrics.Font = font;
					metrics.FontBake = fontBake;
					metrics.FontSize = fontSize;
					metrics.Dirty = false;
				}
				const auto drawResults = [&](int first, int last) {
					for (int index = first; index < last; index++) {
						const FindResult &result = FindResults[static_cast<size_t>(index)];
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
				};
				if (FindUniformRows) {
					ImGuiListClipper clipper;
					clipper.Begin(static_cast<int>(FindResults.size()));
					while (clipper.Step())
						drawResults(clipper.DisplayStart, clipper.DisplayEnd);
				} else {
					drawResults(0, static_cast<int>(FindResults.size()));
				}
				if (!FindResults.empty()) {
					// Clipped labels still determine the complete horizontal extent.
					// Keep the existing next-row position and vertical scroll range.
					const auto spacing = ImGui::GetStyle().ItemSpacing;
					ImGui::SetCursorPosY(ImGui::GetCursorPosY() - spacing.y);
					ImGui::Dummy({metrics.MaximumTextWidth + spacing.x, 0});
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
				PathBuffer = ConfigPath("worlds").string();
			}

			ImGui::Separator();

			if (ImGui::MenuItem("Export Universe...")) {
				AskingExport = true;
				ExportChoices = ExportOptions{};
				ExportChoices.Product = engine::game::ExportProduct::UniverseFolder;
				PathBuffer = std::string(GameName.IsValid() ? Label(GameName) : "Game") +
							 std::string(engine::game::UNIVERSE_EXTENSION);
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

				// A double-click is an explicit request to look at this world. The
				// main viewport is retained while closed, so reopening it restores its
				// dock and makes this work even after every viewport was closed.
				if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
					!ShowViewport &&
					std::none_of(Extras.begin(), Extras.end(), [](const ViewportState &view) {
						return view.Open;
					})) {
					ShowViewport = true;
					FocusedViewport = 0;
					RetargetEditingViewport(FocusedViewport, world);
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
							PendingMove.SourceIdentity = moved->StoreIdentity;
							PendingMove.TargetIdentity = 0;
							Universe->Enter(world, [&](const Store &store) {
								PendingMove.TargetIdentity = store.Identity();
							});
						} else {
							// Same world, so it is an unparent rather than a
							// move: the instance becomes a root where it is.
							PendingReparent.World = world;
							PendingReparent.StoreIdentity = moved->StoreIdentity;
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
		if (IsReplicaWorld(source) || IsReplicaWorld(target)) {
			Say("instances cannot move between a play client and another world",
				engine::core::LogLevel::Warning);
			return false;
		}

		// **Read first, in its own `Enter`.** Two worlds cannot be entered at
		// once - the affinity check aborts on re-entry - so the subtree becomes
		// a document while the source is open, and the document is what crosses.
		std::string document;
		Name moved;
		bool protectedInstance = false;
		bool changedSource = false;
		bool omittedInstance = false;
		struct PendingSource {
			Entity Instance;
			bool Shader;
			bool JavaScript;
			Name Path;
			std::string Text;
		};
		std::vector<PendingSource> pendingSources;
		std::optional<engine::script::SourceCache> sourceCache;
		std::vector<engine::game::InstanceDocumentIdentity> sourceIdentities;
		struct SavedLanguage {
			uint32_t LocalId;
			std::optional<engine::script::LuaSourceContainer> Lua;
			std::optional<engine::script::JavaScriptSourceContainer> JavaScript;
			std::optional<engine::script::CodeSourceContainerSelector> Selector;
		};
		std::vector<SavedLanguage> languages;
		struct SourceReading {
			Name Path;
			bool Readable = false;
			std::string Text;
		};
		std::vector<SourceReading> originalSources;
		struct AdditionalProperty {
			Entity Instance;
			Name Property;
			engine::ecs::PropertyType Type;
			bool Readable;
			Name NameValue;
			Entity Reference;
			std::string Text;
			bool operator==(const AdditionalProperty &) const = default;
		};
		const auto additionalProperties = [&](const Store &store, const auto &identities) {
			std::vector<AdditionalProperty> result;
			for (const auto &identity : identities) {
				for (const auto &property : store.PropertiesOf(identity.Instance)) {
					using engine::ecs::PropertyType;
					if (!property.Writable) continue;
					const bool omitted = !property.DocumentWritable ||
										 property.Type == PropertyType::Reference ||
										 property.Spelling == "CameraType";
					engine::game::PropertyValue value;
					const bool readable =
						engine::game::ReadProperty(store, identity.Instance, property, value);
					AdditionalProperty captured{
						identity.Instance, property.Name, property.Type, readable, {}, {}, {}
					};
					if (readable && omitted) {
						if (property.Type == PropertyType::Reference)
							captured.Reference = value.Reference;
						else if (property.Type == PropertyType::Name || property.Type == PropertyType::Enum)
							captured.NameValue = value.Name;
						else
							captured.Text = engine::game::FormatValue(value);
					}
					result.push_back(std::move(captured));
				}
			}
			return result;
		};
		std::vector<AdditionalProperty> originalAdditionalProperties;
		Entity sourceParent = NULL_ENTITY;
		Universe->Enter(source, [&](Store &store) {
			if (!store.Alive(instance)) {
				return;
			}
			moved = store.InstanceNameOf(instance);
			sourceParent = store.ParentOf(instance);
			if (store.Protected(instance)) {
				protectedInstance = true;
				return;
			}
			for (const OpenScript &tab : Scripts) {
				if (tab.World != source || !tab.Modified || !store.Alive(tab.Instance)) continue;
				for (Entity at = tab.Instance; at != NULL_ENTITY && store.Alive(at);
					 at = store.ParentOf(at)) {
					if (at == instance) {
						std::string current;
						bool readable = false;
						if (!ReadOpenedSource(
								store, tab.Instance, tab.Shader, tab.JavaScript, tab.Path, current, readable
							) ||
							!readable || !tab.SourceBaselineReadable || current != tab.SourceBaseline) {
							changedSource = true;
							break;
						}
						pendingSources.push_back(
							{tab.Instance, tab.Shader, tab.JavaScript, tab.Path, tab.Text}
						);
						break;
					}
				}
			}
			if (const auto *cache = store.Resource<engine::script::SourceCache>()) sourceCache = *cache;
			document = engine::game::WriteInstanceDocument(store, instance, &sourceIdentities);
			originalAdditionalProperties = additionalProperties(store, sourceIdentities);
			size_t subtreeCount = 1;
			store.EachDescendant(instance, [&](Entity) { ++subtreeCount; });
			omittedInstance = sourceIdentities.size() != subtreeCount;
			for (const auto &identity : sourceIdentities) {
				const auto *lua = store.Get<engine::script::LuaSourceContainer>(identity.Instance);
				const auto *js = store.Get<engine::script::JavaScriptSourceContainer>(identity.Instance);
				const auto *selector =
					store.Get<engine::script::CodeSourceContainerSelector>(identity.Instance);
				if (lua == nullptr && js == nullptr && selector == nullptr) continue;
				SavedLanguage language{
					.LocalId = identity.LocalId, .Lua = {}, .JavaScript = {}, .Selector = {}
				};
				if (lua) language.Lua = *lua;
				if (js) language.JavaScript = *js;
				if (selector) language.Selector = *selector;
				const auto rememberSource = [&](Name path) {
					if (!path.IsValid()) return;
					std::string text;
					std::string error;
					const bool readable = engine::script::ReadSource(store, path, text, error);
					originalSources.push_back({path, readable, std::move(text)});
				};
				if (lua) rememberSource(lua->Path);
				if (js) rememberSource(js->Path);
				languages.push_back(std::move(language));
			}
		});

		if (changedSource) {
			Say("the script source changed while its tab was open, so it was not moved",
				engine::core::LogLevel::Warning);
			return false;
		}
		if (protectedInstance) {
			Say("that instance is protected, so it was not moved", engine::core::LogLevel::Warning);
			return false;
		}
		if (omittedInstance) {
			Say("the subtree contains instances that cannot be moved between worlds",
				engine::core::LogLevel::Warning);
			return false;
		}
		if (document.empty()) {
			Say("that instance could not be written out, so it was not moved", engine::core::LogLevel::Error);
			return false;
		}
		// The document sent to the destination is later rewritten for dirty tabs
		// and path conflicts; retain the source view for post-notification validation.
		const std::string sourceDocument = document;
		std::unordered_set<uint64_t> sourceEntityIds;
		sourceEntityIds.reserve(sourceIdentities.size());
		for (const auto &identity : sourceIdentities)
			sourceEntityIds.insert(identity.Instance.Id);
		std::unordered_multimap<uint64_t, const PendingSource *> pendingByEntity;
		pendingByEntity.reserve(pendingSources.size());
		for (const auto &pending : pendingSources)
			pendingByEntity.emplace(pending.Instance.Id, &pending);
		struct PathContent {
			Name Path;
			std::string Text;
			bool Readable;
		};
		std::vector<PathContent> occupied;
		std::vector<Name> reserved;
		std::optional<engine::script::SourceCache> targetCache;
		Universe->Enter(target, [&](Store &store) {
			if (const auto *cache = store.Resource<engine::script::SourceCache>()) {
				targetCache = *cache;
				for (const auto &row : cache->Rows)
					occupied.push_back({row.Path, row.Text, true});
			}
			const auto reservePath = [&](Name path) {
				if (!path.IsValid() || std::any_of(occupied.begin(), occupied.end(), [&](const auto &row) {
						return row.Path == path;
					}))
					return;
				std::string text;
				std::string error;
				const bool readable = engine::script::ReadSource(store, path, text, error);
				occupied.push_back({path, std::move(text), readable});
			};
			store.Each<const engine::script::LuaSourceContainer>([&](Entity, const auto &container) {
				reservePath(container.Path);
			});
			store.Each<const engine::script::JavaScriptSourceContainer>([&](Entity, const auto &container) {
				reservePath(container.Path);
			});
		});
		for (const OpenScript &tab : Scripts) {
			if (tab.World == target && tab.Modified && !tab.Shader && tab.Path.IsValid())
				occupied.push_back({tab.Path, {}, false});
		}
		for (const auto &row : occupied)
			reserved.push_back(row.Path);

		// Prepare every referenced source together. A destination or another tab
		// may already use the same path for different text.
		Store copy("studio-move-document");
		std::vector<engine::game::InstanceDocumentIdentity> copiedIdentities;
		std::string copyError;
		const Entity copyRoot =
			engine::game::ReadInstanceDocument(copy, document, NULL_ENTITY, copyError, &copiedIdentities);
		if (copyRoot == NULL_ENTITY) {
			Say("could not prepare the scripts for moving", engine::core::LogLevel::Error);
			return false;
		}
		const auto restoreLanguage = [](Store &store, Entity entity, const SavedLanguage &language) {
			if (language.Lua)
				store.Set(entity, *language.Lua);
			else
				store.Remove<engine::script::LuaSourceContainer>(entity);
			if (language.JavaScript)
				store.Set(entity, *language.JavaScript);
			else
				store.Remove<engine::script::JavaScriptSourceContainer>(entity);
			if (language.Selector)
				store.Set(entity, *language.Selector);
			else
				store.Remove<engine::script::CodeSourceContainerSelector>(entity);
		};
		for (const SavedLanguage &language : languages) {
			const auto found =
				std::find_if(copiedIdentities.begin(), copiedIdentities.end(), [&](const auto &row) {
					return row.LocalId == language.LocalId;
				});
			if (found != copiedIdentities.end()) restoreLanguage(copy, found->Instance, language);
		}

		// The instance document carries only active source rows. The private copy
		// also needs inactive cached programs before it can remap either path.
		if (sourceCache) copy.SetResource(*sourceCache);
		struct CopiedSource {
			Entity Instance;
			Name Path;
			std::string Text;
			bool Lua;
			bool Readable;
		};
		std::vector<CopiedSource> sources;
		for (const auto &identity : copiedIdentities) {
			const auto add = [&](Name path, bool lua) {
				if (!path.IsValid()) return;
				std::string text;
				std::string error;
				const bool readable = engine::script::ReadSource(copy, path, text, error);
				sources.push_back({identity.Instance, path, std::move(text), lua, readable});
				reserved.push_back(path);
			};
			if (const auto *lua = copy.Get<engine::script::LuaSourceContainer>(identity.Instance))
				add(lua->Path, true);
			if (const auto *js = copy.Get<engine::script::JavaScriptSourceContainer>(identity.Instance))
				add(js->Path, false);
		}
		for (const PendingSource &pending : pendingSources) {
			const auto original =
				std::find_if(sourceIdentities.begin(), sourceIdentities.end(), [&](const auto &row) {
					return row.Instance == pending.Instance;
				});
			const auto restored =
				original == sourceIdentities.end()
					? copiedIdentities.end()
					: std::find_if(copiedIdentities.begin(), copiedIdentities.end(), [&](const auto &row) {
						  return row.LocalId == original->LocalId;
					  });
			if (restored == copiedIdentities.end()) {
				Say("could not prepare the unsaved scripts for moving", engine::core::LogLevel::Error);
				return false;
			}
			if (pending.Shader) {
				Name unused;
				if (!WriteSourceDocument(
						copy, restored->Instance, SourceDocumentKind::Shader, unused, pending.Text
					))
					return false;
				continue;
			}
			const bool lua = !pending.JavaScript;
			auto found = std::find_if(sources.begin(), sources.end(), [&](const auto &row) {
				return row.Instance == restored->Instance && row.Lua == lua;
			});
			if ((found == sources.end() && pending.Path.IsValid()) ||
				(found != sources.end() && found->Path != pending.Path)) {
				Say("the script source changed while its tab was open, so it was not moved",
					engine::core::LogLevel::Warning);
				return false;
			}
			if (found == sources.end()) {
				sources.push_back({restored->Instance, Name{}, pending.Text, lua, true});
			} else {
				found->Text = pending.Text;
				found->Readable = true;
			}
		}
		for (CopiedSource &sourceFile : sources) {
			const auto conflict = std::find_if(occupied.begin(), occupied.end(), [&](const auto &row) {
				return row.Path == sourceFile.Path &&
					   (!row.Readable || !sourceFile.Readable || row.Text != sourceFile.Text);
			});
			if (!sourceFile.Path.IsValid() || conflict != occupied.end()) {
				if (!sourceFile.Readable) {
					Say("a conflicting script source could not be read", engine::core::LogLevel::Error);
					return false;
				}
				const std::filesystem::path original(
					sourceFile.Path.IsValid() ? std::string(sourceFile.Path.Text())
											  : (sourceFile.Lua ? "Scripts/Script.luau" : "Scripts/Script.js")
				);
				for (size_t suffix = 1;; ++suffix) {
					const auto candidate =
						original.parent_path() / (original.stem().string() + "_" + std::to_string(suffix) +
												  original.extension().string());
					const Name name(candidate.generic_string());
					std::error_code error;
					const bool exists =
						std::filesystem::exists(engine::core::Paths::Assets() / candidate, error);
					if (error) return false;
					if (!exists && std::find(reserved.begin(), reserved.end(), name) == reserved.end()) {
						sourceFile.Path = name;
						reserved.push_back(name);
						break;
					}
				}
			}
			if (sourceFile.Lua) {
				auto component = copy.Get<engine::script::LuaSourceContainer>(sourceFile.Instance) == nullptr
									 ? engine::script::LuaSourceContainer{}
									 : *copy.Get<engine::script::LuaSourceContainer>(sourceFile.Instance);
				component.Path = sourceFile.Path;
				copy.Set(sourceFile.Instance, component);
			} else {
				auto component =
					copy.Get<engine::script::JavaScriptSourceContainer>(sourceFile.Instance) == nullptr
						? engine::script::JavaScriptSourceContainer{}
						: *copy.Get<engine::script::JavaScriptSourceContainer>(sourceFile.Instance);
				component.Path = sourceFile.Path;
				copy.Set(sourceFile.Instance, component);
			}
			if (sourceFile.Readable) {
				if (copy.Resource<engine::script::SourceCache>() == nullptr)
					copy.SetResource(engine::script::SourceCache{});
				copy.ResourceMutable<engine::script::SourceCache>()->Set(sourceFile.Path, sourceFile.Text);
			}
			occupied.push_back({sourceFile.Path, sourceFile.Text, sourceFile.Readable});
		}
		std::vector<engine::game::InstanceDocumentIdentity> finalIdentities;
		document = engine::game::WriteInstanceDocument(copy, copyRoot, &finalIdentities);
		if (document.empty()) return false;

		std::string error;
		engine::ecs::Entity rebuilt = NULL_ENTITY;
		Universe->Enter(target, [&](Store &store) {
			// A parent that died between the drop and here leaves the subtree
			// at the world's root, which is where a drop onto a world row puts
			// it anyway.
			const engine::ecs::Entity into =
				parent != NULL_ENTITY && store.Alive(parent) ? parent : NULL_ENTITY;
			std::vector<Entity> before;
			const auto collect = [&](const auto &body) {
				if (into == NULL_ENTITY)
					store.EachRoot(body);
				else
					store.EachChild(into, body);
			};
			collect([&](Entity child) { before.push_back(child); });
			std::vector<engine::game::InstanceDocumentIdentity> targetIdentities;
			rebuilt = engine::game::ReadInstanceDocument(store, document, into, error, &targetIdentities);
			if (rebuilt != NULL_ENTITY) {
				for (const auto &identity : finalIdentities) {
					const auto language =
						std::find_if(languages.begin(), languages.end(), [&](const auto &row) {
							return row.LocalId == identity.LocalId;
						});
					if (language == languages.end()) continue;
					const auto found =
						std::find_if(targetIdentities.begin(), targetIdentities.end(), [&](const auto &row) {
							return row.LocalId == identity.LocalId;
						});
					if (found == targetIdentities.end()) continue;
					SavedLanguage prepared{
						.LocalId = identity.LocalId, .Lua = {}, .JavaScript = {}, .Selector = {}
					};
					if (const auto *lua = copy.Get<engine::script::LuaSourceContainer>(identity.Instance))
						prepared.Lua = *lua;
					if (const auto *js =
							copy.Get<engine::script::JavaScriptSourceContainer>(identity.Instance))
						prepared.JavaScript = *js;
					if (const auto *selector =
							copy.Get<engine::script::CodeSourceContainerSelector>(identity.Instance))
						prepared.Selector = *selector;
					restoreLanguage(store, found->Instance, prepared);
				}
				for (const CopiedSource &sourceFile : sources) {
					if (!sourceFile.Readable) continue;
					if (store.Resource<engine::script::SourceCache>() == nullptr)
						store.SetResource(engine::script::SourceCache{});
					store.ResourceMutable<engine::script::SourceCache>()->Set(
						sourceFile.Path, sourceFile.Text
					);
				}
			}
			if (rebuilt == NULL_ENTITY) {
				std::vector<Entity> added;
				collect([&](Entity child) {
					if (std::find(before.begin(), before.end(), child) == before.end())
						added.push_back(child);
				});
				for (Entity child : added)
					store.DestroyInstance(child);
				if (targetCache)
					store.SetResource(*targetCache);
				else
					store.RemoveResource<engine::script::SourceCache>();
			}
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

		bool removed = false;
		bool validationAccepted = false;
		std::exception_ptr removalFailure;
		try {
			Universe->Enter(source, [&](Store &store) {
				if (!store.Alive(instance)) return;
				auto validateSource = [&] {
					if (!store.Alive(instance) || store.ParentOf(instance) != sourceParent) return false;
					size_t currentCount = 1;
					store.EachDescendant(instance, [&](Entity) { ++currentCount; });
					if (currentCount != sourceIdentities.size()) return false;
					std::vector<engine::game::InstanceDocumentIdentity> currentIdentities;
					if (engine::game::WriteInstanceDocument(store, instance, &currentIdentities) !=
							sourceDocument ||
						currentIdentities.size() != sourceIdentities.size())
						return false;
					if (additionalProperties(store, currentIdentities) != originalAdditionalProperties)
						return false;
					size_t languageIndex = 0;
					for (size_t index = 0; index < currentIdentities.size(); ++index) {
						const auto &current = currentIdentities[index];
						const auto &original = sourceIdentities[index];
						if (current.LocalId != original.LocalId || current.Instance != original.Instance)
							return false;
						const SavedLanguage *saved =
							languageIndex < languages.size() &&
									languages[languageIndex].LocalId == current.LocalId
								? &languages[languageIndex++]
								: nullptr;
						const auto *lua = store.Get<engine::script::LuaSourceContainer>(current.Instance);
						const auto *js =
							store.Get<engine::script::JavaScriptSourceContainer>(current.Instance);
						const auto *selector =
							store.Get<engine::script::CodeSourceContainerSelector>(current.Instance);
						if (saved == nullptr) {
							if (lua || js || selector) return false;
							continue;
						}
						if (static_cast<bool>(saved->Lua) != (lua != nullptr) ||
							static_cast<bool>(saved->JavaScript) != (js != nullptr) ||
							static_cast<bool>(saved->Selector) != (selector != nullptr) ||
							(lua && lua->Path != saved->Lua->Path) ||
							(js && js->Path != saved->JavaScript->Path) ||
							(selector && selector->Active != saved->Selector->Active))
							return false;
					}
					for (const auto &original : originalSources) {
						std::string text;
						std::string error;
						const bool readable = engine::script::ReadSource(store, original.Path, text, error);
						if (readable != original.Readable || (readable && text != original.Text))
							return false;
					}
					size_t pendingCount = 0;
					for (const OpenScript &tab : Scripts) {
						if (tab.World != source || !tab.Modified) continue;
						if (!sourceEntityIds.contains(tab.Instance.Id)) continue;
						++pendingCount;
						const auto range = pendingByEntity.equal_range(tab.Instance.Id);
						const auto found = std::find_if(range.first, range.second, [&](const auto &entry) {
							const auto &row = *entry.second;
							return row.Shader == tab.Shader && row.JavaScript == tab.JavaScript &&
								   row.Path == tab.Path && row.Text == tab.Text;
						});
						if (found == range.second) return false;
						std::string current;
						bool readable = false;
						if (!ReadOpenedSource(
								store, tab.Instance, tab.Shader, tab.JavaScript, tab.Path, current, readable
							) ||
							!readable || !tab.SourceBaselineReadable || current != tab.SourceBaseline)
							return false;
					}
					return pendingCount == pendingSources.size();
				};
				auto prepare = [&] {
					validationAccepted = validateSource();
					return validationAccepted;
				};
				removed = store.DestroyAuthoredPrepared(
					instance,
					[](void *context) { return (*static_cast<decltype(prepare) *>(context))(); },
					&prepare
				);
			});
		} catch (...) {
			// If destruction began, keeping the destination protects against a
			// partially destroyed source when an allocation or component hook throws.
			removalFailure = std::current_exception();
		}
		if (!removed) {
			bool sourceAlive = false;
			Universe->Enter(source, [&](const Store &store) { sourceAlive = store.Alive(instance); });
			if (!removalFailure || (!validationAccepted && sourceAlive))
				Universe->Enter(target, [&](Store &store) {
					if (store.Alive(rebuilt)) store.DestroyInstance(rebuilt);
					if (targetCache)
						store.SetResource(*targetCache);
					else
						store.RemoveResource<engine::script::SourceCache>();
				});
			if (removalFailure) std::rethrow_exception(removalFailure);
			Say("the source instance could not be removed, so it was not moved",
				engine::core::LogLevel::Error);
			return false;
		}
		for (size_t index = Scripts.size(); index > 0; --index) {
			const OpenScript &tab = Scripts[index - 1];
			if (tab.World != source) continue;
			if (std::any_of(sourceIdentities.begin(), sourceIdentities.end(), [&](const auto &identity) {
					return identity.Instance == tab.Instance;
				}))
				CloseScriptTab(index - 1);
		}

		InstanceCounts.clear();

		// The selection follows the instance, which is what makes the move feel
		// like a move rather than like a delete and a paste somewhere else.
		ClearRootSelection();
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

	void Editor::CancelPendingWorldActions() {
		PendingInsert = {};
		PendingReparent = {};
		PendingMove = {};
		PendingOpenScript = {};
		PendingDuplicate = {};
		PendingDelete = {};
		PendingRemoveWorld = {};
		PendingActivate = {};
		PendingRuns.clear();
		ReadyRuns.clear();
		PendingDuplicateWorld = {};
		PendingTeleport = {};
		PendingWorldState = {};
		PendingLookThrough = NULL_ENTITY;
		PendingLookThroughPrevious = NULL_ENTITY;
		PendingLookThroughWorld = {};
		PendingLookThroughTitle.clear();
		PendingLookThroughSet = false;
		PendingZoomWorld = {};
		PendingZoomTo = NULL_ENTITY;
		PendingZoomSelectionWorld = {};
		PendingZoomSelection.clear();
		PendingFrame.clear();
		PendingPick = {};
		PendingCursor = {};
		BoxSelection = {};
	}

	void Editor::ApplyPendingActions() {
		ApplyImageComposerPending();
		ApplyProceduralPending();
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

		// A viewport can close, switch worlds, or select another object while its
		// drag is held. Preview writes already reached the world in those cases,
		// so restore the captured value instead of leaving a change with no undo.
		bool guiDragStoreMatches = false;
		if (GuiDragging.Instance != NULL_ENTITY && Universe != nullptr && GuiDragging.World.IsValid()) {
			Universe->Enter(GuiDragging.World, [&](const Store &store) {
				guiDragStoreMatches = store.Identity() == GuiDragging.StoreIdentity;
			});
		}
		if (GuiDragging.Instance != NULL_ENTITY &&
			(!guiDragStoreMatches || GuiDragging.Viewport >= Overlays.size() ||
			 !Overlays[GuiDragging.Viewport].Drawn ||
			 ViewportWorld(GuiDragging.Viewport) != GuiDragging.World ||
			 SelectionWorld != GuiDragging.World || !IsSelected(GuiDragging.Instance))) {
			PendingGuiEdit = {
				.World = GuiDragging.World,
				.StoreIdentity = GuiDragging.StoreIdentity,
				.Instance = GuiDragging.Instance,
				.Position = GuiDragging.BeforePosition,
				.Size = GuiDragging.BeforeSize,
				.BeforePosition = GuiDragging.BeforePosition,
				.BeforeSize = GuiDragging.BeforeSize,
				.Resize = GuiDragging.Resize,
				.Wanted = true,
			};
			GuiDragging = GuiCanvasDrag{};
		}

		if (PendingGuiEdit.Wanted) {
			const PendingGuiCanvasEdit edit = PendingGuiEdit;
			PendingGuiEdit = PendingGuiCanvasEdit{};
			const bool authoritative = AuthorityOf(edit.World) == EditAuthority::Authoritative;
			bool wrote = false;
			if (authoritative) {
				Universe->Enter(edit.World, [&](Store &store) {
					if (store.Identity() != edit.StoreIdentity || !store.Alive(edit.Instance)) {
						return;
					}
					const engine::core::Name property(edit.Resize ? "Size" : "Position");
					const engine::core::UDim2 &value = edit.Resize ? edit.Size : edit.Position;
					wrote = store.SetPropertyAuthored(edit.Instance, property, &value, sizeof(value));
				});
			}

			if (wrote && edit.Commit) {
				engine::game::PropertyValue before;
				before.Type = engine::ecs::PropertyType::UDim2;
				before.UDim2 = edit.Resize ? edit.BeforeSize : edit.BeforePosition;
				engine::game::PropertyValue after;
				after.Type = engine::ecs::PropertyType::UDim2;
				after.UDim2 = edit.Resize ? edit.Size : edit.Position;
				if (!engine::game::ValuesEqual(before, after)) {
					if (Commands != nullptr) {
						const std::optional<std::string> group = Commands->TryBeginRecording(
							edit.Resize ? "Resize GUI" : "Move GUI", edit.Resize ? "Resize GUI" : "Move GUI"
						);
						Commands->RecordProperty(
							edit.World,
							edit.Instance,
							engine::core::Name(edit.Resize ? "Size" : "Position"),
							before,
							after,
							edit.Resize ? "Resize GUI" : "Move GUI"
						);
						if (group) {
							Commands->FinishRecording(*group, FinishOperation::Commit);
						}
					}
					MarkModified();
				}
			}
		}

		if (PendingReparent.World.IsValid() && PendingReparent.StoreIdentity != 0) {
			bool matches = false;
			Universe->Enter(PendingReparent.World, [&](const Store &store) {
				matches = store.Identity() == PendingReparent.StoreIdentity;
			});
			if (!matches) PendingReparent = {};
		}
		if (PendingReparent.World.IsValid()) {
			const WorldId world = PendingReparent.World;
			const bool authoritative = AuthorityOf(world) == EditAuthority::Authoritative;
			const engine::ecs::Entity parent = PendingReparent.Parent;
			const std::vector<engine::ecs::Entity> moving = PendingReparent.Instances;
			PendingReparent = PendingReparentAction{};

			// Keep completed moves in the batch even if a later move throws.
			std::vector<engine::ecs::Entity> applied;
			size_t refused = 0;
			std::optional<CommandLog::Batch> batch;
			if (Commands != nullptr) batch.emplace(*Commands, authoritative);

			// **The nested members dropped before the world is entered**, by
			// `TopMost` - a free function over the compiled tree, tested as
			// one. Dragging a model and one of its own parts together means
			// "move the model", and moving both would take the part out of the
			// model on the way.
			std::vector<engine::ecs::Entity> topMost;
			TopMost(TreeFor(world).View, moving, topMost);
			applied.reserve(topMost.size());

			Universe->Enter(world, [&](Store &store) {
				for (const engine::ecs::Entity instance : topMost) {
					if (!store.Alive(instance)) {
						continue;
					}
					if (parent != NULL_ENTITY && !store.Alive(parent)) {
						return;
					}

					struct HistoryPreparation {
						CommandLog *Log;
						WorldId World;
						engine::ecs::Entity Instance;
						std::string Description;
						CommandLog::PreparedReparent Prepared;
					};
					HistoryPreparation history{
						authoritative ? Commands.get() : nullptr, world, instance, {}, {}
					};
					if (history.Log != nullptr)
						history.Description = "Move " + std::string(Label(store.InstanceNameOf(instance)));
					const auto prepare = [](void *context, Entity from, Entity to) {
						auto &pending = *static_cast<HistoryPreparation *>(context);
						pending.Prepared = pending.Log->PrepareReparent(
							pending.World, pending.Instance, from, to, std::move(pending.Description)
						);
					};
					const auto result = store.SetParentAuthoredPrepared(
						instance, parent, history.Log != nullptr ? +prepare : nullptr, &history
					);
					if (result == engine::ecs::ReparentResult::Changed) {
						if (history.Log != nullptr) history.Log->CommitReparent(std::move(history.Prepared));
						if (authoritative) MarkModified();
						applied.push_back(instance);
					} else if (result == engine::ecs::ReparentResult::Refused) {
						++refused;
					}
				}
			});

			if (batch) batch->Finish();

			if (!applied.empty()) {
				OpenPathTo(world, applied.front());
			}

			if (refused > 0) {
				Say("that would put an instance inside itself", engine::core::LogLevel::Warning);
			}
		}

		if (PendingRenameStart != NULL_ENTITY) {
			const engine::ecs::Entity starting = PendingRenameStart;
			const WorldId world = PendingRenameStartWorld;
			PendingRenameStart = NULL_ENTITY;
			PendingRenameStartWorld = {};
			BeginRename(world, starting);
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

		if (PendingMove.Source.IsValid() && PendingMove.SourceIdentity != 0) {
			bool sourceMatches = false;
			bool targetMatches = false;
			Universe->Enter(PendingMove.Source, [&](const Store &store) {
				sourceMatches = store.Identity() == PendingMove.SourceIdentity;
			});
			Universe->Enter(PendingMove.Target, [&](const Store &store) {
				targetMatches = store.Identity() == PendingMove.TargetIdentity;
			});
			if (!sourceMatches || !targetMatches) PendingMove = {};
		}
		if (PendingMove.Source.IsValid()) {
			const PendingMoveAction move = PendingMove;
			PendingMove = PendingMoveAction{};
			MoveInstanceToWorld(move.Source, move.Instance, move.Target, move.Parent);
		}

		if (PendingLookThroughSet) {
			PendingLookThroughSet = false;
			const size_t index = PendingLookThroughViewport;
			const WorldId world = PendingLookThroughWorld;
			const Entity requested = PendingLookThrough;
			const Entity previous = PendingLookThroughPrevious;
			const std::string title = std::move(PendingLookThroughTitle);
			PendingLookThrough = NULL_ENTITY;
			PendingLookThroughPrevious = NULL_ENTITY;
			PendingLookThroughWorld = {};
			ViewportState *viewport = ExtraAt(index);
			const bool open = index == 0 ? ShowViewport : viewport != nullptr && viewport->Open;
			if (open && Universe != nullptr && world.IsValid() && ViewportWorld(index) == world &&
				title == ViewportTitle(index)) {
				Entity &follow = viewport != nullptr ? viewport->Follow : FollowCamera;
				bool valid = requested == NULL_ENTITY;
				if (!valid) {
					Universe->Enter(world, [&](Store &store) {
						const ClassId cameraClass = Classes::Find(Name("Camera"));
						valid = store.Alive(requested) && cameraClass.IsValid() &&
								store.IsA(requested, cameraClass);
					});
				}
				if (valid && follow == previous) {
					follow = requested;

					Say(follow == NULL_ENTITY ? "back to the editor camera"
											  : "looking through the scene's camera - right-drag to fly");
				}
			}
		}

		if (PendingZoomWorld.IsValid()) {
			const WorldId world = PendingZoomWorld;
			const Entity instance = PendingZoomTo;
			const bool selectionUnchanged =
				PendingZoomSelectionWorld == SelectionWorld && PendingZoomSelection == Selection;
			PendingZoomWorld = WorldId{};
			PendingZoomTo = NULL_ENTITY;
			PendingZoomSelectionWorld = WorldId{};
			PendingZoomSelection.clear();
			if (selectionUnchanged) {
				ZoomViewportTo(world, instance);
			}
		}

		if (PendingOpenScript.World.IsValid()) {
			OpenScriptTab(PendingOpenScript.World, PendingOpenScript.Instance);
			ShowScripts = true;
			PendingOpenScript = PendingScriptAction{};
		}

		if (PendingDuplicate.World.IsValid()) {
			auto action = std::move(PendingDuplicate);
			PendingDuplicate = {};
			DuplicateInstances(action.World, action.Instances);
		}

		if (PendingDelete.World.IsValid()) {
			auto action = std::move(PendingDelete);
			PendingDelete = {};
			DeleteInstances(action.World, action.Instances);
		}

		if (PendingRemoveWorld.IsValid()) {
			const WorldId world = PendingRemoveWorld;
			PendingRemoveWorld = WorldId{};
			RemoveWorld(world);
		}

		ApplyPendingWorldActions();
	}
}
