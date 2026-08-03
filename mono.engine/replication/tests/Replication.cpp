// A server, a client, and the world going from one to the other.
//
// No transport here on purpose. `Authority` produces messages and `Replica`
// consumes them, so the whole of replication can be driven by handing byte
// vectors from one to the other — which is what lets these cases run in
// microseconds and what makes a lost or reordered message something a test
// *states* rather than something it waits for.

#include <engine/core/Random.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/replication/Authority.hpp>
#include <engine/replication/Prediction.hpp>
#include <engine/replication/Replica.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.replication.stream")

using engine::core::Name;
using engine::core::Random;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::replication::ApplyStatus;
using engine::replication::Authority;
using engine::replication::AuthoritySettings;
using engine::replication::ClientId;
using engine::replication::Prediction;
using engine::replication::Replica;

namespace replication_test {
	struct Spot {
		float X = 0.0f;
		float Y = 0.0f;
	};
	struct Secret {
		int Value = 0;
	};
	struct Marked {
		uint8_t Value = 0;
	};

	// Registered once for the binary. Component ids are process-wide and a
	// second registration of one type under one name is what `Components`
	// aborts on.
	void RegisterTypes() {
		static bool once = [] {
			engine::ecs::Components::Register<Spot>("replication_test.Spot");
			engine::ecs::Components::Register<Secret>("replication_test.Secret");
			engine::ecs::Components::Register<Marked>("replication_test.Marked");
			return true;
		}();
		(void)once;
	}

	std::vector<std::byte> Bytes(std::string_view text) {
		std::vector<std::byte> payload(text.size());
		if (!text.empty()) {
			std::memcpy(payload.data(), text.data(), text.size());
		}
		return payload;
	}

	// A server and a client wired together, with the world on the server side.
	struct Pair {
		Pair() : Server("server"), Client("client") {
			RegisterTypes();
			Authority_.Replicate(Name("replication_test.Spot"));
			Server.Observe<Spot>();
		}

		// One tick: publish, deliver everything, acknowledge.
		//
		// `drop` decides which messages are lost, by index within the tick —
		// an unreliable transport is the normal case, not the exception.
		void Tick(const std::function<bool(size_t)> &drop = {}) {
			Now++;
			Authority_.Publish(Server, Now);

			size_t index = 0;
			for (const std::vector<std::byte> &message : Authority_.Outgoing(Handle)) {
				const bool lost = drop && drop(index++);
				if (!lost) {
					Replica_.Receive(Client, message);
				}
			}

			// The world's change bits are the delta source, so they are cleared
			// after publishing and not before — clearing first is how a tick's
			// worth of movement goes missing.
			Server.ClearChanges();

			const std::vector<std::byte> ack = Replica_.Acknowledge();
			if (!ack.empty()) {
				Authority_.Receive(Handle, ack);
			}
		}

		// Ticks until the client has joined, or gives up.
		bool Join(int limit = 64) {
			for (int attempt = 0; attempt < limit && !Replica_.Joined(); attempt++) {
				Tick();
			}
			return Replica_.Joined();
		}

		Store Server;
		Store Client;
		Authority Authority_;
		Replica Replica_;
		ClientId Handle = Authority_.Admit();
		uint64_t Now = 0;
	};
}

using namespace replication_test;

// --- joining -----------------------------------------------------------------

TEST_CASE("a client joins by full snapshot and sees the world", "[replication]") {
	Pair pair;

	std::vector<Entity> entities;
	for (int index = 0; index < 24; index++) {
		const Entity entity = pair.Server.Create();
		pair.Server.Set<Spot>(entity, Spot{static_cast<float>(index), 0.0f});
		entities.push_back(entity);
	}

	REQUIRE(pair.Join());
	REQUIRE(pair.Replica_.Stats().Snapshots == 1);

	// The same entities, by the same handles — which only holds because the
	// snapshot reproduces the directory exactly, index and generation alike.
	for (int index = 0; index < 24; index++) {
		const Entity entity = entities[static_cast<size_t>(index)];
		REQUIRE(pair.Client.Alive(entity));
		REQUIRE(pair.Client.Get<Spot>(entity) != nullptr);
		REQUIRE(pair.Client.Get<Spot>(entity)->X == static_cast<float>(index));
	}
}

