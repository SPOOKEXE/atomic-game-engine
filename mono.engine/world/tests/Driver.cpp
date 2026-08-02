// One barrier over worlds here and worlds elsewhere.
//
// The claim under test throughout: **a world's bus behaviour does not depend on
// which process holds it.** Every case is built so that the same thing is done
// twice — once entirely locally, once with one side in a host — and the two are
// required to agree.
//
// No process is spawned. The hosts are local channels standing in for one,
// because what a real spawn adds is tested where it belongs: `parallel` proves
// the socket, and `mono.server` proves the exec. What is left here is the
// routing, and the routing is the same either way by construction.

#include <engine/core/Name.hpp>
#include <engine/core/Random.hpp>
#include <engine/parallel/Channel.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Driver.hpp>
#include <engine/world/HostLink.hpp>
#include <engine/world/Postbox.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.world.driver")

using engine::core::Name;
using engine::core::Random;
using engine::ecs::Store;
using engine::parallel::MakeLocalChannel;
using engine::world::BusKind;
using engine::world::Delivery;
using engine::world::Driver;
using engine::world::DriverSettings;
using engine::world::Envelope;
using engine::world::HostFrame;
using engine::world::HostLink;
using engine::world::HostPlan;
using engine::world::HostSignal;
using engine::world::Postbox;
using engine::world::WorldId;
using engine::world::WorldSettings;
using engine::world::WorldState;

namespace driver_test {
	std::vector<std::byte> Bytes(std::string_view text) {
		std::vector<std::byte> payload(text.size());
		if (!text.empty()) {
			std::memcpy(payload.data(), text.data(), text.size());
		}
		return payload;
	}

	std::string Text(std::span<const std::byte> payload) {
		return std::string(reinterpret_cast<const char *>(payload.data()), payload.size());
	}

	WorldSettings Named(const char *name) {
		WorldSettings settings;
		settings.Name = Name(name);
		settings.TickRate = 60.0;
		return settings;
	}

	// A driver whose hosts are local channels rather than processes.
	//
	// The launcher succeeds without spawning, and a link is attached by hand —
	// which is exactly the seam `Supervisor::Attach` exists for, and the reason
	// the transport is injectable at all.
	struct Rig {
		explicit Rig(const std::vector<WorldSettings> &remote, uint32_t perHost = 8) {
			DriverSettings settings;
			settings.Hosts.WorldsPerHost = perHost;
			settings.Hosts.HeartbeatSeconds = 0.0; // deadlines are tested elsewhere
			settings.Hosts.RestartLimit = 0;

			Machine = std::make_unique<Driver>(settings);
			Machine->Hosts().SetLauncher([](const HostPlan &, engine::parallel::Process &) { return true; });

			REQUIRE(Machine->Start(remote) > 0);

			for (const auto &status : Machine->Hosts().Hosts()) {
				auto [driverEnd, hostEnd] = MakeLocalChannel();
				REQUIRE(Machine->Hosts().Attach(status.Name, std::move(driverEnd)));
				Links.emplace(status.Name, std::make_unique<HostLink>(std::move(hostEnd), status.Name));
			}
		}

		HostLink &Link(const char *host) {
			const auto found = Links.find(Name(host));
			REQUIRE(found != Links.end());
			return *found->second;
		}

		// The host holding a world, which is `PlanHosts`' decision rather than
		// this test's.
		Name HostFor(const char *world) {
			return Machine->Worlds().HostOf(Machine->Worlds().Find(Name(world)));
		}

		// Posts an envelope as though a host's world had.
		void PostAs(const char *host, const char *world, const Envelope &envelope) {
			Envelope stamped = envelope;
			stamped.From = Name(world);
			stamped.Sequence = ++Sequences[Name(world)];

			const std::vector<Envelope> one{stamped};
			REQUIRE(Link(host).SendTraffic(one));
		}

		// Everything the driver sent to one host since it was last drained.
		std::vector<HostFrame> Drain(const char *host) {
			std::vector<HostFrame> frames;
			Link(host).Receive(frames);
			return frames;
		}

