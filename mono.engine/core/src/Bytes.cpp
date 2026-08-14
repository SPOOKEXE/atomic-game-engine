#include <engine/core/Bytes.hpp>

#include <cstring>

namespace engine::core {

	namespace {
		// Appends `width` little-endian bytes of `value`.
		//
		// A shift loop rather than a memcpy of the object representation. The
		// memcpy would be one instruction on the machines anybody is building
		// on today and would silently produce a different file on one that is
		// not - and "the format is whatever this compiler emitted" is the thing
		// this file exists to prevent. The optimiser recognises the loop.
		template <class T> void AppendLittleEndian(std::vector<std::byte> &buffer, T value, size_t width) {
			for (size_t index = 0; index < width; index++) {
				buffer.push_back(static_cast<std::byte>((value >> (index * 8)) & 0xFFu));
			}
		}

		// Reads `width` little-endian bytes into a T.
		template <class T> T LoadLittleEndian(const std::byte *source, size_t width) {
			T value = 0;
			for (size_t index = 0; index < width; index++) {
				value |= static_cast<T>(static_cast<uint8_t>(source[index])) << (index * 8);
			}
			return value;
		}
	}

	// --- ByteWriter --------------------------------------------------------

	ByteWriter::ByteWriter(size_t reserveBytes) {
		if (reserveBytes > 0) {
			Buffer.reserve(reserveBytes);
		}
	}

	void ByteWriter::Clear() {
		// clear() rather than a fresh vector: the capacity is the point.
		Buffer.clear();
	}

	void ByteWriter::Reserve(size_t bytes) {
		Buffer.reserve(bytes);
	}

	void ByteWriter::WriteUInt8(uint8_t value) {
		Buffer.push_back(static_cast<std::byte>(value));
	}

	void ByteWriter::WriteUInt16(uint16_t value) {
		AppendLittleEndian<uint32_t>(Buffer, value, 2);
	}

	void ByteWriter::WriteUInt32(uint32_t value) {
		AppendLittleEndian<uint32_t>(Buffer, value, 4);
	}

	void ByteWriter::WriteUInt64(uint64_t value) {
		AppendLittleEndian<uint64_t>(Buffer, value, 8);
	}

	// The signed writers convert to the unsigned type of the same width first.
	// C++20 fixes integers as two's complement, so this conversion is
	// value-preserving in the only sense that matters here: the bit pattern is
	// the one the standard now guarantees, and the reader converts back the
	// same way.

	void ByteWriter::WriteInt8(int8_t value) {
		WriteUInt8(static_cast<uint8_t>(value));
	}

	void ByteWriter::WriteInt16(int16_t value) {
		WriteUInt16(static_cast<uint16_t>(value));
	}

	void ByteWriter::WriteInt32(int32_t value) {
		WriteUInt32(static_cast<uint32_t>(value));
	}

	void ByteWriter::WriteInt64(int64_t value) {
		WriteUInt64(static_cast<uint64_t>(value));
	}

	void ByteWriter::WriteBool(bool value) {
		WriteUInt8(value ? 1u : 0u);
	}

	void ByteWriter::WriteFloat(float value) {
		WriteUInt32(std::bit_cast<uint32_t>(value));
	}

	void ByteWriter::WriteDouble(double value) {
		WriteUInt64(std::bit_cast<uint64_t>(value));
	}

	void ByteWriter::WriteString(std::string_view text) {
		// Truncate rather than write a length the reader is required to refuse.
		// A writer that can emit a buffer its own reader rejects is a worse
		// failure than a lost tail, because it turns up as corruption at the
		// far end rather than as a wrong value here.
		const size_t length = text.size() > MAXIMUM_LENGTH ? MAXIMUM_LENGTH : text.size();

		WriteUInt32(static_cast<uint32_t>(length));
		WriteRaw(text.data(), length);
	}

	void ByteWriter::WriteName(const Name &name) {
		// Text(), never Id(). An invalid Name has empty text, which is what
		// ReadName turns back into an invalid Name.
		WriteString(name.Text());
	}

	void ByteWriter::WriteRaw(const void *data, size_t bytes) {
		if (bytes == 0) {
			return;
		}

		const auto *source = static_cast<const std::byte *>(data);
		Buffer.insert(Buffer.end(), source, source + bytes);
	}

	// --- ByteReader --------------------------------------------------------

	bool ByteReader::Take(size_t bytes) {
		if (Bad) {
			return false;
		}
		// Compared against what is left rather than by adding to the cursor,
		// because `Cursor + bytes` is exactly the overflow a corrupt length
		// field is trying to cause.
		if (bytes > Source.size() - Cursor) {
			Bad = true;
			return false;
		}

		Cursor += bytes;
		return true;
	}

	uint8_t ByteReader::ReadUInt8() {
		const size_t at = Cursor;
		if (!Take(1)) {
			return 0;
		}
		return static_cast<uint8_t>(Source[at]);
	}

	uint16_t ByteReader::ReadUInt16() {
		const size_t at = Cursor;
		if (!Take(2)) {
			return 0;
		}
		return LoadLittleEndian<uint16_t>(Source.data() + at, 2);
	}

	uint32_t ByteReader::ReadUInt32() {
		const size_t at = Cursor;
		if (!Take(4)) {
			return 0;
		}
		return LoadLittleEndian<uint32_t>(Source.data() + at, 4);
	}

	uint64_t ByteReader::ReadUInt64() {
		const size_t at = Cursor;
		if (!Take(8)) {
			return 0;
		}
		return LoadLittleEndian<uint64_t>(Source.data() + at, 8);
	}

	int8_t ByteReader::ReadInt8() {
		return static_cast<int8_t>(ReadUInt8());
	}

	int16_t ByteReader::ReadInt16() {
		return static_cast<int16_t>(ReadUInt16());
	}

	int32_t ByteReader::ReadInt32() {
		return static_cast<int32_t>(ReadUInt32());
	}

	int64_t ByteReader::ReadInt64() {
		return static_cast<int64_t>(ReadUInt64());
	}

	bool ByteReader::ReadBool() {
		return ReadUInt8() != 0;
	}

	float ByteReader::ReadFloat() {
		return std::bit_cast<float>(ReadUInt32());
	}

	double ByteReader::ReadDouble() {
		return std::bit_cast<double>(ReadUInt64());
	}

	std::string_view ByteReader::ReadString() {
		const uint32_t length = ReadUInt32();
		if (Bad) {
			return {};
		}

		const size_t at = Cursor;
		if (!Take(length)) {
			// The length was longer than the buffer. Nothing was allocated and
			// nothing was read - this is the whole reason the length is checked
			// against what remains rather than trusted.
			return {};
		}

		return {reinterpret_cast<const char *>(Source.data() + at), length};
	}

	Name ByteReader::ReadName() {
		const std::string_view text = ReadString();
		if (Bad || text.empty()) {
			return Name{};
		}
		return Name(text);
	}

	bool ByteReader::ReadRaw(void *destination, size_t bytes) {
		if (bytes == 0) {
			return !Bad;
		}

		const size_t at = Cursor;
		if (!Take(bytes)) {
			return false;
		}

		std::memcpy(destination, Source.data() + at, bytes);
		return true;
	}

	std::span<const std::byte> ByteReader::ReadRawView(size_t bytes) {
		const size_t at = Cursor;
		if (!Take(bytes)) {
			return {};
		}
		return Source.subspan(at, bytes);
	}

	bool ByteReader::Skip(size_t bytes) {
		return Take(bytes);
	}
}
