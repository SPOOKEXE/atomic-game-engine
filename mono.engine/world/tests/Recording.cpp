#include <engine/core/Name.hpp>
#include <engine/core/Random.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Postbox.hpp>
#include <engine/world/Recording.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.world.recording")

using engine::core::ByteReader;
using engine::core::ByteWriter;
using engine::core::Name;
using engine::core::Random;
using engine::ecs::Entity;
using engine::ecs::Phase;
using engine::ecs::Scheduler;
using engine::ecs::Store;
using engine::parallel::Jobs;
using engine::world::BusKind;
using engine::world::BusStatus;
using engine::world::Postbox;
using engine::world::Recorder;
using engine::world::Replayer;
using engine::world::Universe;
using engine::world::WorldId;
using engine::world::WorldSettings;
using engine::world::WorldState;

namespace recording_test {
	struct Pool {
		explicit Pool(unsigned workers) {
			Jobs::Start(workers);
		}
		~Pool() {
			Jobs::Stop();
		}
	};

	struct Tally {
		int64_t Value = 0;
	};

	WorldSettings Named(const char *name, double rate = 60.0) {
		WorldSettings settings;
		settings.Name = Name(name);
		settings.TickRate = rate;
		return settings;
	}

	std::vector<std::byte> Bytes(std::string_view text) {
		std::vector<std::byte> payload(text.size());
		std::memcpy(payload.data(), text.data(), text.size());
		return payload;
	}

	std::string Text(std::span<const std::byte> payload) {
		return std::string(reinterpret_cast<const char *>(payload.data()), payload.size());
	}

	// Registers the behaviour, without touching storage.
	//
	// Separate from Build because a restore needs exactly this half: the
	// entities came back in the snapshot, the systems did not.
	void Rebuild(Universe &universe, WorldId id, const char *topic) {
		universe.Enter(id, [topic](Store &, Scheduler &systems) {
			systems.Add("count", Phase::Simulation, [topic](Store &world) {
				world.Each<Tally>([](Entity, Tally &tally) { tally.Value++; });

				Postbox box(world);
				box.Publish(topic, Bytes(std::to_string(world.Time().Tick)));
				box.Set(BusKind::MemoryStore, "last.tick", Bytes(std::to_string(world.Time().Tick)));
			});
		});
	}

	// A world that counts its own ticks and shouts about it.
	void Build(Universe &universe, WorldId id, const char *topic) {
		universe.Enter(id, [](Store &store) { store.Set<Tally>(store.Create(), Tally{0}); });
		Rebuild(universe, id, topic);
	}

	int64_t TallyIn(Universe &universe, WorldId id) {
		int64_t total = 0;
		universe.Enter(id, [&total](Store &store) {
			store.Each<const Tally>([&total](Entity, const Tally &tally) { total += tally.Value; });
		});
		return total;
	}

	// Everything a universe holds that a test can compare.
	std::string Fingerprint(Universe &universe) {
		std::string print;
		for (const WorldId id : universe.Worlds()) {
			print += std::string(universe.NameOf(id).Text());
			print += ":" + std::to_string(universe.StatisticsOf(id).Ticks);
			print += ":" + std::to_string(TallyIn(universe, id));
			print += ";";
		}

		std::vector<std::byte> value;
		if (universe.Peek(BusKind::MemoryStore, Name("last.tick"), &value) == BusStatus::Ok) {
			print += "mem=" + Text(value);
		}
		return print;
	}
}

using namespace recording_test;

// --- universe snapshots ---------------------------------------------------

TEST_CASE("an empty universe round-trips", "[world]") {
	Universe source;
	ByteWriter writer;
	REQUIRE(source.Save(writer));

	Universe restored;
	ByteReader reader(writer.Bytes());
	REQUIRE(restored.Load(reader));
	REQUIRE(restored.Count() == 0);
}

