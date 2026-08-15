#include "Codec.hpp"

#include <algorithm>
#include <cstring>

namespace engine::script {

	namespace {
		// Little-endian, explicitly, on every platform.
		//
		// `WriteRaw` of a float would carry the host's byte order, and the one
		// machine that disagrees is the one nobody has to test on until a
		// message from it decodes as a number a thousand times too large.
		void PutU32(std::vector<std::byte> &out, uint32_t value) {
			for (int shift = 0; shift < 32; shift += 8) {
				out.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
			}
		}

		void PutU64(std::vector<std::byte> &out, uint64_t value) {
			for (int shift = 0; shift < 64; shift += 8) {
				out.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
			}
		}

		void PutFloat(std::vector<std::byte> &out, float value) {
			uint32_t bits = 0;
			std::memcpy(&bits, &value, sizeof(bits));
			PutU32(out, bits);
		}

		void PutDouble(std::vector<std::byte> &out, double value) {
			uint64_t bits = 0;
			std::memcpy(&bits, &value, sizeof(bits));
			PutU64(out, bits);
		}

		void PutTag(std::vector<std::byte> &out, ValueTag tag) {
			out.push_back(static_cast<std::byte>(tag));
		}

		// A cursor that refuses to be led past the end.
		struct Cursor {
			std::span<const std::byte> Bytes;
			size_t At = 0;

			bool Take(size_t count, const std::byte *&data) {
				if (count > Bytes.size() - At) {
					return false;
				}
				data = Bytes.data() + At;
				At += count;
				return true;
			}

			bool U32(uint32_t &value) {
				const std::byte *data = nullptr;
				if (!Take(4, data)) {
					return false;
				}
				value = 0;
				for (int index = 3; index >= 0; index--) {
					value = (value << 8) | static_cast<uint32_t>(data[index]);
				}
				return true;
			}

			bool U64(uint64_t &value) {
				const std::byte *data = nullptr;
				if (!Take(8, data)) {
					return false;
				}
				value = 0;
				for (int index = 7; index >= 0; index--) {
					value = (value << 8) | static_cast<uint64_t>(data[index]);
				}
				return true;
			}

			bool Float(float &value) {
				uint32_t bits = 0;
				if (!U32(bits)) {
					return false;
				}
				std::memcpy(&value, &bits, sizeof(value));
				return true;
			}

			bool Double(double &value) {
				uint64_t bits = 0;
				if (!U64(bits)) {
					return false;
				}
				std::memcpy(&value, &bits, sizeof(value));
				return true;
			}

			bool Remaining() const {
				return At < Bytes.size();
			}
		};

