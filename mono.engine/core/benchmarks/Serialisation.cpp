// What the one byte layout costs, at the sizes a snapshot actually reaches.
//
// `ByteWriter` writes little-endian a byte at a time rather than copying, so a
// big-endian host produces the same bytes as everyone else. That is a
// correctness decision with a price, and this suite is where the price is a
// figure: the `WriteRaw` rows are the same bytes moved by `memcpy`, so the gap
// between them and the scalar rows is exactly what portability costs per
// megabyte.
//
// **The reader is the half that faces hostile input**, and it is bounds-checked
// per read with a sticky failure flag. The `Read` rows measure that check on
// the path where it always passes; the `truncated` row measures the path where
// it always fails, which is what a corrupt packet costs to reject. A rejection
// that costs more than an accept is a denial-of-service surface, so the two are
// reported next to each other on purpose.
//
// Sizes are chosen against real users of this format rather than round numbers:
// a world snapshot is hundreds of kilobytes, a bus envelope is hundreds of
// bytes, and a save file is megabytes. All three are here.

#include <engine/core/Bytes.hpp>
#include <engine/core/Name.hpp>
#include <engine/testing/Bench.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.core.bench.serialisation")

using engine::core::ByteReader;
using engine::core::ByteWriter;
using engine::core::Name;
using engine::testing::Consume;

namespace serialisation_bench {

	// Fields per record, and the shape of one.
	//
	// A component row in a snapshot is a handful of scalars and occasionally a
	// name — not a single scalar and not a megabyte. Benchmarking one
	// `WriteUInt32` in a loop would measure the call and miss the thing that
	// actually costs: the buffer growth and the branch per field.
	constexpr size_t RECORDS = 20'000;

	// Bytes moved by the block rows. A megabyte is a save file; it is also large
	// enough that the result is bandwidth rather than the call.
	constexpr size_t BLOCK_BYTES = 1u << 20;

	// A megabyte of bytes with no structure, for the `WriteRaw` rows.
	const std::vector<std::byte> &Block() {
		static const std::vector<std::byte> block = [] {
			std::vector<std::byte> built(BLOCK_BYTES);
			for (size_t index = 0; index < built.size(); index++) {
				built[index] = static_cast<std::byte>(index * 31u);
			}
			return built;
		}();
		return block;
	}

	// Strings of the length a property name or a short field actually is.
	//
	// Length drives both the memcpy and, for `ReadName`, the hash — so a pool of
	// one-character strings would report a number no call site sees.
	const std::vector<std::string> &Strings() {
		static const std::vector<std::string> strings = [] {
			std::vector<std::string> built;
			built.reserve(256);
			for (size_t index = 0; index < 256; index++) {
				built.push_back("engine.bench.bytes.Field" + std::to_string(index));
			}
			return built;
		}();
		return strings;
	}

	// The names those strings intern to, interned once so the `WriteName` rows
	// do not measure the registry growing.
	const std::vector<Name> &Names() {
		static const std::vector<Name> names = [] {
			std::vector<Name> built;
			for (const std::string &text : Strings()) {
				built.emplace_back(text);
			}
			return built;
		}();
		return names;
	}

	// Writes `RECORDS` mixed-scalar records into `writer`, which the caller has
	// already cleared.
	void WriteScalarRecords(ByteWriter &writer) {
		for (size_t index = 0; index < RECORDS; index++) {
			writer.WriteUInt32(static_cast<uint32_t>(index));
			writer.WriteFloat(static_cast<float>(index) * 0.5f);
			writer.WriteFloat(static_cast<float>(index) * 0.25f);
			writer.WriteFloat(static_cast<float>(index) * 0.125f);
			writer.WriteUInt64(static_cast<uint64_t>(index) << 20);
			writer.WriteBool((index & 1u) != 0u);
			writer.WriteInt16(static_cast<int16_t>(index));
		}
	}

	// One buffer holding `RECORDS` of those, built once. The read rows walk this
	// rather than re-encoding, so they measure the reader and not the writer.
	const std::vector<std::byte> &ScalarBuffer() {
		static const std::vector<std::byte> buffer = [] {
			ByteWriter writer;
			WriteScalarRecords(writer);
			const std::span<const std::byte> bytes = writer.Bytes();
			return std::vector<std::byte>(bytes.begin(), bytes.end());
		}();
		return buffer;
	}

