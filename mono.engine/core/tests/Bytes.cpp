#include <engine/core/Bytes.hpp>
#include <engine/core/Random.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.core.bytes")

using engine::core::ByteReader;
using engine::core::ByteWriter;
using engine::core::Name;
using engine::core::Random;

namespace {
	// A reader over a writer, which is how every round-trip case starts.
	ByteReader ReaderOver(const ByteWriter &writer) {
		return ByteReader(writer.Bytes());
	}

	// The buffer as ordinary integers, for the cases that assert on the layout
	// itself rather than on what reads back.
	std::vector<uint8_t> Raw(const ByteWriter &writer) {
		std::vector<uint8_t> output;
		for (const std::byte value : writer.Bytes()) {
			output.push_back(static_cast<uint8_t>(value));
		}
		return output;
	}

	// Whether two floats are the same *bits*, which is stricter than == and is
	// the property serialisation has to preserve: == calls two NaNs different
	// and two zeros of opposite sign the same, and both of those are wrong here.
	bool SameBits(float left, float right) {
		return std::bit_cast<uint32_t>(left) == std::bit_cast<uint32_t>(right);
	}

	bool SameBits(double left, double right) {
		return std::bit_cast<uint64_t>(left) == std::bit_cast<uint64_t>(right);
	}
}

// --- the layout itself ---------------------------------------------------
//
// These assert on bytes rather than on round trips. A round trip passes just as
// happily against a big-endian writer paired with a big-endian reader, and the
// point of specifying a format is that a *different* build agrees.

TEST_CASE("integers are written little-endian", "[bytes]") {
	ByteWriter writer;
	writer.WriteUInt16(0x1234);
	writer.WriteUInt32(0x89ABCDEFu);
	writer.WriteUInt64(0x0123456789ABCDEFull);

	REQUIRE(
		Raw(writer) == std::vector<uint8_t>{
						   0x34, 0x12, 0xEF, 0xCD, 0xAB, 0x89, 0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01
					   }
	);
}

TEST_CASE("a string is a four-byte length and then the bytes", "[bytes]") {
	ByteWriter writer;
	writer.WriteString("hi");

	REQUIRE(Raw(writer) == std::vector<uint8_t>{0x02, 0x00, 0x00, 0x00, 'h', 'i'});
}

TEST_CASE("nothing is padded or aligned", "[bytes]") {
	ByteWriter writer;
	writer.WriteUInt8(1);
	writer.WriteUInt64(2);
	writer.WriteUInt8(3);

	// A struct with this layout would be 24 bytes with padding. The format is 10.
	REQUIRE(writer.Size() == 10);
}

TEST_CASE("a bool is exactly one byte, normalised", "[bytes]") {
	ByteWriter writer;
	writer.WriteBool(true);
	writer.WriteBool(false);

	REQUIRE(Raw(writer) == std::vector<uint8_t>{1, 0});
}

// --- round trips ---------------------------------------------------------

TEST_CASE("unsigned integers survive their whole range", "[bytes]") {
	ByteWriter writer;
	for (const uint8_t value : {uint8_t{0}, uint8_t{1}, uint8_t{0x7F}, uint8_t{0xFF}}) {
		writer.WriteUInt8(value);
	}
	writer.WriteUInt16(0);
	writer.WriteUInt16(std::numeric_limits<uint16_t>::max());
	writer.WriteUInt32(0);
	writer.WriteUInt32(std::numeric_limits<uint32_t>::max());
	writer.WriteUInt64(0);
	writer.WriteUInt64(std::numeric_limits<uint64_t>::max());

	ByteReader reader = ReaderOver(writer);
	REQUIRE(reader.ReadUInt8() == 0);
	REQUIRE(reader.ReadUInt8() == 1);
	REQUIRE(reader.ReadUInt8() == 0x7F);
	REQUIRE(reader.ReadUInt8() == 0xFF);
	REQUIRE(reader.ReadUInt16() == 0);
	REQUIRE(reader.ReadUInt16() == std::numeric_limits<uint16_t>::max());
	REQUIRE(reader.ReadUInt32() == 0);
	REQUIRE(reader.ReadUInt32() == std::numeric_limits<uint32_t>::max());
	REQUIRE(reader.ReadUInt64() == 0);
	REQUIRE(reader.ReadUInt64() == std::numeric_limits<uint64_t>::max());
	REQUIRE(reader.AtEnd());
}

