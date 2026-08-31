// Four of the ribbon's five tabs - the manipulators, the steps and the two
// flags. The fifth is `Editor::DrawPluginTools`, which lives beside the rest of
// the plugin host in `Plugins.cpp` rather than here: what it draws is whatever
// the installed plugins asked for, and none of it is this editor's own.
//
// **Everything here was already reachable and none of it was reachable while
// working.** The tool modes were keyboard-only, the snap steps were on the
// Preferences page, and `Anchored` was a checkbox in the properties grid - so
// the ordinary loop of "select, move, anchor, move the next one" crossed three
// panels. Roblox puts the same set in one ribbon for that reason, and the
// screenshots `ROADMAP.md` points at are that ribbon.
//
// **On the toolbar rather than in a panel of its own, since v0.13.** It was a
// docked window, on the argument that this editor's furniture is docked windows
// and one bespoke always-on strip would be the only thing in the program that
// could not be moved or closed. That argument was wrong about which cost was
// larger: a floating Tools window sat over the viewport, had to be dragged out
// of the way of the thing it was editing, and duplicated the manipulators the
// toolbar already carried - so the same three buttons existed twice and could
// disagree. There is now one of each, on the strip that was already pinned.
// See `Editor::DrawToolbar`.
//
// ## What each control writes
//
// | Control | What it changes |
// |---|---|
// | Select / Move / Rotate / Scale | `CurrentTool` |
// | Snap, and the two steps | `SnapEnabled`, `SnapDistance`, `SnapDegrees` |
// | Anchor | the `Anchored` property on everything selected |
// | Lock | the `Locked` property on everything selected |
// | Edit Pivot | `PivotEditing`, which points the same handles at `PivotOffset` |
// | Reset Pivot | writes the identity to `PivotOffset` |
//
// Every one of the writes is one command, so it undoes in one press - which is
// the thing a per-instance loop gets wrong by default.

#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/game/Values.hpp>
#include <engine/scene/Part.hpp>
#include <engine/ui/Metrics.hpp>
#include <engine/ui/Theme.hpp>

#include <imgui.h>
#include <studio/Editor.hpp>
#include <studio/Widgets.hpp>

namespace studio {

	using engine::core::Name;
	using engine::ecs::Entity;
	using engine::ecs::Store;
	using studio::Availability;

	namespace {
		// The gap between two groups on the strip.
		//
		// **A dim pipe rather than `ImGui::SeparatorEx`.** A vertical separator
		// sizes itself to the tallest item submitted so far, which on a row
		// holding buttons, checkboxes and drag fields is a line that changes
		// height as the contents change - and the strip already used this
		// spelling before the ribbon existed.
		void Divider() {
			ImGui::SameLine();
			ImGui::TextDisabled("|");
			ImGui::SameLine();
		}
	}

	void Editor::OperatorButton(Action id, const char *label) {
		// **The operator table decides whether it may run and says why it may
		// not.** A button that reached past it would be a second answer to that
		// question, and the two would disagree the first time one of them
		// learned about a running world - which is exactly the duplicate
		// `studio/Operators.hpp` exists to prevent.
		const Availability can = Operators.Available(id);

		ImGui::BeginDisabled(!can.Ready);
		if (ImGui::Button(label, ImVec2(84.0f, 0.0f))) {
			(void)Operators.Run(id);
		}
		ImGui::EndDisabled();

		// **The reason on the disabled button, which is the one place somebody
		// looks.** A greyed control with no explanation is the failure
		// `Availability` carries a string to avoid.
		if (!can.Ready && !can.Reason.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
			ImGui::SetTooltip("%s", can.Reason.c_str());
		}
	}

	bool Editor::SelectionFlag(const char *property) const {
		if (Selection.empty() || Universe == nullptr) {
			return false;
		}

		const engine::core::Name key(property);
		bool every = true;

		// **A mixed selection answers `false`**, which is what decides what one
		// press does next: the first turns everything on rather than half of it
		// off. An instance with no such property counts as off for the same
		// reason - a `Folder` in the selection must not make the button claim
		// everything is anchored.
		Universe->Enter(SelectionWorld, [&](Store &store) {
			for (const Entity instance : Selection) {
				bool value = false;
				if (!store.GetProperty(instance, key, &value, sizeof(value)) || !value) {
					every = false;
					return;
				}
			}
		});

		return every;
	}

