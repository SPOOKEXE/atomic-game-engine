#include <engine/core/Profiling.hpp>
#include <engine/game/Game.hpp>
#include <engine/scene/Awake.hpp>
#include <engine/ui/Theme.hpp>
#include <engine/world/Enums.hpp>
#include <engine/world/Lifecycle.hpp>
#include <engine/world/Postbox.hpp>

#include <algorithm>
#include <client/Scene.hpp>
#include <cstdio>
#include <imgui.h>
#include <studio/Editor.hpp>
#include <studio/Widgets.hpp>

namespace studio {

	using engine::core::Name;
	using engine::ecs::Store;
	using engine::world::WorldId;

	void Editor::DrawWorlds() {
		if (!ShowWorlds) {
			return;
		}

		// **Before `Begin`, and it is the only place it can go.**
		// `SetNextWindowFocus` applies to the next window submitted, and
		// focusing a window that shares a dock node with another is what selects
		// its tab - which is the whole point here: the Worlds panel is docked
		// beside the Explorer and a new game wants it in front. See
		// `FocusWorlds`.
		if (FocusWorlds > 0) {
			FocusWorlds--;
			ImGui::SetNextWindowFocus();
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
		// **Structural changes need every scene stopped, not just one.** New and
		// Import add worlds to a universe that a running scene's snapshot does
		// not know about - see `WorldRun`.
		const bool editing = !AnyRunning();

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

		// **Inside `Begin`, so the rows are separable from the window.** The
		// panel measured 24 us in a real frame while the same widgets in a
		// benchmark measured 4 - and the question that gap asks is whether the
		// cost is what the panel draws or what it costs to *be* the panel.
		ENGINE_PROFILE("world rows");

		for (const WorldId world : Universe->Worlds()) {
			// **A client view is not a scene, so it is not listed among them.**
			// It exists only between Play and Stop, it is never written to a
			// game file, and every authoring action this panel offers is one the
			// editor should not make about somebody's screen. It was listed here
			// anyway - greyed, with a tooltip - which made the Worlds panel the
			// only way back to a client's viewport and, by omission, said there
			// was no way back to the *server's*. Both are rows in Live Instances
			// now. See `src/Instances.cpp`.
			if (IsReplicaWorld(world)) {
				continue;
			}

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

			// Where the player is, which is the fact that decides whether this
			// world stays open. Shown rather than inferred from the state
			// column: "active" and "occupied" are different claims.
			if (world == PlayerWorld) {
				ImGui::SameLine();
				ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::AccentColour());
				ImGui::TextUnformatted("(player)");
				ImGui::PopStyleColor();
			}

			// --- what it is doing ----------------------------------------------
			//
			// **State rather than tick rate.** A world's configured rate is a
			// `WorldSettings` field and `Universe` does not hand those back -
			// which is the right shape, because the interesting question about a
			// scene is what it is doing rather than what it was asked for.

			// **Two different claims, and both belong here.** The run state is
			// what the author did - Play, Run, paused, or editing. The world
			// state is what the universe is doing with it - active, suspended,
			// faulted. They disagree exactly when something interesting has
			// happened, and a panel showing only one of them cannot say why a
			// scene stopped moving. See `Editor::WorldRun`.
			ImGui::TableSetColumnIndex(1);

			if (const WorldRun *run = RunOf(world); run != nullptr) {
				ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::AccentColour());
				ImGui::TextUnformatted(run->Paused ? "paused" : Describe(run->Mode));
				ImGui::PopStyleColor();
				ImGui::SameLine();
			}

			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
			ImGui::TextUnformatted(engine::world::Describe(Universe->StateOf(world)));
			ImGui::PopStyleColor();

			ImGui::TableSetColumnIndex(2);
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());

			if (Universe->IsRemote(world)) {
				// A world held by a host has no store here, so counting its
				// instances is not this process's to do. Said, rather than shown
				// as zero - which would read as an empty scene.
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
	}

