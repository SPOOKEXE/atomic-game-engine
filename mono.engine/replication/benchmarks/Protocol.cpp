// What a tick's worth of replication traffic costs to encode and to parse.
//
// **This is the per-tick, per-client cost of the whole networked-world story.**
// A server sends every connected client a delta every tick; a client parses one
// from every server it is talking to. So a figure here multiplies by population
// and then by tick rate, and a delta that costs 50 microseconds to encode is
// 300 milliseconds a second at a hundred clients — half a core, spent turning
// numbers the server already has into bytes.
//
// **The entity counts are the whole point of the ladder.** A delta carries
// runs, not rows: `ecs::Store::EachChangedBatch` yields adjacent changed rows
// as a block, which is what lets a value copy be a memcpy per run rather than
// one call per entity. Whether that actually pays off is invisible from the
// header and visible here — the `one run` and `fragmented` rows carry the same
// number of entities in a different number of runs, and if they cost the same
// then the run structure is buying nothing and the complexity should go.
//
// The parsing rows face hostile input and are measured against refusals for the
// same reason `net`'s framing suite is: `MAXIMUM_ENTRIES` exists so that four
// bytes from a peer are not an out-of-memory kill, and a bound is only a
// defence if enforcing it is cheaper than obeying it.

#include <engine/core/Bytes.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/replication/Protocol.hpp>
#include <engine/testing/Bench.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.replication.bench.protocol")

using engine::core::ByteReader;
using engine::core::ByteWriter;
using engine::core::Name;
using engine::ecs::Entity;
using engine::replication::Applied;
using engine::replication::ComponentDelta;
using engine::replication::Delta;
using engine::replication::Message;
using engine::replication::ReadMessage;
using engine::replication::SnapshotChunk;
using engine::replication::Structure;
using engine::replication::WriteMessage;
using engine::testing::Consume;

namespace protocol_bench {

	// Bytes per replicated value.
	//
	// A `CFrame` is a position and a rotation — twelve bytes and sixteen — so
	// twenty-eight is what the component every networked game replicates
	// actually weighs. Round numbers here would make the memcpy rows agree with
	// a cache line by accident.
	constexpr size_t VALUE_BYTES = 28;

	// A delta over `entities` rows of `components` components, split into
	// `runs` contiguous blocks per component.
	//
	// `runs` is the parameter that matters. One run is a world where everything
	// that moved is adjacent in the store — the case archetype storage is
	// supposed to produce. `entities` runs is the pathological opposite: every
	// changed row isolated between unchanged ones, which is what a world looks
	// like after a few thousand ticks of creation and destruction with no
	// compaction.
	Delta DeltaOf(size_t entities, size_t components, size_t runs) {
		Delta delta;
		delta.Tick = 4096;
		delta.Baseline = 4095;

		for (size_t component = 0; component < components; component++) {
			ComponentDelta column;
			column.Component = Name("engine.bench.replication.Component" + std::to_string(component));
			column.Entities.reserve(entities);
			column.Values.resize(entities * VALUE_BYTES);

			// Ids laid out as `runs` blocks of consecutive values with a gap
			// between them, so a delta claiming N runs really has N
			// discontinuities in its index space rather than merely being
			// labelled that way.
			const size_t blocks = runs == 0 ? 1 : runs;
			const size_t perBlock = entities / blocks == 0 ? 1 : entities / blocks;
			uint64_t id = 0;
			while (column.Entities.size() < entities) {
				for (size_t within = 0; within < perBlock && column.Entities.size() < entities; within++) {
					column.Entities.emplace_back(id++);
				}
				// Wide enough that no encoder could mistake two blocks for one.
				id += 64;
			}

			for (size_t byte = 0; byte < column.Values.size(); byte++) {
				column.Values[byte] = static_cast<std::byte>(byte * 5u);
			}
			delta.Components.push_back(std::move(column));
		}
		return delta;
	}

	// Encoded once, for the parsing rows.
	std::vector<std::byte> Encoded(const Delta &delta) {
		ByteWriter writer;
		WriteMessage(writer, delta);
		const std::span<const std::byte> bytes = writer.Bytes();
		return std::vector<std::byte>(bytes.begin(), bytes.end());
	}

	// Built lazily and cached, because a delta of 10 000 entities is not
	// something to construct nine times per row.
	const Delta &Cached(size_t entities, size_t components, size_t runs) {
		static std::vector<std::pair<std::array<size_t, 3>, Delta>> built;
		const std::array<size_t, 3> key{entities, components, runs};
		for (const auto &[made, delta] : built) {
			if (made == key) {
				return delta;
			}
		}
		built.emplace_back(key, DeltaOf(entities, components, runs));
		return built.back().second;
	}

	const std::vector<std::byte> &CachedBytes(size_t entities, size_t components, size_t runs) {
		static std::vector<std::pair<std::array<size_t, 3>, std::vector<std::byte>>> built;
		const std::array<size_t, 3> key{entities, components, runs};
		for (const auto &[made, bytes] : built) {
			if (made == key) {
				return bytes;
			}
		}
		built.emplace_back(key, Encoded(Cached(entities, components, runs)));
		return built.back().second;
	}