	void Editor::SetSelectionFlag(const char *property, bool value, const char *label) {
		if (Selection.empty() || Universe == nullptr) {
			return;
		}

		const engine::core::Name key(property);
		size_t written = 0;
		const bool authoritative = AuthorityOf(SelectionWorld) == EditAuthority::Authoritative;

		Universe->Enter(SelectionWorld, [&](Store &store) {
			for (const Entity instance : Selection) {
				bool was = false;
				if (!store.GetProperty(instance, key, &was, sizeof(was))) {
					// Not every selected instance has the property - a `Folder`
					// has neither of these. Skipped rather than refused, because
					// a mixed selection is the ordinary way somebody works.
					continue;
				}
				if (was == value) {
					continue;
				}

				if (!store.SetPropertyAuthored(instance, key, &value, sizeof(value))) {
					continue;
				}

				if (authoritative && Commands != nullptr) {
					engine::game::PropertyValue before;
					before.Type = engine::ecs::PropertyType::Bool;
					before.Bool = was;

					engine::game::PropertyValue after;
					after.Type = engine::ecs::PropertyType::Bool;
					after.Bool = value;

					Commands->RecordProperty(SelectionWorld, instance, key, before, after, label);
				}
				written++;
			}
		});

		if (written > 0) {
			if (authoritative) {
				MarkModified();
			}
			Say(std::string(label) + ": " + std::to_string(written) + " instance(s)");
		}
	}

	void Editor::ResetSelectionPivot() {
		if (Selection.empty() || Universe == nullptr) {
			return;
		}

		const engine::core::Name key("PivotOffset");
		const engine::core::CFrame identity;
		size_t written = 0;
		const bool authoritative = AuthorityOf(SelectionWorld) == EditAuthority::Authoritative;

		Universe->Enter(SelectionWorld, [&](Store &store) {
			for (const Entity instance : Selection) {
				engine::core::CFrame was;
				if (!store.GetProperty(instance, key, &was, sizeof(was))) {
					continue;
				}

				// **Nothing to record for a pivot already at the centre**, which
				// is most of a scene: an undo entry that restores what was
				// already there is a press somebody has to spend to get past.
				if (was.Position == identity.Position && was.Rotation() == identity.Rotation()) {
					continue;
				}

				if (!store.SetPropertyAuthored(instance, key, &identity, sizeof(identity))) {
					continue;
				}

				if (authoritative && Commands != nullptr) {
					engine::game::PropertyValue before;
					before.Type = engine::ecs::PropertyType::CFrame;
					before.CFrame = was;

					engine::game::PropertyValue after;
					after.Type = engine::ecs::PropertyType::CFrame;
					after.CFrame = identity;

					Commands->RecordProperty(SelectionWorld, instance, key, before, after, "Reset Pivot");
				}
				written++;
			}
		});

		if (written > 0) {
			if (authoritative) {
				MarkModified();
			}
			Say("reset the pivot of " + std::to_string(written) + " instance(s)");
		}
	}

