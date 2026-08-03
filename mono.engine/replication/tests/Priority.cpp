// What a client is sent first when not all of it fits.
//
// `Replication.cpp` covers what may be sent — the opt-in list and the interest
// predicate — and says nothing about the order, because until there was a
// per-client cap there was no order to speak of: a tick's delta went out as
// however many messages it took and `net::Link` refused the ones past its
// budget, so what got dropped was whatever happened to be last in the component
// list. `docs/DEFERRED.md` D00007 is about that being a policy nobody chose.
//
// These cases are about the policy that replaced it. No transport again, for
// the reason `Replication.cpp` gives: the budget is a number this suite sets,
// so "the link will carry one message and the world needs five" is a fact a
// case *states* rather than one it reproduces under load.
//
// **The two claims worth breaking on purpose.** Delete the rotation and the
// starvation cases must go red; delete the tie-break that makes the ordering a
// total order and the determinism cases must go red. Both were checked.

#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/replication/Authority.hpp>
#include <engine/replication/Replica.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.replication.priority")

using engine::core::ByteReader;
using engine::core::Name;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::replication::Authority;
using engine::replication::AuthoritySettings;
using engine::replication::ClientId;
using engine::replication::ComponentDelta;
using engine::replication::Message;
using engine::replication::MessageKind;
using engine::replication::ReadMessage;
using engine::replication::Replica;

namespace priority_test {
	// Eight bytes, so the arithmetic a case writes down is arithmetic a reader
	// can check: an entity costs its handle plus this.
	struct Mark {
		float X = 0.0f;
		float Y = 0.0f;
	};

	// A second replicated component, so that "last in the component list" is a
	// position something can actually be stuck in.
	struct Tint {
		float Shade = 0.0f;
	};

	void RegisterTypes() {
		static bool once = [] {
			engine::ecs::Components::Register<Mark>("priority_test.Mark");
			engine::ecs::Components::Register<Tint>("priority_test.Tint");
			return true;
		}();
		(void)once;
	}

	// What one tick put on the wire, decoded.
	struct Carried {
		// Every entity whose value went out, by component name.
		std::map<std::string, std::vector<uint64_t>> Values;

		// The same thing in wire order, which is what the ordering decision is
		// actually about — a map would sort the answer and hide it.
		std::vector<std::pair<std::string, uint64_t>> Sequence;

		// The messages themselves, for a byte-for-byte comparison.
		std::vector<std::vector<std::byte>> Messages;
	};

	// A server, a client, and a record of what crossed.
	struct Bench {
		explicit Bench(const AuthoritySettings &settings) : Server("server"), Client("client") {
			RegisterTypes();
			Authority_ = Authority(settings);
			Handle = Authority_.Admit();
			Authority_.Replicate(Name("priority_test.Mark"));
			Authority_.Replicate(Name("priority_test.Tint"));
			Server.Observe<Mark>();
			Server.Observe<Tint>();
		}

		// One tick, for one client: publish, deliver, acknowledge, and report
		// what went.
		Carried Tick(ClientId client) {
			Now++;
			Authority_.Publish(Server, Now);

			Carried carried;
			for (const std::vector<std::byte> &message : Authority_.Outgoing(client)) {
				carried.Messages.push_back(message);

				ByteReader reader(message);
				Message read;
				REQUIRE(ReadMessage(reader, read));
				if (read.Kind == MessageKind::Delta) {
					for (const ComponentDelta &component : read.Delta.Components) {
						const std::string named(component.Component.Text());
						std::vector<uint64_t> &into = carried.Values[named];
						for (const Entity entity : component.Entities) {
							into.push_back(entity.Id);
							carried.Sequence.emplace_back(named, entity.Id);
						}
					}
				}

				if (client == Handle) {
					Replica_.Receive(Client, message);
				}
			}

			Server.ClearChanges();

			const std::vector<std::byte> ack = Replica_.Acknowledge();
			if (!ack.empty()) {
				Authority_.Receive(Handle, ack);
			}
			return carried;
		}

