// What is running in this process, and the way back to each of its views.
//
// **A run is not a scene, and the Worlds panel was being asked to be both.** A
// universe holds authored scenes; a run holds a server and the clients admitted
// to it, and a client's replica world exists only between Play and Stop. Listing
// the two together meant a client view could be reopened - it had a row like any
// other world - while the *server's* view could not, because the server is not a
// separate world at all: it is the scene, and closing the panel that showed it
// left nothing in any list to press.
//
// So this panel lists the run, and every row can open its own view. That is the
// whole reason it exists: `mono.studio/AGENTS.md` says a closable panel with no
// way back is a panel somebody loses, and a *view of a live instance* is the one
// thing in this editor that had no way back at all.
//
// **Nothing here is cached and nothing here is authoritative.** The rows are
// read from `Runs` and each link's `LinkReport` every frame, for the reason at
// the top of `mono.studio/AGENTS.md`: a report is replaced on every step of the
// link, so a copy kept between frames describes a step that has already gone.

#include <engine/ui/Theme.hpp>

#include <cstdio>
#include <imgui.h>
#include <studio/Editor.hpp>
#include <studio/Viewports.hpp>
#include <vector>

namespace studio {

	using engine::core::Name;
	using engine::world::WorldId;

	namespace {
		// A byte count in the units somebody reads at a glance, which is the
		// same rule the Bus panel's sizes follow.
		void WriteInstanceSize(char *into, size_t capacity, uint64_t bytes) {
			if (bytes < 1024) {
				std::snprintf(into, capacity, "%llu B", static_cast<unsigned long long>(bytes));
			} else if (bytes < 1024 * 1024) {
				std::snprintf(into, capacity, "%.1f KB", static_cast<double>(bytes) / 1024.0);
			} else {
				std::snprintf(into, capacity, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
			}
		}
	}

	size_t Editor::ShowWorldInViewport(WorldId world) {
		if (!world.IsValid()) {
			return NO_VIEWPORT;
		}

		// **Which panel is `ChooseViewportFor`'s to decide**, in a header a test
		// can reach - the three ways that choice can be wrong all look like
		// something else from here. What is left is the half that needs an
		// editor: opening the panel, pointing it at the world, bringing it
		// forward. A vector per press rather than a member, because this is a
		// button and not a frame.
		std::vector<PanelView> panels;
		panels.reserve(Extras.size());
		for (const ViewportState &view : Extras) {
			panels.push_back(PanelView{view.World, view.Open});
		}

		const size_t decided = ChooseViewportFor(world, ViewportWorld(0), ShowViewport, panels);

		if (decided == 0) {
			// The main panel. It follows the active scene, so there is nothing
			// to pin - only to reopen, which is what a closed one needs.
			ShowViewport = true;
			ImGui::SetWindowFocus(ViewportIdentity(0));
			return 0;
		}

		// `NO_VIEWPORT` means every panel is spoken for. Making one is the
		// remaining honest answer: taking somebody's would lose the scene they
		// were watching.
		const size_t panel = decided == NO_VIEWPORT ? AddExtraViewport() : decided;

		ViewportState *view = ExtraAt(panel);
		if (view == nullptr) {
			return NO_VIEWPORT;
		}

		// Already assigned to it, including a closed panel: bring it forward and
		// leave its remembered camera exactly where somebody put it.
		if (view->World == world) {
			view->Open = true;
			ImGui::SetWindowFocus(ViewportIdentity(panel));
			return panel;
		}

		view->World = world;
		view->Open = true;

		view->Follow = engine::ecs::NULL_ENTITY;

		ImGui::SetWindowFocus(ViewportIdentity(panel));
		return panel;
	}

	void Editor::DrawLiveInstances() {
		if (!ShowLiveInstances) {
			return;
		}

		// Before `Begin`, which is the only place it can go - see `FocusWorlds`
		// for why focusing selects the tab this panel shares with the Worlds one.
		if (FocusInstances > 0) {
			FocusInstances--;
			ImGui::SetNextWindowFocus();
		}

		if (!ImGui::Begin("Live Instances", &ShowLiveInstances)) {
			ImGui::End();
			return;
		}

		if (Runs.empty()) {
			// **Said rather than left blank**, because an empty panel and a
			// panel that has lost its rows look identical.
			ImGui::TextDisabled("nothing is running");
			ImGui::Spacing();
			ImGui::TextDisabled("Play starts a server and a client in this process.");
			ImGui::TextDisabled("Run starts the server alone.");
			ImGui::End();
			return;
		}

		constexpr ImGuiTableFlags FLAGS =
			ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV;

		if (!ImGui::BeginTable("##instances", 4, FLAGS)) {
			ImGui::End();
			return;
		}

		// **The buttons get the width they need and the name gets the rest.**
		// This column was a proportional stretch like the other three, at 0.22
		// of the panel - and it holds `View`, `+ Player` and `Stop` side by
		// side. Twenty-two per cent of a docked panel is not three buttons wide
		// at any realistic size, so the row was clipped and `Stop` was cut in
		// half or missing entirely. A proportional width cannot be right for a
		// cell whose content has a fixed size.
		//
		// Measured from the labels rather than guessed at a pixel count,
		// because the font is a setting: two frame paddings and one item
		// spacing per button, which is what `SmallButton` and `SameLine` add.
		// The widest row is the server's three; a client row is `View` and
		// `Remove` and fits inside it.
		const ImGuiStyle &style = ImGui::GetStyle();
		const float buttonPadding = style.FramePadding.x * 2.0f;
		const float actionsWidth = ImGui::CalcTextSize("View").x + buttonPadding +
								   ImGui::CalcTextSize("+ Player").x + buttonPadding +
								   ImGui::CalcTextSize("Stop").x + buttonPadding +
								   style.ItemSpacing.x * 2.0f + style.CellPadding.x * 2.0f;

		ImGui::TableSetupColumn("instance", ImGuiTableColumnFlags_WidthStretch, 0.44f);
		ImGui::TableSetupColumn("role", ImGuiTableColumnFlags_WidthStretch, 0.20f);
		ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthStretch, 0.36f);
		ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, actionsWidth);
		ImGui::TableHeadersRow();

