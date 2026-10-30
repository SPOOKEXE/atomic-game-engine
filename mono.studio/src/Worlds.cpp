#include "SourceEditor.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/examples/DemosLoader.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/game/Game.hpp>
#include <engine/render/PortalImageHost.hpp>
#include <engine/scene/Awake.hpp>
#include <engine/scene/Components.hpp>
#include <engine/script/Instances.hpp>
#include <engine/ui/Theme.hpp>
#include <engine/world/Enums.hpp>
#include <engine/world/Lifecycle.hpp>
#include <engine/world/Postbox.hpp>

#include <algorithm>
#include <client/Scene.hpp>
#include <cstdio>
#include <imgui.h>
#include <string>
#include <studio/Editor.hpp>
#include <studio/TeamCreate.hpp>
#include <studio/Widgets.hpp>
#include <unordered_map>
#include <vector>

namespace studio {

	using engine::core::Name;
	using engine::ecs::Store;
	using engine::world::WorldId;

	namespace {
		// How tall the list is allowed to get before it scrolls.
		//
		// **There are more than forty staged scenes**, which at a menu row each
		// is taller than most windows this editor is opened in - and an imgui
		// menu that does not fit is clamped rather than scrolled, so the last
		// entries are simply unreachable. A child with its own scrollbar
		// behaves the same inside a menu and inside a popup, which is what lets
		// the three places that offer this share one function.
		constexpr float EXAMPLE_LIST_WIDTH = 240.0f;
		constexpr float EXAMPLE_LIST_HEIGHT = 360.0f;
	}

	void Editor::DrawExampleSceneItems() {
		const std::vector<engine::examples::DemoEntry> demos = engine::examples::DemosLoader().List();
		if (demos.empty()) {
			// A build staged without the examples is a real situation. Saying so
			// beats an empty list, which reads as the editor being broken.
			ImGui::TextDisabled("no staged examples");
			return;
		}

		ImGui::BeginChild("##example-list", ImVec2(EXAMPLE_LIST_WIDTH, EXAMPLE_LIST_HEIGHT));
		for (const engine::examples::DemoKind kind :
			 {engine::examples::DemoKind::World, engine::examples::DemoKind::Script}) {
			ImGui::SeparatorText(
				kind == engine::examples::DemoKind::World ? "Authored worlds" : "Script demos"
			);
			for (const engine::examples::DemoEntry &demo : demos) {
				if (demo.Kind != kind) {
					continue;
				}
				if (ImGui::MenuItem(demo.Name.c_str())) {
					AddExampleWorld(demo);
				}
			}
		}
		ImGui::EndChild();
	}