TEST_CASE("a big world joins over several ticks rather than one", "[replication]") {
	// A ten-megabyte world sent in one tick is ten megabytes into a link sized
	// for a few kilobytes. Spreading the join costs the joiner a moment and
	// costs everybody else nothing.
	AuthoritySettings settings;
	settings.ChunkBytes = 256;
	settings.ChunksPerTick = 2;

	Pair pair;
	pair.Authority_ = Authority(settings);
	pair.Handle = pair.Authority_.Admit();
	pair.Authority_.Replicate(Name("replication_test.Spot"));

	for (int index = 0; index < 400; index++) {
		pair.Server.Set<Spot>(pair.Server.Create(), Spot{static_cast<float>(index), 0.0f});
	}

	pair.Tick();
	REQUIRE_FALSE(pair.Replica_.Joined());
	REQUIRE(pair.Replica_.SnapshotOutstanding() > 0);

	REQUIRE(pair.Join(512));
	REQUIRE(pair.Replica_.SnapshotOutstanding() == 0);
}

TEST_CASE("an entity with no replicated component is not in the snapshot", "[replication]") {
	// **A row with nothing in it still says how many entities the world holds.**
	// Interest filters entities and `Replicate` filters components, and the
	// entity that passed the first and had nothing left after the second used to
	// cross as a bare row — no data, and a count of a world the client was never
	// told it could see.
	//
	// Counted rather than inspected: what is being asserted is the *number* of
	// rows, because the number is the thing that leaked.
	Pair pair;

	std::vector<Entity> shown;
	for (int index = 0; index < 6; index++) {
		const Entity entity = pair.Server.Create();
		pair.Server.Set<Spot>(entity, Spot{static_cast<float>(index), 0.0f});
		shown.push_back(entity);
	}

	// Twelve with nothing replicated on them at all, and six carrying only a
	// component nobody opted in to. Both are entities a client may not know
	// about, and neither has a value to send.
	for (int index = 0; index < 12; index++) {
		pair.Server.Create();
	}
	for (int index = 0; index < 6; index++) {
		pair.Server.Set<Secret>(pair.Server.Create(), Secret{index});
	}

	REQUIRE(pair.Join());

	size_t rows = 0;
	pair.Client.EachEntity([&rows](Entity) { rows++; });
	CHECK(rows == shown.size());

	// The server agrees about what it told them, which is what stops a later
	// delta naming a row the client does not hold.
	CHECK(pair.Authority_.StatusOf(pair.Handle).Known == shown.size());

	for (const Entity entity : shown) {
		CHECK(pair.Client.Alive(entity));
	}
}

TEST_CASE("an entity becomes visible when it gains a replicated component", "[replication]") {
	// The other half of the case above, and the reason it is not simply a
	// filter on the join: an entity with nothing to send is *not yet* something
	// to send, rather than something excluded for ever.
	Pair pair;
	pair.Server.Set<Spot>(pair.Server.Create(), Spot{0.0f, 0.0f});

	const Entity later = pair.Server.Create();
	REQUIRE(pair.Join());

	size_t rows = 0;
	pair.Client.EachEntity([&rows](Entity) { rows++; });
	REQUIRE(rows == 1);
	REQUIRE_FALSE(pair.Client.Alive(later));

	pair.Server.Set<Spot>(later, Spot{9.0f, 0.0f});
	pair.Tick();

	CHECK(pair.Client.Alive(later));
	CHECK(pair.Client.Get<Spot>(later)->X == 9.0f);
}

TEST_CASE("a client is not acknowledged before it has joined", "[replication]") {
	// A client that acknowledged a tick it had not applied would stop the
	// server sending the very thing it is still waiting for.
	Pair pair;
	pair.Server.Set<Spot>(pair.Server.Create(), Spot{1.0f, 0.0f});

	REQUIRE(pair.Replica_.Acknowledge().empty());
	REQUIRE(pair.Replica_.Applied() == 0);
}

// --- streaming ----------------------------------------------------------------