		// **By value, because the buttons in the loop restructure `Runs`.** Stop
		// erases a run and Remove erases a link, and both are pressed from
		// inside the walk over them. The ids are handles rather than pointers,
		// so a row acting on a run that has just gone is a lookup that answers
		// nothing rather than a dangling read.
		WorldId stopping;
		WorldId spawning;
		WorldId removing;
		WorldId viewing;

		for (const WorldRun &run : Runs) {
			const Name name = Universe->NameOf(run.World);

			ImGui::PushID(static_cast<int>(run.World.Index));
			ImGui::TableNextRow();

			// --- the server ---------------------------------------------------

			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(name.IsValid() ? Label(name) : "(unnamed)");

			ImGui::TableSetColumnIndex(1);
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::AccentColour());
			ImGui::TextUnformatted(run.Paused ? "paused" : Describe(run.Mode));
			ImGui::PopStyleColor();

			ImGui::TableSetColumnIndex(2);
			const size_t held = InstanceCountOf(run.World);
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
			ImGui::Text("%zu instance%s", held, held == 1 ? "" : "s");
			ImGui::PopStyleColor();

			ImGui::TableSetColumnIndex(3);
			if (ImGui::SmallButton("View")) {
				viewing = run.World;
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("+ Player")) {
				spawning = run.World;
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("Stop")) {
				stopping = run.World;
			}

			// --- its clients ----------------------------------------------------
			//
			// **Under the server rather than in a list of their own**, because a
			// client is only meaningful as somebody's client: two runs each with
			// a "client 1" is what a flat list would show, and that is exactly
			// the ambiguity `FollowTeleports` had to be taught to resolve.

			for (const std::unique_ptr<PlayLink> &link : run.Links) {
				if (link == nullptr || !link->IsRunning()) {
					continue;
				}

				const WorldId replica = link->ReplicaWorld();
				const LinkReport &report = link->Report();

				ImGui::PushID(static_cast<int>(replica.Index));
				ImGui::TableNextRow();

				ImGui::TableSetColumnIndex(0);
				const Name client = Universe->NameOf(replica);
				ImGui::TextUnformatted("  ");
				ImGui::SameLine(0.0f, 0.0f);
				ImGui::TextUnformatted(client.IsValid() ? Label(client) : "(client)");

				// Where this client is standing, which is the fact that says a
				// teleport was followed. Their authority world and the run's are
				// the same until the first one and different after it.
				if (link->AuthorityWorld() != run.World) {
					ImGui::SameLine();
					ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::AccentColour());
					ImGui::Text("(in %s)", Label(Universe->NameOf(link->AuthorityWorld())));
					ImGui::PopStyleColor();
				}

				ImGui::TableSetColumnIndex(1);
				ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
				ImGui::TextUnformatted("client");
				ImGui::PopStyleColor();

				// **How much world it holds and how far behind it is**, which
				// are the two numbers that separate "the box is in the wrong
				// place" from "nothing has arrived since tick 400". They used to
				// be a tooltip on a Worlds row; a client is an instance and this
				// is where instances are reported.
				ImGui::TableSetColumnIndex(2);
				const bool behind = report.ServerEntities != report.ClientEntities;
				ImGui::PushStyleColor(
					ImGuiCol_Text, behind ? engine::ui::WarningColour() : engine::ui::MutedColour()
				);
				ImGui::Text(
					"%zu/%zu at %llu",
					report.ClientEntities,
					report.ServerEntities,
					static_cast<unsigned long long>(report.Applied)
				);
				ImGui::PopStyleColor();

				if (ImGui::IsItemHovered()) {
					char total[32];
					WriteInstanceSize(total, sizeof(total), report.TotalBytes);

					char largest[32];
					WriteInstanceSize(largest, sizeof(largest), report.LargestMessage);

					ImGui::SetTooltip(
						"a replica of '%s', served in this process\n"
						"tick %llu, applied %llu\n"
						"%zu of %zu entities\n"
						"%zu message(s) last step, largest %s\n"
						"%s total",
						Label(Universe->NameOf(link->AuthorityWorld())),
						static_cast<unsigned long long>(report.Tick),
						static_cast<unsigned long long>(report.Applied),
						report.ClientEntities,
						report.ServerEntities,
						report.Messages,
						largest,
						total
					);
				}

				ImGui::TableSetColumnIndex(3);
				if (ImGui::SmallButton("View")) {
					viewing = replica;
				}
				ImGui::SameLine();
				if (ImGui::SmallButton("Remove")) {
					removing = replica;
				}

				ImGui::PopID();
			}

			ImGui::PopID();
		}

		ImGui::EndTable();
		ImGui::End();

		// **Outside the walk, for the reason the fields above give.** Opening a
		// viewport is safe anywhere; the three that restructure a run are not,
		// and doing them here is the same discipline `ApplyPendingActions` keeps
		// for everything that reaches into a world.
		if (viewing.IsValid()) {
			(void)ShowWorldInViewport(viewing);
		}
		if (spawning.IsValid()) {
			(void)SpawnPlayer(spawning);
		}
		if (removing.IsValid()) {
			(void)RemovePlayer(removing);
		}
		if (stopping.IsValid()) {
			SetRunMode(stopping, RunMode::Edit);
		}
	}
}