		std::unique_ptr<Driver> Machine;
		std::map<Name, std::unique_ptr<HostLink>> Links;
		std::map<Name, uint64_t> Sequences;
	};

	Envelope Publish(const char *topic, const char *payload) {
		Envelope envelope;
		envelope.Bus = BusKind::Messaging;
		envelope.Operation = engine::world::BusOperation::Publish;
		envelope.Key = Name(topic);
		envelope.Payload = Bytes(payload);
		return envelope;
	}

	Envelope Subscribe(const char *topic) {
		Envelope envelope;
		envelope.Bus = BusKind::Messaging;
		envelope.Operation = engine::world::BusOperation::Subscribe;
		envelope.Key = Name(topic);
		return envelope;
	}

	Envelope Set(BusKind bus, const char *key, const char *value) {
		Envelope envelope;
		envelope.Bus = bus;
		envelope.Operation = engine::world::BusOperation::Set;
		envelope.Key = Name(key);
		envelope.Payload = Bytes(value);
		return envelope;
	}

	Envelope Get(BusKind bus, const char *key, uint64_t ticket) {
		Envelope envelope;
		envelope.Bus = bus;
		envelope.Operation = engine::world::BusOperation::Get;
		envelope.Key = Name(key);
		envelope.Reply.Value = ticket;
		return envelope;
	}

	// Every delivery in a batch of frames, flattened.
	std::vector<engine::world::HostDelivery> Deliveries(const std::vector<HostFrame> &frames) {
		std::vector<engine::world::HostDelivery> found;
		for (const HostFrame &frame : frames) {
			if (frame.Signal == HostSignal::Deliveries) {
				found.insert(found.end(), frame.Deliveries.begin(), frame.Deliveries.end());
			}
		}
		return found;
	}
}

using namespace driver_test;

TEST_CASE("a world given to a host is in the directory from the first barrier", "[world]") {
	// Registered at `Start`, not at whenever the host answers. A subscription
	// or a teleport arriving in between has somewhere to go.
	Rig rig({Named("lobby"), Named("arena")});

	const WorldId lobby = rig.Machine->Worlds().Find(Name("lobby"));
	REQUIRE(lobby.IsValid());
	REQUIRE(rig.Machine->Worlds().IsRemote(lobby));
	REQUIRE(rig.Machine->Worlds().StateOf(lobby) == WorldState::Remote);
	REQUIRE(rig.Machine->Worlds().HostOf(lobby).IsValid());
}

TEST_CASE("a remote world is never ticked here", "[world]") {
	Rig rig({Named("lobby")});
	const WorldId lobby = rig.Machine->Worlds().Find(Name("lobby"));

	for (int tick = 0; tick < 30; tick++) {
		rig.Machine->Tick(1.0f / 60.0f, static_cast<double>(tick));
	}

	REQUIRE(rig.Machine->Worlds().StatisticsOf(lobby).Ticks == 0);
	REQUIRE(rig.Machine->Worlds().Statistics().Remote == 1);
}

TEST_CASE("a remote world cannot be presented here", "[world]") {
	// It draws in its own host or not at all. Presenting it would run PreRender
	// against an empty store, which is a frame of nothing rather than an error.
	Rig rig({Named("lobby")});
	const WorldId lobby = rig.Machine->Worlds().Find(Name("lobby"));

	REQUIRE(rig.Machine->Present(lobby, 1.0f / 60.0f, 0.0f) == engine::world::WorldStatus::NoSuchWorld);
}

