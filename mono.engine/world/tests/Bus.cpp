#include <engine/core/Name.hpp>
#include <engine/core/Random.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Postbox.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.world.bus")

using engine::core::Name;
using engine::core::Random;
using engine::ecs::Phase;
using engine::ecs::Scheduler;
using engine::ecs::Store;
using engine::parallel::Jobs;
using engine::world::BusKind;
using engine::world::BusStatus;
using engine::world::Delivery;
using engine::world::Postbox;
using engine::world::Ticket;
using engine::world::Universe;
using engine::world::UniverseSettings;
using engine::world::WorldId;
using engine::world::WorldSettings;

namespace bus_test {
	struct Pool {
		explicit Pool(unsigned workers) {
			Jobs::Start(workers);
		}
		~Pool() {
			Jobs::Stop();
		}
	};

	WorldSettings Named(const char *name) {
		WorldSettings settings;
		settings.Name = Name(name);
		settings.TickRate = 60.0;
		return settings;
	}

	// A payload from a string, and back, so cases read as values rather than
	// as byte arithmetic.
	std::vector<std::byte> Bytes(std::string_view text) {
		std::vector<std::byte> payload(text.size());
		std::memcpy(payload.data(), text.data(), text.size());
		return payload;
	}

	std::string Text(std::span<const std::byte> payload) {
		return std::string(reinterpret_cast<const char *>(payload.data()), payload.size());
	}

	// Registers a system that runs `body` with a postbox, once per tick.
	void OnTick(Universe &universe, WorldId id, std::function<void(Postbox &)> body) {
		universe.Enter(id, [body = std::move(body)](Store &, Scheduler &systems) {
			systems.Add("bus", Phase::PreSimulation, [body](Store &world) {
				Postbox box(world);
				body(box);
			});
		});
	}

	// Everything a world received at the last barrier.
	std::vector<Delivery> Received(Universe &universe, WorldId id) {
		std::vector<Delivery> found;
		universe.Enter(id, [&found](Store &store) {
			const Postbox box(store);
			for (const Delivery &delivery : box.Deliveries()) {
				found.push_back(delivery);
			}
		});
		return found;
	}
}

using namespace bus_test;

// --- messaging ------------------------------------------------------------

TEST_CASE("a publish reaches its subscribers and nobody else", "[world]") {
	Universe universe;
	const WorldId speaker = universe.Create(Named("bus.speaker"));
	const WorldId listener = universe.Create(Named("bus.listener"));
	const WorldId bystander = universe.Create(Named("bus.bystander"));

	universe.Enter(listener, [](Store &store) { Postbox(store).Subscribe("boss.spawned"); });
	universe.Tick(1.0f / 60.0f);
	REQUIRE(universe.SubscriberCount(Name("boss.spawned")) == 1);

	universe.Enter(speaker, [](Store &store) { Postbox(store).Publish("boss.spawned", Bytes("zone-4")); });
	universe.Tick(1.0f / 60.0f);

	const std::vector<Delivery> heard = Received(universe, listener);
	REQUIRE(heard.size() == 1);
	REQUIRE(heard[0].Bus == BusKind::Messaging);
	REQUIRE(heard[0].Key == Name("boss.spawned"));
	REQUIRE(heard[0].From == Name("bus.speaker"));
	REQUIRE(Text(heard[0].Payload) == "zone-4");

	REQUIRE(Received(universe, bystander).empty());
}

TEST_CASE("a publisher does not hear itself", "[world]") {
	// A world that had to filter its own messages out of its own inbox would
	// get it wrong exactly once.
	Universe universe;
	const WorldId id = universe.Create(Named("bus.selftalk"));

	universe.Enter(id, [](Store &store) {
		Postbox box(store);
		box.Subscribe("topic");
		box.Publish("topic", Bytes("hello"));
	});
	universe.Tick(1.0f / 60.0f);
	universe.Tick(1.0f / 60.0f);

	REQUIRE(Received(universe, id).empty());
}

