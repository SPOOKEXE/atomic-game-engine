// Players in the editor's own process, and the viewport you drive one from.
//
// **The studio hosts both halves natively — there is no second program.**
// `PlayLink` has held an `Authority` over the world being played and a `Replica`
// of it in a second world since v0.7, which is a whole client short of a client:
// the replica received state and nobody was in it. This file is the missing
// half. A link now admits a `Player`, gives it a blocky character, and tells the
// replica which player it is; a viewport pinned to that replica looks through
// the camera behind that character and feeds it the keyboard.
//
// **Run and Play stop being two features and become a count.** `RunMode::Server`
// is a run with no clients and `RunMode::Play` is a run with one; Spawn Player
// adds one to either, so a Run becomes a Play by pressing a button, and a Play
// becomes the two-client arrangement that is the only way to see one client
// disagree with another.
//
// What is deliberately *not* here: any second way to stand a client up.
// `BeginRun` calls `SpawnPlayer` for the clients Play asks for, so there is one
// path that admits somebody and one place that can be wrong about it.

#include <engine/core/Log.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Input.hpp>
#include <engine/scene/Services.hpp>
#include <engine/world/Universe.hpp>

#include <algorithm>
#include <imgui.h>
#include <string>
#include <studio/Editor.hpp>
#include <studio/PlayLink.hpp>

namespace studio {

	namespace {
		using engine::ecs::Entity;
		using engine::ecs::NULL_ENTITY;
		using engine::ecs::Store;
		using engine::scene::InputState;
		using engine::scene::KeyCode;
		using engine::scene::MouseButton;

		// The keys a character controller reads, and no others.
		//
		// **A table rather than a run of `if`s**, because the interesting
		// property is that this list and `scene::ReadMoveIntent`'s list are the
		// same list — a key offered here that nothing reads is dead, and one
		// read there and missing here is a key that works in a real client and
		// not in the studio. Short enough to compare by eye, which is the point.
		constexpr struct {
			ImGuiKey From;
			KeyCode To;
		} PLAYED_KEYS[] = {
			{ImGuiKey_W, KeyCode::W},
			{ImGuiKey_A, KeyCode::A},
			{ImGuiKey_S, KeyCode::S},
			{ImGuiKey_D, KeyCode::D},
			{ImGuiKey_UpArrow, KeyCode::Up},
			{ImGuiKey_DownArrow, KeyCode::Down},
			{ImGuiKey_LeftArrow, KeyCode::Left},
			{ImGuiKey_RightArrow, KeyCode::Right},
			{ImGuiKey_Space, KeyCode::Space},
			{ImGuiKey_LeftShift, KeyCode::LeftShift},
		};

		// How long a client may be in no world at all before it is given up on.
		//
		// **A second at sixty frames, against a window that is two ticks wide.**
		// The generous figure is for the case the tight one gets wrong: a paused
		// destination world does not pump its inbox, so an author who paused the
		// far side while somebody was crossing would watch the client be
		// destroyed for it.
		constexpr int LOST_FRAMES = 60;

		// Whether this world has somebody in it to drive.
		//
		// A replica that has been told which player it is *and* has received
		// that player's character. Both halves, because the two arrive on
		// different ticks: the identity is written when the link starts and the
		// body comes over the wire, so a viewport that took the keyboard on the
		// first alone would swallow WASD for a frame with nothing to walk.
		bool HasBody(Store &store) {
			const auto *local = store.Resource<engine::scene::LocalPlayer>();
			return local != nullptr && engine::scene::CharacterOf(store, local->Instance) != NULL_ENTITY;
		}
	}