TEST_CASE("only what changed is sent after the join", "[replication]") {
	Pair pair;

	std::vector<Entity> entities;
	for (int index = 0; index < 50; index++) {
		const Entity entity = pair.Server.Create();
		pair.Server.Set<Spot>(entity, Spot{static_cast<float>(index), 0.0f});
		entities.push_back(entity);
	}
	REQUIRE(pair.Join());

	// A quiet tick sends nothing at all.
	pair.Tick();
	REQUIRE(pair.Authority_.Stats().Messages == 0);

	// One entity moves, so one delta goes out.
	pair.Server.GetMutable<Spot>(entities[7])->X = 999.0f;
	pair.Tick();

	REQUIRE(pair.Client.Get<Spot>(entities[7])->X == 999.0f);
	REQUIRE(pair.Replica_.Stats().Deltas == 1);

	// And nothing else moved with it.
	REQUIRE(pair.Client.Get<Spot>(entities[8])->X == 8.0f);
}

TEST_CASE("an entity created after the join reaches the client", "[replication]") {
	Pair pair;
	pair.Server.Set<Spot>(pair.Server.Create(), Spot{1.0f, 0.0f});
	REQUIRE(pair.Join());

	const Entity late = pair.Server.Create();
	pair.Server.Set<Spot>(late, Spot{42.0f, 0.0f});
	pair.Tick();

	REQUIRE(pair.Client.Alive(late));
}

TEST_CASE("an entity destroyed on the server is destroyed on the client", "[replication]") {
	Pair pair;
	const Entity doomed = pair.Server.Create();
	pair.Server.Set<Spot>(doomed, Spot{1.0f, 0.0f});
	REQUIRE(pair.Join());
	REQUIRE(pair.Client.Alive(doomed));

	pair.Server.Destroy(doomed);
	pair.Tick();

	REQUIRE_FALSE(pair.Client.Alive(doomed));
}

TEST_CASE("a component nobody replicated never crosses", "[replication]") {
	// Opt in, not opt out. A world holds components no client has any business
	// receiving, and a default of "everything" makes leaking one the
	// consequence of forgetting rather than of deciding.
	Pair pair;
	pair.Server.Observe<Secret>();

	const Entity entity = pair.Server.Create();
	pair.Server.Set<Spot>(entity, Spot{1.0f, 0.0f});
	REQUIRE(pair.Join());

	pair.Server.Set<Secret>(entity, Secret{9000});
	pair.Tick();

	REQUIRE(pair.Client.Alive(entity));
	REQUIRE_FALSE(pair.Authority_.Replicated(Name("replication_test.Secret")));
	REQUIRE(pair.Client.Get<Secret>(entity) == nullptr);
}

// --- interest ------------------------------------------------------------------

TEST_CASE("a client is only sent what it may see", "[replication]") {
	Pair pair;

	std::vector<Entity> entities;
	for (int index = 0; index < 20; index++) {
		const Entity entity = pair.Server.Create();
		pair.Server.Set<Spot>(entity, Spot{static_cast<float>(index), 0.0f});
		entities.push_back(entity);
	}

	// Only the first ten.
	std::vector<uint64_t> allowed;
	for (int index = 0; index < 10; index++) {
		allowed.push_back(entities[static_cast<size_t>(index)].Id);
	}
	pair.Authority_.SetInterest([allowed](ClientId, Entity entity) {
		return std::find(allowed.begin(), allowed.end(), entity.Id) != allowed.end();
	});

	REQUIRE(pair.Join());
	REQUIRE(pair.Authority_.StatusOf(pair.Handle).Known == 10);
}

TEST_CASE("losing sight of an entity is a forget, never a destroy", "[replication]") {
	// A client that conflated the two would delete something still there and
	// then be wrong about it the moment it came back into view.
	Pair pair;

	const Entity watched = pair.Server.Create();
	pair.Server.Set<Spot>(watched, Spot{1.0f, 0.0f});
	pair.Server.Set<Spot>(pair.Server.Create(), Spot{2.0f, 0.0f});

	bool visible = true;
	pair.Authority_.SetInterest([&visible, watched](ClientId, Entity entity) {
		return entity != watched || visible;
	});

	REQUIRE(pair.Join());
	REQUIRE(pair.Client.Alive(watched));

	visible = false;
	pair.Tick();

	// Told to forget it, and told nothing about it being destroyed.
	REQUIRE(pair.Replica_.Forgotten().size() == 1);
	REQUIRE(pair.Replica_.Forgotten()[0] == watched);
	REQUIRE(pair.Server.Alive(watched));
}

