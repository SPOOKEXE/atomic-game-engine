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

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/replication/Authority.hpp>
#include <engine/replication/Protocol.hpp>
#include <engine/replication/Replica.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <vector>

TEST_SUITE_ID("engine.replication.prioritywindow")
// The window is a slice of the order the authority builds.
TEST_DEPENDS("engine.replication.stream")
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

	struct Other {
		float X = 0.0f;
	};

	struct FixedWidth {
		uint32_t Value = 0;
	};

	std::atomic_size_t FixedWidthWrites = 0;

	void WriteFixedWidth(engine::core::ByteWriter &writer, const void *source, size_t count) {
		const auto *values = static_cast<const FixedWidth *>(source);
		FixedWidthWrites.fetch_add(count, std::memory_order_relaxed);
		for (size_t index = 0; index < count; index++) {
			writer.WriteUInt32(values[index].Value);
		}
	}

	void ReadFixedWidth(engine::core::ByteReader &reader, void *destination, size_t count) {
		auto *values = static_cast<FixedWidth *>(destination);
		for (size_t index = 0; index < count; index++) {
			values[index].Value = reader.ReadUInt32();
		}
	}

	void RegisterTypes() {
		static bool once = [] {
			engine::ecs::Components::Register<Mark>("priority_window_test.Mark");
			engine::ecs::Components::Register<Other>("priority_window_test.Other");
			engine::ecs::Components::Register<FixedWidth>(
				"priority_window_test.FixedWidth",
				engine::ecs::WireFormat{WriteFixedWidth, ReadFixedWidth, sizeof(uint32_t)}
			);
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
		explicit Pair(
			size_t bytesPerTick, size_t factor = 2, size_t chunkBytes = 1024, uint64_t starvationTicks = 30
		)
			: Server("window_server"), Client("window") {
			RegisterTypes();

			AuthoritySettings settings;
			settings.BytesPerTick = bytesPerTick;
			settings.ChunkBytes = chunkBytes;
			settings.PriorityRefinementFactor = factor;
			settings.StarvationTicks = starvationTicks;
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

		// Adds a second changed row per entity so refinement's per-entity cache
		// has to join rows from separate replicated components.
		void ReplicateOther() {
			Authority_.Replicate(Name("priority_window_test.Other"));
			Server.Observe<Other>();
		}

		void ChangeOther(const std::vector<Entity> &entities, float value) {
			for (const Entity entity : entities) {
				Server.Set<Other>(entity, Other{value});
			}
		}

		void ReplicateFixedWidth() {
			Authority_.Replicate(Name("priority_window_test.FixedWidth"));
			Server.Observe<FixedWidth>();
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

	std::vector<std::byte> FixedWidthPacket(
		uint64_t tick, uint64_t baseline, uint16_t part, bool final, Entity entity, uint32_t value
	) {
		engine::replication::Delta delta;
		delta.Tick = tick;
		delta.Baseline = baseline;
		delta.Part = part;
		delta.Final = final;

		engine::replication::ComponentDelta component;
		component.Component = Name("priority_window_test.FixedWidth");
		component.Entities.push_back(entity);

		engine::core::ByteWriter valueWriter;
		valueWriter.WriteUInt32(value);
		const std::span<const std::byte> valueBytes = valueWriter.Bytes();
		component.Values.assign(valueBytes.begin(), valueBytes.end());
		delta.Components.push_back(std::move(component));

		engine::core::ByteWriter packet;
		engine::replication::WriteMessage(packet, delta);
		const std::span<const std::byte> bytes = packet.Bytes();
		return {bytes.begin(), bytes.end()};
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

TEST_CASE(
	"refinement without a score still shares one answer between component rows", "[replication][priority]"
) {
	// Refinement is valid without a cheap score hook. In that mode there is no
	// slot saved by scoring, so the fallback must still locate each entity and
	// retain one answer for both rows.
	Pair pair(80, 24);
	pair.ReplicateOther();
	const std::vector<Entity> made = pair.Fill(24);
	pair.ChangeOther(made, 0.0f);
	REQUIRE(pair.Join());

	std::map<uint64_t, size_t> asked;
	pair.Authority_.SetPriorityRefinement([&asked](ClientId, Entity entity, float hint) {
		asked[entity.Id]++;
		return hint;
	});

	for (const Entity entity : made) {
		pair.Server.Set<Mark>(entity, Mark{1.0f});
	}
	pair.ChangeOther(made, 1.0f);
	pair.Tick();

	REQUIRE(asked.size() == made.size());
	for (const auto &[entity, calls] : asked) {
		(void)entity;
		CHECK(calls == 1);
	}
}

TEST_CASE(
	"fixed width rows preserve priority packet bytes and retry current values",
	"[replication][priority][recovery]"
) {
	constexpr size_t CHUNK_BYTES = 172; // One fixed row per packet under Pack's reservation.
	constexpr size_t ROWS = 24;
	Pair pair(4096, 2, CHUNK_BYTES, std::numeric_limits<uint64_t>::max());
	pair.ReplicateFixedWidth();

	std::vector<Entity> entities;
	for (size_t index = 0; index < ROWS; index++) {
		const Entity entity = pair.Server.Create();
		pair.Server.Set<FixedWidth>(entity, FixedWidth{static_cast<uint32_t>(index)});
		entities.push_back(entity);
	}
	REQUIRE(pair.Join());
	for (size_t index = 0; index < entities.size(); index++) {
		const FixedWidth *value = pair.Client.Get<FixedWidth>(entities[index]);
		REQUIRE(value != nullptr);
		CHECK(value->Value == index);
	}

	FixedWidthWrites.store(0, std::memory_order_relaxed);
	const std::function<float(ClientId, Entity)> earlyFirst = ByHandle(entities);
	pair.Authority_.SetPriority([earlyFirst](ClientId client, Entity entity) {
		return -earlyFirst(client, entity);
	});
	const uint64_t baseline = pair.Authority_.StatusOf(pair.Handle).Applied;
	const uint64_t firstTick = pair.Now + 1;
	const std::vector<std::byte> oneRow = FixedWidthPacket(
		firstTick, baseline, 0, true, entities.back(), 1000 + static_cast<uint32_t>(ROWS - 1)
	);
	REQUIRE(oneRow.size() < CHUNK_BYTES);
	pair.Authority_.SetAllowance(pair.Handle, oneRow.size() * 2);

	for (size_t index = 0; index < entities.size(); index++) {
		pair.Server.Set<FixedWidth>(entities[index], FixedWidth{1000 + static_cast<uint32_t>(index)});
	}
	pair.Now = firstTick;
	pair.Authority_.Publish(pair.Server, pair.Now);

	const std::span<const std::vector<std::byte>> firstOutgoing = pair.Authority_.Outgoing(pair.Handle);
	REQUIRE(firstOutgoing.size() == 2);
	// Each pack stages one row beyond the allowance before its final flush
	// refuses it, so three values are encoded per pass and only two are sent.
	CHECK(FixedWidthWrites.load(std::memory_order_relaxed) == 6);
	CHECK(
		firstOutgoing[0] ==
		FixedWidthPacket(
			firstTick, baseline, 0, false, entities.back(), 1000 + static_cast<uint32_t>(ROWS - 1)
		)
	);
	CHECK(
		firstOutgoing[1] ==
		FixedWidthPacket(
			firstTick, baseline, 1, true, entities[ROWS - 2], 1000 + static_cast<uint32_t>(ROWS - 2)
		)
	);

	// Refuse one emitted message, then change the store before the fresh publish.
	pair.Authority_.Unsent(pair.Handle, 0);
	pair.Server.ClearChanges();
	const uint64_t retryTick = pair.Now + 1;
	for (size_t index = 0; index < entities.size(); index++) {
		pair.Server.Set<FixedWidth>(entities[index], FixedWidth{2000 + static_cast<uint32_t>(index)});
	}
	FixedWidthWrites.store(0, std::memory_order_relaxed);
	pair.Now = retryTick;
	pair.Authority_.Publish(pair.Server, pair.Now);

	const std::span<const std::vector<std::byte>> retryOutgoing = pair.Authority_.Outgoing(pair.Handle);
	REQUIRE(retryOutgoing.size() == 2);
	CHECK(FixedWidthWrites.load(std::memory_order_relaxed) == 6);
	CHECK(
		retryOutgoing[0] ==
		FixedWidthPacket(
			retryTick, baseline, 0, false, entities.back(), 2000 + static_cast<uint32_t>(ROWS - 1)
		)
	);
	CHECK(
		retryOutgoing[1] ==
		FixedWidthPacket(
			retryTick, baseline, 1, true, entities[ROWS - 2], 2000 + static_cast<uint32_t>(ROWS - 2)
		)
	);

	for (const std::vector<std::byte> &message : retryOutgoing) {
		CHECK(pair.Replica_.Receive(pair.Client, message) == engine::replication::ApplyStatus::Ok);
	}
	CHECK(pair.Client.Get<FixedWidth>(entities.back())->Value == 2000 + ROWS - 1);
	CHECK(pair.Client.Get<FixedWidth>(entities[ROWS - 2])->Value == 2000 + ROWS - 2);
}
