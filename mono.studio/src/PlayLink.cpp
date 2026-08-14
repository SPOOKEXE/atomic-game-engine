#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/game/Play.hpp>
#include <engine/gui/Services.hpp>
#include <engine/replication/Defaults.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Input.hpp>
#include <engine/scene/Services.hpp>
#include <engine/world/Postbox.hpp>
#include <engine/world/Universe.hpp>

#include <algorithm>
#include <client/Replicated.hpp>
#include <string>
#include <studio/PlayLink.hpp>
#include <vector>

namespace studio {

	namespace {
		using engine::core::Name;
		using engine::ecs::Store;
		using engine::replication::ChangeDetection;

		size_t PosedEntities(Store &store) {
			size_t count = 0;
			store.Each<const engine::scene::Transform>(
				[&count](engine::ecs::Entity, const engine::scene::Transform &) { count++; }
			);
			return count;
		}
	}

	bool PlayLink::Start(
		engine::world::Universe &universe,
		engine::world::WorldId authority,
		double tickRate,
		std::string &error,
		std::string_view label,
		engine::ecs::Entity adopt
	) {
		if (IsRunning()) {
			error = "this link is already running";
			return false;
		}

		if (!authority.IsValid() || universe.IsRemote(authority)) {
			error = "there is no local world to be the authority";
			return false;
		}

		const Name authorityName = universe.NameOf(authority);

		engine::world::WorldSettings settings;
		settings.Name = Name(std::string(authorityName.Text()) + " (" + std::string(label) + ")");

		// Interpolation delay is measured against the authority's tick rate.
		settings.TickRate = tickRate;
		settings.IdleTickRate = tickRate;

		engine::world::WorldStatus status = engine::world::WorldStatus::Ok;
		const engine::world::WorldId replica = universe.Create(settings, &status);
		if (!replica.IsValid()) {
			error = "could not create the client view: " + std::string(Describe(status));
			return false;
		}

		engine::replication::InterpolationSettings interpolation;
		interpolation.TickRate = tickRate;

		universe.Enter(replica, [&interpolation](Store &store, engine::ecs::Scheduler &systems) {
			// **Both refusals first, because the build now asks about them.**
			// Replicas cannot publish bus writes or mint authoritative entities,
			// and `BuildReplicatedWorld` opens a VM and installs `GuiService` -
			// each of which asks the store whether minting is legal. Setting the
			// flag afterwards left a window in which the answer was wrong.
			store.SetResource(engine::world::Replica{});
			store.SetAdoptOnly(true);

			// Replicas present received state and run the client's own scripts;
			// they do not simulate. The runtime is held by the scheduler, which
			// drops it with the world.
			(void)client::BuildReplicatedWorld(store, systems, interpolation);
		});

		for (const engine::replication::ReplicatedComponent &component :
			 engine::replication::DefaultReplicatedComponents()) {
			Server.Replicate(Name(component.Name), component.Detection);

			// After `Replicate`, which is what declares the slot this names. The
			// editor applies the same filter as a real server, or playing in the
			// editor and playing for real would send different rows.
			if (!component.Suppressor.empty()) {
				Server.SuppressWhenTagged(Name(component.Name), Name(component.Suppressor));
			}
		}

		Handle = Server.Admit();

		// **A player and a body in the authority, and the identity in the
		// replica.** This is the studio's version of what `mono.server` does on
		// `OnAdmitted` - a client that is admitted and given nothing to be is a
		// client that watches. The two halves are the same two halves there:
		// the world gains a `Player` and a character, and the viewer is told
		// which player is its own.
		//
		// **Told by writing the resource rather than by sending
		// `game::JoinNotice`.** That message exists because a socket is the only
		// way to reach another process; here the two worlds are two stores in
		// one address space and the notice would be a byte buffer encoded and
		// decoded between two lines of the same function. What crosses a
		// *world* boundary is still nothing - this writes a resource into each
		// world while holding it, which is the same discipline every other
		// studio pass follows.
		universe.Enter(authority, [this, label, adopt](Store &store) {
			// **An adopted player is already in the world with a body.** A
			// teleport rebuilt them there from the arriving payload; admitting a
			// second would put the same person in twice, which is the one thing
			// following a teleport must not do.
			if (adopt != engine::ecs::NULL_ENTITY && store.Alive(adopt)) {
				Player_ = adopt;
				return;
			}

			if (engine::scene::PlayersOf(store) == engine::ecs::NULL_ENTITY) {
				// A world with no `Players` service gets no player, quietly.
				// `mono.server` says the same: that is the placeholder scene,
				// which is furnished by nobody.
				return;
			}

			Player_ = engine::scene::AddPlayer(store, label);
			if (Player_ != engine::ecs::NULL_ENTITY) {
				// **The interface before the body, which is `mono.server`'s
				// order and for its reason**: a `ScreenGui` a script reaches
				// for from a spawn handler has to exist by the time the handler
				// runs. `StarterGui` is a template and what a player sees is
				// their own copy - see `gui::ResetPlayerGui`.
				(void)engine::gui::ResetPlayerGui(store, Player_);
				(void)engine::scene::LoadCharacter(store, Player_);
			}
		});

		if (Player_ != engine::ecs::NULL_ENTITY) {
			universe.Enter(authority, [this](Store &store) {
				const Name held = store.InstanceNameOf(Player_);
				PlayerName_ = held.IsValid() ? std::string(held.Text()) : std::string();
			});
		}

		if (Player_ != engine::ecs::NULL_ENTITY) {
			universe.Enter(replica, [this](Store &store) {
				store.SetResource(engine::scene::LocalPlayer{Player_});
			});
		}

		Authority_ = authority;
		Replica_ = replica;
		Last = LinkReport{};

		ENGINE_INFO(
			"play: '{}' is served to '{}' in this process", authorityName.Text(), settings.Name.Text()
		);
		return true;
	}