		bool Join(int limit = 64) {
			for (int attempt = 0; attempt < limit && !Replica_.Joined(); attempt++) {
				Tick(Handle);
			}
			return Replica_.Joined();
		}

		// Moves every entity, so that every one of them has something to send —
		// **both components, because one owed value per entity would make "last
		// in the component list" a position nothing is ever in.**
		void MoveAll(const std::vector<Entity> &entities, float to) {
			for (const Entity entity : entities) {
				Server.GetMutable<Mark>(entity)->X = to;
				Server.GetMutable<Tint>(entity)->Shade = to;
			}
		}

		Store Server;
		Store Client;
		Authority Authority_;
		Replica Replica_;
		ClientId Handle;
		uint64_t Now = 0;
	};

	// Builds `count` entities, all carrying both replicated components.
	std::vector<Entity> Populate(Bench &bench, int count) {
		std::vector<Entity> entities;
		for (int index = 0; index < count; index++) {
			const Entity entity = bench.Server.Create();
			bench.Server.Set<Mark>(entity, Mark{static_cast<float>(index), 0.0f});
			bench.Server.Set<Tint>(entity, Tint{static_cast<float>(index)});
			entities.push_back(entity);
		}
		return entities;
	}

	// The longest run of consecutive ticks any entity went without being sent.
	//
	// Measured over the whole window rather than from the last send, because an
	// entity sent on the first tick and never again would otherwise look
	// healthy.
	uint64_t LongestWait(const std::map<uint64_t, std::vector<uint64_t>> &sentAt, uint64_t lastTick) {
		uint64_t longest = 0;
		for (const std::pair<const uint64_t, std::vector<uint64_t>> &entry : sentAt) {
			uint64_t previous = 0;
			for (const uint64_t tick : entry.second) {
				longest = std::max(longest, tick - previous);
				previous = tick;
			}
			longest = std::max(longest, lastTick - previous);
		}
		return longest;
	}
}

using namespace priority_test;

// --- no pressure, no cost ------------------------------------------------------

TEST_CASE("a budget that fits everything changes nothing", "[replication][priority]") {
	// **The ordering must be free when it is not needed.** A scheme that ranked
	// every entity on every tick would be a tax on every server that was never
	// over budget, and the tax would be invisible — it costs bytes only if it
	// also reorders, and it costs time either way.
	//
	// Two runs of one world, one with a cap ten thousand messages wide and one
	// with the default, compared byte for byte.
	AuthoritySettings roomy;
	roomy.MessagesPerTick = 10000;

	AuthoritySettings normal;

	std::vector<std::vector<std::vector<std::byte>>> runs;
	std::vector<Carried> lastTicks;
	for (const AuthoritySettings &settings : {roomy, normal}) {
		Bench bench(settings);
		const std::vector<Entity> entities = Populate(bench, 40);
		REQUIRE(bench.Join());

		std::vector<std::vector<std::byte>> messages;
		Carried last;
		for (int round = 1; round <= 8; round++) {
			bench.MoveAll(entities, static_cast<float>(round));
			last = bench.Tick(bench.Handle);
			for (const std::vector<std::byte> &message : last.Messages) {
				messages.push_back(message);
			}
		}

		REQUIRE(bench.Authority_.Stats().Deferred == 0);
		runs.push_back(std::move(messages));
		lastTicks.push_back(std::move(last));
	}

	// Raising the cap out of reach produces the same bytes as leaving it where
	// it is, so the cap costs nothing while it is not binding.
	REQUIRE(runs[0] == runs[1]);
	REQUIRE_FALSE(runs[0].empty());

	// **And the order is still the dirty bits' own, not a ranking that happened
	// to agree with it.** A message carries one component at a time in this
	// world, in the order `Replicate` was called, with each component's entities
	// in the order the store walks them — which the priority pass would break
	// up, because it interleaves an entity's components to keep the entity
	// together. Comparing two capped runs alone would not notice a build that
	// ranked everything on every tick.
	const std::vector<std::pair<std::string, uint64_t>> &wire = lastTicks[1].Sequence;
	REQUIRE(wire.size() == 80);

	std::vector<std::pair<std::string, uint64_t>> natural;
	for (const std::pair<const std::string, std::vector<uint64_t>> &entry : lastTicks[1].Values) {
		std::vector<uint64_t> ascending = entry.second;
		std::sort(ascending.begin(), ascending.end());
		for (const uint64_t id : ascending) {
			natural.emplace_back(entry.first, id);
		}
	}
	REQUIRE(wire == natural);
}

