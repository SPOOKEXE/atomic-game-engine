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

		universe.Enter(
			replica, [&interpolation, authorityName, label](Store &store, engine::ecs::Scheduler &systems) {
				// **Both refusals first, because the build now asks about them.**
				// Replicas cannot publish bus writes or mint authoritative entities,
				// and `BuildReplicatedWorld` opens a VM and installs `GuiService` -
				// each of which asks the store whether minting is legal. Setting the
				// flag afterwards left a window in which the answer was wrong.
				//
				// **The two names beside the flag, because a replica is named
				// apart from what it mirrors.** This world is
				// `"<authority> (client 1)"` so the registry can tell the copies
				// apart, and every string a scene authored still says
				// `"<authority>"` - a `Portal.DestinationWorld`, a teleport
				// place. `client::SurveyWorlds` reads these two, so a hole drawn
				// from inside this copy leads to this viewer's copy of the world
				// it names rather than to nothing.
				store.SetResource(engine::world::Replica{true, authorityName, Name(std::string(label))});
				store.SetAdoptOnly(true);

				// Replicas present received state and run the client's own scripts;
				// they do not simulate. The runtime is held by the scheduler, which
				// drops it with the world.
				(void)client::BuildReplicatedWorld(store, systems, interpolation);
			}
		);

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
		PlayLink *link = this;
		StepMany(universe, std::span<PlayLink *const>(&link, 1));
	}

	void PlayLink::StepMany(engine::world::Universe &universe, std::span<PlayLink *const> links) {
		ENGINE_PROFILE("play links (batched)");

		struct Pending {
			PlayLink *Link = nullptr;
			LinkReport Report;
		};
		static thread_local std::vector<Pending> pending;
		static thread_local std::vector<engine::replication::Authority::PublishRequest> requests;
		static thread_local std::vector<engine::ecs::Entity> spawned;
		pending.clear();
		requests.clear();
		pending.reserve(links.size());
		requests.reserve(links.size());

		// Inputs and respawns mutate their own worlds and stay on the universe's
		// driver thread. Each authority entry combines both operations, reducing
		// the scoped world crossings before the compute batch begins.
		{
			ENGINE_PROFILE("play links.prepare");
			for (PlayLink *link : links) {
				if (link == nullptr || !link->IsRunning()) {
					continue;
				}

				Pending &step = pending.emplace_back();
				step.Link = link;
				step.Report.TotalMessages = link->Last.TotalMessages;
				step.Report.TotalBytes = link->Last.TotalBytes;

				engine::game::MoveInput arrived;
				bool hasInput = false;
				if (link->Player_ != engine::ecs::NULL_ENTITY) {
					engine::game::MoveInput wanted;
					universe.Enter(link->Replica_, [&wanted](Store &store) {
						const engine::scene::MoveIntent intent = engine::scene::ReadMoveIntent(store);
						wanted.Direction = intent.Direction;
						wanted.Jump = intent.Jump;
						if (auto *input = store.ResourceMutable<engine::scene::InputState>();
							input != nullptr) {
							input->ConsumeTaps();
						}
					});
					hasInput = engine::game::DecodeMoveInput(engine::game::EncodeMoveInput(wanted), arrived);
				}

				universe.Enter(link->Authority_, [link, hasInput, &arrived](Store &store) {
					if (hasInput) {
						(void)engine::game::ApplyMoveInput(store, link->Player_, arrived);
					}

					spawned.clear();
					if (engine::scene::UpdateRespawns(store, spawned) == 0) {
						return;
					}
					for (const engine::ecs::Entity player : spawned) {
						(void)engine::gui::ResetPlayerGui(store, player);
					}
				});
			}
		}

		// Keep every authority store scoped until the engine's one signing
		// dispatch has joined. `Authority::PublishMany` reads the gathered column
		// runs on workers and completes each publish before this recursion unwinds.
		const auto publish = [&](auto &&self, size_t index) -> void {
			if (index == pending.size()) {
				(void)engine::replication::Authority::PublishMany(requests);
				return;
			}

			Pending &step = pending[index];
			PlayLink &link = *step.Link;
			universe.Enter(link.Authority_, [&](Store &store) {
				step.Report.Tick = store.Time().Tick;
				step.Report.ServerEntities = PosedEntities(store);
				requests.push_back({link.Server, store, step.Report.Tick});
				self(self, index + 1);
				requests.pop_back();
			});
		};
		publish(publish, 0);

		{
			ENGINE_PROFILE("play links.deliver");
			for (Pending &step : pending) {
				PlayLink &link = *step.Link;
				const std::span<const std::vector<std::byte>> messages = link.Server.Outgoing(link.Handle);
				step.Report.Messages = messages.size();

				universe.Enter(link.Replica_, [&link, &step, messages](Store &store) {
					for (const std::vector<std::byte> &message : messages) {
						step.Report.Bytes += message.size();
						step.Report.LargestMessage = std::max(step.Report.LargestMessage, message.size());
						link.Client.Receive(store, message);
					}

					step.Report.Applied = link.Client.Applied();
					step.Report.ClientEntities = PosedEntities(store);
					client::RecordReplicatedTick(store, step.Report.Applied);
				});

				const std::vector<std::byte> acknowledgement = link.Client.Acknowledge();
				if (!acknowledgement.empty()) {
					link.Server.Receive(link.Handle, acknowledgement);
				}

				step.Report.TotalMessages += step.Report.Messages;
				step.Report.TotalBytes += step.Report.Bytes;
				link.Last = step.Report;
			}
		}
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