	void PlayLink::Step(engine::world::Universe &universe) {
		if (!IsRunning()) {
			return;
		}

		ENGINE_PROFILE("play link");

		LinkReport report;
		report.TotalMessages = Last.TotalMessages;
		report.TotalBytes = Last.TotalBytes;

		// --- what the client wants, going up ---------------------------------
		//
		// **Before the publish, so a move made this frame is in the tick this
		// frame describes.** After it, every input would be one tick late and
		// the character would answer the keyboard a tick behind - invisible at
		// sixty ticks and exactly the kind of lag nobody can find by reading the
		// controller.
		//
		// **Through the real codec, even though nothing is being sent.** There
		// is no socket here - that is the whole point of this class - but
		// `game::EncodeMoveInput` and `game::DecodeMoveInput` are what a real
		// client's bytes go through, and running them costs nothing and means
		// the editor exercises the format rather than a shortcut past it. It
		// also puts the *validation* on the same side it is on for a real
		// client: the direction the authority acts on is the normalised, finite
		// one the decoder produced.
		if (Player_ != engine::ecs::NULL_ENTITY) {
			engine::game::MoveInput wanted;

			universe.Enter(Replica_, [&wanted](Store &store) {
				const engine::scene::MoveIntent intent = engine::scene::ReadMoveIntent(store);

				// **Both halves out of one read now.** The direction is a hold
				// and reads correctly from whatever frame this lands on; the
				// jump is an edge and used to be smuggled past this call in
				// `PendingJump` because a frame-shaped edge does not survive to
				// a tick. `InputState::Pressed` holds it instead, so
				// `ReadMoveIntent` answers for the whole keyboard.
				wanted.Direction = intent.Direction;
				wanted.Jump = intent.Jump;

				// **Consumed here, and only here.** This is the one reader of
				// the replica's taps per tick, so clearing them is this
				// function's job - a tap left latched would jump again on the
				// next tick and a tap cleared by a second reader would not jump
				// at all.
				if (auto *input = store.ResourceMutable<engine::scene::InputState>(); input != nullptr) {
					input->ConsumeTaps();
				}
			});

			engine::game::MoveInput arrived;
			if (engine::game::DecodeMoveInput(engine::game::EncodeMoveInput(wanted), arrived)) {
				universe.Enter(Authority_, [this, &arrived](Store &store) {
					(void)engine::game::ApplyMoveInput(store, Player_, arrived);
				});
			}
		}

		// **The respawn loop, and it is here rather than in a scheduler for the
		// one reason that decides where it can go: only the authority may run
		// it.** The editor registers its character systems through
		// `client::InstallPresentation`, which runs on *both* of this link's
		// worlds - and a replica handing itself a new body would be minting an
		// entity the server never issued. This function is the one place that
		// knows which of the two is the authority.
		//
		// The interface half is the caller's, exactly as it is in `mono.server`:
		// `scene::UpdateRespawns` hands back who it spawned and
		// `gui::ResetPlayerGui` is what gives each of them their own copy of
		// `StarterGui`.
		universe.Enter(Authority_, [](Store &store) {
			std::vector<engine::ecs::Entity> spawned;
			if (engine::scene::UpdateRespawns(store, spawned) == 0) {
				return;
			}
			for (const engine::ecs::Entity player : spawned) {
				(void)engine::gui::ResetPlayerGui(store, player);
			}
		});

		// Publish while the authority store is scoped; its outgoing span is borrowed.
		universe.Enter(Authority_, [this, &report](Store &store) {
			report.Tick = store.Time().Tick;
			report.ServerEntities = PosedEntities(store);

			Server.Publish(store, report.Tick);
		});

		const std::span<const std::vector<std::byte>> messages = Server.Outgoing(Handle);
		report.Messages = messages.size();

		// Direct handoff isolates replication from transport failures.
		universe.Enter(Replica_, [this, &report, messages](Store &store) {
			for (const std::vector<std::byte> &message : messages) {
				report.Bytes += message.size();
				report.LargestMessage = std::max(report.LargestMessage, message.size());

				Client.Receive(store, message);
			}

			report.Applied = Client.Applied();
			report.ClientEntities = PosedEntities(store);

			// Record on receipt, before a render-rate pass can miss the tick.
			client::RecordReplicatedTick(store, report.Applied);
		});

		const std::vector<std::byte> acknowledgement = Client.Acknowledge();
		if (!acknowledgement.empty()) {
			Server.Receive(Handle, acknowledgement);
		}

		report.TotalMessages += report.Messages;
		report.TotalBytes += report.Bytes;
		Last = report;
	}

	void PlayLink::Stop(engine::world::Universe &universe) {
		if (!IsRunning()) {
			return;
		}

		// **The player goes before the world it is a player of.** Destroying the
		// instance is what makes `scene.ownership`'s reclaim fire - a body owned
		// by an entity that is no longer alive is a body nothing will ever
		// simulate again - and it takes the character with it, because
		// `RemoveCharacter` is what `DestroyInstance` on a `Player` cannot do
		// for itself.
		if (Player_ != engine::ecs::NULL_ENTITY && Authority_.IsValid()) {
			const engine::ecs::Entity player = Player_;
			universe.Enter(Authority_, [player](Store &store) {
				(void)engine::scene::RemoveCharacter(store, player);
				store.DestroyInstance(player);
			});
		}
		Player_ = engine::ecs::Entity{};

		// Clear handles before destroying the world; other editor systems inspect them.
		const engine::world::WorldId replica = Replica_;
		Replica_ = engine::world::WorldId{};
		Authority_ = engine::world::WorldId{};
		Last = LinkReport{};

		universe.Destroy(replica);

		Server = engine::replication::Authority{};
		Client = engine::replication::Replica{};
		Handle = engine::replication::ClientId{};
	}
}