	void Editor::DrawExampleSceneMenu() {
		// Enabled while a scene runs. Stop restores one world from its own
		// document and cannot reach a scene created after the run began.
		if (ImGui::BeginMenu("Create World From Demo")) {
			DrawExampleSceneItems();
			ImGui::EndMenu();
		}
	}

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
		// **Adding a scene while another one runs is allowed, and the reason it
		// was not is gone.** This was refused because "the snapshot Stop
		// restores was taken before the run began, so a world added during Play
		// would vanish on Stop" - which was true when the snapshot was
		// `Universe::Save`, every world at once.
		//
		// It is not a universe snapshot any more. `WorldRun::Snapshot` is a
		// *world document* written by `WriteWorldDocument`, and `StopWorld`
		// destroys and rebuilds exactly the one world it names. A scene created
		// during a run is in no run's snapshot, so Stop cannot reach it. The
		// comment on `WorldRun::Snapshot` records that change and this refusal
		// outlived it.
		//
		// **Safe against the tick as well as against Stop.** `Universe::Tick`
		// blocks, `Editor::Simulate` runs from the frame loop, and this button
		// is pressed while the interface draws - so a world is added between
		// ticks on the thread that owns them, never during one.
		//
		// **Removal is a different question and is not decided here.** Deleting
		// a running world would leave a `WorldRun` naming storage that is gone;
		// that path carries its own guard.
		if (ImGui::Button("New")) {
			AskingNewWorld = true;
			NameBuffer = "World " + std::to_string(Universe->Count() + 1);
		}
		ImGui::SameLine();
		if (ImGui::Button("Import...")) {
			AskingImport = true;
			PathBuffer = ConfigPath("worlds").string();
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
		// A running scene cannot be renamed while its traffic uses its name.
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
			PendingRuns.push_back({world, mode == RunMode::Play ? RunMode::Edit : RunMode::Play, false});
		}
		if (ImGui::MenuItem("Run", nullptr, mode == RunMode::Server, local)) {
			PendingRuns.push_back({world, mode == RunMode::Server ? RunMode::Edit : RunMode::Server, false});
		}
		if (ImGui::MenuItem("Stop", nullptr, false, mode != RunMode::Edit)) {
			PendingRuns.push_back({world, RunMode::Edit, false});
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

		if (ImGui::MenuItem(
				"Rename...",
				nullptr,
				false,
				editing && local && !AnyRunning() && FactoryHost == nullptr &&
					(Team == nullptr || Team->Edits() == nullptr)
			)) {
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
		ApplyPendingRunActions();

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
		std::vector<engine::game::InstanceDocumentIdentity> sourceIdentities;
		const std::string document =
			engine::game::WriteWorldDocument(*Universe, source, error, &sourceIdentities);
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

		struct PendingEdit {
			uint32_t LocalId;
			bool Shader;
			bool JavaScript;
			Name Path;
			std::string Text;
		};
		struct ProgramState {
			uint32_t LocalId;
			std::optional<engine::script::LuaSourceContainer> Lua;
			std::optional<engine::script::JavaScriptSourceContainer> Js;
			std::optional<engine::script::CodeSourceContainerSelector> Selector;
		};
		std::vector<ProgramState> programs;
		std::vector<PendingEdit> edits;
		bool sourcesUnchanged = true;
		Universe->Enter(source, [&](Store &store) {
			for (const auto &identity : sourceIdentities) {
				const auto *lua = store.Get<engine::script::LuaSourceContainer>(identity.Instance);
				const auto *js = store.Get<engine::script::JavaScriptSourceContainer>(identity.Instance);
				const auto *selector =
					store.Get<engine::script::CodeSourceContainerSelector>(identity.Instance);
				if (lua == nullptr && js == nullptr && selector == nullptr) continue;
				ProgramState state{identity.LocalId, {}, {}, {}};
				if (lua) state.Lua = *lua;
				if (js) state.Js = *js;
				if (selector) state.Selector = *selector;
				programs.push_back(std::move(state));
			}
			for (const OpenScript &tab : Scripts) {
				if (tab.World != source || !tab.Modified) continue;
				std::string current;
				bool readable = false;
				const auto identity =
					std::find_if(sourceIdentities.begin(), sourceIdentities.end(), [&](const auto &row) {
						return row.Instance == tab.Instance;
					});
				if (identity == sourceIdentities.end() ||
					!ReadOpenedSource(
						store, tab.Instance, tab.Shader, tab.JavaScript, tab.Path, current, readable
					) ||
					!readable || !tab.SourceBaselineReadable || current != tab.SourceBaseline) {
					sourcesUnchanged = false;
					break;
				}
				edits.push_back({identity->LocalId, tab.Shader, tab.JavaScript, tab.Path, tab.Text});
			}
		});
		if (!sourcesUnchanged) {
			Say("a script source changed while its tab was open, so the scene was not copied",
				engine::core::LogLevel::Warning);
			return false;
		}

		std::vector<engine::game::InstanceDocumentIdentity> targetIdentities;
		const WorldId copy =
			engine::game::ReadWorldDocument(*Universe, document, wanted, error, &targetIdentities);
		if (!copy.IsValid()) {
			Say("could not copy that scene: " + error, engine::core::LogLevel::Error);
			return false;
		}
		std::unordered_map<uint32_t, engine::ecs::Entity> targetById;
		targetById.reserve(targetIdentities.size());
		for (const auto &identity : targetIdentities)
			targetById.emplace(identity.LocalId, identity.Instance);
		bool editsCopied = true;
		Universe->Enter(copy, [&](Store &store) {
			for (const ProgramState &program : programs) {
				const auto target = targetById.find(program.LocalId);
				if (target == targetById.end()) {
					editsCopied = false;
					return;
				}
				if (program.Lua)
					store.Set(target->second, *program.Lua);
				else
					store.Remove<engine::script::LuaSourceContainer>(target->second);
				if (program.Js)
					store.Set(target->second, *program.Js);
				else
					store.Remove<engine::script::JavaScriptSourceContainer>(target->second);
				if (program.Selector)
					store.Set(target->second, *program.Selector);
				else
					store.Remove<engine::script::CodeSourceContainerSelector>(target->second);
			}
			for (PendingEdit &edit : edits) {
				const auto target = targetById.find(edit.LocalId);
				if (target == targetById.end()) {
					editsCopied = false;
					break;
				}

				const bool written =
					edit.Shader ? WriteSourceDocument(
									  store, target->second, SourceDocumentKind::Shader, edit.Path, edit.Text
								  )
								: WriteProgramDocument(
									  store, target->second, edit.JavaScript, false, edit.Path, edit.Text
								  );
				if (!written) {
					editsCopied = false;
					break;
				}
			}
		});
		if (!editsCopied) {
			Universe->Destroy(copy);
			Say("could not copy the unsaved script edits", engine::core::LogLevel::Error);
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

		if (Commands == nullptr) return false;
		Commands->SetWorldRenameCoordinator([this](WorldId target, Name expected, Name desired) {
			if (Universe == nullptr || Universe->NameOf(target) != expected || AnyRunning() ||
				FactoryHost != nullptr || (Team != nullptr && Team->Edits() != nullptr))
				return false;
			for (const WorldId candidate : Universe->Worlds()) {
				if (Universe->IsRemote(candidate) || IsReplicaWorld(candidate)) return false;
			}
			std::vector<engine::world::PresentationAddress> receipts;
			if (PortalImages != nullptr && !PortalImages->RenameReceipts(target, receipts)) return false;
			if (Universe->Rename(target, desired, receipts) != engine::world::WorldStatus::Ok) return false;
			return true;
		});
		Commands->SetWorldRenameFinalizer([this](WorldId target, Name previous) {
			if (PortalImages != nullptr) PortalImages->RemoveWorld(target, previous);
			Renderer.ForgetWorld(target.Index, previous);
			InstanceCounts.clear();
		});
		if (!Commands->RenameWorld(world, wanted)) {
			ShowOutput = true;
			ShowWarnings = true;
			FocusOutput = 3;
			Say("could not rename scene while edits or world traffic are active",
				engine::core::LogLevel::Warning);
			return false;
		}
		MarkModified();

		Say("renamed '" + std::string(Label(current)) + "' to '" + std::string(Label(wanted)) + "'");
		return true;
	}
}
