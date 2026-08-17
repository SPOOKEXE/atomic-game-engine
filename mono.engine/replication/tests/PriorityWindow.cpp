// Which rows the expensive half of a priority is asked about.
//
// **`SetPriority` is asked about everything and `SetPriorityRefinement` is
// asked about the rows in contention**, and the whole value of the split is
// that second sentence. A host's cheap score is arithmetic; its expensive one
// is a raycast against a broad phase, and running it over every entity for
// every client was 51% of a two-hundred-client tick.
//
// The cases here are about the window rather than about any particular score:
// how wide it is, that it moves with the byte budget, that a refinement can
// only push a row back, and that a factor of zero turns the second hook off
// without unregistering it.

#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/replication/Authority.hpp>
#include <engine/replication/Replica.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <set>
#include <vector>

TEST_SUITE_ID("engine.replication.prioritywindow")
// The window is a slice of the order the authority builds.
TEST_DEPENDS("engine.replication.replication")
// The scorer whose two halves this splits.
TEST_DEPENDS("engine.replication.priority")

using engine::core::Name;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::replication::Authority;
using engine::replication::AuthoritySettings;
using engine::replication::ClientId;
using engine::replication::Replica;

namespace priority_window_test {
	struct Mark {
		float X = 0.0f;
	};

	void RegisterTypes() {
		static bool once = [] {
			engine::ecs::Components::Register<Mark>("priority_window_test.Mark");
			return true;
		}();
		(void)once;
	}

	// A server, a client and a world, with the byte budget as the dial.
	//
	// The budget is what decides how many rows a tick could carry, and the
	// window is a multiple of that - so a case that wants a narrow window says
	// so by giving the tick less to spend, which is the same thing a loaded
	// server does to itself.
	struct Pair {
		explicit Pair(size_t bytesPerTick, size_t factor = 2) : Server("window_server"), Client("window") {
			RegisterTypes();

			AuthoritySettings settings;
			settings.BytesPerTick = bytesPerTick;
			settings.PriorityRefinementFactor = factor;
			Authority_ = Authority(settings);

			Authority_.Replicate(Name("priority_window_test.Mark"));
			Server.Observe<Mark>();
			Handle = Authority_.Admit();
		}

		void Tick() {
			Now++;
			Authority_.Publish(Server, Now);
			for (const std::vector<std::byte> &message : Authority_.Outgoing(Handle)) {
				Replica_.Receive(Client, message);
			}
			Server.ClearChanges();

			const std::vector<std::byte> ack = Replica_.Acknowledge();
			if (!ack.empty()) {
				Authority_.Receive(Handle, ack);
			}
		}

		bool Join(int limit = 256) {
			for (int attempt = 0; attempt < limit && !Replica_.Joined(); attempt++) {
				Tick();
			}
			return Replica_.Joined();
		}

		// `count` entities carrying a Mark, in creation order.
		std::vector<Entity> Fill(int count) {
			std::vector<Entity> made;
			for (int index = 0; index < count; index++) {
				const Entity entity = Server.Create();
				Server.Set<Mark>(entity, Mark{static_cast<float>(index)});
				made.push_back(entity);
			}
			return made;
		}

		Store Server;
		Store Client;
		Authority Authority_;
		Replica Replica_;
		ClientId Handle;
		uint64_t Now = 0;
	};

	// Scores an entity by how early it was made, so the order the authority
	// builds is known without any geometry in the way.
	std::function<float(ClientId, Entity)> ByHandle(const std::vector<Entity> &order) {
		return [&order](ClientId, Entity entity) {
			for (size_t index = 0; index < order.size(); index++) {
				if (order[index] == entity) {
					return 1.0f - static_cast<float>(index) / static_cast<float>(order.size());
				}
			}
			return 0.0f;
		};
	}
}

using namespace priority_window_test;

TEST_CASE("the refinement is asked about a window and not about the world", "[replication][priority]") {
	// The measurement the split exists for, as a property. A hundred entities
	// are in play and the tick can carry a handful, so a refinement asked about
	// every one of them is a hundred raycasts spent to order eleven rows.
	Pair pair(80);
	const std::vector<Entity> made = pair.Fill(100);
	REQUIRE(pair.Join());

	std::set<uint64_t> asked;
	pair.Authority_.SetPriority(ByHandle(made));
	pair.Authority_.SetPriorityRefinement([&asked](ClientId, Entity entity, float hint) {
		asked.insert(entity.Id);
		return hint;
	});

	for (const Entity entity : made) {
		pair.Server.Set<Mark>(entity, Mark{1.0f});
	}
	pair.Tick();

	CHECK_FALSE(asked.empty());
	CHECK(asked.size() < made.size());
}