	// The same for string records, which is the path with a length prefix and a
	// bounds check that can actually fail.
	const std::vector<std::byte> &StringBuffer() {
		static const std::vector<std::byte> buffer = [] {
			ByteWriter writer;
			const std::vector<std::string> &strings = Strings();
			for (size_t index = 0; index < RECORDS; index++) {
				writer.WriteString(strings[index % strings.size()]);
			}
			const std::span<const std::byte> bytes = writer.Bytes();
			return std::vector<std::byte>(bytes.begin(), bytes.end());
		}();
		return buffer;
	}

	// A writer held across samples, because that is how the header says to use
	// one: `Clear` keeps the capacity, so anything per-tick stops allocating
	// after the frame that reached its high-water mark. Constructing a fresh
	// writer per sample would measure the allocator instead.
	ByteWriter &Reused() {
		static ByteWriter writer(1u << 21);
		return writer;
	}
}

using namespace serialisation_bench;

// --- writing scalars ----------------------------------------------------------

BENCH("write · 20k mixed-scalar records, reused writer", RECORDS) {
	ByteWriter &writer = Reused();
	writer.Clear();
	WriteScalarRecords(writer);
	Consume(writer.Size());
}

BENCH("write · 20k mixed-scalar records, fresh writer", RECORDS) {
	// The same work with the capacity thrown away between samples, which is what
	// a caller constructing a writer per tick gets. **The difference against the
	// row above is the whole reason `Clear` keeps the capacity**, and it is the
	// number to quote at anybody who wants to make `ByteWriter` a value type
	// returned by a function.
	ByteWriter writer;
	WriteScalarRecords(writer);
	Consume(writer.Size());
}

BENCH("write · 20k uint32 only", RECORDS) {
	// One field, so the per-call overhead is undiluted by the memory traffic of
	// a wide record. Read against the mixed row divided by seven.
	ByteWriter &writer = Reused();
	writer.Clear();
	for (size_t index = 0; index < RECORDS; index++) {
		writer.WriteUInt32(static_cast<uint32_t>(index));
	}
	Consume(writer.Size());
}

// --- reading scalars ----------------------------------------------------------

BENCH("read · 20k mixed-scalar records", RECORDS) {
	const std::vector<std::byte> &buffer = ScalarBuffer();
	ByteReader reader(buffer);
	uint64_t mixed = 0;
	for (size_t index = 0; index < RECORDS; index++) {
		mixed += reader.ReadUInt32();
		mixed += static_cast<uint64_t>(reader.ReadFloat());
		mixed += static_cast<uint64_t>(reader.ReadFloat());
		mixed += static_cast<uint64_t>(reader.ReadFloat());
		mixed += reader.ReadUInt64();
		mixed += reader.ReadBool() ? 1u : 0u;
		mixed += static_cast<uint64_t>(reader.ReadInt16());
	}
	Consume(mixed);
	Consume(reader.AtEnd());
}

BENCH("read · 20k uint32 only", RECORDS) {
	const std::vector<std::byte> &buffer = ScalarBuffer();
	ByteReader reader(buffer);
	uint64_t total = 0;
	for (size_t index = 0; index < RECORDS; index++) {
		total += reader.ReadUInt32();
		reader.Skip(23);
	}
	Consume(total);
}

// --- text ---------------------------------------------------------------------

BENCH("write · 20k strings", RECORDS) {
	ByteWriter &writer = Reused();
	writer.Clear();
	const std::vector<std::string> &strings = Strings();
	for (size_t index = 0; index < RECORDS; index++) {
		writer.WriteString(strings[index % strings.size()]);
	}
	Consume(writer.Size());
}

BENCH("read · 20k strings, no copy", RECORDS) {
	// `ReadString` hands back a view into the source, which is what makes a
	// message with a dozen strings cost nothing to parse. This row is that claim
	// as a number, and the `ReadName` row below is what the same bytes cost when
	// they do have to be looked up.
	const std::vector<std::byte> &buffer = StringBuffer();
	ByteReader reader(buffer);
	size_t bytes = 0;
	for (size_t index = 0; index < RECORDS; index++) {
		bytes += reader.ReadString().size();
	}
	Consume(bytes);
	Consume(reader.AtEnd());
}

