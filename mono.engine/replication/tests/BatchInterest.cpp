// Batch interest chooses visibility once per client from the sorted replicated
// entity set. These cases cover the structural changes that decision produces.

#include <engine/core/Name.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/replication/Authority.hpp>
#include <engine/replication/Replica.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <span>
#include <vector>

TEST_SUITE_ID("engine.replication.batchinterest")
// The direct authority and replica path that receives this module's messages.
TEST_DEPENDS("engine.replication.stream")

using engine::core::Name;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::replication::Authority;
using engine::replication::ClientId;
using engine::replication::Replica;

namespace batch_interest_test {
	struct Mark {
		float Value = 0.0f;
	};

	void RegisterTypes() {
		static const bool once = [] {
			engine::ecs::Components::Register<Mark>("batch_interest_test.Mark");
			return true;
		}();
		(void)once;
	}

	struct Pair {
		Pair() : Server("batch_interest_server"), Client("batch_interest_client") {
			RegisterTypes();
			Authority_.Replicate(Name("batch_interest_test.Mark"));
			Server.Observe<Mark>();
			Handle = Authority_.Admit();
		}

		void Tick() {
			TickNumber++;
			Authority_.Publish(Server, TickNumber);
			for (const std::vector<std::byte> &message : Authority_.Outgoing(Handle)) {
				Replica_.Receive(Client, message);
			}
			Server.ClearChanges();

			const std::vector<std::byte> acknowledgement = Replica_.Acknowledge();
			if (!acknowledgement.empty()) {
				Authority_.Receive(Handle, acknowledgement);
			}
		}

		bool Settle(int attempts = 32) {
			for (int attempt = 0; attempt < attempts && !Replica_.Joined(); attempt++) {
				Tick();
			}
			return Replica_.Joined();
		}

		Store Server;
		Store Client;
		Authority Authority_;
		Replica Replica_;
		ClientId Handle;
		uint64_t TickNumber = 0;
	};
}

using namespace batch_interest_test;

TEST_CASE("batch interest hides then restores a stationary entity", "[replication][interest]") {
	Pair pair;
	const Entity visible = pair.Server.Create();
	pair.Server.Set<Mark>(visible, Mark{1.0f});
	const Entity hidden = pair.Server.Create();
	pair.Server.Set<Mark>(hidden, Mark{2.0f});

	bool allowHidden = false;
	std::vector<uint64_t> candidates;
	pair.Authority_.SetInterestBatch(
		[&](ClientId, const Store &, std::span<const Entity> rows, std::span<uint8_t> accepted) {
			candidates.clear();
			candidates.reserve(rows.size());
			for (size_t index = 0; index < rows.size(); index++) {
				candidates.push_back(rows[index].Id);
				accepted[index] = rows[index] != hidden || allowHidden;
			}
		}
	);

	REQUIRE(pair.Settle());
	REQUIRE(pair.Client.Alive(visible));
	REQUIRE_FALSE(pair.Client.Alive(hidden));
	REQUIRE(candidates.size() == 2);
	CHECK(candidates[0] < candidates[1]);

	allowHidden = true;
	for (int attempt = 0; attempt < 16 && !pair.Client.Alive(hidden); attempt++) {
		pair.Tick();
	}

	REQUIRE(pair.Client.Alive(hidden));
	REQUIRE(pair.Client.Get<Mark>(hidden) != nullptr);
	CHECK(pair.Client.Get<Mark>(hidden)->Value == 2.0f);
}

TEST_CASE("batch interest follows a destroyed and reused entity handle", "[replication][interest]") {
	Pair pair;
	const Entity first = pair.Server.Create();
	pair.Server.Set<Mark>(first, Mark{1.0f});
	pair.Authority_.SetInterestBatch(
		[](ClientId, const Store &, std::span<const Entity>, std::span<uint8_t> accepted) {
			for (uint8_t &acceptedRow : accepted) {
				acceptedRow = 1;
			}
		}
	);
	REQUIRE(pair.Settle());
	REQUIRE(pair.Client.Alive(first));

	pair.Server.Destroy(first);
	const Entity replacement = pair.Server.Create();
	pair.Server.Set<Mark>(replacement, Mark{9.0f});
	pair.Authority_.SetInterestBatch(
		[replacement](ClientId, const Store &, std::span<const Entity> rows, std::span<uint8_t> accepted) {
			for (size_t index = 0; index < rows.size(); index++) {
				accepted[index] = rows[index] == replacement;
			}
		}
	);

	for (int attempt = 0; attempt < 16 && (!pair.Client.Alive(replacement) || pair.Client.Alive(first));
		 attempt++) {
		pair.Tick();
	}

	REQUIRE_FALSE(pair.Client.Alive(first));
	REQUIRE(pair.Client.Alive(replacement));
	REQUIRE(pair.Client.Get<Mark>(replacement) != nullptr);
	CHECK(pair.Client.Get<Mark>(replacement)->Value == 9.0f);
}