TEST_CASE("worlds, their state and their storage all come back", "[world]") {
	Universe source;
	const WorldId busy = source.Create(Named("snap.busy", 60.0));
	const WorldId dozing = source.Create(Named("snap.dozing", 30.0));
	REQUIRE(source.SetRenderingProfile(busy, Name("Cinematic")) == engine::world::WorldStatus::Ok);

	Build(source, busy, "snap.topic");
	Build(source, dozing, "snap.topic");
	source.SetState(dozing, WorldState::Suspended);

	for (int frame = 0; frame < 10; frame++) {
		source.Tick(1.0f / 60.0f);
	}

	ByteWriter writer;
	REQUIRE(source.Save(writer));

	Universe restored;
	ByteReader reader(writer.Bytes());
	REQUIRE(restored.Load(reader));

	REQUIRE(restored.Count() == 2);

	const WorldId restoredBusy = restored.Find(Name("snap.busy"));
	REQUIRE(restoredBusy.IsValid());
	REQUIRE(restored.StatisticsOf(restoredBusy).Ticks == source.StatisticsOf(busy).Ticks);
	REQUIRE(TallyIn(restored, restoredBusy) == TallyIn(source, busy));
	REQUIRE(restored.SettingsOf(restoredBusy).RenderingProfile == Name("Cinematic"));

	// State came back too, so a suspended world does not silently wake up.
	REQUIRE(restored.StateOf(restored.Find(Name("snap.dozing"))) == WorldState::Suspended);
}

TEST_CASE("bus state comes back", "[world]") {
	Universe source;
	const WorldId writer_ = source.Create(Named("snap.writer"));
	const WorldId listener = source.Create(Named("snap.listener"));

	source.Enter(listener, [](Store &store) { Postbox(store).Subscribe("snap.news"); });
	source.Enter(writer_, [](Store &store) {
		Postbox box(store);
		box.Set(BusKind::MemoryStore, "snap.key", Bytes("memory"));
		box.Set(BusKind::DataStore, "snap.record", Bytes("durable"));
		box.Push("snap.queue", Bytes("first"));
		box.Push("snap.queue", Bytes("second"));
	});
	source.Tick(1.0f / 60.0f);

	ByteWriter writer;
	REQUIRE(source.Save(writer));

	Universe restored;
	ByteReader reader(writer.Bytes());
	REQUIRE(restored.Load(reader));

	// Subscriptions were written by world *name*, so they resolve again in a
	// process that numbered its worlds differently.
	REQUIRE(restored.SubscriberCount(Name("snap.news")) == 1);

	std::vector<std::byte> value;
	REQUIRE(restored.Peek(BusKind::MemoryStore, Name("snap.key"), &value) == BusStatus::Ok);
	REQUIRE(Text(value) == "memory");

	REQUIRE(restored.Peek(BusKind::DataStore, Name("snap.record"), &value) == BusStatus::Ok);
	REQUIRE(Text(value) == "durable");

	// The queue survived in order, which a matchmaker restarted from a snapshot
	// depends on.
	const WorldId popper = restored.Find(Name("snap.writer"));
	restored.Enter(popper, [](Store &store) { Postbox(store).Pop("snap.queue"); });
	restored.Tick(1.0f / 60.0f);

	restored.Enter(popper, [](Store &store) {
		const Postbox box(store);
		REQUIRE(box.Deliveries().size() == 1);
		REQUIRE(Text(box.Deliveries()[0].Payload) == "first");
	});
}

TEST_CASE("a universe snapshot of a snapshot is the same snapshot", "[world]") {
	Universe source;
	const WorldId id = source.Create(Named("snap.stable"));
	Build(source, id, "snap.topic2");
	for (int frame = 0; frame < 5; frame++) {
		source.Tick(1.0f / 60.0f);
	}

	ByteWriter first;
	REQUIRE(source.Save(first));

	Universe restored;
	ByteReader reader(first.Bytes());
	REQUIRE(restored.Load(reader));

	ByteWriter second;
	REQUIRE(restored.Save(second));

	REQUIRE(first.Size() == second.Size());
	REQUIRE(std::equal(first.Bytes().begin(), first.Bytes().end(), second.Bytes().begin()));
}

