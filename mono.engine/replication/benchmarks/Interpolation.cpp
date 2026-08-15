// What smoothing the world costs a client, per frame and per received tick.
//
// **Two loops, running at different rates, and confusing them is easy.**
// `Record` runs once per entity per *received tick* - thirty a second from a
// listen server. `Sample` runs once per entity per *rendered frame* - a hundred
// and forty-four a second on a machine that can manage it. So a cost in
// `Sample` is worth nearly five times a cost in `Record` on the same hardware,
// and a suite that reported one number for "interpolation" would hide which of
// the two a regression landed in.
//
// The entity counts run to 20 000 because that is what a client holds *after*
// interest management, not before: the whole point of `Structure::Forgotten` is
// that a client is told about the part of the world it can see. If a deployment
// finds itself at the top of this ladder, the number to fix is upstream in the
// interest set and this suite is where that becomes obvious rather than
// arguable.
//
// **`Prune` is the row with a shape.** History is bounded by time, and dropping
// what has aged out means walking tracks. Everything else here is a lookup and
// a lerp; if the per-entity cost climbs with the number of entities rather than
// staying flat, that walk is where it came from.

#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/replication/SnapshotBuffer.hpp>
#include <engine/testing/Bench.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.replication.bench.interpolation")

using engine::core::CFrame;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::replication::InterpolationSettings;
using engine::replication::SnapshotBuffer;
using engine::testing::Consume;

namespace interpolation_bench {

	// The authority's tick rate the ladder simulates. `server --listen` paces at
	// 30, which is the more demanding case for interpolation because the gap
	// between samples is twice as long and the buffer holds proportionally
	// fewer of them for the same wall-clock history.
	constexpr double TICK_RATE = 30.0;
	constexpr double TICK_SECONDS = 1.0 / TICK_RATE;

	// A rendered frame at 144 Hz, which is where `Sample` runs hardest.
	constexpr double FRAME_SECONDS = 1.0 / 144.0;

	// Entity handles, made once. `Entity` wraps a generation-bearing id and the
	// layout is the store's, so these are simply distinct rather than meaningful.
	const std::vector<Entity> &Entities(size_t count) {
		static std::vector<std::pair<size_t, std::vector<Entity>>> built;
		for (const auto &[size, entities] : built) {
			if (size == count) {
				return entities;
			}
		}
		std::vector<Entity> made;
		made.reserve(count);
		for (size_t index = 0; index < count; index++) {
			made.emplace_back(static_cast<uint64_t>(index + 1));
		}
		built.emplace_back(count, std::move(made));
		return built.back().second;
	}

	// A pose that moves, so successive ticks differ and the interpolation has
	// something to do. A buffer fed the same pose twice can shortcut, and a
	// benchmark that fed it one would be measuring the shortcut.
	CFrame PoseAt(size_t entity, uint64_t tick) {
		const auto drift = static_cast<float>(tick) * 0.05f;
		return CFrame(Vector3(static_cast<float>(entity) + drift, drift, drift * 0.5f));
	}

	// A buffer already holding a full history for `count` entities, built once
	// and reused.
	//
	// **Filled to the history bound rather than to one tick**, because the cheap
	// path and the expensive path differ exactly there: a buffer with one sample
	// per entity has nothing to interpolate between and nothing to prune, which
	// is the state a client is in for the first fraction of a second and never
	// again.
	SnapshotBuffer &Warm(size_t count) {
		static std::vector<std::pair<size_t, std::unique_ptr<SnapshotBuffer>>> built;
		for (auto &[size, buffer] : built) {
			if (size == count) {
				return *buffer;
			}
		}

		InterpolationSettings settings;
		settings.TickRate = TICK_RATE;
		auto buffer = std::make_unique<SnapshotBuffer>(settings);

		const std::vector<Entity> &entities = Entities(count);
		for (uint64_t tick = 1; tick <= 16; tick++) {
			for (size_t index = 0; index < entities.size(); index++) {
				buffer->Record(tick, entities[index], PoseAt(index, tick));
			}
			buffer->Advance(TICK_SECONDS);
		}

		built.emplace_back(count, std::move(buffer));
		return *built.back().second;
	}
}

using namespace interpolation_bench;

// --- recording, once per entity per received tick -----------------------------

BENCH("Record · 1k entities", 1000) {
	static SnapshotBuffer buffer{[] {
		InterpolationSettings settings;
		settings.TickRate = TICK_RATE;
		return settings;
	}()};
	static uint64_t tick = 0;

	tick++;
	const std::vector<Entity> &entities = Entities(1000);
	for (size_t index = 0; index < entities.size(); index++) {
		buffer.Record(tick, entities[index], PoseAt(index, tick));
	}
	buffer.Advance(TICK_SECONDS);
}