	void Editor::DrawHomeTools() {
		// **One strip, and the dividers are what make it scannable.** A ribbon
		// row is read left to right in groups - what to insert, which handle,
		// how far it steps, what the selection is - and a run of twelve
		// evenly-spaced buttons is a row nobody can find anything in.

		const bool all = DrawingBuiltinTool == BuiltinStudioTool::None;

		if (all || DrawingBuiltinTool == BuiltinStudioTool::InsertObject) {
			ImGui::BeginDisabled(!Active.IsValid());
			if (ImGui::Button("Insert Object")) {
				ImGui::OpenPopup("insert-object");
			}
			ImGui::EndDisabled();

			if (ImGui::BeginPopup("insert-object")) {
				if (const engine::ecs::ClassId chosen = DrawClassPicker("insert-toolbar"); chosen.IsValid()) {
					InsertInstance(
						Active, chosen, Selection.empty() ? engine::ecs::NULL_ENTITY : Selection.front()
					);
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
			}
		}

		if (all) {
			Divider();
		}

		// --- the manipulators ------------------------------------------------
		//
		// One mode at a time, which the `ToolMode` declaration argues for: three
		// sets of handles over one object is a target nobody can hit.
		//
		// **Select is a button of its own as well as what the others toggle back
		// to.** Clicking the held mode puts the handles away without reaching
		// across the strip, and somebody who wants no handles and has not
		// learned that has an obvious thing to press.
		if (all || DrawingBuiltinTool == BuiltinStudioTool::SelectMode) {
			if (RunButton("Select", CurrentTool == ToolMode::Select, engine::ui::AccentColour())) {
				CurrentTool = ToolMode::Select;
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("click to select - no handles");
			}
			if (all) {
				ImGui::SameLine();
			}
		}

		const auto tool = [this](ToolMode mode, const char *label, const char *tip) {
			if (RunButton(label, CurrentTool == mode, engine::ui::AccentColour())) {
				CurrentTool = CurrentTool == mode ? ToolMode::Select : mode;
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", tip);
			}
		};

		if (all || DrawingBuiltinTool == BuiltinStudioTool::MoveMode) {
			tool(ToolMode::Move, "Move", "drag an axis to move the selection along it");
			if (all) {
				ImGui::SameLine();
			}
		}
		if (all || DrawingBuiltinTool == BuiltinStudioTool::RotateMode) {
			tool(ToolMode::Rotate, "Rotate", "drag a ring to turn the selection about its axis");
			if (all) {
				ImGui::SameLine();
			}
		}
		if (all || DrawingBuiltinTool == BuiltinStudioTool::ScaleMode) {
			tool(ToolMode::Scale, "Scale", "drag an axis to grow the selection along it");
		}

		if (all) {
			Divider();
		}

		// --- the steps -------------------------------------------------------

		if (all || DrawingBuiltinTool == BuiltinStudioTool::SnapToggle) {
			ImGui::Checkbox("Snap", &SnapEnabled);
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(
					"Off by default. Snapping is a constraint you turn on for a job,\n"
					"and an editor that quietly rounded every drag could not place\n"
					"anything where it was asked to."
				);
			}

			// **Disabled rather than hidden when snapping is off.** A field that
			// vanishes takes its value with it as far as a reader is concerned, and
			// the number is exactly what somebody wants to check before switching
			// the checkbox back on.
		}

		if (all || DrawingBuiltinTool == BuiltinStudioTool::SnapDistance) {
			if (all) {
				ImGui::SameLine();
			}
			ImGui::BeginDisabled(!SnapEnabled);
			ImGui::SetNextItemWidth(engine::ui::Scaled(70.0f));
			ImGui::DragFloat(
				"##snap-distance", &SnapDistance, 0.1f, 0.05f, 100.0f, "%.2f m", ImGuiSliderFlags_AlwaysClamp
			);
			ImGui::EndDisabled();
		}

		if (all || DrawingBuiltinTool == BuiltinStudioTool::SnapDegrees) {
			if (all) {
				ImGui::SameLine();
			}
			ImGui::BeginDisabled(!SnapEnabled);
			ImGui::SetNextItemWidth(engine::ui::Scaled(70.0f));
			ImGui::DragFloat(
				"##snap-degrees", &SnapDegrees, 1.0f, 1.0f, 90.0f, "%.0f deg", ImGuiSliderFlags_AlwaysClamp
			);
			ImGui::EndDisabled();
		}

		if (all) {
			Divider();
		}

		// --- which faces a resize moves --------------------------------------
		//
		// **Beside the steps rather than in Preferences**, because it is the
		// same kind of decision: how far a drag goes and which end of the part
		// it goes from are one thought, and separating them puts half of it two
		// clicks away.
		//
		// Not disabled when the scale tool is not selected. A person sets this
		// before reaching for the tool as often as after, and a control that
		// greys itself out until you are already doing the thing it configures
		// is a control you find by accident.
		if (all || DrawingBuiltinTool == BuiltinStudioTool::ScaleFaces) {
			ImGui::TextDisabled("Faces");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(engine::ui::Scaled(110.0f));
			if (ImGui::BeginCombo("##scale-sides", Describe(ScaleSides))) {
				for (size_t index = 0; index < SCALE_SIDE_COUNT; index++) {
					const auto side = static_cast<ScaleSide>(index);
					if (ImGui::Selectable(Describe(side), side == ScaleSides)) {
						ScaleSides = side;
					}
				}
				ImGui::EndCombo();
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(
					"Which faces a scale drag moves.\\n\\n"
					"Side       the face you grabbed, and the opposite one stays put\\n"
					"Both       both faces move by the step, so the part grows by twice it\\n"
					"Both Half  both faces move by half the step, so it grows by the step"
				);
			}
		}

		if (all) {
			Divider();
		}

		// --- what is selected ------------------------------------------------

		if (all || DrawingBuiltinTool == BuiltinStudioTool::Anchor) {
			ImGui::BeginDisabled(Selection.empty());

			// **A press turns the whole selection on when any of it is off**, which
			// is what one button means to a person. `SelectionFlag` answers `false`
			// for a mixed selection for exactly this reason.
			const bool anchored = SelectionFlag("Anchored");
			if (ImGui::Button(anchored ? "Unanchor" : "Anchor", ImVec2(engine::ui::Scaled(84.0f), 0.0f))) {
				SetSelectionFlag("Anchored", !anchored, anchored ? "Unanchor" : "Anchor");
			}
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
				ImGui::SetTooltip(
					"Whether physics moves it. An anchored part carries no rigid body\n"
					"at all, so this moves the row to another archetype."
				);
			}

			ImGui::EndDisabled();
		}