TEST_CASE("a local world hears a remote world's publish", "[world]") {
	Rig rig({Named("remote.speaker")});

	const WorldId listener = rig.Machine->Worlds().Create(Named("local.listener"));
	rig.Machine->Worlds().Enter(listener, [](Store &store) { Postbox(store).Subscribe("news"); });
	rig.Machine->Tick(1.0f / 60.0f, 0.0);

	const Name host = rig.HostFor("remote.speaker");
	rig.PostAs(host.Text().data(), "remote.speaker", Publish("news", "from over there"));

	rig.Machine->Tick(1.0f / 60.0f, 1.0);
	REQUIRE(rig.Machine->Statistics().TrafficAccepted == 1);
	REQUIRE(rig.Machine->Statistics().TrafficRefused == 0);

	std::string heard;
	rig.Machine->Worlds().Enter(listener, [&heard](Store &store) {
		for (const Delivery &delivery : Postbox(store).Deliveries()) {
			heard += Text(delivery.Payload);
		}
	});
	REQUIRE(heard == "from over there");
}

TEST_CASE("a remote world hears a local world's publish", "[world]") {
	// The return path, which is the half that needs the driver to send
	// deliveries out rather than into a store it does not have.
	Rig rig({Named("remote.listener")});
	const Name host = rig.HostFor("remote.listener");

	const WorldId speaker = rig.Machine->Worlds().Create(Named("local.speaker"));

	rig.PostAs(host.Text().data(), "remote.listener", Subscribe("news"));
	rig.Machine->Tick(1.0f / 60.0f, 0.0);

	rig.Machine->Worlds().Enter(speaker, [](Store &store) {
		Postbox(store).Publish("news", Bytes("from over here"));
	});
	rig.Machine->Tick(1.0f / 60.0f, 1.0);

	REQUIRE(rig.Machine->Statistics().DeliveriesSent == 1);
	REQUIRE(rig.Machine->Statistics().DeliveriesDropped == 0);

	const auto deliveries = Deliveries(rig.Drain(host.Text().data()));
	REQUIRE(deliveries.size() == 1);
	REQUIRE(deliveries[0].World == Name("remote.listener"));
	REQUIRE(Text(deliveries[0].Message.Payload) == "from over here");
	REQUIRE(deliveries[0].Message.From == Name("local.speaker"));
}

TEST_CASE("two remote worlds in different hosts hear each other", "[world]") {
	// Neither one is local, so every hop crosses the driver: two links in, one
	// router, two links out.
	WorldSettings alone = Named("solo");
	alone.IsolationLevel = engine::world::Isolation::Dedicated;

	Rig rig({Named("shared.one"), alone}, 1);

	const Name first = rig.HostFor("shared.one");
	const Name second = rig.HostFor("solo");
	REQUIRE(first != second);

	rig.PostAs(second.Text().data(), "solo", Subscribe("cross"));
	rig.Machine->Tick(1.0f / 60.0f, 0.0);

	rig.PostAs(first.Text().data(), "shared.one", Publish("cross", "hello"));
	rig.Machine->Tick(1.0f / 60.0f, 1.0);

	const auto deliveries = Deliveries(rig.Drain(second.Text().data()));
	REQUIRE(deliveries.size() == 1);
	REQUIRE(deliveries[0].World == Name("solo"));
	REQUIRE(Text(deliveries[0].Message.Payload) == "hello");

	// And not back to the publisher, which is the same rule a local publish
	// follows.
	REQUIRE(Deliveries(rig.Drain(first.Text().data())).empty());
}

TEST_CASE("a reply owed to a remote world reaches it", "[world]") {
	Rig rig({Named("asker")});
	const Name host = rig.HostFor("asker");

	rig.PostAs(host.Text().data(), "asker", Set(BusKind::MemoryStore, "key", "value"));
	rig.Machine->Tick(1.0f / 60.0f, 0.0);
	rig.Drain(host.Text().data());

	rig.PostAs(host.Text().data(), "asker", Get(BusKind::MemoryStore, "key", 77));
	rig.Machine->Tick(1.0f / 60.0f, 1.0);

	const auto deliveries = Deliveries(rig.Drain(host.Text().data()));
	REQUIRE(deliveries.size() == 1);
	REQUIRE(deliveries[0].Message.Reply.Value == 77);
	REQUIRE(Text(deliveries[0].Message.Payload) == "value");
}