TEST_CASE("nothing is deferred while the budget is not reached", "[replication][priority]") {
	// The counter that tells "the budget was exceeded" apart from "a component
	// is broken" has to stay at zero for the second to mean anything.
	Bench bench(AuthoritySettings{});
	const std::vector<Entity> entities = Populate(bench, 30);
	REQUIRE(bench.Join());

	for (int round = 1; round <= 10; round++) {
		bench.MoveAll(entities, static_cast<float>(round));
		bench.Tick(bench.Handle);
		REQUIRE(bench.Authority_.Stats().Deferred == 0);
		REQUIRE(bench.Authority_.Stats().Stalest == 0);
	}
}

TEST_CASE("the counter moves exactly when the budget is exceeded", "[replication][priority]") {
	AuthoritySettings tight;
	tight.MessagesPerTick = 1;

	Bench bench(tight);
	const std::vector<Entity> entities = Populate(bench, 240);
	REQUIRE(bench.Join(256));

	// A world that moves does not fit, and says so.
	bench.MoveAll(entities, 1.0f);
	bench.Tick(bench.Handle);
	REQUIRE(bench.Authority_.Stats().Deferred > 0);

	// A world that goes quiet stops saying so, once the backlog has drained.
	// **Not immediately**: what was held over is still owed, and a counter that
	// dropped to zero while entities were still waiting would be reporting the
	// absence of new work rather than the absence of a backlog.
	for (int drain = 0; drain < 64; drain++) {
		bench.Tick(bench.Handle);
	}
	REQUIRE(bench.Authority_.Stats().Deferred == 0);
	REQUIRE(bench.Authority_.Stats().Stalest == 0);
}

// --- the rotation ---------------------------------------------------------------

TEST_CASE("under a budget that fits a fraction, everything is still sent", "[replication][priority]") {
	// **The starvation case, with the bound asserted.** One message per tick
	// against two hundred and forty entities that all move every tick, so a
	// tick carries a small and measurable fraction of what is owed. Nothing may
	// wait longer than it takes to work through the rest once.
	AuthoritySettings tight;
	tight.MessagesPerTick = 1;

	Bench bench(tight);
	const std::vector<Entity> entities = Populate(bench, 240);
	REQUIRE(bench.Join(256));

	std::map<uint64_t, std::vector<uint64_t>> sentAt;
	size_t smallestTick = SIZE_MAX;
	const int rounds = 120;

	for (int round = 1; round <= rounds; round++) {
		bench.MoveAll(entities, static_cast<float>(round));

		const Carried carried = bench.Tick(bench.Handle);
		size_t thisTick = 0;
		for (const std::pair<const std::string, std::vector<uint64_t>> &entry : carried.Values) {
			for (const uint64_t id : entry.second) {
				sentAt[id].push_back(static_cast<uint64_t>(round));
				thisTick++;
			}
		}

		REQUIRE(thisTick > 0);
		smallestTick = std::min(smallestTick, thisTick);
	}

	// Every entity, not merely most of them. An entity missing from this map
	// was never sent once in a hundred and twenty ticks, which is the failure
	// the whole rotation exists to prevent.
	REQUIRE(sentAt.size() == entities.size());

	// The bound. With `k` values carried per tick and `n` owed, a round robin
	// reaches every one of them inside `ceil(n / k)` ticks, and one more for
	// the tick a value first became owed on. Both components are asserted so
	// that a `k` of one would fail the second rather than quietly widen the
	// first.
	const size_t owed = entities.size() * 2;
	const uint64_t bound = static_cast<uint64_t>((owed + smallestTick - 1) / smallestTick) + 1;
	REQUIRE(bound <= 16);
	REQUIRE(LongestWait(sentAt, static_cast<uint64_t>(rounds)) <= bound);
}