BENCH("write · 20k names", RECORDS) {
	// A name is written as its text, so this is the string row plus a `Text()`
	// call under the registry's shared lock. Anything more than that gap means
	// the lock has become the format's problem.
	ByteWriter &writer = Reused();
	writer.Clear();
	const std::vector<Name> &names = Names();
	for (size_t index = 0; index < RECORDS; index++) {
		writer.WriteName(names[index % names.size()]);
	}
	Consume(writer.Size());
}

BENCH("read · 20k names", RECORDS) {
	// **The expensive half of the format, and the one a snapshot pays per
	// field.** Reading a name is a bounds-checked length, a view, and then a
	// registry lookup that hashes the whole string — so this row should sit far
	// above `read · 20k strings`, and how far above is what a caller saves by
	// interning field names once at load instead of per message.
	const std::vector<std::byte> &buffer = StringBuffer();
	ByteReader reader(buffer);
	uint64_t ids = 0;
	for (size_t index = 0; index < RECORDS; index++) {
		ids += reader.ReadName().Id();
	}
	Consume(ids);
	Consume(reader.AtEnd());
}

// --- blocks -------------------------------------------------------------------
//
// The one path that is a straight copy. Everything above is measured against
// this: the scalar rows do the same byte movement with a branch and a shift per
// byte, and the ratio is what the wire format's portability costs.

BENCH("write · 1 MiB raw block", 1) {
	ByteWriter &writer = Reused();
	writer.Clear();
	const std::vector<std::byte> &block = Block();
	writer.WriteRaw(block.data(), block.size());
	Consume(writer.Size());
}

BENCH("read · 1 MiB raw block, copied", 1) {
	static std::vector<std::byte> destination(BLOCK_BYTES);
	const std::vector<std::byte> &block = Block();
	ByteReader reader(block);
	Consume(reader.ReadRaw(destination.data(), destination.size()));
}

BENCH("read · 1 MiB raw block, borrowed", 1) {
	// No copy at all — a bounds check and a span. Against the row above, this is
	// what a caller saves by borrowing a component column instead of copying it,
	// and it should be very nearly free.
	const std::vector<std::byte> &block = Block();
	ByteReader reader(block);
	Consume(reader.ReadRawView(block.size()).size());
}

BENCH("control · memcpy 1 MiB", 1) {
	static std::vector<std::byte> destination(BLOCK_BYTES);
	const std::vector<std::byte> &block = Block();
	std::memcpy(destination.data(), block.data(), block.size());
	Consume(destination[0]);
}

// --- the hostile path ---------------------------------------------------------

BENCH("read · 20k refused reads on a truncated buffer", RECORDS) {
	// **What rejecting a corrupt buffer costs**, which is the number that says
	// whether a malformed packet is cheaper or dearer to handle than a valid
	// one. The failure flag is sticky, so every read after the first is supposed
	// to return immediately without touching the buffer — if this row is not
	// comfortably *faster* than `read · 20k mixed-scalar records`, the sticky
	// flag is not short-circuiting and a truncated packet costs a peer more than
	// a real one does.
	static const std::vector<std::byte> truncated(3);
	ByteReader reader(truncated);
	uint64_t total = 0;
	for (size_t index = 0; index < RECORDS; index++) {
		total += reader.ReadUInt64();
	}
	Consume(total);
	Consume(reader.Failed());
}

BENCH("read · 20k strings claiming a length the buffer has not got", RECORDS) {
	// The allocation-from-a-length-field attack, refused. `ReadString` must fail
	// on a length larger than the bytes remaining rather than reserving it, so
	// this row is bounded and flat; a row that grew with the claimed length
	// would be the bug itself.
	static const std::vector<std::byte> lying = [] {
		ByteWriter writer;
		writer.WriteUInt32(0xFFFF'FFF0u);
		const std::span<const std::byte> bytes = writer.Bytes();
		return std::vector<std::byte>(bytes.begin(), bytes.end());
	}();

	size_t bytes = 0;
	for (size_t index = 0; index < RECORDS; index++) {
		ByteReader reader(lying);
		bytes += reader.ReadString().size();
	}
	Consume(bytes);
}