	// A writer held across ticks, which is how a server holds one: `Clear` keeps
	// the capacity, so a per-tick encode stops allocating after the first frame
	// that reached its high-water mark.
	ByteWriter &Reused() {
		static ByteWriter writer(1u << 20);
		return writer;
	}

	// A message reused across parses, likewise. `ReadMessage` fills a
	// caller-supplied one precisely so the vectors inside it keep their
	// capacity, and parsing into a fresh `Message` every packet would measure
	// the allocator rather than the parser.
	Message &Parsed() {
		static Message message;
		return message;
	}
}

using namespace protocol_bench;

// --- encoding a delta ---------------------------------------------------------
//
// One iteration is one entity, so every row on this ladder is directly
// comparable and the figure is the per-entity cost of getting a value onto the
// wire.

BENCH("WriteMessage delta · 1k entities, 1 component", 1000) {
	ByteWriter &writer = Reused();
	writer.Clear();
	WriteMessage(writer, Cached(1000, 1, 1));
	Consume(writer.Size());
}

BENCH("WriteMessage delta · 10k entities, 1 component", 10'000) {
	ByteWriter &writer = Reused();
	writer.Clear();
	WriteMessage(writer, Cached(10'000, 1, 1));
	Consume(writer.Size());
}

BENCH("WriteMessage delta · 10k entities, 4 components", 40'000) {
	// Four columns over the same population, so the iteration count is the
	// number of *values*. Read against the single-component row: if the
	// per-value cost is flat, the per-component overhead is negligible and a
	// game may replicate as many components as it likes. If it climbs, each
	// component is paying a fixed cost — a name written as text, a count, a
	// header — and a game with forty small components is being taxed forty
	// times a tick.
	ByteWriter &writer = Reused();
	writer.Clear();
	WriteMessage(writer, Cached(10'000, 4, 1));
	Consume(writer.Size());
}

BENCH("WriteMessage delta · 10k entities fragmented into 1k runs", 10'000) {
	// **The run structure, tested.** Same entities, same bytes, a thousand
	// discontinuities instead of one. If this matches the contiguous row, the
	// encoder is not exploiting runs at all and the `EachChangedBatch` shape
	// upstream is buying nothing on this side of the wire.
	ByteWriter &writer = Reused();
	writer.Clear();
	WriteMessage(writer, Cached(10'000, 1, 1000));
	Consume(writer.Size());
}

// --- parsing a delta ----------------------------------------------------------

BENCH("ReadMessage delta · 1k entities, 1 component", 1000) {
	const std::vector<std::byte> &bytes = CachedBytes(1000, 1, 1);
	Message &message = Parsed();
	ByteReader reader(bytes);
	Consume(ReadMessage(reader, message));
}

BENCH("ReadMessage delta · 10k entities, 1 component", 10'000) {
	const std::vector<std::byte> &bytes = CachedBytes(10'000, 1, 1);
	Message &message = Parsed();
	ByteReader reader(bytes);
	Consume(ReadMessage(reader, message));
}

BENCH("ReadMessage delta · 10k entities, 4 components", 40'000) {
	const std::vector<std::byte> &bytes = CachedBytes(10'000, 4, 1);
	Message &message = Parsed();
	ByteReader reader(bytes);
	Consume(ReadMessage(reader, message));
}

BENCH("ReadMessage delta · 10k entities, into a fresh Message", 10'000) {
	// **What a caller pays for not reusing the message.** Every vector inside
	// it — entities, values, one pair per component — is reallocated from
	// nothing. The gap against the reused row is the per-tick allocation bill a
	// client avoids by keeping one `Message` across its poll loop, and it is the
	// argument for the fill-in-place signature the function has.
	const std::vector<std::byte> &bytes = CachedBytes(10'000, 1, 1);
	Message message;
	ByteReader reader(bytes);
	Consume(ReadMessage(reader, message));
	Consume(message.Delta.Components.size());
}

// --- the other message kinds --------------------------------------------------

BENCH("WriteMessage structure · 1k created", 1000) {
	// Structural changes ride the reliable channel, so they are rarer than
	// deltas and much more expensive to lose. A thousand creations in one tick
	// is a world streaming in, which is the case where this stops being rare.
	static const Structure structure = [] {
		Structure built;
		built.Tick = 4096;
		for (size_t index = 0; index < 1000; index++) {
			built.Created.emplace_back(static_cast<uint64_t>(index));
		}
		return built;
	}();

	ByteWriter &writer = Reused();
	writer.Clear();
	WriteMessage(writer, structure);
	Consume(writer.Size());
}

BENCH("WriteMessage applied · 100k acknowledgements", 100'000) {
	// The smallest message the protocol has, sent once per client per tick. It
	// should be immeasurably cheap; this row is here so that a change making it
	// *not* cheap shows up, because at a hundred clients and sixty ticks it is
	// six thousand of the smallest thing in the system every second.
	static const Applied applied{4096};
	ByteWriter &writer = Reused();
	for (size_t index = 0; index < 100'000; index++) {
		writer.Clear();
		WriteMessage(writer, applied);
	}
	Consume(writer.Size());
}

BENCH("WriteMessage snapshot chunk · 1 MiB in 1 KiB pieces", 1024) {
	// The join path: a whole world serialised and cut into datagram-sized
	// pieces. One iteration is one chunk, so the figure multiplies by however
	// many chunks a world is — and a world is megabytes, so this is the number
	// that decides how long a player waits at a loading screen.
	static const SnapshotChunk chunk = [] {
		SnapshotChunk built;
		built.Tick = 4096;
		built.TotalBytes = 1u << 20;
		built.Offset = 0;
		built.Bytes.resize(1024);
		return built;
	}();

	ByteWriter &writer = Reused();
	for (size_t index = 0; index < 1024; index++) {
		writer.Clear();
		SnapshotChunk moving = chunk;
		moving.Offset = static_cast<uint32_t>(index * 1024);
		WriteMessage(writer, moving);
	}
	Consume(writer.Size());
}

// --- the hostile path ---------------------------------------------------------
//
// **Refusing has to be cheaper than obeying.** `MAXIMUM_ENTRIES` bounds what a
// count field can make a receiver allocate; these rows are what enforcing it
// costs, and they must stay flat however large the claim. A row that grew with
// the number a peer wrote down would mean the bound is checked after the
// allocation rather than before it, which is the same as not having one.

BENCH("ReadMessage · 10k messages with an absurd entity count", 10'000) {
	static const std::vector<std::byte> lying = [] {
		std::vector<std::byte> bytes = CachedBytes(1000, 1, 1);
		// Rewrite the tail so some count field in the body is nonsense. The
		// exact offset is deliberately not computed from the format: this row
		// asks whether *any* corruption of the counts is refused cheaply, and
		// hard-coding a field offset would tie the benchmark to a layout it
		// does not own.
		for (size_t index = bytes.size() / 2; index < bytes.size(); index++) {
			bytes[index] = static_cast<std::byte>(0xFFu);
		}
		return bytes;
	}();

	Message &message = Parsed();
	uint32_t accepted = 0;
	for (size_t index = 0; index < 10'000; index++) {
		ByteReader reader(lying);
		accepted += ReadMessage(reader, message) ? 1u : 0u;
	}
	Consume(accepted);
}

BENCH("ReadMessage · 10k truncated messages", 10'000) {
	static const std::vector<std::byte> truncated = [] {
		const std::vector<std::byte> &whole = CachedBytes(1000, 1, 1);
		return std::vector<std::byte>(whole.begin(), whole.begin() + 16);
	}();

	Message &message = Parsed();
	uint32_t accepted = 0;
	for (size_t index = 0; index < 10'000; index++) {
		ByteReader reader(truncated);
		accepted += ReadMessage(reader, message) ? 1u : 0u;
	}
	Consume(accepted);
}

BENCH("ReadMessage · 10k messages with an unknown kind byte", 10'000) {
	// **A kind byte is range-checked before the cast.** Casting it anyway
	// produces a value no switch handles, and every dispatch downstream then
	// reads something the type says cannot exist. The check is one comparison
	// and this row is what it costs — which should be the cheapest refusal in
	// the file, because it happens before anything else is looked at.
	static const std::vector<std::byte> unknown = [] {
		std::vector<std::byte> bytes = CachedBytes(1000, 1, 1);
		// The kind rides near the front, after the version. Corrupting the
		// first few bytes reaches it whichever way round they are ordered.
		bytes[0] = static_cast<std::byte>(0xEEu);
		bytes[1] = static_cast<std::byte>(0xEEu);
		return bytes;
	}();

	Message &message = Parsed();
	uint32_t accepted = 0;
	for (size_t index = 0; index < 10'000; index++) {
		ByteReader reader(unknown);
		accepted += ReadMessage(reader, message) ? 1u : 0u;
	}
	Consume(accepted);
}

// --- a server's tick ----------------------------------------------------------

BENCH("tick · 100 clients each sent a 500-entity delta", 100) {
	// **The shape of the real bill.** One iteration is one client, so the figure
	// is what a server spends per client per tick on encoding alone — before
	// sealing, before framing, before the socket. Multiply by 60 for a second.
	//
	// 500 entities per client rather than the whole world, because interest
	// management is supposed to have narrowed it; if a deployment is sending
	// every client the whole world, use the 10k rows above instead and the
	// number gets twenty times worse in exactly the way that should.
	const Delta &delta = Cached(500, 2, 1);
	ByteWriter &writer = Reused();
	size_t bytes = 0;
	for (size_t client = 0; client < 100; client++) {
		writer.Clear();
		WriteMessage(writer, delta);
		bytes += writer.Size();
	}
	Consume(bytes);
}