TEST_CASE("a score cannot starve what it ranks last", "[replication][priority]") {
	// **A score alone re-creates the same problem with better manners.** Half
	// the world is worth ten times the other half on every tick of the run, so
	// a weighted sum would send the favoured half forever and the rest never —
	// and D00007 says the symptom of that is somebody concluding the component
	// is broken.
	//
	// The deadline is what stops it: a value that has waited that long outranks
	// every score there is, so the bound is the deadline plus the ticks it
	// takes to drain what was already waiting.
	AuthoritySettings tight;
	tight.MessagesPerTick = 1;
	tight.StarvationTicks = 10;

	Bench bench(tight);
	const std::vector<Entity> entities = Populate(bench, 240);
	REQUIRE(bench.Join(256));

	// Split by handle rather than by index so the predicate is a pure function
	// of the entity, which is what the authority is allowed to assume.
	std::vector<uint64_t> favoured;
	for (size_t index = 0; index < entities.size(); index += 2) {
		favoured.push_back(entities[index].Id);
	}
	std::sort(favoured.begin(), favoured.end());

	bench.Authority_.SetPriority([&favoured](ClientId, Entity entity) {
		return std::binary_search(favoured.begin(), favoured.end(), entity.Id) ? 10.0f : 0.0f;
	});

	std::map<uint64_t, std::vector<uint64_t>> sentAt;
	std::map<uint64_t, std::vector<uint64_t>> neglected;
	size_t smallestTick = SIZE_MAX;
	const int rounds = 160;

	for (int round = 1; round <= rounds; round++) {
		bench.MoveAll(entities, static_cast<float>(round));

		const Carried carried = bench.Tick(bench.Handle);
		size_t thisTick = 0;
		for (const std::pair<const std::string, std::vector<uint64_t>> &entry : carried.Values) {
			for (const uint64_t id : entry.second) {
				sentAt[id].push_back(static_cast<uint64_t>(round));
				if (!std::binary_search(favoured.begin(), favoured.end(), id)) {
					neglected[id].push_back(static_cast<uint64_t>(round));
				}
				thisTick++;
			}
		}
		REQUIRE(thisTick > 0);
		smallestTick = std::min(smallestTick, thisTick);
	}

	// Nothing was left out, score or no score.
	REQUIRE(sentAt.size() == entities.size());
	REQUIRE(neglected.size() == entities.size() / 2);

	const size_t owed = entities.size() * 2;
	const uint64_t drain = static_cast<uint64_t>((owed + smallestTick - 1) / smallestTick) + 1;
	const uint64_t bound = tight.StarvationTicks + drain;
	REQUIRE(bound <= 32);
	REQUIRE(LongestWait(neglected, static_cast<uint64_t>(rounds)) <= bound);

	// And the score was not merely ignored: the favoured half is sent more
	// often than the rest. Without this the case above would pass on a build
	// that dropped `SetPriority` entirely.
	size_t favouredSends = 0;
	size_t neglectedSends = 0;
	for (const std::pair<const uint64_t, std::vector<uint64_t>> &entry : sentAt) {
		const bool wanted = std::binary_search(favoured.begin(), favoured.end(), entry.first);
		(wanted ? favouredSends : neglectedSends) += entry.second.size();
	}
	REQUIRE(favouredSends > neglectedSends);
}

