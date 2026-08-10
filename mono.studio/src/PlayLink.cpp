#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/replication/Defaults.hpp>
#include <engine/scene/Components.hpp>
#include <engine/world/Postbox.hpp>
#include <engine/world/Universe.hpp>

#include <algorithm>
#include <client/Replicated.hpp>
#include <string>
#include <studio/PlayLink.hpp>

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
		std::string_view label
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
			// Replicas present received state; they do not simulate.
			client::BuildReplicatedWorld(store, systems, interpolation);

			// Replicas cannot publish bus writes or mint authoritative entities.
			store.SetResource(engine::world::Replica{});
			store.SetAdoptOnly(true);
		});

		for (const engine::replication::ReplicatedComponent &component :
			 engine::replication::DefaultReplicatedComponents()) {
			Server.Replicate(Name(component.Name), component.Detection);
		}

		Handle = Server.Admit();

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