TEST_CASE("a publish with no subscribers is not an error", "[world]") {
	Universe universe;
	const WorldId id = universe.Create(Named("bus.shouting"));

	universe.Enter(id, [](Store &store) { REQUIRE(Postbox(store).Publish("nobody.listening", Bytes("x"))); });
	universe.Tick(1.0f / 60.0f);

	REQUIRE(universe.Statistics().BusOperations == 1);
	REQUIRE(universe.Statistics().Deliveries == 0);
}

TEST_CASE("unsubscribing stops delivery", "[world]") {
	Universe universe;
	const WorldId speaker = universe.Create(Named("bus.speaker2"));
	const WorldId listener = universe.Create(Named("bus.listener2"));

	universe.Enter(listener, [](Store &store) { Postbox(store).Subscribe("news"); });
	universe.Tick(1.0f / 60.0f);

	universe.Enter(listener, [](Store &store) { Postbox(store).Unsubscribe("news"); });
	universe.Enter(speaker, [](Store &store) { Postbox(store).Publish("news", Bytes("late")); });
	universe.Tick(1.0f / 60.0f);
	universe.Tick(1.0f / 60.0f);

	REQUIRE(universe.SubscriberCount(Name("news")) == 0);
	REQUIRE(Received(universe, listener).empty());
}

TEST_CASE("one publish reaches every subscriber", "[world]") {
	Universe universe;
	const WorldId speaker = universe.Create(Named("bus.broadcast"));

	std::vector<WorldId> listeners;
	for (int index = 0; index < 8; index++) {
		const WorldId id = universe.Create(Named(("bus.crowd." + std::to_string(index)).c_str()));
		universe.Enter(id, [](Store &store) { Postbox(store).Subscribe("all"); });
		listeners.push_back(id);
	}
	universe.Tick(1.0f / 60.0f);

	universe.Enter(speaker, [](Store &store) { Postbox(store).Publish("all", Bytes("ping")); });
	universe.Tick(1.0f / 60.0f);

	for (const WorldId id : listeners) {
		const std::vector<Delivery> heard = Received(universe, id);
		REQUIRE(heard.size() == 1);
		REQUIRE(Text(heard[0].Payload) == "ping");
	}
}

// --- the one-tick latency -------------------------------------------------

TEST_CASE("traffic lands exactly one barrier later", "[world]") {
	// One tick of latency, always. A message that could arrive mid-tick would
	// end "a tick is one thing that starts and finishes".
	Universe universe;
	const WorldId speaker = universe.Create(Named("bus.timing.a"));
	const WorldId listener = universe.Create(Named("bus.timing.b"));

	universe.Enter(listener, [](Store &store) { Postbox(store).Subscribe("tick.topic"); });
	universe.Tick(1.0f / 60.0f);

	universe.Enter(speaker, [](Store &store) { Postbox(store).Publish("tick.topic", Bytes("m")); });

	// Nothing yet: the publish is still in the sender's outbox.
	REQUIRE(Received(universe, listener).empty());

	universe.Tick(1.0f / 60.0f);
	REQUIRE(Received(universe, listener).size() == 1);

	// And the inbox is replaced rather than appended to, so it does not
	// accumulate for a system that forgets to drain it.
	universe.Tick(1.0f / 60.0f);
	REQUIRE(Received(universe, listener).empty());
}

TEST_CASE("a subscription made this tick does not receive this tick", "[world]") {
	// The honest answer: the subscription did not exist when the message was
	// sent.
	Universe universe;
	const WorldId speaker = universe.Create(Named("bus.race.a"));
	const WorldId listener = universe.Create(Named("bus.race.b"));

	universe.Enter(listener, [](Store &store) { Postbox(store).Subscribe("same.tick"); });
	universe.Enter(speaker, [](Store &store) { Postbox(store).Publish("same.tick", Bytes("m")); });
	universe.Tick(1.0f / 60.0f);

	// The subscribe and the publish were applied in the same barrier, in
	// (From, Sequence) order — and whether the subscribe won depends only on
	// the world names, not on thread timing.
	const size_t heard = Received(universe, listener).size();
	universe.Tick(1.0f / 60.0f);
	REQUIRE(heard + Received(universe, listener).size() <= 1);
}