TEST_CASE("a creation the cap held over is announced again", "[replication][priority]") {
	// **A creation is said exactly once and the known set moves when it is
	// built**, so the cap has to put back whatever it then could not carry.
	// Three hundred entities appearing at once is three messages of handles
	// against a cap of one, and an entity the server believes it announced is
	// one whose values are sent for a row the client does not hold — which
	// `Replica` drops without a word, for as long as the entity lives.
	AuthoritySettings tight;
	tight.MessagesPerTick = 1;

	Bench bench(tight);
	Populate(bench, 1);
	REQUIRE(bench.Join());

	std::vector<Entity> late;
	for (int index = 0; index < 300; index++) {
		const Entity entity = bench.Server.Create();
		bench.Server.Set<Mark>(entity, Mark{static_cast<float>(index), 1.0f});
		bench.Server.Set<Tint>(entity, Tint{static_cast<float>(index)});
		late.push_back(entity);
	}

	size_t restarts = 0;
	for (int tick = 0; tick < 400; tick++) {
		bench.Tick(bench.Handle);
		restarts += bench.Authority_.Stats().Resnapshots;
	}

	for (const Entity entity : late) {
		REQUIRE(bench.Client.Alive(entity));
	}

	// By being told again, not by being sent the world again. Falling far
	// enough behind does repair a client, and it repairs it by resending
	// everything — which is a cost that hides rather than a fix.
	REQUIRE(restarts == 0);
}

// --- determinism -----------------------------------------------------------------

TEST_CASE("the first tick under pressure sends the lowest handles first", "[replication][priority]") {
	// On the tick pressure first appears nothing has waited longer than
	// anything else and no score has been set, so every candidate compares
	// equal on both keys that matter — and `std::sort` is not stable, so
	// without a tie-break the order would be whatever the implementation
	// happened to produce. It has to be the entity handle, because that is the
	// only key both runs of a server are guaranteed to agree on.
	AuthoritySettings tight;
	tight.MessagesPerTick = 1;

	Bench bench(tight);
	const std::vector<Entity> entities = Populate(bench, 240);
	REQUIRE(bench.Join(256));

	bench.MoveAll(entities, 1.0f);
	const Carried carried = bench.Tick(bench.Handle);

	// A message groups an entity's value under its component's entry, so what
	// the candidate order shows up as on the wire is each entry being in
	// ascending handle order and the tick as a whole covering the lowest
	// handles. Without the tie-break the sort has nothing left to order two
	// hundred and forty equal candidates by, and hands back whatever its
	// partitioning produced — which is neither of those things.
	std::vector<uint64_t> distinct;
	for (const std::pair<const std::string, std::vector<uint64_t>> &entry : carried.Values) {
		REQUIRE_FALSE(entry.second.empty());
		REQUIRE(std::is_sorted(entry.second.begin(), entry.second.end()));
		distinct.insert(distinct.end(), entry.second.begin(), entry.second.end());
	}
	std::sort(distinct.begin(), distinct.end());
	distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
	REQUIRE_FALSE(distinct.empty());
	REQUIRE(distinct.size() < entities.size());

	std::vector<uint64_t> lowest;
	for (const Entity entity : entities) {
		lowest.push_back(entity.Id);
	}
	std::sort(lowest.begin(), lowest.end());
	lowest.resize(distinct.size());
	REQUIRE(distinct == lowest);
}