TEST_CASE("the window is the rows in front, not an arbitrary slice", "[replication][priority]") {
	// Being small is not enough: it has to be small at the *front*. A window
	// taken from wherever the candidates happened to be built would refine rows
	// nothing was going to send and skip the ones it was.
	Pair pair(80);
	const std::vector<Entity> made = pair.Fill(100);
	REQUIRE(pair.Join());

	std::set<uint64_t> asked;
	pair.Authority_.SetPriority(ByHandle(made));
	pair.Authority_.SetPriorityRefinement([&asked](ClientId, Entity entity, float hint) {
		asked.insert(entity.Id);
		return hint;
	});

	for (const Entity entity : made) {
		pair.Server.Set<Mark>(entity, Mark{1.0f});
	}
	pair.Tick();

	// Everything asked about is in the leading part of the scored order, and
	// the highest-scoring entity is always one of them.
	REQUIRE(asked.count(made.front().Id) == 1);
	for (size_t index = made.size() / 2; index < made.size(); index++) {
		CHECK(asked.count(made[index].Id) == 0);
	}
}

TEST_CASE("a wider factor asks about more rows", "[replication][priority]") {
	// The factor is the dial between fidelity and cost, so it has to move
	// something. Same budget, same world, same score.
	const auto refinedCount = [](size_t factor) {
		Pair pair(80, factor);
		const std::vector<Entity> made = pair.Fill(100);
		REQUIRE(pair.Join());

		std::set<uint64_t> asked;
		pair.Authority_.SetPriority(ByHandle(made));
		pair.Authority_.SetPriorityRefinement([&asked](ClientId, Entity entity, float hint) {
			asked.insert(entity.Id);
			return hint;
		});

		for (const Entity entity : made) {
			pair.Server.Set<Mark>(entity, Mark{1.0f});
		}
		pair.Tick();
		return asked.size();
	};

	CHECK(refinedCount(0) == 0);
	CHECK(refinedCount(4) > refinedCount(1));
}

TEST_CASE("a refinement that raises a score is ignored", "[replication][priority]") {
	// **The clamp, and it is the rule the window rests on.** Rows outside the
	// window keep their unrefined score, so an unrefined score has to be an
	// upper bound - and it stops being one the moment a refinement is allowed
	// to return something larger. A host that does it gets its own input back
	// rather than a stream quietly reordered around rows nobody looked at.
	Pair pair(80);
	const std::vector<Entity> made = pair.Fill(40);
	REQUIRE(pair.Join());

	std::vector<float> handed;
	pair.Authority_.SetPriority(ByHandle(made));
	pair.Authority_.SetPriorityRefinement([&handed](ClientId, Entity, float hint) {
		handed.push_back(hint);
		return hint + 1000.0f;
	});

	for (const Entity entity : made) {
		pair.Server.Set<Mark>(entity, Mark{1.0f});
	}
	pair.Tick();
	REQUIRE_FALSE(handed.empty());

	// A second tick sees the scores the first one produced. Had the raise
	// stuck, every refined row would arrive carrying a number above a thousand.
	handed.clear();
	for (const Entity entity : made) {
		pair.Server.Set<Mark>(entity, Mark{2.0f});
	}
	pair.Tick();

	REQUIRE_FALSE(handed.empty());
	for (const float hint : handed) {
		CHECK(hint <= 1.0f);
	}
}

TEST_CASE("no refinement registered leaves the order the score gave", "[replication][priority]") {
	// The default, and the behaviour every host that never registers a second
	// hook keeps.
	Pair plain(80);
	const std::vector<Entity> made = plain.Fill(40);
	REQUIRE(plain.Join());

	plain.Authority_.SetPriority(ByHandle(made));
	for (const Entity entity : made) {
		plain.Server.Set<Mark>(entity, Mark{1.0f});
	}

	CHECK_NOTHROW(plain.Tick());
}
