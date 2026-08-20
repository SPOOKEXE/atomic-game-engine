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
#include <thread>
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

TEST_CASE("bus and service traffic crosses pinned world lanes at the barrier", "[world][bus]") {
	Pool pool{4};
	if (Jobs::PinnedWorkerCount() < 2) {
		SUCCEED("this platform or process affinity exposes fewer than two pinned workers");
		return;
	}

	Universe universe;
	const WorldId listener = universe.Create(Named("cross-core.listener"));
	const WorldId sender = universe.Create(Named("cross-core.sender"));

	std::thread::id listenerThread;
	std::thread::id senderThread;
	bool listenerMoved = false;
	bool senderMoved = false;
	bool channelQueued = false;
	bool channelHeard = false;
	bool storeWritten = false;
	bool storeHeard = false;
	int listenerTicks = 0;
	int senderTicks = 0;

	OnTick(universe, listener, [&](Postbox &box) {
		const std::thread::id current = std::this_thread::get_id();
		if (listenerThread == std::thread::id{}) {
			listenerThread = current;
		} else if (listenerThread != current) {
			listenerMoved = true;
		}

		if (listenerTicks == 0) {
			(void)box.OpenChannel("updates");
		} else if (listenerTicks == 1) {
			(void)box.Get(BusKind::MemoryStore, "cross-core.value");
		} else {
			for (const Delivery &delivery : box.Deliveries()) {
				if (delivery.Bus == BusKind::Channel && Text(delivery.Payload) == "hello") {
					channelHeard = true;
				}
				if (delivery.Bus == BusKind::MemoryStore && delivery.Status == BusStatus::Ok &&
					Text(delivery.Payload) == "stored") {
					storeHeard = true;
				}
			}
		}
		listenerTicks++;
	});

	OnTick(universe, sender, [&](Postbox &box) {
		const std::thread::id current = std::this_thread::get_id();
		if (senderThread == std::thread::id{}) {
			senderThread = current;
		} else if (senderThread != current) {
			senderMoved = true;
		}

		if (senderTicks == 0) {
			storeWritten = box.Set(BusKind::MemoryStore, "cross-core.value", Bytes("stored")).Expected();
		} else if (senderTicks == 1) {
			channelQueued = box.SendTo("cross-core.listener", "updates", Bytes("hello")).Expected();
		}
		senderTicks++;
	});

	for (int frame = 0; frame < 3; frame++) {
		universe.Tick(1.0f / 60.0f);
	}

	CHECK(listenerThread != std::thread::id{});
	CHECK(senderThread != std::thread::id{});
	CHECK(listenerThread != senderThread);
	CHECK_FALSE(listenerMoved);
	CHECK_FALSE(senderMoved);
	CHECK(storeWritten);
	CHECK(channelQueued);
	CHECK(storeHeard);
	CHECK(channelHeard);
}

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
	// (From, Sequence) order - and whether the subscribe won depends only on
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
		store.SetResource<engine::world::Replica>({});
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
		store.SetResource<engine::world::Replica>({});

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
	universe.Enter(client, [](Store &store) { store.SetResource<engine::world::Replica>({}); });

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
	// without the resource having to be removed - which a snapshot round trip
	// would make awkward.
	Universe universe;
	const WorldId world = universe.Create(Named("bus.replica.promote"));

	universe.Enter(world, [](Store &store) {
		store.SetResource<engine::world::Replica>({});
		REQUIRE_FALSE(Postbox(store).Set(BusKind::MemoryStore, "k", Bytes("v")).Expected());
	});
	universe.Tick(1.0f / 60.0f);

	universe.Enter(world, [](Store &store) {
		store.SetResource<engine::world::Replica>({false, {}, {}});
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
	universe.Enter(client, [](Store &store) { store.SetResource<engine::world::Replica>({}); });

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

TEST_CASE("a channel message reaches the channel it names and nobody else", "[world]") {
	// **The addressed route, and the address is two names.** `Publish` is a topic
	// fan-out with no destination - right for "the boss died", wrong for "world
	// B, here is the score you asked me for" - and the only other operation that
	// named a world moved a *person*. v0.15 closed the first half and left the
	// second: one unnamed pipe per pair of worlds, so a match controller and a
	// chat relay talking between the same two worlds each heard the other.
	Universe universe;
	const WorldId lobby = universe.Create(Named("channel.lobby"));
	const WorldId arena = universe.Create(Named("channel.arena"));
	const WorldId other = universe.Create(Named("channel.other"));

	universe.Enter(arena, [](Store &store) {
		Postbox box(store);
		REQUIRE(box.OpenChannel("scores").Expected());
		REQUIRE(box.OpenChannel("chat").Expected());
	});
	universe.Tick(1.0f / 60.0f);

	universe.Enter(lobby, [](Store &store) {
		Postbox box(store);
		REQUIRE(box.SendTo("channel.arena", "scores", Bytes("score:12")).Expected());
		REQUIRE(box.SendTo("channel.arena", "chat", Bytes("hello")).Expected());
	});
	universe.Tick(1.0f / 60.0f);

	std::vector<Delivery> arrived = Received(universe, arena);
	std::erase_if(arrived, [](const Delivery &delivery) { return delivery.Bus != BusKind::Channel; });
	REQUIRE(arrived.size() == 2);

	// **`Key` is the channel and `From` is the sender**, which is `Messaging`'s
	// shape. v0.15 put the sender in both, because there was no channel to name -
	// so a receiver could not tell two conversations apart without opening the
	// payload.
	CHECK(arrived[0].Key == Name("scores"));
	CHECK(Text(arrived[0].Payload) == "score:12");
	CHECK(arrived[1].Key == Name("chat"));
	CHECK(Text(arrived[1].Payload) == "hello");

	for (const Delivery &delivery : arrived) {
		CHECK(delivery.From == Name("channel.lobby"));
	}

	// **And nobody else got it**, which is the entire difference from a publish
	// and the one thing a fan-out cannot promise.
	CHECK(Received(universe, other).empty());
}

TEST_CASE("a channel is not a teleport, and a receiver can tell", "[world]") {
	// **Why this is a fifth `BusKind` rather than a flag on the fourth.** The
	// two look alike on the wire and mean entirely different things to whoever
	// receives them: a teleport is a person arriving, and a receiving world
	// builds a `Player` and a character out of it. `script::AdmitTeleports` runs
	// on every world whether or not it is running scripts - so a channel message
	// arriving as a `Teleport` would have it trying to construct a player out of
	// a chat line.
	Universe universe;
	const WorldId from = universe.Create(Named("channel.sender"));
	const WorldId to = universe.Create(Named("channel.receiver"));

	universe.Enter(to, [](Store &store) { REQUIRE(Postbox(store).OpenChannel("news").Expected()); });
	universe.Tick(1.0f / 60.0f);

	universe.Enter(from, [](Store &store) {
		Postbox box(store);
		box.Teleport("channel.receiver", Bytes("a person"));
		box.SendTo("channel.receiver", "news", Bytes("a message"));
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

TEST_CASE("every way a channel send can fail is a status the sender reads", "[world]") {
	// **Named rather than silent, and that is half the reason this exists beside
	// `Publish`.** A publish with no subscribers is a quiet afternoon and cannot
	// be told from a publish nobody wanted. Every case below is something the
	// sender has to be able to act on, and each is a different thing to go and
	// fix - which is why they are four statuses rather than one.
	Universe universe;
	const WorldId sender = universe.Create(Named("channel.asker"));
	const WorldId listener = universe.Create(Named("channel.listener"));
	const WorldId broken = universe.Create(Named("channel.broken"));

	universe.Enter(listener, [](Store &store) { REQUIRE(Postbox(store).OpenChannel("open").Expected()); });
	universe.Enter(broken, [](Store &store) { REQUIRE(Postbox(store).OpenChannel("open").Expected()); });
	universe.Tick(1.0f / 60.0f);

	REQUIRE(universe.SetState(broken, engine::world::WorldState::Faulted) == engine::world::WorldStatus::Ok);
	universe.Tick(1.0f / 60.0f);

	universe.Enter(sender, [](Store &store) {
		Postbox box(store);
		box.SendTo("channel.nowhere", "open", Bytes("x"));
		box.SendTo("channel.listener", "never.opened", Bytes("x"));
		box.SendTo("channel.broken", "open", Bytes("x"));
		box.SendTo("channel.listener", "open", Bytes("x"));
	});
	universe.Tick(1.0f / 60.0f);

	// Replies come back in the order the sends were made, because a world's own
	// outbox is ordered by construction and the barrier keeps it.
	const std::vector<Delivery> replies = Received(universe, sender);
	REQUIRE(replies.size() == 4);
	CHECK(replies[0].Status == BusStatus::NoSuchWorld);
	CHECK(replies[1].Status == BusStatus::NoSuchChannel);
	CHECK(replies[2].Status == BusStatus::WorldNotReady);
	CHECK(replies[3].Status == BusStatus::Ok);

	// A closed channel is `NoSuchChannel` again rather than a world that has
	// gone: the world is still there and the sender's name for it is still right.
	universe.Enter(listener, [](Store &store) { REQUIRE(Postbox(store).CloseChannel("open")); });
	universe.Tick(1.0f / 60.0f);

	universe.Enter(sender, [](Store &store) {
		REQUIRE(Postbox(store).SendTo("channel.listener", "open", Bytes("x")).Expected());
	});
	universe.Tick(1.0f / 60.0f);

	const std::vector<Delivery> closed = Received(universe, sender);
	REQUIRE(closed.size() == 1);
	CHECK(closed[0].Status == BusStatus::NoSuchChannel);
}

TEST_CASE("a full destination is Overflow rather than an unbounded queue", "[world]") {
	// **The bound made visible.** `BusBudgetPerTick` bounds what a sender emits
	// and nothing bounded what a destination accumulated, so a thousand worlds
	// each spending their allowance on one victim queued sixty-four thousand
	// payloads into one inbox in a single barrier. Discarding the tail silently
	// would be worse than the leak: a game that works until the day it is busy.
	UniverseSettings settings;
	settings.ChannelQueueLimit = 3;

	Universe universe(settings);
	const WorldId sender = universe.Create(Named("channel.flood.sender"));
	const WorldId victim = universe.Create(Named("channel.flood.victim"));

	universe.Enter(victim, [](Store &store) { REQUIRE(Postbox(store).OpenChannel("firehose").Expected()); });
	universe.Tick(1.0f / 60.0f);

	universe.Enter(sender, [](Store &store) {
		Postbox box(store);
		for (int message = 0; message < 5; message++) {
			REQUIRE(box.SendTo("channel.flood.victim", "firehose", Bytes("x")).Expected());
		}
	});
	universe.Tick(1.0f / 60.0f);

	CHECK(Received(universe, victim).size() == 3);

	const std::vector<Delivery> replies = Received(universe, sender);
	REQUIRE(replies.size() == 5);
	CHECK(replies[2].Status == BusStatus::Ok);
	CHECK(replies[3].Status == BusStatus::Overflow);
	CHECK(replies[4].Status == BusStatus::Overflow);

	// **The bound is per barrier, not for ever.** The next barrier's fanout is
	// cleared with the counter, so a destination that was full once is not
	// permanently refused - which is what makes `Overflow` backpressure rather
	// than a broken channel.
	universe.Enter(sender, [](Store &store) {
		REQUIRE(Postbox(store).SendTo("channel.flood.victim", "firehose", Bytes("x")).Expected());
	});
	universe.Tick(1.0f / 60.0f);

	const std::vector<Delivery> after = Received(universe, sender);
	REQUIRE(after.size() == 1);
	CHECK(after[0].Status == BusStatus::Ok);
}

TEST_CASE("a world may not hold more channels than the universe allows", "[world]") {
	// **The bound on the channel *table*, where `ChannelQueueLimit` bounds the
	// queue.** A channel costs one bus operation to open and one entry in the
	// router's table for ever after, and `BusBudgetPerTick` bounds the rate rather
	// than the total - a world opening a distinct channel every tick for an hour
	// leaves two hundred thousand live entries that every snapshot then carries.
	UniverseSettings settings;
	settings.ChannelsPerWorld = 3;

	Universe universe(settings);
	const WorldId hoarder = universe.Create(Named("channel.cap.hoarder"));
	const WorldId modest = universe.Create(Named("channel.cap.modest"));
	const WorldId sender = universe.Create(Named("channel.cap.sender"));

	universe.Enter(hoarder, [](Store &store) {
		Postbox box(store);
		for (int index = 0; index < 5; index++) {
			// Every one is accepted at the *call*: the send budget is a rate and
			// the cap is a total, so only the barrier can answer this.
			REQUIRE(box.OpenChannel(("cap.c" + std::to_string(index)).c_str()).Expected());
		}
	});
	universe.Enter(modest, [](Store &store) { REQUIRE(Postbox(store).OpenChannel("cap.c0").Expected()); });
	universe.Tick(1.0f / 60.0f);

	// **The refusal arrives, which is the whole reason an open grew a reply.** The
	// world that asked is the only party that can act on it, and the answer is a
	// status rather than a silence - the same promise every other verdict on this
	// bus makes.
	const std::vector<Delivery> verdicts = Received(universe, hoarder);
	REQUIRE(verdicts.size() == 5);
	CHECK(verdicts[2].Status == BusStatus::Ok);
	CHECK(verdicts[3].Status == BusStatus::TooManyChannels);
	CHECK(verdicts[3].Key == Name("cap.c3"));
	CHECK(verdicts[4].Status == BusStatus::TooManyChannels);

	// A refused open opened nothing, and a world under the cap is untouched by
	// its neighbour spending one - the bound is per world and not per universe.
	universe.Enter(sender, [](Store &store) {
		Postbox box(store);
		box.SendTo("channel.cap.hoarder", "cap.c2", Bytes("x"));
		box.SendTo("channel.cap.hoarder", "cap.c3", Bytes("x"));
		box.SendTo("channel.cap.modest", "cap.c0", Bytes("x"));
	});
	universe.Tick(1.0f / 60.0f);

	const std::vector<Delivery> sent = Received(universe, sender);
	REQUIRE(sent.size() == 3);
	CHECK(sent[0].Status == BusStatus::Ok);
	CHECK(sent[1].Status == BusStatus::NoSuchChannel);
	CHECK(sent[2].Status == BusStatus::Ok);

	// **`TooManyChannels` rather than `OverBudget`, and this is the difference.**
	// A budget is spent per tick and comes back at the next one, so a world told
	// it is over budget waits and retries. The cap is a total: retrying it a
	// barrier later answers the same, and the only thing that frees a slot is the
	// world closing one it holds. Reopening a channel it already holds needs no
	// slot at all, or a startup that ran its own opens twice would fail the
	// second time.
	universe.Enter(hoarder, [](Store &store) {
		Postbox box(store);
		REQUIRE(box.OpenChannel("cap.c3").Expected());
		REQUIRE(box.OpenChannel("cap.c0").Expected());
		REQUIRE(box.CloseChannel("cap.c1"));
		REQUIRE(box.OpenChannel("cap.c3").Expected());
	});
	universe.Tick(1.0f / 60.0f);

	const std::vector<Delivery> retried = Received(universe, hoarder);
	REQUIRE(retried.size() == 3);
	CHECK(retried[0].Status == BusStatus::TooManyChannels);
	CHECK(retried[1].Status == BusStatus::Ok);
	CHECK(retried[2].Status == BusStatus::Ok);

	universe.Enter(sender, [](Store &store) {
		REQUIRE(Postbox(store).SendTo("channel.cap.hoarder", "cap.c3", Bytes("x")).Expected());
	});
	universe.Tick(1.0f / 60.0f);

	const std::vector<Delivery> freed = Received(universe, sender);
	REQUIRE(freed.size() == 1);
	CHECK(freed[0].Status == BusStatus::Ok);
}

TEST_CASE("channel bounds retune between barriers", "[world]") {
	// **The setters are read at the barrier, not copied at construction.**
	// `BusRouter::Route` takes the settings by reference every tick, and this
	// pins that: a router that copied them once would keep enforcing the
	// numbers the universe was built with, and the studio's Universe panel
	// would be writing to a dead copy nothing ever reads.
	Universe universe;
	const WorldId sender = universe.Create(Named("channel.retune.sender"));
	const WorldId victim = universe.Create(Named("channel.retune.victim"));

	universe.Enter(victim, [](Store &store) { REQUIRE(Postbox(store).OpenChannel("first").Expected()); });
	universe.Tick(1.0f / 60.0f);

	universe.SetChannelQueueLimit(2);
	universe.SetChannelsPerWorld(1);

	// The table bound: a second open is past the retuned cap of one, though
	// the default the universe was built with would have taken it.
	universe.Enter(victim, [](Store &store) { REQUIRE(Postbox(store).OpenChannel("second").Expected()); });
	universe.Tick(1.0f / 60.0f);

	const std::vector<Delivery> verdicts = Received(universe, victim);
	REQUIRE(verdicts.size() == 1);
	CHECK(verdicts[0].Status == BusStatus::TooManyChannels);

	// The queue bound: the third send in one barrier is past the retuned two.
	universe.Enter(sender, [](Store &store) {
		Postbox box(store);
		for (int message = 0; message < 3; message++) {
			REQUIRE(box.SendTo("channel.retune.victim", "first", Bytes("x")).Expected());
		}
	});
	universe.Tick(1.0f / 60.0f);

	CHECK(Received(universe, victim).size() == 2);

	const std::vector<Delivery> replies = Received(universe, sender);
	REQUIRE(replies.size() == 3);
	CHECK(replies[1].Status == BusStatus::Ok);
	CHECK(replies[2].Status == BusStatus::Overflow);
}

TEST_CASE("what a world holds is what it comes back holding", "[world]") {
	// The cap is a count of what is in the router's table, and a snapshot carries
	// that table - so the count has to be rebuilt from it. Otherwise saving and
	// loading is how a world gets a second allowance, and the entries it came back
	// with are the ones nothing will ever close.
	UniverseSettings settings;
	settings.ChannelsPerWorld = 2;

	Universe universe(settings);
	const WorldId holder = universe.Create(Named("channel.cap.saved"));
	universe.Enter(holder, [](Store &store) {
		Postbox box(store);
		REQUIRE(box.OpenChannel("cap.kept.a").Expected());
		REQUIRE(box.OpenChannel("cap.kept.b").Expected());
	});
	universe.Tick(1.0f / 60.0f);

	engine::core::ByteWriter writer;
	REQUIRE(universe.Save(writer));

	Universe restored(settings);
	engine::core::ByteReader reader(writer.Bytes());
	REQUIRE(restored.Load(reader));

	const WorldId back = restored.Find(Name("channel.cap.saved"));
	REQUIRE(back.IsValid());
	restored.Enter(back, [](Store &store) { REQUIRE(Postbox(store).OpenChannel("cap.kept.c").Expected()); });
	restored.Tick(1.0f / 60.0f);

	const std::vector<Delivery> verdicts = Received(restored, back);
	REQUIRE(verdicts.size() == 1);
	CHECK(verdicts[0].Status == BusStatus::TooManyChannels);
}

TEST_CASE("two senders into one receiver arrive in sender-name order", "[world]") {
	// **The ordering rule, and the sort key it turns on.** Traffic is applied in
	// `(From.Text(), Sequence)` order - both recorded in the envelope, neither a
	// function of which worker claimed a world or of the order the registry
	// happened to be walked in.
	//
	// **The names below are chosen so interning order is the reverse of name
	// order**, which is what makes this catch the bug it was written for: the
	// router sorted on `core::Name::Id()` until v0.17, and an id is handed out in
	// interning order. A universe restored from a snapshot interns in file order
	// where the run that wrote it interned in creation order, so the same two
	// envelopes applied in the opposite order and a replay diverged from its
	// recording. Under the id, "zulu" - interned first here - arrives first.
	const auto arrivalsInto = [](WorldId first, WorldId second, Universe &universe, WorldId sink) {
		universe.Enter(sink, [](Store &store) { REQUIRE(Postbox(store).OpenChannel("race").Expected()); });
		universe.Tick(1.0f / 60.0f);

		universe.Enter(first, [](Store &store) {
			REQUIRE(Postbox(store).SendTo("order.sink", "race", Bytes("from-zulu")).Expected());
		});
		universe.Enter(second, [](Store &store) {
			REQUIRE(Postbox(store).SendTo("order.sink", "race", Bytes("from-alpha")).Expected());
		});
		universe.Tick(1.0f / 60.0f);

		std::vector<std::string> order;
		for (const Delivery &delivery : Received(universe, sink)) {
			if (delivery.Bus == BusKind::Channel) {
				order.push_back(Text(delivery.Payload));
			}
		}
		return order;
	};

	// Named - and therefore interned - zulu first, alpha second.
	Universe universe;
	const WorldId zulu = universe.Create(Named("order.zulu"));
	const WorldId alpha = universe.Create(Named("order.alpha"));
	const WorldId sink = universe.Create(Named("order.sink"));

	const std::vector<std::string> once = arrivalsInto(zulu, alpha, universe, sink);
	REQUIRE(once.size() == 2);
	CHECK(once[0] == "from-alpha");
	CHECK(once[1] == "from-zulu");

	// **Run twice, and the second universe creates its worlds in the opposite
	// order.** Registry index decides which outbox the barrier collects first, so
	// a comparator that was not a total order over recorded data - or a barrier
	// that skipped the sort - would answer differently here and identically above.
	Universe again;
	const WorldId sinkAgain = again.Create(Named("order.sink"));
	const WorldId alphaAgain = again.Create(Named("order.alpha"));
	const WorldId zuluAgain = again.Create(Named("order.zulu"));

	CHECK(arrivalsInto(zuluAgain, alphaAgain, again, sinkAgain) == once);
}

TEST_CASE("an open channel survives a snapshot", "[world]") {
	// Otherwise a restored universe answers `NoSuchChannel` to every addressed
	// send until each world happens to open its channels again - which for a
	// world whose open ran once at startup is never. A topic subscription has
	// been carried since v0.2 for the same reason.
	Universe universe;
	const WorldId listener = universe.Create(Named("channel.saved.listener"));
	universe.Create(Named("channel.saved.sender"));

	universe.Enter(listener, [](Store &store) { REQUIRE(Postbox(store).OpenChannel("kept").Expected()); });
	universe.Tick(1.0f / 60.0f);

	engine::core::ByteWriter writer;
	REQUIRE(universe.Save(writer));

	Universe restored;
	engine::core::ByteReader reader(writer.Bytes());
	REQUIRE(restored.Load(reader));

	const WorldId sender = restored.Find(Name("channel.saved.sender"));
	REQUIRE(sender.IsValid());
	restored.Enter(sender, [](Store &store) {
		REQUIRE(Postbox(store).SendTo("channel.saved.listener", "kept", Bytes("x")).Expected());
	});
	restored.Tick(1.0f / 60.0f);

	const std::vector<Delivery> replies = Received(restored, sender);
	REQUIRE(replies.size() == 1);
	CHECK(replies[0].Status == BusStatus::Ok);
}