TEST_CASE("a remote world and a local world share one bus", "[world]") {
	// The point of routing everything through the driver: two answers to one
	// DataStore key is the thing that cannot be allowed, and a host owning its
	// own backend is how it would happen.
	Rig rig({Named("remote.writer")});
	const Name host = rig.HostFor("remote.writer");

	const WorldId reader = rig.Machine->Worlds().Create(Named("local.reader"));

	rig.PostAs(
		host.Text().data(), "remote.writer", Set(BusKind::DataStore, "shared.key", "written remotely")
	);
	rig.Machine->Tick(1.0f / 60.0f, 0.0);

	std::vector<std::byte> value;
	REQUIRE(
		rig.Machine->Worlds().Peek(BusKind::DataStore, Name("shared.key"), &value) ==
		engine::world::BusStatus::Ok
	);
	REQUIRE(Text(value) == "written remotely");

	// And the local world reads back what the remote one wrote.
	rig.Machine->Worlds().Enter(reader, [](Store &store) {
		Postbox(store).Get(BusKind::DataStore, "shared.key");
	});
	rig.Machine->Tick(1.0f / 60.0f, 1.0);

	std::string got;
	rig.Machine->Worlds().Enter(reader, [&got](Store &store) {
		for (const Delivery &delivery : Postbox(store).Deliveries()) {
			got += Text(delivery.Payload);
		}
	});
	REQUIRE(got == "written remotely");
}

TEST_CASE("a host claiming to be a world it does not hold is refused", "[world]") {
	// `Envelope::From` is stamped rather than trusted, carried across a process
	// boundary. A host that could claim to be a neighbour's world could read
	// that world's replies and publish in its name.
	WorldSettings alone = Named("victim");
	alone.IsolationLevel = engine::world::Isolation::Dedicated;

	Rig rig({Named("attacker"), alone}, 1);

	const Name attackerHost = rig.HostFor("attacker");
	const Name victimHost = rig.HostFor("victim");
	REQUIRE(attackerHost != victimHost);

	// The victim subscribes, honestly.
	rig.PostAs(victimHost.Text().data(), "victim", Subscribe("private"));
	rig.Machine->Tick(1.0f / 60.0f, 0.0);
	rig.Drain(victimHost.Text().data());

	// The attacker's host posts as the victim's world.
	rig.PostAs(attackerHost.Text().data(), "victim", Publish("private", "forged"));
	rig.Machine->Tick(1.0f / 60.0f, 1.0);

	REQUIRE(rig.Machine->Statistics().TrafficRefused == 1);
	REQUIRE(rig.Machine->Statistics().TrafficAccepted == 0);
	REQUIRE(Deliveries(rig.Drain(victimHost.Text().data())).empty());
}

TEST_CASE("a host posting as a world nobody holds is refused", "[world]") {
	Rig rig({Named("lobby")});
	const Name host = rig.HostFor("lobby");

	rig.PostAs(host.Text().data(), "does.not.exist", Publish("news", "nothing"));
	rig.Machine->Tick(1.0f / 60.0f, 0.0);

	REQUIRE(rig.Machine->Statistics().TrafficRefused == 1);
	REQUIRE(rig.Machine->Statistics().TrafficAccepted == 0);
}

TEST_CASE("a host posting as a local world is refused", "[world]") {
	// A local world's traffic comes from its own outbox. A host that could
	// supply it would be able to act as a world running in the driver.
	Rig rig({Named("remote.one")});
	const Name host = rig.HostFor("remote.one");

	rig.Machine->Worlds().Create(Named("local.one"));

	rig.PostAs(host.Text().data(), "local.one", Publish("news", "forged"));
	rig.Machine->Tick(1.0f / 60.0f, 0.0);

	REQUIRE(rig.Machine->Statistics().TrafficRefused == 1);
}

