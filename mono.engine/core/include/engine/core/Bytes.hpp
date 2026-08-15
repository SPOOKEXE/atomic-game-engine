#pragma once

// The one byte layout everything in this engine serialises through.
//
// A world snapshot, a bus envelope, a save file and - later - a network packet
// are all the same problem: turn values into bytes that a *different build on a
// different machine* will read back identically. So the layout is specified
// here rather than discovered from whatever `memcpy` happened to produce:
//
// - **Little-endian, always**, written a byte at a time rather than copied, so
//   a big-endian host produces the same bytes as everyone else.
// - **No padding and no alignment.** Fields sit where the previous one ended.
// - **No `size_t` on the wire.** It is 4 bytes on one target and 8 on another,
//   and a format that changes width with the compiler is not a format. Lengths
//   are `uint32_t`, counts are `uint64_t`, and both are written explicitly.
// - **`Name` is written as its text.** `core::Name`'s own header says to
//   serialize `Text()` and never `Id()`, because an id is first-seen order
//   within one process. Making that the only way to write a Name turns the rule
//   into something the API enforces instead of something a reviewer catches.
//
// Reading is the half that faces hostile input - a truncated snapshot, a
// corrupt packet, a file from a newer build - so `ByteReader` never trusts what
// it is given. It cannot read out of bounds, cannot be made to allocate from a
// length field, and cannot throw. A read that will not fit sets a sticky
// failure flag and returns a zero value, so a caller may run a whole sequence
// of reads and check `Failed()` once at the end rather than after every field.
//
// This is deliberately not a reflection system. It knows about scalars, text
// and opaque blocks; anything structured is the caller's to lay out, because
// the caller is the one whose format it is.
//
// @tier L1 · shared