BENCH("Record · 20k entities", 20'000) {
	// Twenty times the population. **A flat per-entity figure against the row
	// above is the result to want**; a climbing one means recording an entity
	// costs something that depends on how many others there are, which is a
	// per-tick search rather than a per-entity insert.
	static SnapshotBuffer buffer{[] {
		InterpolationSettings settings;
		settings.TickRate = TICK_RATE;
		return settings;
	}()};
	static uint64_t tick = 0;

	tick++;
	const std::vector<Entity> &entities = Entities(20'000);
	for (size_t index = 0; index < entities.size(); index++) {
		buffer.Record(tick, entities[index], PoseAt(index, tick));
	}
	buffer.Advance(TICK_SECONDS);
}

BENCH("Holds · 100k calls", 100'000) {
	// The cheap question a client asks before walking a world to record it,
	// many times per received tick. It has to be far cheaper than the walk it
	// guards or it is not a guard.
	SnapshotBuffer &buffer = Warm(1000);
	uint32_t held = 0;
	for (size_t index = 0; index < 100'000; index++) {
		held += buffer.Holds(static_cast<uint64_t>(index % 32)) ? 1u : 0u;
	}
	Consume(held);
}

// --- sampling, once per entity per rendered frame ------------------------------
//
// **The hot loop.** At 144 Hz over 20 000 entities this is nearly three million
// calls a second, and it is the only per-frame cost in this module.

BENCH("Sample · 1k entities", 1000) {
	SnapshotBuffer &buffer = Warm(1000);
	const std::vector<Entity> &entities = Entities(1000);
	buffer.Advance(FRAME_SECONDS);

	size_t placed = 0;
	for (const Entity &entity : entities) {
		const std::optional<CFrame> pose = buffer.Sample(entity);
		placed += pose ? 1u : 0u;
	}
	Consume(placed);
}

BENCH("Sample · 20k entities", 20'000) {
	SnapshotBuffer &buffer = Warm(20'000);
	const std::vector<Entity> &entities = Entities(20'000);
	buffer.Advance(FRAME_SECONDS);

	size_t placed = 0;
	for (const Entity &entity : entities) {
		const std::optional<CFrame> pose = buffer.Sample(entity);
		placed += pose ? 1u : 0u;
	}
	Consume(placed);
}

BENCH("Sample · 20k entities the buffer has never seen", 20'000) {
	// **The miss path, and it is not hypothetical.** A client draws entities the
	// buffer has no business placing - one it has never recorded, one gone
	// quiet longer than the history, the predicted local player - and takes its
	// own live value instead. Whatever a miss costs is paid on every such entity
	// on every frame, so a miss that is dearer than a hit would make the
	// unhandled case the expensive one.
	SnapshotBuffer &buffer = Warm(1000);
	const std::vector<Entity> &entities = Entities(20'000);
	buffer.Advance(FRAME_SECONDS);

	size_t placed = 0;
	for (size_t index = 1000; index < entities.size(); index++) {
		placed += buffer.Sample(entities[index]) ? 1u : 0u;
	}
	Consume(placed);
}

BENCH("Sample · 20k entities while stalled", 20'000) {
	// The clock has run past the newest sample, so there is nothing to
	// interpolate toward and the world holds its last received pose. This is
	// what every frame costs during a network hiccup - the moment when the
	// client is least able to afford anything extra - and the shape to want is
	// that it is *cheaper* than a hit, because there is no lerp to do.
	SnapshotBuffer &buffer = Warm(20'000);
	const std::vector<Entity> &entities = Entities(20'000);
	// Far past anything recorded.
	buffer.Advance(10.0);

	size_t placed = 0;
	for (const Entity &entity : entities) {
		placed += buffer.Sample(entity) ? 1u : 0u;
	}
	Consume(placed);
}

// --- the clock ----------------------------------------------------------------

BENCH("Advance · 100k frames", 100'000) {
	// Once per frame, not once per entity - but it is where pruning happens, so
	// its cost is not independent of the population. Read it against the 20k
	// version below: if they differ, `Advance` is doing per-entity work and the
	// per-frame cost of the module is larger than `Sample` alone suggests.
	SnapshotBuffer &buffer = Warm(1000);
	for (size_t index = 0; index < 100'000; index++) {
		buffer.Advance(FRAME_SECONDS);
	}
	Consume(buffer.RenderTick());
}

BENCH("Advance · 100k frames over 20k entities", 100'000) {
	SnapshotBuffer &buffer = Warm(20'000);
	for (size_t index = 0; index < 100'000; index++) {
		buffer.Advance(FRAME_SECONDS);
	}
	Consume(buffer.RenderTick());
}

// --- a client's second ---------------------------------------------------------

BENCH("client second · 30 ticks recorded, 144 frames sampled, 5k entities", 144) {
	// **The whole client-side interpolation bill for one second**, at the rates
	// a real client runs: a 30 Hz authority and a 144 Hz display over five
	// thousand visible entities. One iteration is one rendered frame, so the
	// figure is the per-frame share and the row total is the per-second one.
	//
	// This is the number to compare against a frame budget. At 144 Hz a frame is
	// 6.9 ms; if interpolation is a visible fraction of that before anything has
	// been drawn, the interest set is too large or the sample path is too slow,
	// and the rows above say which.
	static SnapshotBuffer buffer{[] {
		InterpolationSettings settings;
		settings.TickRate = TICK_RATE;
		return settings;
	}()};
	static uint64_t tick = 0;

	const std::vector<Entity> &entities = Entities(5000);

	for (size_t frame = 0; frame < 144; frame++) {
		// A tick arrives roughly every fifth frame at 30 Hz against 144.
		if (frame % 5 == 0) {
			tick++;
			for (size_t index = 0; index < entities.size(); index++) {
				buffer.Record(tick, entities[index], PoseAt(index, tick));
			}
		}

		buffer.Advance(FRAME_SECONDS);

		size_t placed = 0;
		for (const Entity &entity : entities) {
			placed += buffer.Sample(entity) ? 1u : 0u;
		}
		Consume(placed);
	}
}