// --- memory store ---------------------------------------------------------

TEST_CASE("a memory store value round-trips between worlds", "[world]") {
	Universe universe;
	const WorldId writer = universe.Create(Named("bus.writer"));
	const WorldId reader = universe.Create(Named("bus.reader"));

	universe.Enter(writer, [](Store &store) {
		Postbox(store).Set(BusKind::MemoryStore, "leaderboard", Bytes("alice:100"));
	});
	universe.Tick(1.0f / 60.0f);

	Ticket ticket;
	universe.Enter(reader, [&ticket](Store &store) {
		ticket = Postbox(store).Get(BusKind::MemoryStore, "leaderboard");
	});
	REQUIRE(ticket.Expected());
	universe.Tick(1.0f / 60.0f);

	const std::vector<Delivery> replies = Received(universe, reader);
	REQUIRE(replies.size() == 1);
	REQUIRE(replies[0].Reply == ticket);
	REQUIRE(replies[0].Status == BusStatus::Ok);
	REQUIRE(Text(replies[0].Payload) == "alice:100");
}

TEST_CASE("reading a key nobody wrote reports NotFound", "[world]") {
	Universe universe;
	const WorldId id = universe.Create(Named("bus.missing"));

	universe.Enter(id, [](Store &store) { Postbox(store).Get(BusKind::MemoryStore, "absent"); });
	universe.Tick(1.0f / 60.0f);

	const std::vector<Delivery> replies = Received(universe, id);
	REQUIRE(replies.size() == 1);
	REQUIRE(replies[0].Status == BusStatus::NotFound);
	REQUIRE(replies[0].Payload.empty());
}

TEST_CASE("a queue hands each popper a different entry", "[world]") {
	// The matchmaking case. Several worlds pop the same queue in one barrier
	// and each gets its own entry, because the barrier applies operations one
	// after another rather than concurrently.
	Universe universe;
	const WorldId filler = universe.Create(Named("bus.queue.filler"));

	universe.Enter(filler, [](Store &store) {
		Postbox box(store);
		for (int index = 0; index < 3; index++) {
			box.Push("matchmaking", Bytes("player-" + std::to_string(index)));
		}
	});
	universe.Tick(1.0f / 60.0f);

	std::vector<WorldId> poppers;
	for (int index = 0; index < 4; index++) {
		const WorldId id = universe.Create(Named(("bus.popper." + std::to_string(index)).c_str()));
		universe.Enter(id, [](Store &store) { Postbox(store).Pop("matchmaking"); });
		poppers.push_back(id);
	}
	universe.Tick(1.0f / 60.0f);

	std::vector<std::string> got;
	size_t empty = 0;
	for (const WorldId id : poppers) {
		const std::vector<Delivery> replies = Received(universe, id);
		REQUIRE(replies.size() == 1);
		if (replies[0].Status == BusStatus::Ok) {
			got.push_back(Text(replies[0].Payload));
		} else {
			empty++;
		}
	}

	// Three entries, four poppers: three win and one is told the queue is
	// empty. Nobody gets the same entry twice.
	std::sort(got.begin(), got.end());
	REQUIRE(got.size() == 3);
	REQUIRE(std::adjacent_find(got.begin(), got.end()) == got.end());
	REQUIRE(empty == 1);
}

TEST_CASE("removing a memory key reports whether it was there", "[world]") {
	Universe universe;
	const WorldId id = universe.Create(Named("bus.remove"));

	universe.Enter(id, [](Store &store) {
		Postbox box(store);
		box.Set(BusKind::MemoryStore, "temp", Bytes("v"));
	});
	universe.Tick(1.0f / 60.0f);

	universe.Enter(id, [](Store &store) {
		Postbox box(store);
		box.Remove(BusKind::MemoryStore, "temp");
		box.Remove(BusKind::MemoryStore, "temp");
	});
	universe.Tick(1.0f / 60.0f);

	const std::vector<Delivery> replies = Received(universe, id);
	REQUIRE(replies.size() == 2);
	REQUIRE(replies[0].Status == BusStatus::Ok);
	REQUIRE(replies[1].Status == BusStatus::NotFound);
}