TEST_CASE("a corrupt universe snapshot leaves nothing behind", "[world]") {
	Universe universe;
	universe.Create(Named("snap.doomed"));
	REQUIRE(universe.Count() == 1);

	ByteWriter writer;
	writer.WriteUInt64(0xBADD'CAFE'BADD'CAFEull);

	ByteReader reader(writer.Bytes());
	REQUIRE_FALSE(universe.Load(reader));
	REQUIRE(universe.Count() == 0);
}

TEST_CASE("a truncated universe snapshot leaves nothing behind", "[world]") {
	Universe source;
	source.Create(Named("snap.trunc.a"));
	source.Create(Named("snap.trunc.b"));
	Build(source, source.Find(Name("snap.trunc.a")), "snap.t");
	source.Tick(1.0f / 60.0f);

	ByteWriter writer;
	REQUIRE(source.Save(writer));

	size_t survived = 0;
	for (size_t length = 0; length < writer.Size(); length += 11) {
		std::vector<std::byte> cut(writer.Bytes().begin(), writer.Bytes().begin() + length);

		Universe target;
		ByteReader reader(cut);
		if (!target.Load(reader) && target.Count() != 0) {
			survived++;
		}
	}
	REQUIRE(survived == 0);
}

// --- recording ------------------------------------------------------------

TEST_CASE("a recording replays to the same final state", "[world]") {
	// The headline property. Anything that depended on thread scheduling
	// rather than on the data would make the replay diverge.
	Pool pool{4};

	Universe live;
	std::vector<WorldId> worlds;
	for (int index = 0; index < 6; index++) {
		const WorldId id = live.Create(Named(("rec.world." + std::to_string(index)).c_str(), 40.0 + index));
		Build(live, id, "rec.topic");
		worlds.push_back(id);
	}
	live.Enter(worlds[0], [](Store &store) { Postbox(store).Subscribe("rec.topic"); });

	Recorder recorder;
	REQUIRE(recorder.Begin(live));

	for (int frame = 0; frame < 60; frame++) {
		const float seconds = 1.0f / 60.0f;
		live.Tick(seconds);
		recorder.Capture(live, seconds);
	}
	REQUIRE(recorder.Barriers() == 60);

	const std::string expected = Fingerprint(live);

	ByteWriter writer;
	REQUIRE(recorder.Write(writer));

	Replayer replayer;
	ByteReader reader(writer.Bytes());
	REQUIRE(replayer.Load(reader));
	REQUIRE(replayer.Barriers() == 60);

	// Systems are re-registered on restore. A snapshot carries state, never
	// code, so a restored world ticks and does nothing until somebody says
	// what it does - exactly as a respawned host does by running the same
	// program.
	Universe replayed;
	REQUIRE(replayer.Restore(replayed, [](Universe &into, WorldId id) { Rebuild(into, id, "rec.topic"); }));
	REQUIRE(replayer.Run(replayed) == 60);

	REQUIRE(Fingerprint(replayed) == expected);
}

TEST_CASE("a recording begun mid-run replays from mid-run", "[world]") {
	Universe live;
	const WorldId id = live.Create(Named("rec.mid"));
	Build(live, id, "rec.mid.topic");

	for (int frame = 0; frame < 20; frame++) {
		live.Tick(1.0f / 60.0f);
	}

	Recorder recorder;
	REQUIRE(recorder.Begin(live));
	const uint64_t startedAt = live.StatisticsOf(id).Ticks;

	for (int frame = 0; frame < 10; frame++) {
		live.Tick(1.0f / 60.0f);
		recorder.Capture(live, 1.0f / 60.0f);
	}

	ByteWriter writer;
	recorder.Write(writer);

	Replayer replayer;
	ByteReader reader(writer.Bytes());
	REQUIRE(replayer.Load(reader));

	Universe replayed;
	REQUIRE(replayer.Restore(replayed, [](Universe &into, WorldId world) {
		Rebuild(into, world, "rec.mid.topic");
	}));

	// Restored to where recording began, not to nothing.
	const WorldId restored = replayed.Find(Name("rec.mid"));
	REQUIRE(replayed.StatisticsOf(restored).Ticks == startedAt);

	replayer.Run(replayed);
	REQUIRE(replayed.StatisticsOf(restored).Ticks == live.StatisticsOf(id).Ticks);
}

TEST_CASE("replaying twice from one recording gives the same answer twice", "[world]") {
	Pool pool{4};

	Universe live;
	for (int index = 0; index < 4; index++) {
		const WorldId id = live.Create(Named(("rec.twice." + std::to_string(index)).c_str()));
		Build(live, id, "rec.twice.topic");
	}

	Recorder recorder;
	recorder.Begin(live);
	for (int frame = 0; frame < 30; frame++) {
		live.Tick(1.0f / 60.0f);
		recorder.Capture(live, 1.0f / 60.0f);
	}

	ByteWriter writer;
	recorder.Write(writer);

	const auto play = [&writer] {
		Replayer replayer;
		ByteReader reader(writer.Bytes());
		REQUIRE(replayer.Load(reader));

		Universe universe;
		REQUIRE(replayer.Restore(universe, [](Universe &into, WorldId id) {
			Rebuild(into, id, "rec.twice.topic");
		}));
		replayer.Run(universe);
		return Fingerprint(universe);
	};

	REQUIRE(play() == play());
}

TEST_CASE("a rewound replayer plays the same recording again", "[world]") {
	Universe live;
	const WorldId id = live.Create(Named("rec.rewind"));
	Build(live, id, "rec.rewind.topic");

	Recorder recorder;
	recorder.Begin(live);
	for (int frame = 0; frame < 8; frame++) {
		live.Tick(1.0f / 60.0f);
		recorder.Capture(live, 1.0f / 60.0f);
	}

	ByteWriter writer;
	recorder.Write(writer);

	Replayer replayer;
	ByteReader reader(writer.Bytes());
	REQUIRE(replayer.Load(reader));

	const auto rebuild = [](Universe &into, WorldId world) { Rebuild(into, world, "rec.rewind.topic"); };

	Universe first;
	replayer.Restore(first, rebuild);
	REQUIRE(replayer.Run(first) == 8);
	REQUIRE(replayer.Remaining() == 0);

	replayer.Rewind();
	REQUIRE(replayer.Remaining() == 8);

	Universe second;
	replayer.Restore(second, rebuild);
	REQUIRE(replayer.Run(second) == 8);

	REQUIRE(Fingerprint(second) == Fingerprint(first));
}

TEST_CASE("stepping one barrier at a time matches running them all", "[world]") {
	Universe live;
	const WorldId id = live.Create(Named("rec.step"));
	Build(live, id, "rec.step.topic");

	Recorder recorder;
	recorder.Begin(live);
	for (int frame = 0; frame < 12; frame++) {
		live.Tick(1.0f / 60.0f);
		recorder.Capture(live, 1.0f / 60.0f);
	}

	ByteWriter writer;
	recorder.Write(writer);

	Replayer stepping;
	ByteReader first(writer.Bytes());
	stepping.Load(first);

	const auto rebuild = [](Universe &into, WorldId world) { Rebuild(into, world, "rec.step.topic"); };

	Universe stepped;
	stepping.Restore(stepped, rebuild);

	size_t barriers = 0;
	while (stepping.Step(stepped)) {
		barriers++;
	}
	REQUIRE(barriers == 12);

	Replayer running;
	ByteReader second(writer.Bytes());
	running.Load(second);

	Universe ran;
	running.Restore(ran, rebuild);
	running.Run(ran);

	REQUIRE(Fingerprint(stepped) == Fingerprint(ran));
}

TEST_CASE("bus traffic is reproduced rather than re-derived", "[world]") {
	// The point of recording the *applied* envelopes: a replayed world
	// re-derives the same requests, and applying both copies would double
	// every operation.
	Universe live;
	const WorldId id = live.Create(Named("rec.double"));
	Build(live, id, "rec.double.topic");

	Recorder recorder;
	recorder.Begin(live);
	for (int frame = 0; frame < 5; frame++) {
		live.Tick(1.0f / 60.0f);
		recorder.Capture(live, 1.0f / 60.0f);
	}

	const uint64_t liveOperations = live.Statistics().BusOperations;

	ByteWriter writer;
	recorder.Write(writer);

	Replayer replayer;
	ByteReader reader(writer.Bytes());
	replayer.Load(reader);

	Universe replayed;
	replayer.Restore(replayed, [](Universe &into, WorldId world) {
		Rebuild(into, world, "rec.double.topic");
	});
	replayer.Run(replayed);

	// Same count, not twice the count.
	REQUIRE(replayed.Statistics().BusOperations == liveOperations);
}

// --- refusals -------------------------------------------------------------

TEST_CASE("a recorder that was never begun writes nothing", "[world]") {
	Recorder recorder;
	REQUIRE_FALSE(recorder.Recording_());

	ByteWriter writer;
	REQUIRE_FALSE(recorder.Write(writer));
	REQUIRE(writer.Empty());

	// And capturing without beginning is a no-op rather than a crash.
	Universe universe;
	recorder.Capture(universe, 0.016f);
	REQUIRE(recorder.Barriers() == 0);
}

TEST_CASE("a stream that is not a recording is refused", "[world]") {
	ByteWriter writer;
	writer.WriteUInt64(0x0123'4567'89AB'CDEFull);
	writer.WriteUInt32(1);

	Replayer replayer;
	ByteReader reader(writer.Bytes());
	REQUIRE_FALSE(replayer.Load(reader));
	REQUIRE(replayer.Barriers() == 0);
}

TEST_CASE("a future recording version is refused rather than guessed at", "[world]") {
	Universe universe;
	universe.Create(Named("rec.version"));

	Recorder recorder;
	recorder.Begin(universe);

	ByteWriter writer;
	recorder.Write(writer);

	std::vector<std::byte> tampered(writer.Bytes().begin(), writer.Bytes().end());
	tampered[8] = static_cast<std::byte>(Recorder::VERSION + 1);

	Replayer replayer;
	ByteReader reader(tampered);
	REQUIRE_FALSE(replayer.Load(reader));
}

TEST_CASE("a truncated recording loads nothing", "[world]") {
	Universe universe;
	const WorldId id = universe.Create(Named("rec.cut"));
	Build(universe, id, "rec.cut.topic");

	Recorder recorder;
	recorder.Begin(universe);
	for (int frame = 0; frame < 6; frame++) {
		universe.Tick(1.0f / 60.0f);
		recorder.Capture(universe, 1.0f / 60.0f);
	}

	ByteWriter writer;
	recorder.Write(writer);

	size_t partial = 0;
	for (size_t length = 0; length < writer.Size(); length += 13) {
		std::vector<std::byte> cut(writer.Bytes().begin(), writer.Bytes().begin() + length);

		Replayer replayer;
		ByteReader reader(cut);
		if (!replayer.Load(reader) && replayer.Barriers() != 0) {
			partial++;
		}
	}
	REQUIRE(partial == 0);
}

TEST_CASE("random bytes never load as a recording", "[world]") {
	size_t loaded = 0;
	for (uint32_t iteration = 0; iteration < 400; iteration++) {
		const size_t size = Random::Bits(iteration, 811) % 128;
		std::vector<std::byte> buffer(size);
		for (size_t index = 0; index < size; index++) {
			buffer[index] = static_cast<std::byte>(Random::Bits(iteration, static_cast<uint32_t>(index)));
		}

		Replayer replayer;
		ByteReader reader(buffer);
		if (replayer.Load(reader) && replayer.Barriers() > 0) {
			loaded++;
		}
	}
	REQUIRE(loaded == 0);
}