TEST_CASE("signed integers survive their whole range", "[bytes]") {
	ByteWriter writer;
	writer.WriteInt8(std::numeric_limits<int8_t>::min());
	writer.WriteInt8(-1);
	writer.WriteInt8(0);
	writer.WriteInt8(std::numeric_limits<int8_t>::max());
	writer.WriteInt16(std::numeric_limits<int16_t>::min());
	writer.WriteInt16(std::numeric_limits<int16_t>::max());
	writer.WriteInt32(std::numeric_limits<int32_t>::min());
	writer.WriteInt32(std::numeric_limits<int32_t>::max());
	writer.WriteInt64(std::numeric_limits<int64_t>::min());
	writer.WriteInt64(std::numeric_limits<int64_t>::max());

	ByteReader reader = ReaderOver(writer);
	REQUIRE(reader.ReadInt8() == std::numeric_limits<int8_t>::min());
	REQUIRE(reader.ReadInt8() == -1);
	REQUIRE(reader.ReadInt8() == 0);
	REQUIRE(reader.ReadInt8() == std::numeric_limits<int8_t>::max());
	REQUIRE(reader.ReadInt16() == std::numeric_limits<int16_t>::min());
	REQUIRE(reader.ReadInt16() == std::numeric_limits<int16_t>::max());
	REQUIRE(reader.ReadInt32() == std::numeric_limits<int32_t>::min());
	REQUIRE(reader.ReadInt32() == std::numeric_limits<int32_t>::max());
	REQUIRE(reader.ReadInt64() == std::numeric_limits<int64_t>::min());
	REQUIRE(reader.ReadInt64() == std::numeric_limits<int64_t>::max());
	REQUIRE(reader.AtEnd());
}

TEST_CASE("floating point survives bit-for-bit, specials included", "[bytes]") {
	// The bit pattern rather than the value, because a format that turns -0.0
	// into 0.0 or collapses a NaN payload has lost information the simulation
	// may be relying on being identical between two runs.
	const float floats[] = {
		0.0f,
		-0.0f,
		1.0f,
		-1.0f,
		std::numeric_limits<float>::min(),
		std::numeric_limits<float>::max(),
		std::numeric_limits<float>::lowest(),
		std::numeric_limits<float>::denorm_min(),
		std::numeric_limits<float>::epsilon(),
		std::numeric_limits<float>::infinity(),
		-std::numeric_limits<float>::infinity(),
		std::numeric_limits<float>::quiet_NaN(),
	};

	ByteWriter writer;
	for (const float value : floats) {
		writer.WriteFloat(value);
	}
	writer.WriteDouble(-0.0);
	writer.WriteDouble(std::numeric_limits<double>::denorm_min());
	writer.WriteDouble(std::numeric_limits<double>::infinity());
	writer.WriteDouble(std::numeric_limits<double>::quiet_NaN());

	ByteReader reader = ReaderOver(writer);
	size_t mismatches = 0;
	for (const float value : floats) {
		if (!SameBits(reader.ReadFloat(), value)) {
			mismatches++;
		}
	}

	REQUIRE(mismatches == 0);
	REQUIRE(SameBits(reader.ReadDouble(), -0.0));
	REQUIRE(SameBits(reader.ReadDouble(), std::numeric_limits<double>::denorm_min()));
	REQUIRE(SameBits(reader.ReadDouble(), std::numeric_limits<double>::infinity()));
	REQUIRE(std::isnan(reader.ReadDouble()));
	REQUIRE(reader.AtEnd());
}

TEST_CASE("a signalling NaN keeps its payload", "[bytes]") {
	// Separate from the case above because this is the one a bit_cast round
	// trip protects and a float-to-double-to-float one would quietly destroy.
	const uint32_t pattern = 0x7FA0'1234u;
	const float value = std::bit_cast<float>(pattern);

	ByteWriter writer;
	writer.WriteFloat(value);

	ByteReader reader = ReaderOver(writer);
	REQUIRE(std::bit_cast<uint32_t>(reader.ReadFloat()) == pattern);
}