		CodecStatus Write(ScriptValue &value, std::vector<std::byte> &out, uint32_t depth) {
			if (depth > CODEC_MAX_DEPTH) {
				return CodecStatus::TooDeep;
			}

			// Checked as it grows rather than at the end, so a runaway table
			// stops at the limit instead of after the allocation that would have
			// mattered.
			if (out.size() > CODEC_MAX_BYTES) {
				return CodecStatus::TooLarge;
			}

			switch (value.Tag) {
			case ValueTag::Nil:
				PutTag(out, ValueTag::Nil);
				return CodecStatus::Ok;

			case ValueTag::False:
			case ValueTag::True:
				PutTag(out, value.Boolean ? ValueTag::True : ValueTag::False);
				return CodecStatus::Ok;

			case ValueTag::Number:
				PutTag(out, ValueTag::Number);
				PutDouble(out, value.Number);
				return CodecStatus::Ok;

			case ValueTag::String: {
				if (value.Text.size() > CODEC_MAX_BYTES) {
					return CodecStatus::TooLarge;
				}
				PutTag(out, ValueTag::String);
				PutU32(out, static_cast<uint32_t>(value.Text.size()));
				const auto *bytes = reinterpret_cast<const std::byte *>(value.Text.data());
				out.insert(out.end(), bytes, bytes + value.Text.size());
				return CodecStatus::Ok;
			}

			case ValueTag::Array: {
				PutTag(out, ValueTag::Array);
				PutU32(out, static_cast<uint32_t>(value.Items.size()));
				for (ScriptValue &item : value.Items) {
					const CodecStatus status = Write(item, out, depth + 1);
					if (status != CodecStatus::Ok) {
						return status;
					}
				}
				return CodecStatus::Ok;
			}

			case ValueTag::Map: {
				// **The sort, and it is the whole determinism guarantee.** By
				// bytes rather than by either language's own comparison, so
				// `"é"` lands in the same place whichever VM built the table.
				std::sort(
					value.Entries.begin(), value.Entries.end(), [](const auto &left, const auto &right) {
						return left.first < right.first;
					}
				);

				PutTag(out, ValueTag::Map);
				PutU32(out, static_cast<uint32_t>(value.Entries.size()));

				for (auto &entry : value.Entries) {
					if (entry.first.size() > CODEC_MAX_BYTES) {
						return CodecStatus::TooLarge;
					}

					PutU32(out, static_cast<uint32_t>(entry.first.size()));
					const auto *bytes = reinterpret_cast<const std::byte *>(entry.first.data());
					out.insert(out.end(), bytes, bytes + entry.first.size());

					const CodecStatus status = Write(entry.second, out, depth + 1);
					if (status != CodecStatus::Ok) {
						return status;
					}
				}
				return CodecStatus::Ok;
			}

			case ValueTag::Vector3:
				PutTag(out, ValueTag::Vector3);
				PutFloat(out, value.Vector.X);
				PutFloat(out, value.Vector.Y);
				PutFloat(out, value.Vector.Z);
				return CodecStatus::Ok;

			case ValueTag::Color3:
				PutTag(out, ValueTag::Color3);
				PutFloat(out, value.Colour.R);
				PutFloat(out, value.Colour.G);
				PutFloat(out, value.Colour.B);
				return CodecStatus::Ok;

			case ValueTag::CFrame:
				// Position then quaternion, which is the field order of the
				// struct - but written component by component rather than as a
				// memcpy of it, so a padding byte or a reordered field cannot
				// change what an old recording means.
				PutTag(out, ValueTag::CFrame);
				PutFloat(out, value.Frame.Position.X);
				PutFloat(out, value.Frame.Position.Y);
				PutFloat(out, value.Frame.Position.Z);
				PutFloat(out, value.Frame.QuaternionX);
				PutFloat(out, value.Frame.QuaternionY);
				PutFloat(out, value.Frame.QuaternionZ);
				PutFloat(out, value.Frame.QuaternionW);
				return CodecStatus::Ok;
			}
			return CodecStatus::Unsupported;
		}

		CodecStatus Read(Cursor &cursor, ScriptValue &out, uint32_t depth) {
			if (depth > CODEC_MAX_DEPTH) {
				return CodecStatus::TooDeep;
			}

			const std::byte *tagByte = nullptr;
			if (!cursor.Take(1, tagByte)) {
				return CodecStatus::Malformed;
			}

			const auto tag = static_cast<ValueTag>(*tagByte);
			out = ScriptValue{tag};

			switch (tag) {
			case ValueTag::Nil:
				return CodecStatus::Ok;

			case ValueTag::False:
				out.Boolean = false;
				return CodecStatus::Ok;

			case ValueTag::True:
				out.Boolean = true;
				return CodecStatus::Ok;

			case ValueTag::Number:
				return cursor.Double(out.Number) ? CodecStatus::Ok : CodecStatus::Malformed;

			case ValueTag::String: {
				uint32_t length = 0;
				if (!cursor.U32(length)) {
					return CodecStatus::Malformed;
				}

				// Checked against what is left **before** anything is reserved.
				// A corrupt payload claiming four billion bytes fails here
				// rather than in the allocator.
				const std::byte *data = nullptr;
				if (!cursor.Take(length, data)) {
					return CodecStatus::Malformed;
				}
				out.Text.assign(reinterpret_cast<const char *>(data), length);
				return CodecStatus::Ok;
			}

			case ValueTag::Array: {
				uint32_t count = 0;
				if (!cursor.U32(count)) {
					return CodecStatus::Malformed;
				}

				// One byte is the least an encoded value can be, so a count
				// larger than the bytes remaining is a lie. Reserving on a
				// trusted count is how a decoder turns a corrupt packet into an
				// allocation failure.
				if (count > cursor.Bytes.size() - cursor.At) {
					return CodecStatus::Malformed;
				}

				out.Items.resize(count);
				for (uint32_t index = 0; index < count; index++) {
					const CodecStatus status = Read(cursor, out.Items[index], depth + 1);
					if (status != CodecStatus::Ok) {
						return status;
					}
				}
				return CodecStatus::Ok;
			}

			case ValueTag::Map: {
				uint32_t count = 0;
				if (!cursor.U32(count)) {
					return CodecStatus::Malformed;
				}
				if (count > cursor.Bytes.size() - cursor.At) {
					return CodecStatus::Malformed;
				}

				out.Entries.resize(count);
				for (uint32_t index = 0; index < count; index++) {
					uint32_t length = 0;
					if (!cursor.U32(length)) {
						return CodecStatus::Malformed;
					}

					const std::byte *data = nullptr;
					if (!cursor.Take(length, data)) {
						return CodecStatus::Malformed;
					}
					out.Entries[index].first.assign(reinterpret_cast<const char *>(data), length);

					const CodecStatus status = Read(cursor, out.Entries[index].second, depth + 1);
					if (status != CodecStatus::Ok) {
						return status;
					}
				}
				return CodecStatus::Ok;
			}

			case ValueTag::Vector3:
				return cursor.Float(out.Vector.X) && cursor.Float(out.Vector.Y) && cursor.Float(out.Vector.Z)
						   ? CodecStatus::Ok
						   : CodecStatus::Malformed;

			case ValueTag::Color3:
				return cursor.Float(out.Colour.R) && cursor.Float(out.Colour.G) && cursor.Float(out.Colour.B)
						   ? CodecStatus::Ok
						   : CodecStatus::Malformed;

			case ValueTag::CFrame:
				return cursor.Float(out.Frame.Position.X) && cursor.Float(out.Frame.Position.Y) &&
							   cursor.Float(out.Frame.Position.Z) && cursor.Float(out.Frame.QuaternionX) &&
							   cursor.Float(out.Frame.QuaternionY) && cursor.Float(out.Frame.QuaternionZ) &&
							   cursor.Float(out.Frame.QuaternionW)
						   ? CodecStatus::Ok
						   : CodecStatus::Malformed;
			}

			// A tag from a newer build. Named rather than skipped: this format
			// carries no length in front of a value, so there is no way to step
			// over something unknown, and guessing would decode the rest of the
			// payload as garbage that looks like data.
			return CodecStatus::Malformed;
		}
	}