TEST_CASE("a teleport to a remote world crosses as a payload", "[world]") {
	Rig rig({Named("destination")});
	const Name host = rig.HostFor("destination");

	const WorldId source = rig.Machine->Worlds().Create(Named("origin"));
	rig.Machine->Worlds().Enter(source, [](Store &store) {
		Postbox(store).Teleport("destination", Bytes("player:7"));
	});
	rig.Machine->Tick(1.0f / 60.0f, 0.0);

	const auto deliveries = Deliveries(rig.Drain(host.Text().data()));
	REQUIRE(deliveries.size() == 1);
	REQUIRE(deliveries[0].World == Name("destination"));
	REQUIRE(deliveries[0].Message.Bus == BusKind::Teleport);
	REQUIRE(deliveries[0].Message.From == Name("origin"));
	REQUIRE(Text(deliveries[0].Message.Payload) == "player:7");
}

TEST_CASE("several deliveries for one host go in one frame", "[world]") {
	// A host with twenty deliveries gets one frame rather than twenty. A frame
	// per delivery would put the framing cost on the wrong axis.
	Rig rig({Named("a"), Named("b")}, 8);
	const Name host = rig.HostFor("a");
	REQUIRE(rig.HostFor("b") == host);

	rig.PostAs(host.Text().data(), "a", Subscribe("topic"));
	rig.PostAs(host.Text().data(), "b", Subscribe("topic"));
	rig.Machine->Tick(1.0f / 60.0f, 0.0);
	rig.Drain(host.Text().data());

	const WorldId speaker = rig.Machine->Worlds().Create(Named("local"));
	rig.Machine->Worlds().Enter(speaker, [](Store &store) { Postbox(store).Publish("topic", Bytes("one")); });
	rig.Machine->Tick(1.0f / 60.0f, 1.0);

	const std::vector<HostFrame> frames = rig.Drain(host.Text().data());
	size_t deliveryFrames = 0;
	for (const HostFrame &frame : frames) {
		if (frame.Signal == HostSignal::Deliveries) {
			deliveryFrames++;
		}
	}
	REQUIRE(deliveryFrames == 1);
	REQUIRE(Deliveries(frames).size() == 2);
}

TEST_CASE("a delivery for a host that died is counted rather than swallowed", "[world]") {
	Rig rig({Named("gone")});
	const Name host = rig.HostFor("gone");

	rig.PostAs(host.Text().data(), "gone", Subscribe("topic"));
	rig.Machine->Tick(1.0f / 60.0f, 0.0);

	const WorldId speaker = rig.Machine->Worlds().Create(Named("local"));
	rig.Machine->Worlds().Enter(speaker, [](Store &store) {
		Postbox(store).Publish("topic", Bytes("into the void"));
	});

	// The host's end goes away between the subscribe and the publish.
	rig.Link(host.Text().data()).Close();
	rig.Machine->Tick(1.0f / 60.0f, 1.0);

	REQUIRE(rig.Machine->Statistics().DeliveriesDropped == 1);
	REQUIRE(rig.Machine->Statistics().DeliveriesSent == 0);
}

TEST_CASE("a universe with remote worlds round-trips through a snapshot", "[world]") {
	// Without the host on the record, a restored driver would bring every
	// remote world back as a local one — empty, ticking, and answering for a
	// world still running somewhere else.
	Rig rig({Named("held.elsewhere")});
	rig.Machine->Worlds().Create(Named("held.here"));

	engine::core::ByteWriter writer;
	REQUIRE(rig.Machine->Worlds().Save(writer));

	engine::world::Universe restored;
	engine::core::ByteReader reader(writer.Bytes());
	REQUIRE(restored.Load(reader));

	const WorldId elsewhere = restored.Find(Name("held.elsewhere"));
	const WorldId here = restored.Find(Name("held.here"));

	REQUIRE(restored.IsRemote(elsewhere));
	REQUIRE(restored.HostOf(elsewhere).IsValid());
	REQUIRE(restored.StateOf(elsewhere) == WorldState::Remote);

	REQUIRE_FALSE(restored.IsRemote(here));
	REQUIRE_FALSE(restored.HostOf(here).IsValid());
}