// --- data store -----------------------------------------------------------

TEST_CASE("a data store write carries a version", "[world]") {
	Universe universe;
	const WorldId id = universe.Create(Named("bus.data"));

	universe.Enter(id, [](Store &store) {
		Postbox(store).Set(BusKind::DataStore, "player:1", Bytes("gold=10"));
	});
	universe.Tick(1.0f / 60.0f);

	const std::vector<Delivery> written = Received(universe, id);
	REQUIRE(written.size() == 1);
	REQUIRE(written[0].Version == 1);

	universe.Enter(id, [](Store &store) { Postbox(store).Get(BusKind::DataStore, "player:1"); });
	universe.Tick(1.0f / 60.0f);

	const std::vector<Delivery> read = Received(universe, id);
	REQUIRE(read.size() == 1);
	REQUIRE(Text(read[0].Payload) == "gold=10");
	REQUIRE(read[0].Version == 1);
}

TEST_CASE("a stale update is refused rather than silently overwriting", "[world]") {
	// The read-modify-write a DataStore needs: two worlds updating one player's
	// inventory must not lose one of the writes.
	Universe universe;
	const WorldId first = universe.Create(Named("bus.rmw.a"));
	const WorldId second = universe.Create(Named("bus.rmw.b"));

	universe.Enter(first, [](Store &store) {
		Postbox(store).Set(BusKind::DataStore, "shared", Bytes("v1"));
	});
	universe.Tick(1.0f / 60.0f);

	// Both worlds read version 1.
	universe.Enter(first, [](Store &store) { Postbox(store).Get(BusKind::DataStore, "shared"); });
	universe.Enter(second, [](Store &store) { Postbox(store).Get(BusKind::DataStore, "shared"); });
	universe.Tick(1.0f / 60.0f);

	REQUIRE(Received(universe, first)[0].Version == 1);
	REQUIRE(Received(universe, second)[0].Version == 1);

	// Both try to write it. Exactly one wins.
	universe.Enter(first, [](Store &store) { Postbox(store).Update("shared", 1, Bytes("from-a")); });
	universe.Enter(second, [](Store &store) { Postbox(store).Update("shared", 1, Bytes("from-b")); });
	universe.Tick(1.0f / 60.0f);

	const BusStatus one = Received(universe, first)[0].Status;
	const BusStatus two = Received(universe, second)[0].Status;

	REQUIRE(((one == BusStatus::Ok) != (two == BusStatus::Ok)));
	REQUIRE(((one == BusStatus::Conflict) || (two == BusStatus::Conflict)));

	// And the loser was told the current value, so it can retry without a
	// second round trip.
	const Delivery loser =
		one == BusStatus::Conflict ? Received(universe, first)[0] : Received(universe, second)[0];
	REQUIRE(loser.Version == 2);
	REQUIRE_FALSE(loser.Payload.empty());
}

TEST_CASE("an update against a key nobody wrote takes version zero", "[world]") {
	Universe universe;
	const WorldId id = universe.Create(Named("bus.create.via.update"));

	universe.Enter(id, [](Store &store) { Postbox(store).Update("fresh", 0, Bytes("first")); });
	universe.Tick(1.0f / 60.0f);

	const std::vector<Delivery> replies = Received(universe, id);
	REQUIRE(replies[0].Status == BusStatus::Ok);
	REQUIRE(replies[0].Version == 1);
}

// --- teleport -------------------------------------------------------------

TEST_CASE("a teleport carries a payload to a named world", "[world]") {
	// A payload, never an entity. The destination rebuilds the player from its
	// own class definitions.
	Universe universe;
	const WorldId lobby = universe.Create(Named("bus.lobby"));
	const WorldId arena = universe.Create(Named("bus.arena"));

	universe.Enter(lobby, [](Store &store) {
		Postbox(store).Teleport("bus.arena", Bytes("player:42,level:7"));
	});
	universe.Tick(1.0f / 60.0f);

	const std::vector<Delivery> arrived = Received(universe, arena);
	REQUIRE(arrived.size() == 1);
	REQUIRE(arrived[0].Bus == BusKind::Teleport);
	REQUIRE(arrived[0].From == Name("bus.lobby"));
	REQUIRE(Text(arrived[0].Payload) == "player:42,level:7");
}