	const char *Describe(CodecStatus status) {
		switch (status) {
		case CodecStatus::Ok:
			return "ok";
		case CodecStatus::TooDeep:
			return "nested too deeply";
		case CodecStatus::TooLarge:
			return "too large";
		case CodecStatus::Cyclic:
			return "contains itself";
		case CodecStatus::Unsupported:
			return "holds something that cannot cross a world boundary";
		case CodecStatus::Malformed:
			return "malformed";
		}
		return "unknown";
	}

	bool ScriptValue::operator==(const ScriptValue &other) const {
		if (Tag != other.Tag) {
			return false;
		}

		switch (Tag) {
		case ValueTag::Nil:
			return true;
		case ValueTag::False:
		case ValueTag::True:
			return Boolean == other.Boolean;
		case ValueTag::Number:
			return Number == other.Number;
		case ValueTag::String:
			return Text == other.Text;
		case ValueTag::Array:
			return Items == other.Items;
		case ValueTag::Map:
			return Entries == other.Entries;
		case ValueTag::Vector3:
			return Vector == other.Vector;
		case ValueTag::Color3:
			return Colour.R == other.Colour.R && Colour.G == other.Colour.G && Colour.B == other.Colour.B;
		case ValueTag::CFrame:
			return Frame.Position == other.Frame.Position && Frame.QuaternionX == other.Frame.QuaternionX &&
				   Frame.QuaternionY == other.Frame.QuaternionY &&
				   Frame.QuaternionZ == other.Frame.QuaternionZ &&
				   Frame.QuaternionW == other.Frame.QuaternionW;
		}
		return false;
	}

	CodecStatus Encode(ScriptValue &value, std::vector<std::byte> &out) {
		out.clear();

		const CodecStatus status = Write(value, out, 0);
		if (status != CodecStatus::Ok) {
			out.clear();
			return status;
		}

		// The final check. The incremental one above stops a runaway early; this
		// one is what the limit actually means, and a payload that only crossed
		// on its last field would otherwise get through.
		if (out.size() > CODEC_MAX_BYTES) {
			out.clear();
			return CodecStatus::TooLarge;
		}
		return CodecStatus::Ok;
	}

	CodecStatus Decode(std::span<const std::byte> bytes, ScriptValue &out) {
		Cursor cursor{bytes, 0};

		const CodecStatus status = Read(cursor, out, 0);
		if (status != CodecStatus::Ok) {
			out = ScriptValue{};
			return status;
		}

		// Trailing bytes mean the payload was not what it claimed. Accepting
		// them would let a sender append anything it liked to a well-formed
		// message and have it ignored - which is a channel, and a channel
		// nobody audits.
		if (cursor.Remaining()) {
			out = ScriptValue{};
			return CodecStatus::Malformed;
		}
		return CodecStatus::Ok;
	}
}