TEST_CASE("a forget too big for one datagram is split", "[replication]") {
	// **The same rule as a snapshot and a delta, and the path that missed it.**
	// A world going out of view all at once names every entity in one message,
	// and three hundred handles is well past a datagram — which `Link::Reserve`
	// refuses outright rather than fragmenting, so the one message that says
	// "stop drawing these" would be the one that never arrives and the client
	// would draw a world that is no longer there.
	AuthoritySettings settings;

	Pair pair;
	pair.Authority_ = Authority(settings);
	pair.Handle = pair.Authority_.Admit();
	pair.Authority_.Replicate(Name("replication_test.Spot"));

	for (int index = 0; index < 300; index++) {
		pair.Server.Set<Spot>(pair.Server.Create(), Spot{static_cast<float>(index), 0.0f});
	}

	bool visible = true;
	pair.Authority_.SetInterest([&visible](ClientId, Entity) { return visible; });

	REQUIRE(pair.Join(256));
	REQUIRE(pair.Authority_.StatusOf(pair.Handle).Known == 300);

	visible = false;
	pair.Tick();

	// Every handle arrived, and no message it arrived in could have been
	// refused for being oversized.
	REQUIRE(pair.Replica_.Forgotten().size() == 300);
	for (const std::vector<std::byte> &message : pair.Authority_.Outgoing(pair.Handle)) {
		REQUIRE(message.size() <= settings.ChunkBytes);
	}
}

// --- what a hostile peer sends ---------------------------------------------------

TEST_CASE("a malformed message is refused rather than partly applied", "[replication]") {
	Pair pair;
	pair.Server.Set<Spot>(pair.Server.Create(), Spot{1.0f, 0.0f});
	REQUIRE(pair.Join());

	std::vector<std::byte> rubbish(96);
	for (size_t index = 0; index < rubbish.size(); index++) {
		rubbish[index] = static_cast<std::byte>(Random::Bits(3u, static_cast<uint32_t>(index)));
	}

	REQUIRE(pair.Replica_.Receive(pair.Client, rubbish) == ApplyStatus::Malformed);
	REQUIRE_FALSE(pair.Authority_.Receive(pair.Handle, rubbish));
	REQUIRE(pair.Authority_.Stats().Refused == 1);
}

TEST_CASE("a client cannot tell the server what the world is", "[replication]") {
	// The whole thing authority means. A client sending a delta is a client
	// trying to decide the world, and the server refuses it as malformed
	// rather than merging it.
	Pair pair;
	REQUIRE(pair.Join());

	engine::core::ByteWriter writer;
	engine::replication::Delta delta;
	delta.Tick = 99;
	WriteMessage(writer, delta);

	REQUIRE_FALSE(pair.Authority_.Receive(pair.Handle, writer.Bytes()));
}

TEST_CASE("a truncated message is refused at every length", "[replication]") {
	engine::core::ByteWriter writer;
	engine::replication::Delta delta;
	delta.Tick = 7;
	delta.Created.push_back(Entity{123});
	WriteMessage(writer, delta);

	const std::span<const std::byte> whole = writer.Bytes();
	for (size_t length = 0; length < whole.size(); length++) {
		engine::core::ByteReader reader(whole.subspan(0, length));
		engine::replication::Message message;
		REQUIRE_FALSE(ReadMessage(reader, message));
	}
}

TEST_CASE("a stale delta is ignored without being an error", "[replication]") {
	// An unreliable transport reorders and the newer state is already applied.
	// Not a reason to disconnect.
	Pair pair;
	const Entity entity = pair.Server.Create();
	pair.Server.Set<Spot>(entity, Spot{1.0f, 0.0f});
	REQUIRE(pair.Join());

	pair.Server.GetMutable<Spot>(entity)->X = 5.0f;
	pair.Tick();

	engine::core::ByteWriter writer;
	engine::replication::Delta old;
	old.Tick = 1;
	WriteMessage(writer, old);

	REQUIRE(pair.Replica_.Receive(pair.Client, writer.Bytes()) == ApplyStatus::Stale);
	REQUIRE(pair.Client.Get<Spot>(entity)->X == 5.0f);
}

// --- prediction ------------------------------------------------------------------