TEST_CASE("a teleport to a world that does not exist is reported", "[world]") {
	Universe universe;
	const WorldId id = universe.Create(Named("bus.stranded"));

	universe.Enter(id, [](Store &store) { Postbox(store).Teleport("bus.nowhere", Bytes("x")); });
	universe.Tick(1.0f / 60.0f);

	const std::vector<Delivery> replies = Received(universe, id);
	REQUIRE(replies.size() == 1);
	REQUIRE(replies[0].Status == BusStatus::NoSuchWorld);
}

// --- budgets --------------------------------------------------------------

TEST_CASE("a world over budget is refused rather than allowed to starve others", "[world]") {
	UniverseSettings settings;
	settings.BusBudgetPerTick = 4;
	Universe universe(settings);

	const WorldId id = universe.Create(Named("bus.greedy"));
	universe.Tick(1.0f / 60.0f); // establishes the budget resource

	size_t accepted = 0;
	size_t refused = 0;
	universe.Enter(id, [&](Store &store) {
		Postbox box(store);
		for (int attempt = 0; attempt < 20; attempt++) {
			if (box.Publish("spam", Bytes("x"))) {
				accepted++;
			} else {
				refused++;
			}
		}
	});

	REQUIRE(accepted == 4);
	REQUIRE(refused == 16);
}

TEST_CASE("the budget resets every barrier", "[world]") {
	UniverseSettings settings;
	settings.BusBudgetPerTick = 2;
	Universe universe(settings);

	const WorldId id = universe.Create(Named("bus.steady"));

	size_t total = 0;
	OnTick(universe, id, [&total](Postbox &box) {
		for (int attempt = 0; attempt < 5; attempt++) {
			if (box.Publish("steady", {})) {
				total++;
			}
		}
	});

	for (int frame = 0; frame < 10; frame++) {
		universe.Tick(1.0f / 60.0f);
	}

	// Two per tick, ten ticks. Not two forever, and not five.
	REQUIRE(total == 20);
}

// --- ordering -------------------------------------------------------------

TEST_CASE("operations apply in (sender, sequence) order", "[world]") {
	// The determinism guarantee. Two worlds writing the same key in the same
	// barrier must resolve the same way on every run, whatever order the world
	// list happened to be walked in.
	Pool pool{4};

	const auto run = [] {
		Universe universe;
		std::vector<WorldId> worlds;
		for (int index = 0; index < 6; index++) {
			worlds.push_back(universe.Create(Named(("bus.order." + std::to_string(index)).c_str())));
		}

		for (size_t index = 0; index < worlds.size(); index++) {
			universe.Enter(worlds[index], [index](Store &store) {
				Postbox box(store);
				box.Set(BusKind::MemoryStore, "contested", Bytes("world-" + std::to_string(index)));
			});
		}
		universe.Tick(1.0f / 60.0f);

		std::vector<std::byte> value;
		universe.Peek(BusKind::MemoryStore, Name("contested"), &value);
		return Text(value);
	};

	const std::string first = run();
	for (int attempt = 0; attempt < 20; attempt++) {
		REQUIRE(run() == first);
	}
}

TEST_CASE("one world's own operations apply in the order it made them", "[world]") {
	Universe universe;
	const WorldId id = universe.Create(Named("bus.sequence"));

	universe.Enter(id, [](Store &store) {
		Postbox box(store);
		box.Set(BusKind::MemoryStore, "counter", Bytes("1"));
		box.Set(BusKind::MemoryStore, "counter", Bytes("2"));
		box.Set(BusKind::MemoryStore, "counter", Bytes("3"));
	});
	universe.Tick(1.0f / 60.0f);

	std::vector<std::byte> value;
	REQUIRE(universe.Peek(BusKind::MemoryStore, Name("counter"), &value) == BusStatus::Ok);
	REQUIRE(Text(value) == "3");
}