	bool Editor::DrivePlayer(WorldId world, bool hovered, bool active, bool focused) {
		if (!world.IsValid() || !IsReplicaWorld(world)) {
			return false;
		}

		ImGuiIO &io = ImGui::GetIO();

		// **`WantTextInput` and not `WantCaptureKeyboard`**, which is the same
		// correction `DriveCameraFor` carries at length: hovering an imgui
		// window raises the capture flags, so a guard on those is a guard that
		// is true exactly when the viewport is the thing being used. Typing in
		// the script editor must not walk a character; clicking the picture must
		// not stop it.
		const bool driving = (hovered || active || focused) && !io.WantTextInput;

		bool drove = false;

		Universe->Enter(world, [&](Store &store) {
			auto *input = store.ResourceMutable<InputState>();
			if (input == nullptr || !HasBody(store)) {
				return;
			}

			// **Last frame's keys before this frame's**, because the difference
			// between the two *is* `WasKeyPressed`, and the jump is an edge.
			// Copying after would make every frame look like the first.
			input->Previous = input->Down;
			input->PreviousButtons = input->Buttons;

			// Rolled with the two above because it is read the same way — the
			// device change is an edge, and `input::Translator::BeginFrame` rolls
			// it there for this reason. This panel is that translator for a played
			// world.
			input->PreviousLastSource = input->LastSource;

			input->Down = {};
			input->Buttons = 0;
			input->MouseDelta = {};
			input->WheelDelta = 0.0f;

			// **`Focused` means "this panel has the input", not "the window
			// does".** A studio with two client views must have at most one of
			// them walking, and the panel under the pointer is the one somebody
			// means — `scene::UpdateCharacterControl` reads this flag and stops
			// the character dead when it is false, which is exactly the wanted
			// behaviour for the other panel.
			input->Focused = driving;

			if (!driving) {
				return;
			}

			for (const auto &key : PLAYED_KEYS) {
				if (ImGui::IsKeyDown(key.From)) {
					input->Down.Set(key.To, true);
					input->LastSource = engine::scene::InputSource::Keyboard;
				}
			}

			// Turning needs the right button held, which is `scene::
			// UpdateCameraControl`'s rule and not this file's — it is repeated
			// here only in the sense that the button state is forwarded.
			if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
				input->Buttons = static_cast<uint8_t>(1u << static_cast<uint8_t>(MouseButton::Right));
				input->MouseDelta = {io.MouseDelta.x, io.MouseDelta.y};
				input->LastSource = engine::scene::InputSource::MouseButton2;
			}

			input->WheelDelta = io.MouseWheel;
			if (io.MouseWheel != 0.0f) {
				input->LastSource = engine::scene::InputSource::MouseWheel;
			}

			// **The press edges, kept for the tick that will read them.**
			// `PlayLink::Step` runs once per tick and frames outnumber ticks, so
			// a space bar tapped between two ticks lands on a frame no tick ever
			// looks at. This used to be answered here, by reaching past
			// `InputState` for imgui's own edge and calling `PlayLink::Jump` on
			// the link that happened to own this world — a second input path
			// that only jump travelled, wired by hand to one key.
			//
			// `InputState::Pressed` is that latch in the one place both hosts
			// share, so jump is now an ordinary key: it goes into `Down` with W,
			// A, S and D above and leaves through `ReadMoveIntent` with them.
			input->LatchPresses();
			drove = true;
		});