TEST_CASE("both interest hooks report an entity leaving view", "[replication][interest]") {
	auto expectForgotten = [](auto install) {
		Pair pair;
		const Entity entity = pair.Server.Create();
		pair.Server.Set<Mark>(entity, Mark{1.0f});

		bool visible = true;
		install(pair.Authority_, entity, visible);
		REQUIRE(pair.Settle());
		REQUIRE(pair.Client.Alive(entity));

		visible = false;
		pair.Tick();

		// An interest transition does not destroy a live authoritative entity.
		// The replica hands the host its structural forget so presentation can
		// stop exposing it while the row remains ready to reappear.
		REQUIRE(pair.Replica_.Forgotten().size() == 1);
		CHECK(pair.Replica_.Forgotten().front() == entity);
		CHECK(pair.Client.Alive(entity));
	};

	SECTION("per entity predicate") {
		expectForgotten([](Authority &authority, Entity hidden, bool &visible) {
			authority.SetInterest([hidden, &visible](ClientId, Entity entity, const Store &) {
				return entity != hidden || visible;
			});
		});
	}

	SECTION("batch selector") {
		expectForgotten([](Authority &authority, Entity hidden, bool &visible) {
			authority.SetInterestBatch(
				[hidden, &visible](
					ClientId, const Store &, std::span<const Entity> rows, std::span<uint8_t> accepted
				) {
					for (size_t index = 0; index < rows.size(); index++) {
						accepted[index] = rows[index] != hidden || visible;
					}
				}
			);
		});
	}
}

TEST_CASE("batch interest clears acceptance before every selector call", "[replication][interest]") {
	Pair pair;
	const Entity entity = pair.Server.Create();
	pair.Server.Set<Mark>(entity, Mark{1.0f});

	bool accept = true;
	pair.Authority_.SetInterestBatch(
		[&accept](ClientId, const Store &, std::span<const Entity>, std::span<uint8_t> accepted) {
			if (accept) {
				accepted.front() = 1;
			}
		}
	);
	REQUIRE(pair.Settle());
	REQUIRE(pair.Client.Alive(entity));

	// The selector deliberately leaves every byte untouched this time. A stale
	// lane buffer would keep the entity visible instead of producing this forget.
	accept = false;
	pair.Tick();
	REQUIRE(pair.Replica_.Forgotten().size() == 1);
	CHECK(pair.Replica_.Forgotten().front() == entity);
}

TEST_CASE("interest hooks replace each other and no hook shows every entity", "[replication][interest]") {
	Pair pair;
	const Entity first = pair.Server.Create();
	pair.Server.Set<Mark>(first, Mark{1.0f});
	const Entity second = pair.Server.Create();
	pair.Server.Set<Mark>(second, Mark{2.0f});

	REQUIRE(pair.Settle());
	REQUIRE(pair.Client.Alive(first));
	REQUIRE(pair.Client.Alive(second));

	Pair batchReplacesPredicate;
	const Entity hiddenByBatch = batchReplacesPredicate.Server.Create();
	batchReplacesPredicate.Server.Set<Mark>(hiddenByBatch, Mark{3.0f});
	const Entity selectedByBatch = batchReplacesPredicate.Server.Create();
	batchReplacesPredicate.Server.Set<Mark>(selectedByBatch, Mark{4.0f});
	batchReplacesPredicate.Authority_.SetInterest([hiddenByBatch](ClientId, Entity entity, const Store &) {
		return entity == hiddenByBatch;
	});
	batchReplacesPredicate.Authority_.SetInterestBatch(
		[selectedByBatch](
			ClientId, const Store &, std::span<const Entity> rows, std::span<uint8_t> accepted
		) {
			for (size_t index = 0; index < rows.size(); index++) {
				accepted[index] = rows[index] == selectedByBatch;
			}
		}
	);
	REQUIRE(batchReplacesPredicate.Settle());
	REQUIRE_FALSE(batchReplacesPredicate.Client.Alive(hiddenByBatch));
	REQUIRE(batchReplacesPredicate.Client.Alive(selectedByBatch));

	Pair predicateReplacesBatch;
	const Entity selectedByPredicate = predicateReplacesBatch.Server.Create();
	predicateReplacesBatch.Server.Set<Mark>(selectedByPredicate, Mark{5.0f});
	const Entity hiddenByPredicate = predicateReplacesBatch.Server.Create();
	predicateReplacesBatch.Server.Set<Mark>(hiddenByPredicate, Mark{6.0f});
	predicateReplacesBatch.Authority_.SetInterestBatch(
		[](ClientId, const Store &, std::span<const Entity>, std::span<uint8_t> accepted) {
			for (uint8_t &acceptedRow : accepted) {
				acceptedRow = 1;
			}
		}
	);
	predicateReplacesBatch.Authority_.SetInterest([selectedByPredicate](
													  ClientId, Entity entity, const Store &
												  ) { return entity == selectedByPredicate; });
	REQUIRE(predicateReplacesBatch.Settle());
	REQUIRE(predicateReplacesBatch.Client.Alive(selectedByPredicate));
	REQUIRE_FALSE(predicateReplacesBatch.Client.Alive(hiddenByPredicate));
}