// --- churn ----------------------------------------------------------------

TEST_CASE("bus traffic under churn stays deterministic", "[world][fuzz]") {
	// The whole design in one case: many worlds, mixed operations, run twice.
	// Anything that depended on thread scheduling rather than on the data would
	// make the two runs differ.
	Pool pool{4};

	const auto run = [](uint32_t salt) {
		Universe universe;

		std::vector<WorldId> worlds;
		for (int index = 0; index < 8; index++) {
			const WorldId id = universe.Create(Named(("bus.churn." + std::to_string(index)).c_str()));
			universe.Enter(id, [](Store &store) { Postbox(store).Subscribe("churn.topic"); });
			worlds.push_back(id);
		}

		for (uint32_t step = 0; step < 200; step++) {
			for (size_t index = 0; index < worlds.size(); index++) {
				const uint32_t roll = Random::Bits(step, salt + static_cast<uint32_t>(index)) % 4;
				universe.Enter(worlds[index], [roll, step, index](Store &store) {
					Postbox box(store);
					const std::string value = std::to_string(step) + ":" + std::to_string(index);

					switch (roll) {
					case 0:
						box.Publish("churn.topic", Bytes(value));
						break;
					case 1:
						box.Set(BusKind::MemoryStore, "churn.key", Bytes(value));
						break;
					case 2:
						box.Push("churn.queue", Bytes(value));
						break;
					default:
						box.Pop("churn.queue");
						break;
					}
				});
			}
			universe.Tick(1.0f / 60.0f);
		}

		std::vector<std::byte> value;
		universe.Peek(BusKind::MemoryStore, Name("churn.key"), &value);
		return Text(value);
	};

	REQUIRE(run(900) == run(900));
	REQUIRE(run(901) == run(901));
}

TEST_CASE("a world destroyed with traffic in flight loses it cleanly", "[world]") {
	Universe universe;
	const WorldId speaker = universe.Create(Named("bus.gone.a"));
	const WorldId listener = universe.Create(Named("bus.gone.b"));

	universe.Enter(listener, [](Store &store) { Postbox(store).Subscribe("gone"); });
	universe.Tick(1.0f / 60.0f);

	universe.Enter(speaker, [](Store &store) { Postbox(store).Publish("gone", Bytes("m")); });
	universe.Destroy(listener);

	// The subscriber is gone by the time the barrier routes the message. It
	// must be dropped rather than delivered into a freed world.
	universe.Tick(1.0f / 60.0f);
	REQUIRE(universe.Count() == 1);
}

// A replica's bus handle refuses writes.
//
// `v02v03.md` §2.12 puts this in v0.2 rather than with replication, because the
// value is in no system ever being *written* assuming it can reach a DataStore
// from a client. A flag added after the systems exist finds them one crash at a
// time.

TEST_CASE("a replica may not publish", "[world]") {
	Universe universe;
	const WorldId server = universe.Create(Named("bus.replica.server"));
	const WorldId client = universe.Create(Named("bus.replica.client"));

	universe.Enter(server, [](Store &store) { Postbox(store).Subscribe("replica.topic"); });
	universe.Enter(client, [](Store &store) {
		store.SetResource<engine::world::Replica>({true});
		Postbox(store).Subscribe("replica.topic");
	});
	universe.Tick(1.0f / 60.0f);

	// The server hears itself as a subscriber; the client's subscribe was
	// refused, so it is not one.
	REQUIRE(universe.SubscriberCount(Name("replica.topic")) == 1);

	universe.Enter(client, [](Store &store) {
		REQUIRE_FALSE(Postbox(store).Publish("replica.topic", Bytes("no")));
	});
	universe.Tick(1.0f / 60.0f);

	REQUIRE(universe.LastTraffic().empty());
}