TEST_CASE("strings survive emptiness, nulls and length", "[bytes]") {
	const std::string embedded("a\0b\0\0c", 6);
	const std::string longer(70'000, 'x');

	ByteWriter writer;
	writer.WriteString("");
	writer.WriteString("plain");
	writer.WriteString(embedded);
	writer.WriteString(longer);

	ByteReader reader = ReaderOver(writer);
	REQUIRE(reader.ReadString().empty());
	REQUIRE(reader.ReadString() == "plain");

	const std::string_view read = reader.ReadString();
	REQUIRE(read.size() == 6);
	REQUIRE(std::memcmp(read.data(), embedded.data(), 6) == 0);

	REQUIRE(reader.ReadString() == longer);
	REQUIRE(reader.AtEnd());
}

TEST_CASE("a name round-trips through its text", "[bytes]") {
	const Name original("engine.core.bytes.round-trip");

	ByteWriter writer;
	writer.WriteName(original);

	// The id must not be on the wire: an id is first-seen order in one process.
	// Four bytes of length plus the text is the whole record.
	REQUIRE(writer.Size() == 4 + original.Text().size());

	ByteReader reader = ReaderOver(writer);
	const Name read = reader.ReadName();
	REQUIRE(read == original);
	REQUIRE(read.Text() == original.Text());
	REQUIRE(reader.AtEnd());
}

TEST_CASE("an invalid name round-trips as invalid", "[bytes]") {
	ByteWriter writer;
	writer.WriteName(Name{});

	ByteReader reader = ReaderOver(writer);
	const Name read = reader.ReadName();
	REQUIRE_FALSE(read.IsValid());
	REQUIRE(reader.AtEnd());
}

TEST_CASE("a name interns on read when the process has not seen it", "[bytes]") {
	// The reading process is a different one in every case that matters - a
	// restored snapshot, a packet from a server. Nothing may assume the text
	// was already interned.
	ByteWriter writer;
	writer.WriteString("engine.core.bytes.not-yet-interned");

	ByteReader reader = ReaderOver(writer);
	const Name read = reader.ReadName();
	REQUIRE(read.IsValid());
	REQUIRE(read.Text() == "engine.core.bytes.not-yet-interned");
}

TEST_CASE("raw blocks copy and borrow the same bytes", "[bytes]") {
	const uint32_t source[] = {1, 2, 3, 4};

	ByteWriter writer;
	writer.WriteRaw(source, sizeof(source));
	REQUIRE(writer.Size() == sizeof(source));

	uint32_t copied[4] = {};
	ByteReader reader = ReaderOver(writer);
	REQUIRE(reader.ReadRaw(copied, sizeof(copied)));
	REQUIRE(std::memcmp(copied, source, sizeof(source)) == 0);
	REQUIRE(reader.AtEnd());

	ByteReader borrowing = ReaderOver(writer);
	const std::span<const std::byte> view = borrowing.ReadRawView(sizeof(source));
	REQUIRE(view.size() == sizeof(source));
	REQUIRE(std::memcmp(view.data(), source, sizeof(source)) == 0);
}

TEST_CASE("a refused raw read leaves the destination alone", "[bytes]") {
	// **The contract `ReadRaw` states and the one a caller relies on without
	// noticing.** A component is decoded straight into its own storage, so a
	// read that copied what it had and *then* discovered it was short would
	// leave half a value there and report failure - and a caller checking
	// `Failed()` once at the end, which is the usage this class is shaped for,
	// would already have a half-written object.
	ByteWriter writer;
	writer.WriteUInt32(0xAABB'CCDDu);

	ByteReader reader(writer.Bytes());

	unsigned char destination[8];
	std::memset(destination, 0x5A, sizeof(destination));

	REQUIRE_FALSE(reader.ReadRaw(destination, sizeof(destination)));
	REQUIRE(reader.Failed());

	for (const unsigned char byte : destination) {
		CHECK(byte == 0x5A);
	}

	// And nothing was consumed either, so the four bytes that *are* there are
	// still where a caller recovering from the refusal would look for them.
	CHECK(reader.Position() == 0);
}

TEST_CASE("a zero-length write and read are both no-ops", "[bytes]") {
	ByteWriter writer;
	writer.WriteRaw(nullptr, 0);
	REQUIRE(writer.Empty());

	ByteReader reader = ReaderOver(writer);
	REQUIRE(reader.ReadRaw(nullptr, 0));
	REQUIRE_FALSE(reader.Failed());
	REQUIRE(reader.AtEnd());
}

// --- hostile and truncated input -----------------------------------------

TEST_CASE("every read on an empty buffer fails and returns zero", "[bytes]") {
	const auto fresh = [] { return ByteReader({}); };

	REQUIRE(fresh().ReadUInt8() == 0);
	REQUIRE(fresh().ReadUInt16() == 0);
	REQUIRE(fresh().ReadUInt32() == 0);
	REQUIRE(fresh().ReadUInt64() == 0);
	REQUIRE(fresh().ReadInt64() == 0);
	REQUIRE(fresh().ReadBool() == false);
	REQUIRE(SameBits(fresh().ReadFloat(), 0.0f));
	REQUIRE(SameBits(fresh().ReadDouble(), 0.0));
	REQUIRE(fresh().ReadString().empty());
	REQUIRE_FALSE(fresh().ReadName().IsValid());
	REQUIRE_FALSE(fresh().Skip(1));

	ByteReader reader = fresh();
	REQUIRE(reader.ReadUInt32() == 0);
	REQUIRE(reader.Failed());
	REQUIRE(reader.Remaining() == 0);
}

TEST_CASE("a read one byte short of the buffer fails", "[bytes]") {
	// Every width, because an off-by-one in the bounds check would show up in
	// exactly one of them.
	const auto shortBy = [](size_t bytes) {
		std::vector<std::byte> buffer(bytes - 1);
		return buffer;
	};

	{
		auto buffer = shortBy(2);
		ByteReader reader(buffer);
		REQUIRE(reader.ReadUInt16() == 0);
		REQUIRE(reader.Failed());
	}
	{
		auto buffer = shortBy(4);
		ByteReader reader(buffer);
		REQUIRE(reader.ReadUInt32() == 0);
		REQUIRE(reader.Failed());
	}
	{
		auto buffer = shortBy(8);
		ByteReader reader(buffer);
		REQUIRE(reader.ReadUInt64() == 0);
		REQUIRE(reader.Failed());
	}
}

TEST_CASE("a truncated string fails rather than reading past the end", "[bytes]") {
	ByteWriter writer;
	writer.WriteString("abcdef");

	// Drop the last two bytes of the text. The length prefix still claims six.
	std::vector<std::byte> truncated(writer.Bytes().begin(), writer.Bytes().end() - 2);

	ByteReader reader(truncated);
	REQUIRE(reader.ReadString().empty());
	REQUIRE(reader.Failed());
}

TEST_CASE("a corrupt length allocates nothing and fails", "[bytes]") {
	// The hostile case: four bytes of length claiming four gigabytes, followed
	// by nothing. A reader that trusted the length would try to reserve it.
	ByteWriter writer;
	writer.WriteUInt32(0xFFFF'FFFEu);

	ByteReader reader = ReaderOver(writer);
	REQUIRE(reader.ReadString().empty());
	REQUIRE(reader.Failed());
	REQUIRE(reader.Remaining() == 0);
}

TEST_CASE("a length near SIZE_MAX cannot overflow the cursor", "[bytes]") {
	// `Cursor + bytes` would wrap and compare as in-bounds. The check is
	// written against what remains for exactly this input.
	std::vector<std::byte> buffer(16);
	ByteReader reader(buffer);

	REQUIRE_FALSE(reader.Skip(std::numeric_limits<size_t>::max()));
	REQUIRE(reader.Failed());
}

TEST_CASE("failure is sticky", "[bytes]") {
	ByteWriter writer;
	writer.WriteUInt32(7);
	writer.WriteUInt32(9);

	// Eight bytes wanted and eight present, so this succeeds - and reads the
	// two 32-bit fields as one 64-bit value, which is what a format mismatch
	// looks like when nothing bounds-checks it into being obvious.
	ByteReader reader = ReaderOver(writer);
	REQUIRE(reader.ReadUInt64() == 0x0000'0009'0000'0007ull);
	REQUIRE_FALSE(reader.Failed());

	REQUIRE(reader.ReadUInt8() == 0); // nothing left
	REQUIRE(reader.Failed());

	// A second reader over the same bytes would succeed here. This one must
	// not: once a buffer has been misread, every later value in it is suspect.
	ByteReader poisoned = ReaderOver(writer);
	poisoned.Fail();
	REQUIRE(poisoned.ReadUInt32() == 0);
	REQUIRE(poisoned.Failed());
	REQUIRE_FALSE(poisoned.AtEnd());
}

TEST_CASE("AtEnd distinguishes exhausted from failed", "[bytes]") {
	ByteWriter writer;
	writer.WriteUInt8(1);

	ByteReader consumed = ReaderOver(writer);
	REQUIRE(consumed.ReadUInt8() == 1);
	REQUIRE(consumed.AtEnd());

	ByteReader overrun = ReaderOver(writer);
	REQUIRE(overrun.ReadUInt32() == 0);
	REQUIRE_FALSE(overrun.AtEnd());
	REQUIRE(overrun.Failed());

	ByteReader leftover = ReaderOver(writer);
	REQUIRE_FALSE(leftover.AtEnd());
}

TEST_CASE("position and remaining track the cursor", "[bytes]") {
	ByteWriter writer;
	writer.WriteUInt32(1);
	writer.WriteUInt32(2);

	ByteReader reader = ReaderOver(writer);
	REQUIRE(reader.Position() == 0);
	REQUIRE(reader.Remaining() == 8);

	reader.ReadUInt32();
	REQUIRE(reader.Position() == 4);
	REQUIRE(reader.Remaining() == 4);

	REQUIRE(reader.Skip(4));
	REQUIRE(reader.Remaining() == 0);
	REQUIRE(reader.AtEnd());
}

// --- reuse ---------------------------------------------------------------

TEST_CASE("Clear keeps the capacity", "[bytes]") {
	// The reason a writer is worth holding across ticks rather than
	// constructing per message.
	ByteWriter writer;
	for (int index = 0; index < 1000; index++) {
		writer.WriteUInt64(static_cast<uint64_t>(index));
	}

	const size_t grown = writer.Size();
	writer.Clear();

	REQUIRE(writer.Empty());
	REQUIRE(writer.Size() == 0);

	// Refilling to the same size must not have to grow again. Measured
	// indirectly: the data pointer is stable across the refill.
	const void *before = writer.Bytes().data();
	for (int index = 0; index < 1000; index++) {
		writer.WriteUInt64(static_cast<uint64_t>(index));
	}

	REQUIRE(writer.Size() == grown);
	REQUIRE(writer.Bytes().data() == before);
}

TEST_CASE("a reserved writer does not reallocate under its reservation", "[bytes]") {
	ByteWriter writer(4096);
	const void *before = writer.Bytes().data();

	for (int index = 0; index < 512; index++) {
		writer.WriteUInt64(static_cast<uint64_t>(index));
	}

	REQUIRE(writer.Size() == 4096);
	REQUIRE(writer.Bytes().data() == before);
}

// --- fuzzing -------------------------------------------------------------
//
// `core::Random` rather than a standard generator, so a failure reproduces on
// every machine from the seed alone. That is the whole reason that type exists.

TEST_CASE("random buffers never read out of bounds", "[bytes]") {
	// The reader's contract is that no input can make it misbehave, so hand it
	// arbitrary bytes and an arbitrary sequence of reads. Nothing here asserts
	// a value - the assertion is that the process survives and the failure flag
	// is consistent with how much was consumed.
	constexpr uint32_t ITERATIONS = 2'000;

	for (uint32_t iteration = 0; iteration < ITERATIONS; iteration++) {
		const size_t size = Random::Bits(iteration, 1) % 64;
		std::vector<std::byte> buffer(size);
		for (size_t index = 0; index < size; index++) {
			buffer[index] = static_cast<std::byte>(Random::Bits(iteration, static_cast<uint32_t>(index) + 2));
		}

		ByteReader reader(buffer);
		const int reads = static_cast<int>(Random::Bits(iteration, 7) % 12);

		for (int read = 0; read < reads; read++) {
			switch (Random::Bits(iteration, static_cast<uint32_t>(read) + 11) % 8) {
			case 0:
				reader.ReadUInt8();
				break;
			case 1:
				reader.ReadUInt16();
				break;
			case 2:
				reader.ReadUInt32();
				break;
			case 3:
				reader.ReadUInt64();
				break;
			case 4:
				reader.ReadFloat();
				break;
			case 5:
				reader.ReadString();
				break;
			case 6:
				reader.ReadName();
				break;
			default:
				reader.Skip(Random::Bits(iteration, 23) % 128);
				break;
			}
		}

		// Consumed at most what was there, and a reader that failed reports
		// nothing remaining rather than a stale count.
		REQUIRE(reader.Position() <= size);
		if (reader.Failed()) {
			REQUIRE(reader.Remaining() == 0);
			REQUIRE_FALSE(reader.AtEnd());
		} else {
			REQUIRE(reader.Position() + reader.Remaining() == size);
		}
	}
}

TEST_CASE("random records round-trip", "[bytes]") {
	// The other direction: whatever is written comes back. Mixed widths in a
	// random order, because a field whose width is wrong only shows up when
	// something follows it.
	constexpr uint32_t ITERATIONS = 500;
	size_t mismatches = 0;

	for (uint32_t iteration = 0; iteration < ITERATIONS; iteration++) {
		std::vector<uint64_t> integers;
		std::vector<float> reals;
		std::vector<std::string> texts;

		ByteWriter writer;
		const int fields = static_cast<int>(Random::Bits(iteration, 31) % 16) + 1;

		for (int field = 0; field < fields; field++) {
			const uint32_t salt = static_cast<uint32_t>(field) + 41;
			switch (Random::Bits(iteration, salt) % 3) {
			case 0: {
				const uint64_t value = (static_cast<uint64_t>(Random::Bits(iteration, salt + 100)) << 32) |
									   Random::Bits(iteration, salt + 200);
				integers.push_back(value);
				writer.WriteUInt8(0);
				writer.WriteUInt64(value);
				break;
			}
			case 1: {
				const float value = Random::Range(iteration, salt + 300, -1e6f, 1e6f);
				reals.push_back(value);
				writer.WriteUInt8(1);
				writer.WriteFloat(value);
				break;
			}
			default: {
				const size_t length = Random::Bits(iteration, salt + 400) % 40;
				std::string text;
				for (size_t index = 0; index < length; index++) {
					text.push_back(
						static_cast<char>(Random::Bits(iteration, salt + 500 + static_cast<uint32_t>(index)))
					);
				}
				texts.push_back(text);
				writer.WriteUInt8(2);
				writer.WriteString(text);
				break;
			}
			}
		}

		ByteReader reader(writer.Bytes());
		size_t nextInteger = 0;
		size_t nextReal = 0;
		size_t nextText = 0;

		for (int field = 0; field < fields; field++) {
			switch (reader.ReadUInt8()) {
			case 0:
				if (reader.ReadUInt64() != integers[nextInteger++]) {
					mismatches++;
				}
				break;
			case 1:
				if (!SameBits(reader.ReadFloat(), reals[nextReal++])) {
					mismatches++;
				}
				break;
			case 2: {
				const std::string_view read = reader.ReadString();
				const std::string &expected = texts[nextText++];
				if (read.size() != expected.size() ||
					std::memcmp(read.data(), expected.data(), read.size()) != 0) {
					mismatches++;
				}
				break;
			}
			default:
				mismatches++;
				break;
			}
		}

		if (!reader.AtEnd()) {
			mismatches++;
		}
	}

	REQUIRE(mismatches == 0);
}