TEST_CASE("reconciliation retires what the server consumed and keeps the rest", "[replication]") {
	Prediction prediction;
	for (uint64_t tick = 1; tick <= 10; tick++) {
		prediction.Record(tick, Bytes("input"));
	}
	REQUIRE(prediction.Ahead() == 10);

	REQUIRE(prediction.Reconcile(6) == 6);
	REQUIRE(prediction.Ahead() == 4);

	// What is left is exactly what has to be replayed to arrive back at the
	// present — the inputs *after* the acknowledged tick, oldest first.
	REQUIRE(prediction.Pending()[0].Tick == 7);
	REQUIRE(prediction.Pending().back().Tick == 10);
}

TEST_CASE("reconciling a tick nobody sent retires nothing", "[replication]") {
	Prediction prediction;
	prediction.Record(5, Bytes("a"));
	prediction.Record(6, Bytes("b"));

	REQUIRE(prediction.Reconcile(1) == 0);
	REQUIRE(prediction.Ahead() == 2);
}

TEST_CASE("the prediction buffer is bounded and says when it overflowed", "[replication]") {
	// The server may simply stop acknowledging, and an unbounded history is a
	// memory leak driven by the other end.
	engine::replication::PredictionSettings settings;
	settings.MaximumPending = 4;

	Prediction prediction(settings);
	for (uint64_t tick = 1; tick <= 10; tick++) {
		prediction.Record(tick, Bytes("input"));
	}

	REQUIRE(prediction.Ahead() == 4);
	REQUIRE(prediction.Dropped() == 6);

	// The oldest went, not the newest — the newest is the input the player just
	// made, and it is the one they can see not happening.
	REQUIRE(prediction.Pending().front().Tick == 7);
	REQUIRE(prediction.Pending().back().Tick == 10);
}

TEST_CASE("input reaches the server and is handed over once", "[replication]") {
	Pair pair;
	REQUIRE(pair.Join());

	engine::core::ByteWriter writer;
	WriteMessage(writer, engine::replication::Input{4, Bytes("jump")});
	REQUIRE(pair.Authority_.Receive(pair.Handle, writer.Bytes()));

	REQUIRE(pair.Authority_.Inputs(pair.Handle).size() == 1);
	REQUIRE(pair.Authority_.Inputs(pair.Handle)[0].Tick == 4);

	pair.Authority_.ClearInputs(pair.Handle);
	REQUIRE(pair.Authority_.Inputs(pair.Handle).empty());
}

// --- clients ---------------------------------------------------------------------

TEST_CASE("a dropped client's handle stops resolving", "[replication]") {
	Pair pair;
	REQUIRE(pair.Authority_.Holds(pair.Handle));

	REQUIRE(pair.Authority_.Remove(pair.Handle));
	REQUIRE_FALSE(pair.Authority_.Holds(pair.Handle));
	REQUIRE_FALSE(pair.Authority_.Remove(pair.Handle));

	// The slot is reused and the generation moves, so the old handle does not
	// start naming the new client.
	const ClientId next = pair.Authority_.Admit();
	REQUIRE(next.Index == pair.Handle.Index);
	REQUIRE_FALSE(next == pair.Handle);
	REQUIRE_FALSE(pair.Authority_.Holds(pair.Handle));
	REQUIRE(pair.Authority_.Holds(next));
}

// --- the whole thing, under loss --------------------------------------------------

TEST_CASE("a client converges however much is lost on the way", "[replication][fuzz]") {
	// The property the whole layer exists for, stated as a differential test:
	// whatever is dropped or reordered, a client that keeps receiving ends up
	// agreeing with the server.
	Pair pair;

	std::vector<Entity> entities;
	for (int index = 0; index < 40; index++) {
		const Entity entity = pair.Server.Create();
		pair.Server.Set<Spot>(entity, Spot{static_cast<float>(index), 0.0f});
		entities.push_back(entity);
	}
	REQUIRE(pair.Join());

	for (uint32_t round = 0; round < 300; round++) {
		// The server moves some of the world.
		for (size_t index = 0; index < entities.size(); index++) {
			if (Random::Bits(round, static_cast<uint32_t>(index)) % 3 == 0) {
				pair.Server.GetMutable<Spot>(entities[index])->X = static_cast<float>(round * 10 + index);
			}
		}

		// And the link drops a third of what it is given.
		pair.Tick([round](size_t message) {
			return Random::Bits(round, 900u + static_cast<uint32_t>(message)) % 3 == 0;
		});
	}

	// A last clean tick, with everything moving and nothing lost. A delta only
	// carries what changed, so convergence is a property of the *stream*: once
	// every row has moved once without loss, the two agree.
	for (size_t index = 0; index < entities.size(); index++) {
		pair.Server.GetMutable<Spot>(entities[index])->X = 7777.0f;
	}
	pair.Tick();

	for (const Entity entity : entities) {
		REQUIRE(pair.Client.Get<Spot>(entity) != nullptr);
		REQUIRE(pair.Client.Get<Spot>(entity)->X == 7777.0f);
	}
}