#include <engine/core/Name.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::core {

	// IEEE-754 is assumed by the float and double paths, which move the bit
	// pattern rather than the value. A target without it would need a
	// conversion here rather than a silent reinterpretation somewhere else.
	static_assert(sizeof(float) == 4, "ByteWriter assumes a 4-byte float.");
	static_assert(sizeof(double) == 8, "ByteWriter assumes an 8-byte double.");

	// Appends values to a growable buffer in the layout described above.
	//
	// Reusable on purpose: `Clear` keeps the capacity, so a writer held across
	// ticks stops allocating after the first frame that reached its high-water
	// mark. That is the intended usage for anything per-tick - one writer per
	// world, cleared rather than constructed.
	//
	// @since v0.2
	class ByteWriter {
	  public:
		// Creates an empty writer, optionally reserving space up front.
		//
		// @param reserveBytes Capacity to allocate immediately, or zero for none.
		explicit ByteWriter(size_t reserveBytes = 0);

		// Drops the contents and keeps the capacity.
		//
		// The distinction is the whole reason a writer is worth holding onto.
		void Clear();

		// Grows the capacity to at least `bytes`, never shrinking it.
		//
		// @param bytes The capacity to guarantee.
		void Reserve(size_t bytes);

		// The number of bytes written so far.
		//
		// @return The current size in bytes.
		size_t Size() const {
			return Buffer.size();
		}

		// Reports whether anything has been written.
		//
		// @return `true` when no bytes have been written since construction or Clear.
		bool Empty() const {
			return Buffer.empty();
		}

		// The bytes written so far.
		//
		// Invalidated by any further write and by Clear, like any view into a
		// vector. Copy it if it has to outlive the next call.
		//
		// @return A view of the buffer, valid until the next mutation.
		std::span<const std::byte> Bytes() const {
			return {Buffer.data(), Buffer.size()};
		}

		// --- scalars -------------------------------------------------------
		//
		// Named per width rather than overloaded, because an overload set makes
		// the wire format depend on the static type at the call site. A caller
		// passing a `short` where the format says four bytes would then write
		// two, and nothing would say so until something else read it back.

		// Appends one byte.
		//
		// @param value The value to append.
		void WriteUInt8(uint8_t value);

		// Appends two little-endian bytes.
		//
		// @param value The value to append.
		void WriteUInt16(uint16_t value);

		// Appends four little-endian bytes.
		//
		// @param value The value to append.
		void WriteUInt32(uint32_t value);

		// Appends eight little-endian bytes.
		//
		// @param value The value to append.
		void WriteUInt64(uint64_t value);

		// Appends one byte holding a two's-complement value.
		//
		// @param value The value to append.
		void WriteInt8(int8_t value);

		// Appends two little-endian bytes holding a two's-complement value.
		//
		// @param value The value to append.
		void WriteInt16(int16_t value);

		// Appends four little-endian bytes holding a two's-complement value.
		//
		// @param value The value to append.
		void WriteInt32(int32_t value);

		// Appends eight little-endian bytes holding a two's-complement value.
		//
		// @param value The value to append.
		void WriteInt64(int64_t value);

		// Appends one byte, zero or one.
		//
		// Normalised rather than copied, so a `bool` holding some other bit
		// pattern cannot put an out-of-range byte into a format.
		//
		// @param value The value to append.
		void WriteBool(bool value);

		// Appends the four-byte IEEE-754 bit pattern, little-endian.
		//
		// The bit pattern rather than the value, so a NaN payload, a signalling
		// NaN and a negative zero all survive the round trip unchanged.
		//
		// @param value The value to append.
		void WriteFloat(float value);

		// Appends the eight-byte IEEE-754 bit pattern, little-endian.
		//
		// @param value The value to append.
		void WriteDouble(double value);

		// --- text and blocks -----------------------------------------------

		// Appends a four-byte length followed by that many bytes of text.
		//
		// Embedded null bytes are preserved: the length is authoritative and
		// nothing is null-terminated. Text longer than `MAXIMUM_LENGTH` is
		// truncated to it rather than writing a length the reader will refuse,
		// which would be a corrupt buffer produced by the writer.
		//
		// @param text The text to append.
		void WriteString(std::string_view text);

		// Appends a name as its interned text.
		//
		// The only way to serialize a Name, because the alternative - the id -
		// is first-seen order within one process and means nothing anywhere
		// else. An invalid Name writes an empty string and reads back invalid.
		//
		// @param name The name whose text to append.
		void WriteName(const Name &name);

		// Appends `bytes` bytes verbatim, with no length and no framing.
		//
		// For a block whose size the caller already knows or has written - a
		// component column, a fixed-size record. Nothing about it is inspected,
		// so endianness is the caller's problem here and this is the one path
		// that can put host layout into a file.
		//
		// @param data  The first byte to append. Ignored when `bytes` is zero.
		// @param bytes The number of bytes to append.
		void WriteRaw(const void *data, size_t bytes);

		// The longest text or block a length prefix can describe.
		//
		// Four bytes rather than eight because nothing this format carries is
		// four gigabytes, and a reader that has to defend against a corrupt
		// length wants the smallest field that fits the real cases.
		static constexpr uint32_t MAXIMUM_LENGTH = 0xFFFFFFFFu - 1u;

	  private:
		std::vector<std::byte> Buffer;
	};

	// Reads back what ByteWriter produced, and refuses to be led anywhere else.
	//
	// Every read is bounds-checked against the buffer it was handed. A read
	// that will not fit sets `Failed()`, consumes nothing, and returns a zero
	// value - and once failed, a reader stays failed, so a later read cannot
	// appear to succeed against bytes that were never meant for it.
	//
	// That shape is chosen over exceptions because deserialisation is a long
	// run of small reads: one check at the end is both cheaper and harder to
	// forget than a try block, and a corrupt buffer is an expected input rather
	// than an exceptional one.
	//
	// @since v0.2
	class ByteReader {
	  public:
		// Creates a reader over bytes it does not own.
		//
		// The buffer must outlive the reader, and outlive any string_view the
		// reader returned.
		//
		// @param bytes The buffer to read.
		explicit ByteReader(std::span<const std::byte> bytes) : Source(bytes) {}

		// Reports whether any read has failed.
		//
		// Sticky: once set, it stays set, and every later read returns a zero
		// value without consuming anything.
		//
		// @return `true` when a read ran past the end or was refused.
		bool Failed() const {
			return Bad;
		}

		// Reports whether every byte has been consumed and nothing failed.
		//
		// The check worth making after reading a message: a buffer with bytes
		// left over usually means the reader and the writer disagree about the
		// format, which is a bug that otherwise surfaces much later.
		//
		// @return `true` when the cursor is at the end and no read has failed.
		bool AtEnd() const {
			return !Bad && Cursor == Source.size();
		}

		// The number of bytes not yet consumed.
		//
		// @return Remaining bytes, or zero once a read has failed.
		size_t Remaining() const {
			return Bad ? 0 : Source.size() - Cursor;
		}

		// The number of bytes consumed so far.
		//
		// @return The current offset from the start of the buffer.
		size_t Position() const {
			return Cursor;
		}

		// Marks the reader failed without reading anything.
		//
		// For a caller that has decided the contents are wrong for a reason
		// this class cannot see - an unknown version, a count that contradicts
		// a header - so that one flag carries the verdict for the whole buffer.
		void Fail() {
			Bad = true;
		}

		// --- scalars -------------------------------------------------------

		// Reads one byte.
		//
		// @return The value, or zero on failure.
		uint8_t ReadUInt8();

		// Reads two little-endian bytes.
		//
		// @return The value, or zero on failure.
		uint16_t ReadUInt16();

		// Reads four little-endian bytes.
		//
		// @return The value, or zero on failure.
		uint32_t ReadUInt32();

		// Reads eight little-endian bytes.
		//
		// @return The value, or zero on failure.
		uint64_t ReadUInt64();

		// Reads one two's-complement byte.
		//
		// @return The value, or zero on failure.
		int8_t ReadInt8();

		// Reads two little-endian two's-complement bytes.
		//
		// @return The value, or zero on failure.
		int16_t ReadInt16();

		// Reads four little-endian two's-complement bytes.
		//
		// @return The value, or zero on failure.
		int32_t ReadInt32();

		// Reads eight little-endian two's-complement bytes.
		//
		// @return The value, or zero on failure.
		int64_t ReadInt64();

		// Reads one byte as a boolean.
		//
		// Any non-zero byte reads as `true` rather than failing. A byte of 2 in
		// this position means the buffer is already wrong, and the reads after
		// it will say so far more usefully than a failure here would.
		//
		// @return The value, or `false` on failure.
		bool ReadBool();

		// Reads a four-byte IEEE-754 bit pattern.
		//
		// @return The value, or zero on failure. NaN and negative zero survive.
		float ReadFloat();

		// Reads an eight-byte IEEE-754 bit pattern.
		//
		// @return The value, or zero on failure.
		double ReadDouble();

		// --- text and blocks -----------------------------------------------

		// Reads a length-prefixed string as a view into the source buffer.
		//
		// No allocation and no copy, which is what makes a message with a dozen
		// strings cost nothing to parse. The view is only valid while the
		// source buffer is, and a caller that keeps it longer must copy.
		//
		// A length larger than the bytes remaining fails rather than
		// allocating, which is the case a corrupt or hostile buffer produces.
		//
		// @return The text, or an empty view on failure.
		std::string_view ReadString();

		// Reads a length-prefixed string and interns it as a Name.
		//
		// An empty string reads back as an invalid Name, matching what
		// ByteWriter::WriteName does with one.
		//
		// @return The interned name, or an invalid Name on failure.
		Name ReadName();

		// Copies `bytes` bytes verbatim into `destination`.
		//
		// @param destination The buffer to fill. Untouched on failure.
		// @param bytes       The number of bytes to copy.
		// @return `true` when the copy happened.
		bool ReadRaw(void *destination, size_t bytes);

		// Borrows `bytes` bytes from the source without copying them.
		//
		// The span is only valid while the source buffer is.
		//
		// @param bytes The number of bytes to borrow.
		// @return The bytes, or an empty span on failure.
		std::span<const std::byte> ReadRawView(size_t bytes);

		// Skips `bytes` bytes.
		//
		// @param bytes The number of bytes to discard.
		// @return `true` when there were that many left.
		bool Skip(size_t bytes);

	  private:
		// Whether `bytes` more can be read, marking the reader failed when not.
		bool Take(size_t bytes);

		std::span<const std::byte> Source;
		size_t Cursor = 0;
		bool Bad = false;
	};
}