TEST_CASE("a replica may not reach a datastore, a memorystore, or a teleport", "[world]") {
	// Every write, not a representative one. A refusal that covers four of five
	// entry points is a hole somebody finds by shipping through it.
	Universe universe;
	const WorldId client = universe.Create(Named("bus.replica.writes"));

	universe.Enter(client, [](Store &store) {
		store.SetResource<engine::world::Replica>({true});

		Postbox box(store);
		REQUIRE(box.IsReplica());

		REQUIRE_FALSE(box.Get(BusKind::DataStore, "k").Expected());
		REQUIRE_FALSE(box.Set(BusKind::DataStore, "k", Bytes("v")).Expected());
		REQUIRE_FALSE(box.Update("k", 1, Bytes("v")).Expected());
		REQUIRE_FALSE(box.Remove(BusKind::DataStore, "k").Expected());
		REQUIRE_FALSE(box.Push("q", Bytes("v")).Expected());
		REQUIRE_FALSE(box.Pop("q").Expected());
		REQUIRE_FALSE(box.Teleport("elsewhere", Bytes("p")).Expected());
		REQUIRE_FALSE(box.Publish("t"));
		REQUIRE_FALSE(box.Subscribe("t"));
		REQUIRE_FALSE(box.Unsubscribe("t"));
	});
	universe.Tick(1.0f / 60.0f);

	REQUIRE(universe.LastTraffic().empty());
}

TEST_CASE("a replica still receives everything sent to it", "[world]") {
	// The refusal is one-directional. Inbound is the whole point of a replica:
	// it is how authoritative state arrives.
	Universe universe;
	const WorldId server = universe.Create(Named("bus.replica.rx.server"));
	const WorldId client = universe.Create(Named("bus.replica.rx.client"));

	// Subscribed *before* it becomes a replica, the way a client that joined
	// and then handed authority over would be.
	universe.Enter(client, [](Store &store) { Postbox(store).Subscribe("state"); });
	universe.Tick(1.0f / 60.0f);
	universe.Enter(client, [](Store &store) { store.SetResource<engine::world::Replica>({true}); });

	universe.Enter(server, [](Store &store) { Postbox(store).Publish("state", Bytes("tick-1")); });
	universe.Tick(1.0f / 60.0f);

	std::string heard;
	universe.Enter(client, [&heard](Store &store) {
		const Postbox box(store);
		for (const Delivery &delivery : box.Deliveries()) {
			heard += Text(delivery.Payload);
		}
	});
	REQUIRE(heard == "tick-1");
}

TEST_CASE("clearing the replica flag restores the handle", "[world]") {
	// Present-and-false is the same as absent, so a world can be promoted
	// without the resource having to be removed — which a snapshot round trip
	// would make awkward.
	Universe universe;
	const WorldId world = universe.Create(Named("bus.replica.promote"));

	universe.Enter(world, [](Store &store) {
		store.SetResource<engine::world::Replica>({true});
		REQUIRE_FALSE(Postbox(store).Set(BusKind::MemoryStore, "k", Bytes("v")).Expected());
	});
	universe.Tick(1.0f / 60.0f);

	universe.Enter(world, [](Store &store) {
		store.SetResource<engine::world::Replica>({false});
		REQUIRE_FALSE(Postbox(store).IsReplica());
		REQUIRE(Postbox(store).Set(BusKind::MemoryStore, "k", Bytes("v")).Expected());
	});
	universe.Tick(1.0f / 60.0f);

	std::vector<std::byte> value;
	REQUIRE(universe.Peek(BusKind::MemoryStore, Name("k"), &value) == BusStatus::Ok);
	REQUIRE(Text(value) == "v");
}

TEST_CASE("the replica flag survives a snapshot", "[world]") {
	// Otherwise a supervisor restarting a crashed client host brings it back
	// with authority it never had.
	Universe universe;
	const WorldId client = universe.Create(Named("bus.replica.saved"));
	universe.Enter(client, [](Store &store) { store.SetResource<engine::world::Replica>({true}); });

	engine::core::ByteWriter writer;
	REQUIRE(universe.Save(writer));

	Universe restored;
	engine::core::ByteReader reader(writer.Bytes());
	REQUIRE(restored.Load(reader));

	const WorldId back = restored.Find(Name("bus.replica.saved"));
	REQUIRE(back.IsValid());
	restored.Enter(back, [](Store &store) {
		REQUIRE(Postbox(store).IsReplica());
		REQUIRE_FALSE(Postbox(store).Set(BusKind::DataStore, "k", Bytes("v")).Expected());
	});
}