// --- the acknowledged baseline -----------------------------------------------
//
// A delta built from the dirty bits alone describes exactly one tick. That is
// correct only while nothing is lost: an entity that moved on a tick whose
// datagram went missing and then stopped moving is wrong on that client
// forever, because no later delta mentions it and the client is acknowledging
// happily so nothing re-snapshots it. Carrying the unconfirmed entries forward
// makes the stream converge instead.

TEST_CASE("a value lost in transit is resent until it is confirmed", "[replication]") {
	Pair pair;

	const Entity entity = pair.Server.Create();
	pair.Server.Set<Spot>(entity, Spot{1.0f, 1.0f});
	REQUIRE(pair.Join());
	REQUIRE(pair.Replica_.Applied() > 0);

	// One move, and then the entity is still for the rest of the test. This is
	// the case that used to be unrecoverable — a change that happens once and
	// whose only delta is dropped.
	pair.Server.Set<Spot>(entity, Spot{9.0f, 9.0f});
	pair.Tick([](size_t) { return true; });

	REQUIRE(pair.Client.Get<Spot>(entity)->X == 1.0f);

	// Nothing changes on the server from here. The value still has to arrive.
	for (int tick = 0; tick < 4; tick++) {
		pair.Tick();
	}

	REQUIRE(pair.Client.Get<Spot>(entity)->X == 9.0f);
	REQUIRE(pair.Client.Get<Spot>(entity)->Y == 9.0f);
}

TEST_CASE("a confirmed value stops being resent", "[replication]") {
	Pair pair;

	const Entity entity = pair.Server.Create();
	pair.Server.Set<Spot>(entity, Spot{1.0f, 1.0f});
	REQUIRE(pair.Join());

	pair.Server.Set<Spot>(entity, Spot{5.0f, 5.0f});
	pair.Tick();
	REQUIRE(pair.Client.Get<Spot>(entity)->X == 5.0f);

	// Acknowledged, so the entry retires. A world where nothing is moving has
	// to fall silent — a baseline that never retired would turn every delta
	// into a full world update forever, which is the opposite of what carrying
	// one is for.
	pair.Tick();
	REQUIRE(pair.Authority_.Outgoing(pair.Handle).empty());
}

TEST_CASE("a run of losses still converges", "[replication]") {
	Pair pair;

	std::vector<Entity> entities;
	for (int index = 0; index < 24; index++) {
		const Entity entity = pair.Server.Create();
		pair.Server.Set<Spot>(entity, Spot{0.0f, 0.0f});
		entities.push_back(entity);
	}
	REQUIRE(pair.Join());

	// Move everything, then lose ten consecutive ticks' worth of deltas. A
	// client this far behind is still not behind enough to be re-snapshotted,
	// which is exactly the window where a one-tick delta leaves it wrong.
	for (size_t index = 0; index < entities.size(); index++) {
		pair.Server.Set<Spot>(entities[index], Spot{static_cast<float>(index), 2.0f});
	}
	for (int tick = 0; tick < 10; tick++) {
		pair.Tick([](size_t) { return true; });
	}

	for (int tick = 0; tick < 4; tick++) {
		pair.Tick();
	}

	for (size_t index = 0; index < entities.size(); index++) {
		REQUIRE(pair.Client.Get<Spot>(entities[index])->X == static_cast<float>(index));
		REQUIRE(pair.Client.Get<Spot>(entities[index])->Y == 2.0f);
	}
}