		if (all || DrawingBuiltinTool == BuiltinStudioTool::Lock) {
			if (all) {
				ImGui::SameLine();
			}
			ImGui::BeginDisabled(Selection.empty());
			const bool locked = SelectionFlag("Locked");
			if (ImGui::Button(locked ? "Unlock" : "Lock", ImVec2(engine::ui::Scaled(84.0f), 0.0f))) {
				SetSelectionFlag("Locked", !locked, locked ? "Unlock" : "Lock");
			}
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
				ImGui::SetTooltip(
					"Whether a click in the viewport can select it. A locked part\n"
					"still draws, still collides and is still reachable from the\n"
					"explorer and from a script."
				);
			}

			ImGui::EndDisabled();
		}

		// **Two buttons about a drag, beside the two about the selection**,
		// because that is what somebody is doing when they reach for either:
		// Anchor and Lock decide what a drag may touch, and these two decide
		// what it does and what it shows while it does it.
		//
		// Not disabled with an empty selection, unlike the pair above. Both are
		// modes rather than edits - there is nothing to apply them to and
		// nothing to fail - and greying a mode until you have chosen a target
		// is the wrong way round for somebody who sets it before reaching for
		// the part.
		if (all || DrawingBuiltinTool == BuiltinStudioTool::Align) {
			if (all) {
				ImGui::SameLine();
			}
			if (RunButton("Align", DragAligns, engine::ui::AccentColour())) {
				DragAligns = !DragAligns;
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(
					"Held: a dragged part turns to sit flat on whatever it lands on.\n"
					"Off: it keeps the rotation it already had.\n\n"
					"The old facing is turned onto the new surface rather than\n"
					"thrown away, so parts dropped on one wall do not all end up\n"
					"pointing the same way."
				);
			}
		}

		if (all || DrawingBuiltinTool == BuiltinStudioTool::Facing) {
			if (all) {
				ImGui::SameLine();
			}
			if (RunButton("Facing", ShowFacing, engine::ui::AccentColour())) {
				ShowFacing = !ShowFacing;
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(
					"Draws which way the selection is facing: a line out of the\n"
					"front face to a ball, and a ring round the ball with an arrow\n"
					"at the point that is up.\n\n"
					"A box says nothing about its orientation - two parts sitting\n"
					"identically may be turned a quarter apart."
				);
			}
		}
	}

	void Editor::DrawModelTools() {
		const bool nothing = Selection.empty();
		const bool all = DrawingBuiltinTool == BuiltinStudioTool::None;

		// --- the pivot -------------------------------------------------------

		if (all || DrawingBuiltinTool == BuiltinStudioTool::EditPivot) {
			ImGui::Checkbox("Edit Pivot", &PivotEditing);
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(
					"Points the Move and Rotate handles at the pivot instead of the\n"
					"part. The part does not move - which is the point: a door whose\n"
					"hinge is wrong is fixed by moving the hinge."
				);
			}
		}

		if (all || DrawingBuiltinTool == BuiltinStudioTool::ResetPivot) {
			if (all) {
				ImGui::SameLine();
			}
			ImGui::BeginDisabled(nothing);
			if (ImGui::Button("Reset Pivot", ImVec2(engine::ui::Scaled(96.0f), 0.0f))) {
				ResetSelectionPivot();
			}
			ImGui::EndDisabled();
		}

		if ((all || DrawingBuiltinTool == BuiltinStudioTool::PivotNotice) && PivotEditing &&
			CurrentTool == ToolMode::Scale) {
			// **Said rather than left to be discovered.** A pivot has no size,
			// so the scale handles go on resizing the part while the mode claims
			// to be editing pivots - which reads as the mode being broken.
			if (all) {
				ImGui::SameLine();
			}
			ImGui::TextDisabled("Scale still resizes the part.");
		}

		if (all) {
			Divider();
		}

		// --- what is on the selection ----------------------------------------
		//
		// **Through the operator table rather than by calling the editor's own
		// methods.** Every one of these is already a command with an
		// availability rule and a keybinding - see `studio/Operators.hpp` - so a
		// button that reached past it would be a second answer to "may this run
		// right now", and the two would disagree the first time one learned
		// about a running world.

		if (all || DrawingBuiltinTool == BuiltinStudioTool::Duplicate) {
			OperatorButton(Action::Duplicate, "Duplicate");
		}
		if (all || DrawingBuiltinTool == BuiltinStudioTool::Delete) {
			if (all) {
				ImGui::SameLine();
			}
			OperatorButton(Action::Delete, "Delete");
		}
		if (all || DrawingBuiltinTool == BuiltinStudioTool::Deselect) {
			if (all) {
				ImGui::SameLine();
			}
			OperatorButton(Action::SelectNone, "Deselect");
		}

		if (all) {
			Divider();
		}

		if (all || DrawingBuiltinTool == BuiltinStudioTool::Undo) {
			OperatorButton(Action::Undo, "Undo");
		}
		if (all || DrawingBuiltinTool == BuiltinStudioTool::Redo) {
			if (all) {
				ImGui::SameLine();
			}
			OperatorButton(Action::Redo, "Redo");
		}

		if (all) {
			Divider();
		}

		if (all || DrawingBuiltinTool == BuiltinStudioTool::SelectionCount) {
			ImGui::BeginDisabled(nothing);
			ImGui::Text("%zu selected", Selection.size());
			ImGui::EndDisabled();
		}
	}

	void Editor::DrawScriptTools() {
		const bool all = DrawingBuiltinTool == BuiltinStudioTool::None;
		// **The three programs, inserted where the explorer would put them.**
		// Which class a file becomes is the whole of Rojo's naming convention
		// one door along - `Script` runs on a server, `LocalScript` on a client
		// and `ModuleScript` runs when something requires it - so offering the
		// three by name is offering the decision an author actually makes.
		static const struct {
			const char *Class;
			const char *Tip;
			BuiltinStudioTool Tool;
		} PROGRAMS[] = {
			{"Script", "Runs on the server.", BuiltinStudioTool::CreateScript},
			{"LocalScript", "Runs on the client.", BuiltinStudioTool::CreateLocalScript},
			{"ModuleScript", "Runs when something requires it.", BuiltinStudioTool::CreateModuleScript},
		};

		for (const auto &program : PROGRAMS) {
			if (!all && DrawingBuiltinTool != program.Tool) {
				continue;
			}
			const engine::ecs::ClassId klass = engine::ecs::Classes::Find(Name(program.Class));
			const bool hasScene = Universe != nullptr && Active.IsValid();
			const float buttonWidth =
				ImGui::CalcTextSize(program.Class).x + ImGui::GetStyle().FramePadding.x * 2.0f;

			ImGui::BeginDisabled(!hasScene || !klass.IsValid());
			if (ImGui::Button(program.Class, ImVec2(buttonWidth, 0.0f))) {
				// Queued for the same reason the explorer's own menu queues it:
				// `InsertInstance` enters the world, and entering twice is what
				// the affinity check exists to catch.
				PendingInsert.World = Active;
				PendingInsert.Class = klass;
				PendingInsert.Parent = Selection.empty() ? engine::ecs::NULL_ENTITY : Selection.front();
			}
			ImGui::EndDisabled();

			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
				ImGui::SetTooltip("%s", hasScene ? program.Tip : "There is no scene open.");
			}
			if (all) {
				ImGui::SameLine();
			}
		}

		if (all) {
			Divider();
		}
		if (all || DrawingBuiltinTool == BuiltinStudioTool::ScriptDestination) {
			ImGui::TextDisabled(Selection.empty() ? "Goes under the scene." : "Goes under what is selected.");
		}

		if (all) {
			Divider();
		}

		// **The script panels, here rather than under View.** Somebody on this
		// tab is writing a program, and the editor and the debugger are the two
		// windows that job needs open.
		if (all || DrawingBuiltinTool == BuiltinStudioTool::ScriptEditorPanel) {
			ImGui::Checkbox("Script Editor", &ShowScripts);
		}
		if (all || DrawingBuiltinTool == BuiltinStudioTool::DebuggerPanel) {
			if (all) {
				ImGui::SameLine();
			}
			ImGui::Checkbox("Debugger", &ShowDebugger);
		}
		if (all || DrawingBuiltinTool == BuiltinStudioTool::CommandBarPanel) {
			if (all) {
				ImGui::SameLine();
			}
			ImGui::Checkbox("Command Bar", &ShowCommandBar);
		}
	}

	void Editor::DrawViewTools() {
		const bool all = DrawingBuiltinTool == BuiltinStudioTool::None;
		// **The panels somebody flips while working**, which is a subset of the
		// View menu rather than a copy of it: a menu is where you go to find
		// something once, and this is where you go to toggle the same three
		// things all afternoon.
		if (all || DrawingBuiltinTool == BuiltinStudioTool::Grid) {
			ImGui::Checkbox("Grid", &ShowGrid);
		}
		if (all || DrawingBuiltinTool == BuiltinStudioTool::Particles) {
			if (all) {
				ImGui::SameLine();
			}
			ImGui::Checkbox("Particles", &ShowParticleEmitters);
		}
		if (all || DrawingBuiltinTool == BuiltinStudioTool::ViewportIndicator) {
			if (all) {
				ImGui::SameLine();
			}
			if (RunButton("Direction Gizmo", ShowDirectionGizmo, engine::ui::AccentColour())) {
				ShowDirectionGizmo = !ShowDirectionGizmo;
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("show the clickable viewport direction indicator");
			}
		}
		if (all || DrawingBuiltinTool == BuiltinStudioTool::Cursor3D) {
			if (all) {
				ImGui::SameLine();
			}
			if (RunButton("3D Cursor", ShowCursor, engine::ui::AccentColour())) {
				ShowCursor = !ShowCursor;
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("show the viewport orbit point; Alt+left-click places it");
			}
		}
		if (all || DrawingBuiltinTool == BuiltinStudioTool::OrbitAroundCursor) {
			if (all) {
				ImGui::SameLine();
			}
			if (RunButton("Orbit Cursor", OrbitCamera, engine::ui::AccentColour())) {
				OrbitCamera = !OrbitCamera;
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("middle-drag to orbit the camera around the 3D cursor");
			}
		}
		if (all || DrawingBuiltinTool == BuiltinStudioTool::DirectionLock) {
			if (all) {
				ImGui::SameLine();
			}
			if (RunButton("Lock Direction", DirectionLocked, engine::ui::AccentColour())) {
				DirectionLocked = !DirectionLocked;
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("keep the camera on its current direction");
			}
		}
		if (all || DrawingBuiltinTool == BuiltinStudioTool::ExplorerPanel) {
			ImGui::Checkbox("Explorer", &ShowExplorer);
		}
		if (all || DrawingBuiltinTool == BuiltinStudioTool::PropertiesPanel) {
			if (all) ImGui::SameLine();
			ImGui::Checkbox("Properties", &ShowProperties);
		}
		if (all || DrawingBuiltinTool == BuiltinStudioTool::OutputPanel) {
			if (all) ImGui::SameLine();
			ImGui::Checkbox("Output", &ShowOutput);
		}
		if (all || DrawingBuiltinTool == BuiltinStudioTool::AssetsPanel) {
			if (all) ImGui::SameLine();
			ImGui::Checkbox("Assets", &ShowAssets);
		}
		if (all || DrawingBuiltinTool == BuiltinStudioTool::StatisticsPanel) {
			if (all) ImGui::SameLine();
			ImGui::Checkbox("Statistics", &ShowStatistics);
		}
		if (all || DrawingBuiltinTool == BuiltinStudioTool::FrameGraphPanel) {
			if (all) ImGui::SameLine();
			ImGui::Checkbox("Frame Graph", &ShowFrameGraph);
		}
		if (all || DrawingBuiltinTool == BuiltinStudioTool::HeapPanel) {
			if (all) ImGui::SameLine();
			ImGui::Checkbox("Heap", &ShowHeap);
		}

		if (all) {
			Divider();
		}

		// **Camera speed is a control, so it lives on the ribbon rather than in
		// the status bar.** The status bar reported it and could not change it,
		// which is the wrong half of the pair to have. It still reads it out;
		// this sets it.
		if (all || DrawingBuiltinTool == BuiltinStudioTool::CameraSpeed) {
			ImGui::TextDisabled("Camera");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(engine::ui::Scaled(140.0f));
			ImGui::SliderFloat("##camera-speed", &CameraSpeed, 1.0f, 200.0f, "%.0f u/s");
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("how fast the viewport camera flies");
			}
		}
	}
}