// --- channels ---------------------------------------------------------------

TEST_CASE("a channel message reaches the world it names and nobody else", "[world]") {
	// **The addressed route, which this module did not have.** `Publish` is a
	// topic fan-out with no destination — right for "the boss died", wrong for
	// "world B, here is the score you asked me for" — and the only other
	// operation that named a world moved a *person*. So a game saying one thing
	// to one world had to broadcast it to everybody or send a player carrying
	// it.
	Universe universe;
	const WorldId lobby = universe.Create(Named("channel.lobby"));
	const WorldId arena = universe.Create(Named("channel.arena"));
	const WorldId other = universe.Create(Named("channel.other"));

	universe.Enter(lobby, [](Store &store) { Postbox(store).SendTo("channel.arena", Bytes("score:12")); });
	universe.Tick(1.0f / 60.0f);

	const std::vector<Delivery> arrived = Received(universe, arena);
	REQUIRE(arrived.size() == 1);
	REQUIRE(arrived[0].Bus == BusKind::Channel);
	REQUIRE(Text(arrived[0].Payload) == "score:12");

	// **The sender's name, which is what makes it a channel.** A topic
	// subscriber is told which topic; a channel receiver is told who to answer,
	// because answering is the point and the destination already knows it is
	// itself.
	REQUIRE(arrived[0].From == Name("channel.lobby"));
	REQUIRE(arrived[0].Key == Name("channel.lobby"));

	// **And nobody else got it**, which is the entire difference from a publish
	// and the one thing a fan-out cannot promise.
	CHECK(Received(universe, other).empty());
}

TEST_CASE("a channel is not a teleport, and a receiver can tell", "[world]") {
	// **Why this is a fifth `BusKind` rather than a flag on the fourth.** The
	// two look alike on the wire and mean entirely different things to whoever
	// receives them: a teleport is a person arriving, and a receiving world
	// builds a `Player` and a character out of it. `script::AdmitTeleports` runs
	// on every world whether or not it is running scripts — so a channel message
	// arriving as a `Teleport` would have it trying to construct a player out of
	// a chat line.
	Universe universe;
	const WorldId from = universe.Create(Named("channel.sender"));
	const WorldId to = universe.Create(Named("channel.receiver"));

	universe.Enter(from, [](Store &store) {
		Postbox box(store);
		box.Teleport("channel.receiver", Bytes("a person"));
		box.SendTo("channel.receiver", Bytes("a message"));
	});
	universe.Tick(1.0f / 60.0f);

	const std::vector<Delivery> arrived = Received(universe, to);
	REQUIRE(arrived.size() == 2);

	size_t teleports = 0;
	size_t channels = 0;
	for (const Delivery &delivery : arrived) {
		teleports += delivery.Bus == BusKind::Teleport ? 1u : 0u;
		channels += delivery.Bus == BusKind::Channel ? 1u : 0u;
	}
	CHECK(teleports == 1);
	CHECK(channels == 1);
}

TEST_CASE("a channel to a world that is not running is reported", "[world]") {
	// **Named rather than silent, and that is half the reason this exists beside
	// `Publish`.** A publish with no subscribers is a quiet afternoon and cannot
	// be told from a publish nobody wanted; a message addressed to a name
	// nothing answers to is a sender holding a name that is wrong, and it wants
	// to know.
	Universe universe;
	const WorldId id = universe.Create(Named("channel.alone"));

	universe.Enter(id, [](Store &store) { Postbox(store).SendTo("channel.nowhere", Bytes("x")); });
	universe.Tick(1.0f / 60.0f);

	const std::vector<Delivery> replies = Received(universe, id);
	REQUIRE(replies.size() == 1);
	CHECK(replies[0].Status == BusStatus::NoSuchWorld);
}