TEST_CASE("routing is the same whether a world is local or remote", "[world][fuzz]") {
	// The claim, stated as a differential test. The same script of publishes
	// and subscribes is run twice: once with every world local, once with half
	// of them in a host. The delivered payloads have to match exactly.
	const auto script = [](uint32_t seed) {
		std::vector<std::pair<int, std::string>> plan; // world index, payload
		for (uint32_t step = 0; step < 120; step++) {
			plan.emplace_back(static_cast<int>(Random::Bits(seed, step) % 4), "m" + std::to_string(step));
		}
		return plan;
	};

	const std::vector<std::string> names{"w0", "w1", "w2", "w3"};

	// --- everything local ---
	std::vector<std::string> allLocal;
	{
		engine::world::Universe universe;
		std::vector<WorldId> worlds;
		for (const std::string &name : names) {
			worlds.push_back(universe.Create(Named(name.c_str())));
		}
		for (const WorldId id : worlds) {
			universe.Enter(id, [](Store &store) { Postbox(store).Subscribe("shared"); });
		}
		universe.Tick(1.0f / 60.0f);

		for (const auto &[which, payload] : script(31)) {
			universe.Enter(worlds[static_cast<size_t>(which)], [payload = payload](Store &store) {
				Postbox(store).Publish("shared", Bytes(payload));
			});
			universe.Tick(1.0f / 60.0f);

			for (size_t index = 0; index < worlds.size(); index++) {
				universe.Enter(worlds[index], [&allLocal, index](Store &store) {
					for (const Delivery &delivery : Postbox(store).Deliveries()) {
						allLocal.push_back(std::to_string(index) + ":" + Text(delivery.Payload));
					}
				});
			}
		}
	}

	// --- half of them in a host ---
	std::vector<std::string> split;
	{
		Rig rig({Named("w2"), Named("w3")}, 8);
		const Name host = rig.HostFor("w2");

		std::vector<WorldId> local;
		local.push_back(rig.Machine->Worlds().Create(Named("w0")));
		local.push_back(rig.Machine->Worlds().Create(Named("w1")));

		for (const WorldId id : local) {
			rig.Machine->Worlds().Enter(id, [](Store &store) { Postbox(store).Subscribe("shared"); });
		}
		rig.PostAs(host.Text().data(), "w2", Subscribe("shared"));
		rig.PostAs(host.Text().data(), "w3", Subscribe("shared"));
		rig.Machine->Tick(1.0f / 60.0f, 0.0);
		rig.Drain(host.Text().data());

		double now = 1.0;
		for (const auto &[which, payload] : script(31)) {
			if (which < 2) {
				rig.Machine->Worlds().Enter(
					local[static_cast<size_t>(which)],
					[payload = payload](Store &store) { Postbox(store).Publish("shared", Bytes(payload)); }
				);
			} else {
				rig.PostAs(
					host.Text().data(),
					names[static_cast<size_t>(which)].c_str(),
					Publish("shared", payload.c_str())
				);
			}
			rig.Machine->Tick(1.0f / 60.0f, now);
			now += 1.0;

			for (size_t index = 0; index < local.size(); index++) {
				rig.Machine->Worlds().Enter(local[index], [&split, index](Store &store) {
					for (const Delivery &delivery : Postbox(store).Deliveries()) {
						split.push_back(std::to_string(index) + ":" + Text(delivery.Payload));
					}
				});
			}
			for (const auto &delivery : Deliveries(rig.Drain(host.Text().data()))) {
				const size_t index = delivery.World == Name("w2") ? 2 : 3;
				split.push_back(std::to_string(index) + ":" + Text(delivery.Message.Payload));
			}
		}
	}

	REQUIRE_FALSE(allLocal.empty());
	REQUIRE(split.size() == allLocal.size());

	// Sorted, because the two runs deliver in the same barrier but this test
	// drains local inboxes and host links in a fixed order that is its own, not
	// the router's. What is being compared is the *set* of deliveries.
	std::sort(allLocal.begin(), allLocal.end());
	std::sort(split.begin(), split.end());
	REQUIRE(split == allLocal);
}