		return drove;
	}

	Editor::WorldRun *Editor::RunOwning(WorldId world) {
		if (!world.IsValid()) {
			return nullptr;
		}

		if (WorldRun *direct = RunOf(world); direct != nullptr) {
			return direct;
		}

		// A replica names no run of its own — it *is* part of one. Found by
		// asking each run's links rather than by keeping a second map, which
		// would be a copy of the same fact and would go stale on Stop.
		for (WorldRun &run : Runs) {
			for (const std::unique_ptr<PlayLink> &link : run.Links) {
				if (link != nullptr && link->ReplicaWorld() == world) {
					return &run;
				}
			}
		}

		return nullptr;
	}

	bool Editor::SpawnPlayer(WorldId world) {
		WorldRun *run = RunOwning(world);
		if (run == nullptr) {
			Say("nothing is running here — press Play or Run first", engine::core::LogLevel::Warning);
			return false;
		}

		auto link = std::make_unique<PlayLink>();

		// Numbered by how many there already are, so the world's name stays the
		// identity it has to be — rule 4. Two clients that took one name would
		// be two worlds nothing could tell apart, in a panel or in a recording.
		const std::string label = "client " + std::to_string(run->Links.size() + 1);

		std::string error;
		if (!link->Start(*Universe, run->World, Settings.TickRate, error, label)) {
			Say("no client view: " + error, engine::core::LogLevel::Error);
			return false;
		}

		const WorldId replica = link->ReplicaWorld();

		// **A panel to see it from, or the client is a log line.** The first
		// free one; a viewport somebody has already pointed at another scene is
		// left alone, because taking it would be the editor rearranging their
		// layout for them.
		bool shown = false;
		for (ViewportState &view : Extras) {
			if (view.World.IsValid() && view.World != replica) {
				continue;
			}

			view.World = replica;
			view.Open = true;

			// Where the main camera is, so a client view that has not received
			// its character yet opens looking at the same thing rather than at
			// the origin.
			view.Frame = CameraFrame;
			view.Yaw = CameraYaw;
			view.Pitch = CameraPitch;
			shown = true;
			break;
		}

		// **Said once per run rather than once per client.** `--run play` starts
		// every world in a game, so with more clients than panels this fired
		// once a world at startup — five lines about a layout nobody had chosen
		// yet. It is worth saying when a run has no client panel at all, because
		// then the client really is invisible; it is noise when the run already
		// has one and this is the second.
		if (!shown) {
			const bool anyShown = std::any_of(
				run->Links.begin(), run->Links.end(), [this](const std::unique_ptr<PlayLink> &other) {
					if (other == nullptr || !other->IsRunning()) {
						return false;
					}
					return std::any_of(Extras.begin(), Extras.end(), [&other](const ViewportState &view) {
						return view.Open && view.World == other->ReplicaWorld();
					});
				}
			);

			if (!anyShown) {
				Say(label + " has no free viewport — pick it in a panel's scene selector");
			}
		}

		// **The run's recorded mode follows the count, and the log says what
		// that does not change.** A Server run started its scripts with
		// `HostRole::OfServer`, so its `LocalScript`s did not run and adding a
		// client does not make them: what a person gets is a real replica of a
		// dedicated server, which is a useful thing and is not the same thing as
		// Play. Saying so beats a mode label that quietly means two things.
		if (run->Mode == RunMode::Server) {
			run->Mode = RunMode::Play;
			Say("this scene was started as a server, so its LocalScripts are not running — "
				"Stop and press Play to run them");
		}

		run->Links.push_back(std::move(link));
		Say(label + " joined");
		SyncWorldStates();
		return true;
	}

	bool Editor::RemovePlayer(WorldId world) {
		WorldRun *run = RunOwning(world);
		if (run == nullptr || run->Links.empty()) {
			Say("there is no client here to remove", engine::core::LogLevel::Warning);
			return false;
		}

		// **The one being looked at, when one is.** Pressing this while looking
		// at a client view means "not this one"; while looking at the server's
		// view it means "one fewer", and the last to arrive is the one that
		// goes — which is the order somebody expects from a button that adds to
		// the end.
		auto chosen = run->Links.end();
		for (auto at = run->Links.begin(); at != run->Links.end(); ++at) {
			if (*at != nullptr && (*at)->ReplicaWorld() == world) {
				chosen = at;
				break;
			}
		}
		if (chosen == run->Links.end()) {
			chosen = std::prev(run->Links.end());
		}

		const WorldId replica = (*chosen)->ReplicaWorld();

		// **Unpinned before the world under it goes.** A pin naming a destroyed
		// world leaves the panel following the active scene with no way to tell
		// that it stopped showing what it was opened for — the same order
		// `EndRun` takes, for the same reason.
		for (ViewportState &view : Extras) {
			if (view.World == replica) {
				view.World = WorldId{};
				view.Follow = NULL_ENTITY;
			}
		}

		(*chosen)->Stop(*Universe);
		run->Links.erase(chosen);

		Say("a client left");
		SyncWorldStates();
		return true;
	}

	bool Editor::Claimed(WorldId world, Entity player) const {
		for (const WorldRun &run : Runs) {
			for (const std::unique_ptr<PlayLink> &link : run.Links) {
				if (link == nullptr || !link->IsRunning()) {
					continue;
				}

				// **The link's world, not the run's, and the difference is a
				// teleport.** A `WorldRun` names the scene an author pressed
				// Play on and keeps that name for its whole life; a `PlayLink`
				// inside it re-homes every time its client walks through a
				// portal — `FollowTeleports` stops the old one and starts a new
				// one against the destination, in the same run. So after one
				// crossing the two disagree, and the run's world is the answer
				// to a question nobody asked.
				//
				// **Filtering by the run's world instead compared entity handles
				// across a world boundary**, which is the part that made it a
				// bug rather than an inefficiency. An `ecs::Entity` is an index
				// and a generation *within one store*: two worlds allocate
				// independently, so a player in one and a player in another
				// routinely have the same number — and two worlds built by the
				// same script in the same order have it near enough always.
				//
				// The failure was exactly asymmetric, which is what it looks
				// like when a guard tests the wrong world. Walking from a run's
				// own scene to another one worked: no run named the destination,
				// so the loop skipped everything and the arrival was accepted.
				// Walking *back* did not: the run did name that world, its link
				// was tested, and the far world's player id matched the arriving
				// one — so the arrival read as already claimed, no destination
				// was found, and the client was dropped as lost after
				// `LOST_FRAMES`. The character stood there with nobody in it.
				if (link->AuthorityWorld() != world) {
					continue;
				}

				if (link->Player() == player) {
					return true;
				}
			}
		}
		return false;
	}

	void Editor::FollowTeleports() {
		for (WorldRun &run : Runs) {
			for (std::unique_ptr<PlayLink> &link : run.Links) {
				if (link == nullptr || !link->IsRunning() || link->PlayerName().empty()) {
					continue;
				}

				// **Still here is the ordinary answer and it costs one lookup.**
				// A teleport is rare and this runs every frame per client, so
				// the fast path has to be a single aliveness check.
				// **The link's own world, not the run's.** They are the same
				// until the first teleport is followed and different after it —
				// asking the run's world would answer "gone" for ever and
				// follow the same teleport on every frame.
				const WorldId living = link->AuthorityWorld();

				bool gone = false;
				Universe->Enter(living, [&](Store &store) { gone = !store.Alive(link->Player()); });
				if (!gone) {
					link->Missing() = 0;
					continue;
				}

				link->Missing()++;

				const std::string name = link->PlayerName();

				// Where they went. **Searched rather than told**, because the
				// destination is decided by a script in another world and
				// nothing may carry a handle out of it — what arrived there is a
				// `Player` with this name, built from that world's own classes.
				//
				// **And a name alone is not enough, which cost a wrong world to
				// find out.** Every run names its clients the same way, so a
				// universe with a client in each of seven worlds has seven
				// players called "client 1" — and the first one this walk met
				// was whichever world was created first, never the one the
				// teleport named. So a candidate that some *other* link is
				// already living in is not an arrival; it is somebody else with
				// the same name.
				WorldId destination;
				Entity landed;
				for (const WorldId candidate : Universe->Worlds()) {
					if (candidate == living || IsReplicaWorld(candidate) || Universe->IsRemote(candidate)) {
						continue;
					}

					Universe->Enter(candidate, [&](Store &store) {
						const Entity players = engine::scene::PlayersOf(store);
						if (players == engine::ecs::NULL_ENTITY) {
							return;
						}

						// **Every child of that name, not the first.** The
						// destination has its own client called the same thing,
						// and `FindFirstChild` returns whichever was parented
						// first — which is the resident, never the arrival. The
						// one nobody is living in is the one who has just
						// walked in.
						Entity unclaimed;
						store.EachChild(players, [&](Entity child) {
							if (unclaimed != engine::ecs::NULL_ENTITY) {
								return;
							}
							if (store.InstanceNameOf(child) != engine::core::Name(name)) {
								return;
							}
							if (!Claimed(candidate, child)) {
								unclaimed = child;
							}
						});

						if (unclaimed == engine::ecs::NULL_ENTITY) {
							return;
						}

						destination = candidate;
						landed = unclaimed;
					});

					if (destination.IsValid()) {
						break;
					}
				}

				const WorldId oldReplica = link->ReplicaWorld();

				if (!destination.IsValid()) {
					// **Not yet is the ordinary answer for a frame or two.** A
					// teleport is routed at one tick's barrier and admitted when
					// the destination's heartbeat next pumps its inbox, so the
					// player is in neither world for at least one tick —
					// `PlayLink::Missing` carries the argument. Giving up here
					// killed every teleport this followed.
					if (link->Missing() < LOST_FRAMES) {
						continue;
					}

					// They left and arrived nowhere this editor can see —
					// a teleport naming a world that does not exist, which the
					// router answers with `NoSuchWorld`. The client has nobody
					// to be, so it goes rather than watching a world it is not
					// in.
					Say(name + " teleported out of every scene here — that client is gone");
					for (ViewportState &view : Extras) {
						if (view.World == oldReplica) {
							view.World = WorldId{};
							view.Follow = NULL_ENTITY;
						}
					}
					link->Stop(*Universe);
					link.reset();
					continue;
				}

				// **Stopped before the new one starts, and the old player is
				// already gone** — so `PlayLink::Stop`'s own destroy finds
				// nothing to destroy, which is exactly right: the teleport did
				// it, in the world that was allowed to.
				link->Stop(*Universe);

				auto moved = std::make_unique<PlayLink>();
				std::string error;
				if (!moved->Start(*Universe, destination, Settings.TickRate, error, name, landed)) {
					Say("could not follow " + name + ": " + error, engine::core::LogLevel::Error);
					link.reset();
					continue;
				}

				const WorldId replica = moved->ReplicaWorld();

				// The panel that was showing them follows too, or the author
				// watches an empty room and the player is somewhere off screen.
				for (ViewportState &view : Extras) {
					if (view.World == oldReplica) {
						view.World = replica;
						view.Follow = NULL_ENTITY;
					}
				}

				link = std::move(moved);
				Say(name + " arrived in '" + std::string(Label(Universe->NameOf(destination))) + "'");
			}

			// **The links that gave up, dropped after the walk rather than
			// during it.** Erasing from a vector being iterated is the bug this
			// avoids by construction, and a null in the list is already a case
			// every reader here handles.
			run.Links.erase(
				std::remove_if(
					run.Links.begin(),
					run.Links.end(),
					[](const std::unique_ptr<PlayLink> &link) { return link == nullptr; }
				),
				run.Links.end()
			);
		}
	}

}