	void Editor::DrawWorldActions(WorldId world) {
		// **This scene's state, not the program's.** Renaming a world rebuilds
		// it, which a *running* world cannot survive - but a world sitting in
		// edit beside one that runs is renameable, and that is the whole point
		// of scenes running independently. See `WorldRun`.
		// **A client view is not a scene, so nothing that authors one applies to
		// it.** It exists between Play and Stop, its rows are somebody else's,
		// and it is never written to a game file - so renaming, duplicating,
		// exporting, removing and running it are all offers the editor should
		// not make. Stop is what takes it away, and Stop belongs to the world it
		// is a view of.
		//
		// Left *visible* rather than hidden, and greyed instead: a world in the
		// list with no menu at all reads as the panel being broken, and the
		// disabled entries say what kind of thing this is.
		const bool replica = IsReplicaWorld(world);
		const bool editing = !IsRunning(world) && !replica;
		const bool local = !Universe->IsRemote(world) && !replica;

		if (ImGui::MenuItem("Set Active", nullptr, false, world != Active)) {
			PendingActivate = world;
		}

		ImGui::Separator();

		// **The transport, per scene, where the scenes are listed.** The toolbar
		// runs whatever the focused viewport shows; this runs the row you are
		// pointing at, which is what makes a universe of scenes manageable
		// without opening a viewport onto each one first.
		const RunMode mode = ModeOf(world);

		if (ImGui::MenuItem("Play", nullptr, mode == RunMode::Play, local)) {
			PendingRunWorld = world;
			PendingRunMode = mode == RunMode::Play ? RunMode::Edit : RunMode::Play;
		}
		if (ImGui::MenuItem("Run", nullptr, mode == RunMode::Server, local)) {
			PendingRunWorld = world;
			PendingRunMode = mode == RunMode::Server ? RunMode::Edit : RunMode::Server;
		}
		if (ImGui::MenuItem("Stop", nullptr, false, mode != RunMode::Edit)) {
			PendingRunWorld = world;
			PendingRunMode = RunMode::Edit;
		}

		// **Teleport is not Set Active, and the difference is what the
		// lifecycle turns on.** Set Active moves the *viewport*: it decides
		// which world is drawn and edited, and costs nothing. Teleport moves
		// the player: it opens the destination if it is closed, and starts the
		// clock on the world being left. One is where you are looking; the
		// other is where somebody is standing.
		if (ImGui::MenuItem(
				"Teleport Player Here", nullptr, world == PlayerWorld, local && world != PlayerWorld
			)) {
			PendingTeleport = world;
		}

		ImGui::Separator();

		if (ImGui::MenuItem(
				"Close",
				nullptr,
				false,
				local && Universe->StateOf(world) != engine::world::WorldState::Suspended &&
					world != PlayerWorld && Universe->Count() > 1
			)) {
			PendingWorldState = world;
			PendingWorldStateTo = engine::world::WorldState::Suspended;
		}

		if (ImGui::MenuItem(
				"Open",
				nullptr,
				false,
				local && Universe->StateOf(world) == engine::world::WorldState::Suspended
			)) {
			PendingWorldState = world;
			PendingWorldStateTo = engine::world::WorldState::Active;
		}

		ImGui::Separator();

		if (ImGui::MenuItem("Rename...", nullptr, false, editing && local)) {
			// Queued, because the prompt has to outlive this popup - a modal
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

	bool Editor::TeleportPlayer(WorldId target) {
		if (!target.IsValid() || Universe->NameOf(target) == Name{}) {
			return false;
		}

		if (Universe->IsRemote(target)) {
			Say("that world is held by another host", engine::core::LogLevel::Warning);
			return false;
		}

		// **Woken before anything is sent to it.** A teleport addressed to a
		// suspended world routes and is delivered - the directory still knows
		// the world exists - and then sits in an inbox nothing drains, because
		// a suspended world does not tick. The arrival would happen whenever
		// somebody happened to resume it, which is not arrival.
		if (Universe->StateOf(target) == engine::world::WorldState::Suspended) {
			Universe->SetState(target, engine::world::WorldState::Active);
			Say("opened '" + std::string(Label(Universe->NameOf(target))) + "'");
		}

		const WorldId from = PlayerWorld;
		PlayerWorld = target;

		// The destination is busy from this instant rather than from its first
		// tick, so the idle timer cannot close it in the gap between being
		// woken and being arrived in.
		Touch(target);

		if (from.IsValid() && from != target) {
			// The world being left starts its clock now. It is not closed here
			// - somebody may teleport straight back, and a world that shut the
			// moment its last occupant stepped out would spend its life
			// starting and stopping.
			Touch(from);
			Say("teleported from '" + std::string(Label(Universe->NameOf(from))) + "' to '" +
				std::string(Label(Universe->NameOf(target))) + "'");
		} else {
			Say("player is in '" + std::string(Label(Universe->NameOf(target))) + "'");
		}

		return true;
	}

	void Editor::Touch(WorldId world) {
		for (WorldLife &life : Lives) {
			if (life.World == world) {
				life.LastActivity = Clock.Now();
				return;
			}
		}
		Lives.push_back(WorldLife{world, Clock.Now()});
	}

	void Editor::UpdateWorldLifecycle() {
		// **Only while something is running.** In Edit no world ticks, so
		// suspending one changes nothing an author can see and leaves a state
		// they have to put back by hand.
		if (!AnyRunning() || !AutoManageWorlds) {
			return;
		}

		const double now = Clock.Now();

		for (const WorldId world : Universe->Worlds()) {
			if (Universe->IsRemote(world)) {
				continue;
			}

			// **What this host knows, handed to the policy that both hosts
			// share.** The decision itself is `world::DecideLifecycle` - see
			// `Lifecycle.hpp` and `DEFERRED.md` D00017: an editor that closes a
			// world and a server that does not would be one policy written
			// twice, and the copy nobody compares is the one that drifts.
			engine::world::LifecycleInputs inputs;
			inputs.State = Universe->StateOf(world);
			inputs.Sleep = IdleSleepMode;
			inputs.IdleLimit = static_cast<double>(IdleCloseSeconds);

			// **Worlds still ticking, not worlds that exist.** `Count()`
			// includes suspended ones, so deriving this from it suspended a
			// whole universe one world at a time - each the "last" only after
			// the rest had already gone, which is exactly what this refusal is
			// named to prevent. `CountInState` is the fact it is about.
			inputs.LastWorld = Universe->CountInState(engine::world::WorldState::Active) <= 1;

			// **A suspended world's inbox is the one queue nothing drains**,
			// which is exactly what makes this reliable rather than a poll that
			// races the tick. Read only while suspended, because a running
			// world replaces its inbox every barrier - so the same read on an
			// active world would be a coin toss about when the frame landed.
			//
			// **Read before the run-scope refusal below, and that ordering is
			// the fix.** It used to come after, so a world outside the run was
			// skipped before anybody looked in its letterbox.
			if (inputs.State == engine::world::WorldState::Suspended) {
				Universe->Enter(world, [&inputs](Store &store) {
					if (const auto *inbox = store.Resource<engine::world::Inbox>()) {
						inputs.InboxWaiting = !inbox->Arrived.empty();
					}
				});
			}

			// **A scoped run's suspended worlds are not the lifecycle's to
			// wake.** Occupancy says an empty world should idle and a visited
			// one should resume - but a world outside the run scope is empty
			// *because* it was deliberately stopped, and resuming it would
			// restart the scene the author chose not to run. See `WorldRun`.
			//
			// **Unless something has arrived for it, which is not the same
			// thing at all.** A teleport destroys the player in the world they
			// left *before* the destination has admitted them -
			// `TeleportService:Teleport` says why it must, and
			// `PlaygroundPad.luau` says what it costs - so an arrival sitting in
			// a closed world's inbox is a player who no longer exists anywhere.
			// Refusing to wake that world does not leave the scene unstarted; it
			// strands somebody in it, and `Editor::FollowTeleports` then spends
			// `LOST_FRAMES` looking for them before reporting the client gone.
			//
			// Which is exactly what the studio's own Playground does: its pad
			// sends whoever stands on it to Arena, Arena is not in the run, and
			// walking onto the pad ended the client every time.
			if (!IsRunning(world) && !inputs.InboxWaiting) {
				continue;
			}

			// Occupied, or being looked at *in either viewport*. Any of those is
			// a reason to keep running: closing a world somebody is watching
			// would freeze it in front of them with no visible cause.
			//
			// **The second viewport counts, and forgetting it was a bug this
			// caught.** With two panels open on two worlds, the one that was
			// not the active scene was closed under the panel showing it -
			// which looks exactly like the second viewport being broken.
			//
			// **This is the half that stays here**, because "somebody is looking
			// at it" is a question only an editor can ask.
			bool watched = false;
			for (const ViewportState &view : Extras) {
				if (view.Open && view.World == world) {
					watched = true;
					break;
				}
			}
			inputs.Occupied = world == PlayerWorld || world == Active || watched;

			// **And whatever the game says is still happening.** The three
			// answers above are all "somebody is here"; a world of NPCs on a
			// route has nobody here and plenty going on. `scene::AwakeWorld` is
			// how a script says so, and the server asks the same question in
			// `Server::UpdateWorldLifecycle` - one component, two hosts, so a
			// world that stays up in Play stays up when it is hosted.
			if (!inputs.Occupied) {
				Universe->Enter(world, [&inputs](Store &store) {
					inputs.Occupied = engine::scene::WorldIsHeldAwake(store);
				});
			}

			// **Only a running world has an idle clock, and only it needs one.**
			// Doing this for a suspended world would make waking it wait for a
			// `Lives` entry it has no use for - one frame of delay on a teleport,
			// on the first pass after the world was seen.
			if (inputs.State == engine::world::WorldState::Active) {
				const auto found = std::find_if(Lives.begin(), Lives.end(), [world](const WorldLife &life) {
					return life.World == world;
				});

				if (found == Lives.end()) {
					// First seen. Its clock starts now rather than at zero, so a
					// world created during a run is not immediately eligible.
					Lives.push_back(WorldLife{world, now});
					continue;
				}
				inputs.IdleSeconds = now - found->LastActivity;
			}

			const engine::world::LifecycleAction action = engine::world::DecideLifecycle(inputs);

			if (action == engine::world::LifecycleAction::Resume) {
				Universe->SetState(world, engine::world::WorldState::Active);
				Touch(world);
				Say("opened '" + std::string(Label(Universe->NameOf(world))) +
					"' - something arrived for it");
				continue;
			}

			if (action == engine::world::LifecycleAction::Leave) {
				// An occupied world's clock is restarted rather than merely not
				// read, which is what makes `IdleSeconds` mean "since somebody
				// left" on the next pass instead of "since it was created".
				if (inputs.Occupied) {
					Touch(world);
				}
				continue;
			}

			Universe->SetState(world, engine::world::WorldState::Suspended);
			// Formatted rather than truncated: an integer cast turns the 0.3
			// somebody passed to `--idle-close` into "empty for 0s", which
			// reads as a world that closed for no reason.
			char elapsed[32];
			std::snprintf(elapsed, sizeof(elapsed), "%.4g", static_cast<double>(IdleCloseSeconds));
			Say("closed '" + std::string(Label(Universe->NameOf(world))) + "' - empty for " + elapsed + "s");
		}
	}

	size_t Editor::InstanceCountOf(WorldId world) {
		// **Cached, because the honest version is a scan of every entity in the
		// world and this panel draws sixty times a second.** A world of a
		// hundred thousand parts would spend a frame counting them for a number
		// nobody reads that fast - so it is recounted on a clock, which is the
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

		// **A span, because a periodic full scan is exactly the kind of cost
		// that hides in a mean.** It runs twice a second rather than every
		// frame, so it shows up as a spike in a panel that is otherwise flat -
		// which is unreadable without a name on it.
		ENGINE_PROFILE("count instances");

		size_t counted = 0;
		Universe->Enter(world, [&counted](Store &store) {
			store.EachEntity([&](engine::ecs::Entity entity) {
				// Instances, not entities. A world holds rows that are not
				// instances - the camera the client installs, a predicted entity
				// - and counting those would report a number the explorer does
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
		// Called by `ApplyPendingActions`, which is called once from
		// `DrawInterface`. See that function for why neither of them runs where
		// it was asked for.

		if (PendingActivate.IsValid()) {
			const WorldId world = PendingActivate;
			PendingActivate = WorldId{};

			if (world != Active) {
				Active = world;
				SelectionWorld = world;
				ClearSelection();
			}
		}

		// **Queued like every other world action.** `SetRunMode` destroys and
		// rebuilds the world, and doing that from inside the popup drawing the
		// row would pull the list out from under the loop walking it.
		if (PendingRunWorld.IsValid()) {
			const WorldId world = PendingRunWorld;
			const RunMode mode = PendingRunMode;
			PendingRunWorld = WorldId{};
			SetRunMode(world, mode);
		}

		if (PendingDuplicateWorld.IsValid()) {
			const WorldId source = PendingDuplicateWorld;
			PendingDuplicateWorld = WorldId{};
			DuplicateWorld(source);
		}

		if (PendingTeleport.IsValid()) {
			const WorldId target = PendingTeleport;
			PendingTeleport = WorldId{};
			TeleportPlayer(target);
		}

		if (PendingWorldState.IsValid()) {
			const WorldId world = PendingWorldState;
			const engine::world::WorldState to = PendingWorldStateTo;
			PendingWorldState = WorldId{};

			Universe->SetState(world, to);
			Touch(world);
			Say(std::string(to == engine::world::WorldState::Suspended ? "closed '" : "opened '") +
				std::string(Label(Universe->NameOf(world))) + "'");
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
		//
		// **The name is found first and the document read once.** Reading it in
		// a loop until one is accepted would retry a malformed document ninety
		// times and report the last failure, which is the same message as the
		// first with the cause a hundred lines further away.
		const std::string base(Label(Universe->NameOf(source), "World"));

		Name wanted;
		for (int attempt = 2; attempt < 1000; attempt++) {
			const Name candidate(base + " " + std::to_string(attempt));
			if (!Universe->Find(candidate).IsValid()) {
				wanted = candidate;
				break;
			}
		}

		if (!wanted.IsValid()) {
			Say("could not find a free name to copy '" + base + "' under", engine::core::LogLevel::Warning);
			return false;
		}

		const WorldId copy = engine::game::ReadWorldDocument(*Universe, document, wanted, error);
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
		// rename and should not grow one - a live world renamed underneath its
		// pending traffic is stranded, and nothing would report it.
		//
		// In Edit mode there is no traffic: nothing ticks and no script has
		// subscribed to anything. So the safe rename is the one that goes
		// through the save format, and that is why the menu item is disabled
		// while a game is running.
		//
		// **The handle survives.** `Universe::Adopt` reuses the hole a destroyed
		// world leaves, so recreating immediately takes the same slot - the
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
