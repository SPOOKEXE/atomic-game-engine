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

		Divider();

		// --- the manipulators ------------------------------------------------
		//
		// One mode at a time, which the `ToolMode` declaration argues for: three
		// sets of handles over one object is a target nobody can hit.
		//
		// **Select is a button of its own as well as what the others toggle back
		// to.** Clicking the held mode puts the handles away without reaching
		// across the strip, and somebody who wants no handles and has not
		// learned that has an obvious thing to press.
		if (RunButton("Select", CurrentTool == ToolMode::Select, engine::ui::AccentColour())) {
			CurrentTool = ToolMode::Select;
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("click to select - no handles");
		}
		ImGui::SameLine();

		const auto tool = [this](ToolMode mode, const char *label, const char *tip) {
			if (RunButton(label, CurrentTool == mode, engine::ui::AccentColour())) {
				CurrentTool = CurrentTool == mode ? ToolMode::Select : mode;
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", tip);
			}
			ImGui::SameLine();
		};

		tool(ToolMode::Move, "Move", "drag an axis to move the selection along it");
		tool(ToolMode::Rotate, "Rotate", "drag a ring to turn the selection about its axis");
		tool(ToolMode::Scale, "Scale", "drag an axis to grow the selection along it");

		Divider();

		// --- the steps -------------------------------------------------------

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
		ImGui::SameLine();
		ImGui::BeginDisabled(!SnapEnabled);
		ImGui::SetNextItemWidth(engine::ui::Scaled(70.0f));
		ImGui::DragFloat(
			"##snap-distance", &SnapDistance, 0.1f, 0.05f, 100.0f, "%.2f m", ImGuiSliderFlags_AlwaysClamp
		);
		ImGui::SameLine();
		ImGui::SetNextItemWidth(engine::ui::Scaled(70.0f));
		ImGui::DragFloat(
			"##snap-degrees", &SnapDegrees, 1.0f, 1.0f, 90.0f, "%.0f deg", ImGuiSliderFlags_AlwaysClamp
		);
		ImGui::EndDisabled();

		Divider();

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

		Divider();

		// --- what is selected ------------------------------------------------

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

		ImGui::SameLine();

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

		ImGui::SameLine();

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
		ImGui::EndDisabled();

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

		ImGui::SameLine();

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

	void Editor::DrawModelTools() {
		const bool nothing = Selection.empty();

		// --- the pivot -------------------------------------------------------

		ImGui::Checkbox("Edit Pivot", &PivotEditing);
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"Points the Move and Rotate handles at the pivot instead of the\n"
				"part. The part does not move - which is the point: a door whose\n"
				"hinge is wrong is fixed by moving the hinge."
			);
		}

		ImGui::SameLine();
		ImGui::BeginDisabled(nothing);
		if (ImGui::Button("Reset Pivot", ImVec2(engine::ui::Scaled(96.0f), 0.0f))) {
			ResetSelectionPivot();
		}
		ImGui::EndDisabled();

		if (PivotEditing && CurrentTool == ToolMode::Scale) {
			// **Said rather than left to be discovered.** A pivot has no size,
			// so the scale handles go on resizing the part while the mode claims
			// to be editing pivots - which reads as the mode being broken.
			ImGui::SameLine();
			ImGui::TextDisabled("Scale still resizes the part.");
		}

		Divider();

		// --- what is on the selection ----------------------------------------
		//
		// **Through the operator table rather than by calling the editor's own
		// methods.** Every one of these is already a command with an
		// availability rule and a keybinding - see `studio/Operators.hpp` - so a
		// button that reached past it would be a second answer to "may this run
		// right now", and the two would disagree the first time one learned
		// about a running world.

		OperatorButton(Action::Duplicate, "Duplicate");
		ImGui::SameLine();
		OperatorButton(Action::Delete, "Delete");
		ImGui::SameLine();
		OperatorButton(Action::SelectNone, "Deselect");

		Divider();

		OperatorButton(Action::Undo, "Undo");
		ImGui::SameLine();
		OperatorButton(Action::Redo, "Redo");

		Divider();

		ImGui::BeginDisabled(nothing);
		ImGui::Text("%zu selected", Selection.size());
		ImGui::EndDisabled();
	}

	void Editor::DrawScriptTools() {
		// **The three programs, inserted where the explorer would put them.**
		// Which class a file becomes is the whole of Rojo's naming convention
		// one door along - `Script` runs on a server, `LocalScript` on a client
		// and `ModuleScript` runs when something requires it - so offering the
		// three by name is offering the decision an author actually makes.
		if (Universe == nullptr || !Active.IsValid()) {
			ImGui::TextDisabled("no scene");
			return;
		}

		static const struct {
			const char *Class;
			const char *Tip;
		} PROGRAMS[] = {
			{"Script", "Runs on the server."},
			{"LocalScript", "Runs on the client."},
			{"ModuleScript", "Runs when something requires it."},
		};

		for (const auto &program : PROGRAMS) {
			const engine::ecs::ClassId klass = engine::ecs::Classes::Find(Name(program.Class));
			const float buttonWidth =
				ImGui::CalcTextSize(program.Class).x + ImGui::GetStyle().FramePadding.x * 2.0f;

			ImGui::BeginDisabled(!klass.IsValid());
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
				ImGui::SetTooltip("%s", program.Tip);
			}
			ImGui::SameLine();
		}

		Divider();

		ImGui::TextDisabled(Selection.empty() ? "Goes under the scene." : "Goes under what is selected.");

		Divider();

		// **The script panels, here rather than under View.** Somebody on this
		// tab is writing a program, and the editor and the debugger are the two
		// windows that job needs open.
		ImGui::Checkbox("Script Editor", &ShowScripts);
		ImGui::SameLine();
		ImGui::Checkbox("Debugger", &ShowDebugger);
		ImGui::SameLine();
		ImGui::Checkbox("Command Bar", &ShowCommandBar);
	}

	void Editor::DrawViewTools() {
		// **The panels somebody flips while working**, which is a subset of the
		// View menu rather than a copy of it: a menu is where you go to find
		// something once, and this is where you go to toggle the same three
		// things all afternoon.
		ImGui::Checkbox("Grid", &ShowGrid);
		ImGui::SameLine();
		ImGui::Checkbox("Particles", &ShowParticleEmitters);
		ImGui::SameLine();
		ImGui::Checkbox("Explorer", &ShowExplorer);
		ImGui::SameLine();
		ImGui::Checkbox("Properties", &ShowProperties);
		ImGui::SameLine();
		ImGui::Checkbox("Output", &ShowOutput);
		ImGui::SameLine();
		ImGui::Checkbox("Assets", &ShowAssets);
		ImGui::SameLine();
		ImGui::Checkbox("Statistics", &ShowStatistics);
		ImGui::SameLine();
		ImGui::Checkbox("Frame Graph", &ShowFrameGraph);
		ImGui::SameLine();
		ImGui::Checkbox("Heap", &ShowHeap);

		Divider();

		// **Camera speed is a control, so it lives on the ribbon rather than in
		// the status bar.** The status bar reported it and could not change it,
		// which is the wrong half of the pair to have. It still reads it out;
		// this sets it.
		ImGui::TextDisabled("Camera");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(engine::ui::Scaled(140.0f));
		ImGui::SliderFloat("##camera-speed", &CameraSpeed, 1.0f, 200.0f, "%.0f u/s");
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("how fast the viewport camera flies");
		}
	}
}