TEST_CASE("the resend order does not depend on the order things went wrong", "[replication][priority]") {
	// **The recovery walk reads an unordered map, and that is a determinism
	// hole with no other symptom.** Two runs of one server inside one process
	// insert the same keys in the same sequence, so comparing two runs of the
	// same scenario cannot catch it — the map obliges by iterating the same way
	// both times. What catches it is the same *set* of unconfirmed entries
	// arrived at by two different sequences of insertions, which is what a
	// server with two different loss patterns actually has.
	//
	// So: the same forty entities, made unconfirmed evens-then-odds in one run
	// and odds-then-evens in the other, never acknowledged, and then a tick
	// where nothing changes at all — so the delta is the recovery walk and
	// nothing else. The bytes have to match.
	const auto capture = [](bool evensFirst) {
		// **Before the store touches either type.** `Store::Observe<T>` and
		// `Store::Set<T>` register a component that nothing has named yet, and
		// they register it under the C++ type name — so a case that reaches
		// them first leaves `Mark` on the table as `priority_test::Mark`,
		// resolves to nothing by its wire name, and aborts the binary when the
		// next case registers it properly. This is the only case here that does
		// not build a `Bench`, which is why it is the only one that had to say
		// so.
		RegisterTypes();

		Store server("server");
		Authority authority{AuthoritySettings{}};
		const ClientId handle = authority.Admit();
		authority.Replicate(Name("priority_test.Mark"));
		server.Observe<Mark>();

		std::vector<Entity> evens;
		std::vector<Entity> odds;
		for (int index = 0; index < 40; index++) {
			const Entity entity = server.Create();
			server.Set<Mark>(entity, Mark{0.0f, 0.0f});
			(index % 2 == 0 ? evens : odds).push_back(entity);
		}

		// The snapshot, which is also what fills the known set. Nothing is
		// acknowledged anywhere in this run, so every value that goes out stays
		// unconfirmed and comes back from the recovery walk.
		uint64_t tick = 0;
		for (int chunked = 0; chunked < 4; chunked++) {
			authority.Publish(server, ++tick);
			server.ClearChanges();
		}

		// One group, then the other. The same value both times, so the only
		// thing that can differ between the two runs is the order.
		for (const std::vector<Entity> *group : {&evens, &odds}) {
			const std::vector<Entity> &moving = evensFirst ? *group : (group == &evens ? odds : evens);
			for (const Entity entity : moving) {
				server.GetMutable<Mark>(entity)->X = 7.0f;
			}
			authority.Publish(server, ++tick);
			server.ClearChanges();
		}

		// Nothing moves. Everything in this delta is a resend.
		authority.Publish(server, ++tick);

		std::vector<std::vector<std::byte>> messages;
		for (const std::vector<std::byte> &message : authority.Outgoing(handle)) {
			messages.push_back(message);
		}
		REQUIRE_FALSE(messages.empty());
		return messages;
	};

	REQUIRE(capture(true) == capture(false));
}

TEST_CASE("two clients with different interest get different, repeatable orders", "[replication][priority]") {
	// **Per client, and that is the decision rather than an accident.** Two
	// clients are owed different worlds because interest is per client, so a
	// shared ordering would spend one client's budget on the other's entities.
	// What each one gets has to depend on nothing but its own interest — and
	// has to be the same on a second run of the same world.
	const auto run = [](std::vector<std::vector<std::byte>> &first,
						std::vector<std::vector<std::byte>> &second) {
		AuthoritySettings tight;
		tight.MessagesPerTick = 1;

		Bench bench(tight);
		const std::vector<Entity> entities = Populate(bench, 200);

		// The second client's handle. `Bench` only applies to the first, which
		// is enough: what is being compared is what the authority produced.
		const ClientId other = bench.Authority_.Admit();

		std::vector<uint64_t> even;
		for (size_t index = 0; index < entities.size(); index += 2) {
			even.push_back(entities[index].Id);
		}
		std::sort(even.begin(), even.end());

		const ClientId watched = bench.Handle;
		bench.Authority_.SetInterest([&even, watched](ClientId client, Entity entity) {
			const bool isEven = std::binary_search(even.begin(), even.end(), entity.Id);
			return client == watched ? isEven : !isEven;
		});

		REQUIRE(bench.Join(256));
		for (int round = 1; round <= 12; round++) {
			bench.MoveAll(entities, static_cast<float>(round));

			// One `Publish` per tick serves both, so the two records are the
			// same tick seen from two clients rather than two runs.
			Carried carried = bench.Tick(watched);
			for (std::vector<std::byte> &message : carried.Messages) {
				first.push_back(std::move(message));
			}
			for (const std::vector<std::byte> &message : bench.Authority_.Outgoing(other)) {
				second.push_back(message);
			}
		}
	};

	std::vector<std::vector<std::byte>> firstA;
	std::vector<std::vector<std::byte>> secondA;
	run(firstA, secondA);

	std::vector<std::vector<std::byte>> firstB;
	std::vector<std::vector<std::byte>> secondB;
	run(firstB, secondB);

	REQUIRE_FALSE(firstA.empty());
	REQUIRE_FALSE(secondA.empty());

	// Each client repeats itself exactly.
	REQUIRE(firstA == firstB);
	REQUIRE(secondA == secondB);

	// And the two clients are not being sent the same thing, which is what
	// makes the two assertions above worth making.
	REQUIRE(firstA != secondA);
}
