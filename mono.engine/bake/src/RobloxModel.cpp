#include <engine/bake/RobloxModel.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>

#include <cstring>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <zstd.h>

namespace engine::bake {

	namespace {
		// A cursor over the file that cannot read past the end.
		//
		// `Pmx.cpp`'s shape, for `Pmx.cpp`'s reason and more so: an `.rbxm` is a
		// densely packed binary format whose every array length comes off the
		// wire, so a hand-rolled `offset +=` per field is exactly what walks off
		// a truncated file. Every read below goes through this.
		class ModelCursor {
		  public:
			explicit ModelCursor(std::span<const std::byte> bytes) : Bytes(bytes) {}

			bool Failed() const {
				return Broken;
			}

			size_t Remaining() const {
				return Broken ? 0 : Bytes.size() - Offset;
			}

			uint8_t Byte() {
				if (Remaining() < 1) {
					Broken = true;
					return 0;
				}
				return static_cast<uint8_t>(Bytes[Offset++]);
			}

			uint16_t Half() {
				const uint32_t low = Byte();
				return static_cast<uint16_t>(low | (static_cast<uint32_t>(Byte()) << 8));
			}

			uint32_t Word() {
				const uint32_t low = Half();
				return low | (static_cast<uint32_t>(Half()) << 16);
			}

			uint64_t Long() {
				const uint64_t low = Word();
				return low | (static_cast<uint64_t>(Word()) << 32);
			}

			// A little-endian IEEE float, stored plainly.
			//
			// **Not every float in this format is one of these.** A value inside
			// an array is byte-transposed and has its sign bit rotated to the
			// bottom; `Rotated` below is that one. This is for the floats the
			// format writes in place, which is the nine of a raw rotation matrix.
			float Real() {
				const uint32_t bits = Word();
				float value = 0.0f;
				std::memcpy(&value, &bits, sizeof(float));
				return value;
			}

			double Double() {
				const uint64_t bits = Long();
				double value = 0.0;
				std::memcpy(&value, &bits, sizeof(double));
				return value;
			}

			// A `uint32` length and that many bytes.
			std::string Text() {
				const uint32_t length = Word();
				if (Broken || length > Remaining()) {
					Broken = true;
					return {};
				}

				const std::span<const std::byte> raw = Bytes.subspan(Offset, length);
				Offset += length;
				return std::string(reinterpret_cast<const char *>(raw.data()), raw.size());
			}

			std::span<const std::byte> Take(uint64_t count) {
				if (Broken || count > Remaining()) {
					Broken = true;
					return {};
				}

				const std::span<const std::byte> raw = Bytes.subspan(Offset, static_cast<size_t>(count));
				Offset += static_cast<size_t>(count);
				return raw;
			}

			bool Skip(uint64_t count) {
				if (count > Remaining()) {
					Broken = true;
					return false;
				}
				Offset += static_cast<size_t>(count);
				return true;
			}

		  private:
			std::span<const std::byte> Bytes;
			size_t Offset = 0;
			bool Broken = false;
		};

		// The most one chunk may inflate to.
		//
		// **The bound is on the number the file states, checked before the
		// allocation it would cause.** Both compressors here expand - LZ4 by up
		// to 255 times - so a chunk header is four bytes an attacker chooses and
		// a gigabyte somebody else allocates. A real place exported whole has no
		// chunk near this; a model file has nothing close.
		constexpr uint64_t MAXIMUM_CHUNK_BYTES = 64ull * 1024ull * 1024ull;

		// What the format calls each type, by the number it writes.
		//
		// Roblox's numbering, so it is written out rather than derived. The gaps
		// are types this does not decode; see `RobloxModel.hpp` for the argument
		// about which and why.
		enum class WireType : uint8_t {
			String = 0x01,
			Bool = 0x02,
			Int32 = 0x03,
			Float32 = 0x04,
			Float64 = 0x05,
			UDim = 0x06,
			UDim2 = 0x07,
			Color3 = 0x0C,
			Vector2 = 0x0D,
			Vector3 = 0x0E,
			CFrame = 0x10,
			Enum = 0x12,
			Referent = 0x13,
			NumberRange = 0x17,
			Rect = 0x18,
			Color3uint8 = 0x1A,
			Int64 = 0x1B,
			SharedString = 0x1C,

			// **A `Script`'s source is this and not `String`**, which is the one
			// thing about this format that a reader written from its own
			// documentation gets wrong: `ProtectedString` is a separate type
			// number carrying identical bytes, so a reader that only knows
			// `String` imports every script in the file with no program in it and
			// says nothing about the type it skipped.
			ProtectedString = 0x1D,
		};

		// A type this reader knows about but will not decode, named for the note.
		//
		// **Only the ones somebody will ask about.** Every other type falls
		// through to its number, which is the honest answer for a type this file
		// has never heard of.
		const char *NameOfRefusedType(uint8_t type) {
			switch (type) {
			case 0x08:
				return "a Ray";
			case 0x09:
				return "a Faces";
			case 0x0A:
				return "an Axes";
			case 0x0B:
				return "a BrickColor";
			case static_cast<uint8_t>(WireType::Enum):
				return "an Enum, which is a number naming a member of Roblox's table";
			case static_cast<uint8_t>(WireType::Referent):
				return "a reference, which is a number naming a row of this file";
			case 0x15:
				return "a NumberSequence";
			case 0x16:
				return "a ColorSequence";
			case 0x19:
				return "a PhysicalProperties";
			case 0x1E:
				return "an optional CFrame";
			case 0x21:
				return "a set of security capabilities";
			case 0x1F:
				return "a UniqueId";
			case 0x20:
				return "a Font";
			default:
				return nullptr;
			}
		}

		// --- the two framings a chunk's payload may arrive in --------------------

		// LZ4 block decompression, bounded by the length the chunk declared.
		//
		// **Written here rather than vendored**, which is the one place this
		// module departs from `mono.vendor/AGENTS.md`'s preference and is worth
		// stating: an LZ4 *block* is a hundred lines with no framing, no
		// checksum and no dictionary, and the alternative is a submodule for a
		// decompressor smaller than the header that would declare it. Zstandard
		// below is the opposite trade and is vendored, because a Zstandard frame
		// is entropy coding and a real library.
		//
		// **Every length is checked against both ends before it is used.** A
		// literal run must fit in what is left of the source, a match offset must
		// name something already produced, and the total must land exactly on the
		// declared size - `bake/AGENTS.md`'s PNG rule, that a stream producing
		// more or less than its own header implies is malformed either way.
		bool DecodeLz4(std::span<const std::byte> source, size_t expected, std::vector<std::byte> &out) {
			out.clear();
			out.reserve(expected);

			size_t at = 0;
			const auto next = [&]() -> uint32_t { return static_cast<uint32_t>(source[at++]); };

			// A length written as fifteen plus a run of 255s. Capped at the
			// declared output size, because anything longer is malformed and the
			// cap is what stops the accumulation overflowing.
			const auto extended = [&](uint32_t base) -> size_t {
				size_t length = base;
				if (base != 15) {
					return length;
				}
				while (at < source.size()) {
					const uint32_t part = next();
					length += part;
					if (part != 255) {
						break;
					}
					if (length > expected) {
						return expected + 1;
					}
				}
				return length;
			};

			while (at < source.size()) {
				const uint32_t token = next();

				const size_t literals = extended(token >> 4);
				if (literals > source.size() - at || out.size() + literals > expected) {
					return false;
				}
				out.insert(out.end(), source.begin() + at, source.begin() + at + literals);
				at += literals;

				// The last sequence of a block is literals and nothing else, so
				// running out here is the end rather than a truncation.
				if (at >= source.size()) {
					break;
				}
				if (source.size() - at < 2) {
					return false;
				}

				const size_t offset = next() | (next() << 8);
				if (offset == 0 || offset > out.size()) {
					return false;
				}

				const size_t match = extended(token & 0x0F) + 4;
				if (out.size() + match > expected) {
					return false;
				}

				// Byte at a time, because a match may overlap its own output -
				// an offset of one is how the format spells a run of one byte,
				// and a block copy would read what has not been written yet.
				// Through a local, so the value is not a reference into the
				// vector being appended to.
				const size_t from = out.size() - offset;
				for (size_t index = 0; index < match; index++) {
					const std::byte repeated = out[from + index];
					out.push_back(repeated);
				}
			}

			return out.size() == expected;
		}

		// Zstandard, through the vendored library.
		bool DecodeZstd(std::span<const std::byte> source, size_t expected, std::vector<std::byte> &out) {
			out.assign(expected, std::byte{});

			const size_t produced = ZSTD_decompress(out.data(), out.size(), source.data(), source.size());
			if (ZSTD_isError(produced) != 0 || produced != expected) {
				out.clear();
				return false;
			}
			return true;
		}

		// --- the transposed arrays every numeric property is written as ----------

		// `count` values of `width` bytes, stored byte plane by byte plane.
		//
		// **The format stores the most significant byte of every value, then the
		// next of every value, and so on** - which is what makes a column of
		// similar numbers compress, and is why nothing in a property payload can
		// be read with a `memcpy`. Reading it as a plain array gives numbers that
		// are wrong rather than absent, which is the failure this comment exists
		// to prevent somebody reintroducing.
		bool ReadWords(ModelCursor &cursor, size_t count, std::vector<uint32_t> &out) {
			const std::span<const std::byte> raw = cursor.Take(static_cast<uint64_t>(count) * 4u);
			if (cursor.Failed()) {
				return false;
			}

			out.resize(count);
			for (size_t index = 0; index < count; index++) {
				out[index] = (static_cast<uint32_t>(raw[index]) << 24) |
							 (static_cast<uint32_t>(raw[count + index]) << 16) |
							 (static_cast<uint32_t>(raw[2 * count + index]) << 8) |
							 static_cast<uint32_t>(raw[3 * count + index]);
			}
			return true;
		}

		bool ReadLongs(ModelCursor &cursor, size_t count, std::vector<uint64_t> &out) {
			const std::span<const std::byte> raw = cursor.Take(static_cast<uint64_t>(count) * 8u);
			if (cursor.Failed()) {
				return false;
			}

			out.resize(count);
			for (size_t index = 0; index < count; index++) {
				uint64_t value = 0;
				for (size_t plane = 0; plane < 8; plane++) {
					value = (value << 8) | static_cast<uint64_t>(raw[plane * count + index]);
				}
				out[index] = value;
			}
			return true;
		}

		// A signed integer out of the zigzag the format writes.
		int32_t Zigzag(uint32_t value) {
			return static_cast<int32_t>(value >> 1) ^ -static_cast<int32_t>(value & 1u);
		}

		int64_t Zigzag(uint64_t value) {
			return static_cast<int64_t>(value >> 1) ^ -static_cast<int64_t>(value & 1u);
		}

		// A float out of the rotation the format writes.
		//
		// The sign bit is moved to the bottom on the way out, so that a column of
		// positive numbers shares its top byte and compresses. Rotating it back is
		// the whole conversion.
		float Rotated(uint32_t value) {
			const uint32_t bits = (value >> 1) | (value << 31);
			float result = 0.0f;
			std::memcpy(&result, &bits, sizeof(float));
			return result;
		}

		// The transposed float array every vector component is written as.
		bool ReadReals(ModelCursor &cursor, size_t count, std::vector<float> &out) {
			std::vector<uint32_t> words;
			if (!ReadWords(cursor, count, words)) {
				return false;
			}

			out.resize(count);
			for (size_t index = 0; index < count; index++) {
				out[index] = Rotated(words[index]);
			}
			return true;
		}

		// The transposed, zigzagged, *cumulative* array a referent list is.
		//
		// **Cumulative is the part that is easy to miss**, and getting it wrong
		// produces a file whose instances all resolve to referent zero - one
		// instance and everything else silently missing.
		bool ReadReferents(ModelCursor &cursor, size_t count, std::vector<int32_t> &out) {
			std::vector<uint32_t> words;
			if (!ReadWords(cursor, count, words)) {
				return false;
			}

			out.resize(count);
			int32_t running = 0;
			for (size_t index = 0; index < count; index++) {
				running += Zigzag(words[index]);
				out[index] = running;
			}
			return true;
		}

		// --- the rotation a CFrame states rather than spells ---------------------

		// The six axis directions a basic rotation is built out of.
		constexpr core::Vector3 BASIS_AXES[6] = {
			{1.0f, 0.0f, 0.0f},
			{0.0f, 1.0f, 0.0f},
			{0.0f, 0.0f, 1.0f},
			{-1.0f, 0.0f, 0.0f},
			{0.0f, -1.0f, 0.0f},
			{0.0f, 0.0f, -1.0f},
		};

		// The quaternion for one of the twenty-four axis-aligned rotations, named
		// by the byte the format writes instead of a matrix.
		//
		// **The id is `6 * right + up + 1` over the axis list above**, and the
		// third axis is their cross product. That is a formula rather than a
		// checked-in table of twenty-four matrices because the two are the same
		// thing and only one of them can be got wrong in a way review would not
		// see. It was verified against a real place file: every rotation id
		// occurring across a hundred and forty thousand instances resolves to a
		// perpendicular pair, and identity - which is by far the most common -
		// comes out at the 0x02 Studio writes for it.
		//
		// A pair that is not perpendicular is not one of the twenty-four, so the
		// caller is told rather than handed a matrix with no inverse.
		bool BasicRotation(uint8_t id, glm::quat &out) {
			if (id == 0) {
				return false;
			}

			const uint32_t index = static_cast<uint32_t>(id) - 1u;
			const uint32_t rightAxis = index / 6u;
			const uint32_t upAxis = index % 6u;
			if (rightAxis >= 6u) {
				return false;
			}

			const core::Vector3 right = BASIS_AXES[rightAxis];
			const core::Vector3 up = BASIS_AXES[upAxis];
			if (right.Dot(up) != 0.0f) {
				return false;
			}

			const core::Vector3 back = right.Cross(up);

			glm::mat3 basis;
			basis[0] = glm::vec3(right.X, right.Y, right.Z);
			basis[1] = glm::vec3(up.X, up.Y, up.Z);
			basis[2] = glm::vec3(back.X, back.Y, back.Z);
			out = glm::quat_cast(basis);
			return true;
		}

		// --- the file, chunk by chunk --------------------------------------------

		// One class's row in the file: what it is called, and which instances of
		// it the `PROP` chunks below are about.
		struct WireClass {
			std::string Name;
			std::vector<int32_t> Referents;
		};

		// Everything one parse accumulates before the tree is assembled.
		//
		// **Flat, and keyed by referent only while the parse runs.** The map dies
		// with this struct; what leaves is a tree with no numbers in it.
		struct Parse {
			std::vector<RobloxInstance> Instances;
			std::vector<uint32_t> ParentOf;
			std::unordered_map<int32_t, uint32_t> ByReferent;
			std::unordered_map<int32_t, WireClass> ByClassIndex;
			std::vector<std::string> SharedStrings;
			std::vector<std::string> *Notes = nullptr;

			static constexpr uint32_t NO_PARENT = 0xFFFFFFFFu;

			// **Every silent drop in this reader passes through here**: a
			// refused property type, a duplicate referent, an orphaned child, an
			// unknown chunk. `Notes` is a vector a caller may never print, and
			// the symptom of not printing it is a model that imported with
			// pieces missing and no complaint.
			void Note(std::string text) {
				ENGINE_DEBUG("rbxm: {}", text);
				core::Metrics::Count("bake.rbxm.notes", 1.0);
				Notes->push_back(std::move(text));
			}
		};

		// Decodes one `PROP` chunk's values, or says why it did not.
		enum class PropertyResult : uint8_t {
			// Every value read.
			Decoded,

			// A type this reader does not turn into a value. The chunk is the
			// caller's to skip; nothing after it depends on its bytes.
			Unsupported,

			// The payload ran short of what its own type says it holds.
			Malformed,
		};

		PropertyResult DecodeValues(
			ModelCursor &cursor, uint8_t type, size_t count, const Parse &parse, std::vector<RobloxValue> &out
		) {
			out.assign(count, RobloxValue{});

			const auto words = [&](std::vector<uint32_t> &into) { return ReadWords(cursor, count, into); };
			const auto reals = [&](std::vector<float> &into) { return ReadReals(cursor, count, into); };

			std::vector<uint32_t> a;
			std::vector<uint32_t> b;
			std::vector<float> x;
			std::vector<float> y;
			std::vector<float> z;
			std::vector<float> w;

			switch (static_cast<WireType>(type)) {
			case WireType::String:
			case WireType::ProtectedString:
				for (RobloxValue &value : out) {
					value.Set(cursor.Text());
					if (cursor.Failed()) {
						return PropertyResult::Malformed;
					}
				}
				return PropertyResult::Decoded;

			case WireType::Bool:
				for (RobloxValue &value : out) {
					value.Set(cursor.Byte() != 0);
				}
				return cursor.Failed() ? PropertyResult::Malformed : PropertyResult::Decoded;

			case WireType::Int32:
				if (!words(a)) {
					return PropertyResult::Malformed;
				}
				for (size_t index = 0; index < count; index++) {
					out[index].Set(static_cast<int64_t>(Zigzag(a[index])));
				}
				return PropertyResult::Decoded;

			case WireType::Int64: {
				std::vector<uint64_t> longs;
				if (!ReadLongs(cursor, count, longs)) {
					return PropertyResult::Malformed;
				}
				for (size_t index = 0; index < count; index++) {
					out[index].Set(Zigzag(longs[index]));
				}
				return PropertyResult::Decoded;
			}

			case WireType::Float32:
				if (!reals(x)) {
					return PropertyResult::Malformed;
				}
				for (size_t index = 0; index < count; index++) {
					out[index].Set(static_cast<double>(x[index]));
				}
				return PropertyResult::Decoded;

			case WireType::Float64:
				// **The one numeric type written plainly.** Doubles are neither
				// transposed nor rotated, which is the format's choice and not a
				// simplification here.
				for (RobloxValue &value : out) {
					value.Set(cursor.Double());
				}
				return cursor.Failed() ? PropertyResult::Malformed : PropertyResult::Decoded;

			case WireType::UDim:
				if (!reals(x) || !words(a)) {
					return PropertyResult::Malformed;
				}
				for (size_t index = 0; index < count; index++) {
					out[index].Set(core::UDim{x[index], static_cast<float>(Zigzag(a[index]))});
				}
				return PropertyResult::Decoded;

			case WireType::UDim2:
				if (!reals(x) || !reals(y) || !words(a) || !words(b)) {
					return PropertyResult::Malformed;
				}
				for (size_t index = 0; index < count; index++) {
					out[index].Set(
						core::UDim2{
							core::UDim{x[index], static_cast<float>(Zigzag(a[index]))},
							core::UDim{y[index], static_cast<float>(Zigzag(b[index]))},
						}
					);
				}
				return PropertyResult::Decoded;

			case WireType::Vector2:
				if (!reals(x) || !reals(y)) {
					return PropertyResult::Malformed;
				}
				for (size_t index = 0; index < count; index++) {
					out[index].Set(core::Vector2{x[index], y[index]});
				}
				return PropertyResult::Decoded;

			case WireType::Vector3:
				if (!reals(x) || !reals(y) || !reals(z)) {
					return PropertyResult::Malformed;
				}
				for (size_t index = 0; index < count; index++) {
					out[index].Set(core::Vector3{x[index], y[index], z[index]});
				}
				return PropertyResult::Decoded;

			case WireType::Color3:
				if (!reals(x) || !reals(y) || !reals(z)) {
					return PropertyResult::Malformed;
				}
				for (size_t index = 0; index < count; index++) {
					out[index].Set(core::Color3{x[index], y[index], z[index]});
				}
				return PropertyResult::Decoded;

			case WireType::Color3uint8: {
				// Three plain byte planes rather than three transposed float
				// ones, and on 0..255. Divided rather than converted: the
				// engine's `Color3` holds whatever the authoring tool meant by
				// the number, which is the same trade the JSON path already
				// makes in `studio::RojoSync`.
				const std::span<const std::byte> raw = cursor.Take(static_cast<uint64_t>(count) * 3u);
				if (cursor.Failed()) {
					return PropertyResult::Malformed;
				}
				for (size_t index = 0; index < count; index++) {
					out[index].Set(
						core::Color3{
							static_cast<float>(raw[index]) / 255.0f,
							static_cast<float>(raw[count + index]) / 255.0f,
							static_cast<float>(raw[2 * count + index]) / 255.0f,
						}
					);
				}
				return PropertyResult::Decoded;
			}

			case WireType::Rect:
				if (!reals(x) || !reals(y) || !reals(z) || !reals(w)) {
					return PropertyResult::Malformed;
				}
				for (size_t index = 0; index < count; index++) {
					out[index].Set(
						core::Rect{
							core::Vector2{x[index], y[index]},
							core::Vector2{z[index], w[index]},
						}
					);
				}
				return PropertyResult::Decoded;

			case WireType::NumberRange:
				// Two plain floats each, in place. Not an array of pairs and not
				// a pair of arrays.
				for (RobloxValue &value : out) {
					const float minimum = cursor.Real();
					const float maximum = cursor.Real();
					value.Set(core::NumberRange{minimum, maximum});
				}
				return cursor.Failed() ? PropertyResult::Malformed : PropertyResult::Decoded;

			case WireType::CFrame: {
				// **The rotations come first, all of them, and the positions
				// after** - one transposed `Vector3` array for the whole chunk.
				// A reader that expected a position beside each rotation would
				// still consume the right number of bytes and produce a whole
				// model in the wrong places.
				std::vector<glm::quat> rotations(count, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
				for (size_t index = 0; index < count; index++) {
					const uint8_t id = cursor.Byte();
					if (cursor.Failed()) {
						return PropertyResult::Malformed;
					}

					if (id == 0) {
						// The nine of a 3x3, written plainly and in rows.
						// Roblox's space is right-handed and Y-up with -Z
						// forward, which is this engine's, so the basis vectors
						// transfer with no conversion at all.
						float matrix[9] = {};
						for (float &component : matrix) {
							component = cursor.Real();
						}
						if (cursor.Failed()) {
							return PropertyResult::Malformed;
						}

						glm::mat3 basis;
						basis[0] = glm::vec3(matrix[0], matrix[3], matrix[6]);
						basis[1] = glm::vec3(matrix[1], matrix[4], matrix[7]);
						basis[2] = glm::vec3(matrix[2], matrix[5], matrix[8]);
						rotations[index] = glm::quat_cast(basis);
						continue;
					}

					if (!BasicRotation(id, rotations[index])) {
						return PropertyResult::Malformed;
					}
				}

				if (!reals(x) || !reals(y) || !reals(z)) {
					return PropertyResult::Malformed;
				}
				for (size_t index = 0; index < count; index++) {
					out[index].Set(
						core::CFrame(core::Vector3{x[index], y[index], z[index]}, rotations[index])
					);
				}
				return PropertyResult::Decoded;
			}

			case WireType::SharedString:
				// An index into the file's own `SSTR` table, resolved here so
				// that nothing outside this parse ever sees the number.
				if (!words(a)) {
					return PropertyResult::Malformed;
				}
				for (size_t index = 0; index < count; index++) {
					if (a[index] < parse.SharedStrings.size()) {
						out[index].Set(parse.SharedStrings[a[index]]);
					} else {
						out[index].Set(std::string{});
					}
				}
				return PropertyResult::Decoded;

			default:
				return PropertyResult::Unsupported;
			}
		}

		// The `INST` chunk: one class, and every instance of it in this file.
		bool ReadInstances(ModelCursor &cursor, Parse &parse, std::string &failure) {
			const int32_t classIndex = static_cast<int32_t>(cursor.Word());
			const std::string className = cursor.Text();
			const uint8_t isService = cursor.Byte();
			const int64_t declared = static_cast<int32_t>(cursor.Word());
			if (cursor.Failed()) {
				failure = "rbxm: an INST chunk ends inside its own header";
				return false;
			}

			if (declared < 0 || static_cast<uint64_t>(declared) > MAXIMUM_ROBLOX_INSTANCES ||
				parse.Instances.size() + static_cast<uint64_t>(declared) > MAXIMUM_ROBLOX_INSTANCES) {
				failure = "rbxm: more instances than this will read";
				return false;
			}

			const size_t count = static_cast<size_t>(declared);
			std::vector<int32_t> referents;
			if (!ReadReferents(cursor, count, referents)) {
				failure = "rbxm: an INST chunk claims more instances than it holds";
				return false;
			}

			// A service instance carries a trailing flag each, saying whether it
			// is the root one. Nothing here has services, so they are stepped
			// over rather than read - but they are *stepped over*, because the
			// chunk does not end where it would without them.
			if (isService != 0 && !cursor.Skip(count)) {
				failure = "rbxm: an INST chunk of services ends before its flags";
				return false;
			}

			if (parse.ByClassIndex.count(classIndex) != 0) {
				parse.Note(className + " is declared under a class index already used - skipped");
				return true;
			}

			WireClass &klass = parse.ByClassIndex[classIndex];
			klass.Name = className;
			klass.Referents = referents;

			for (const int32_t referent : referents) {
				const uint32_t at = static_cast<uint32_t>(parse.Instances.size());
				if (!parse.ByReferent.emplace(referent, at).second) {
					// Two instances cannot be the same row. Kept as its own
					// instance so nothing is lost, and left out of the map so the
					// first claimant keeps the referent.
					parse.Note(className + " reuses a referent another instance already has");
				}

				RobloxInstance &instance = parse.Instances.emplace_back();
				instance.ClassName = className;
				instance.Name = className;
				parse.ParentOf.push_back(Parse::NO_PARENT);
			}
			return true;
		}

		// The `PROP` chunk: one property of one class, for every instance of it.
		bool ReadProperties(ModelCursor &cursor, Parse &parse, std::string &failure) {
			const int32_t classIndex = static_cast<int32_t>(cursor.Word());
			const std::string name = cursor.Text();
			const uint8_t type = cursor.Byte();
			if (cursor.Failed()) {
				failure = "rbxm: a PROP chunk ends inside its own header";
				return false;
			}

			const auto klass = parse.ByClassIndex.find(classIndex);
			if (klass == parse.ByClassIndex.end()) {
				parse.Note(name + " belongs to a class this file never declared - skipped");
				return true;
			}

			const std::vector<int32_t> &referents = klass->second.Referents;
			std::vector<RobloxValue> values;

			switch (DecodeValues(cursor, type, referents.size(), parse, values)) {
			case PropertyResult::Unsupported: {
				const char *refused = NameOfRefusedType(type);
				if (refused != nullptr) {
					parse.Note(klass->second.Name + "." + name + " is " + refused + " - skipped");
				} else {
					parse.Note(
						klass->second.Name + "." + name + " is a type this reader does not know (" +
						std::to_string(static_cast<int>(type)) + ") - skipped"
					);
				}
				return true;
			}

			case PropertyResult::Malformed:
				failure = "rbxm: " + klass->second.Name + "." + name + " holds less than its type says";
				return false;

			case PropertyResult::Decoded:
				break;
			}

			for (size_t index = 0; index < referents.size(); index++) {
				const auto found = parse.ByReferent.find(referents[index]);
				if (found == parse.ByReferent.end()) {
					continue;
				}

				RobloxInstance &instance = parse.Instances[found->second];

				// **`Name` becomes the instance's name and is not also a
				// property**, so that nothing downstream has two places to read
				// one fact from. Anything else keeps the spelling the file used.
				if (name == "Name" && values[index].Kind() == RobloxValueKind::Text) {
					instance.Name = values[index].As<std::string>();
					continue;
				}
				instance.Properties.push_back(RobloxProperty{name, std::move(values[index])});
			}
			return true;
		}

		// The `PRNT` chunk: which instance is inside which.
		bool ReadParents(ModelCursor &cursor, Parse &parse, std::string &failure) {
			const uint8_t version = cursor.Byte();
			const int64_t declared = static_cast<int32_t>(cursor.Word());
			if (cursor.Failed()) {
				failure = "rbxm: a PRNT chunk ends inside its own header";
				return false;
			}

			if (version != 0) {
				parse.Note("the parent table is a version this reader does not know - the tree is flat");
				return true;
			}
			if (declared < 0 || static_cast<uint64_t>(declared) > parse.Instances.size()) {
				failure = "rbxm: the parent table names more instances than the file declared";
				return false;
			}

			const size_t count = static_cast<size_t>(declared);
			std::vector<int32_t> children;
			std::vector<int32_t> parents;
			if (!ReadReferents(cursor, count, children) || !ReadReferents(cursor, count, parents)) {
				failure = "rbxm: the parent table claims more links than it holds";
				return false;
			}

			for (size_t index = 0; index < count; index++) {
				const auto child = parse.ByReferent.find(children[index]);
				if (child == parse.ByReferent.end()) {
					parse.Note("the parent table names an instance this file does not hold - skipped");
					continue;
				}

				// Minus one is the format's spelling of "nothing above it".
				if (parents[index] < 0) {
					continue;
				}

				const auto parent = parse.ByReferent.find(parents[index]);
				if (parent == parse.ByReferent.end()) {
					parse.Note(
						parse.Instances[child->second].Name +
						" is inside an instance this file does not hold - left at the top"
					);
					continue;
				}

				if (parse.ParentOf[child->second] != Parse::NO_PARENT) {
					parse.Note(
						parse.Instances[child->second].Name + " is inside two instances - kept the first"
					);
					continue;
				}
				parse.ParentOf[child->second] = parent->second;
			}
			return true;
		}

		// Moves one instance and everything under it out of the flat list.
		//
		// Bounded by `MAXIMUM_ROBLOX_DEPTH`, which the caller has already proved
		// every chain obeys - so this recursion cannot be driven off the stack by
		// a file claiming a million levels.
		RobloxInstance
		Detach(Parse &parse, const std::vector<std::vector<uint32_t>> &childrenOf, uint32_t at) {
			RobloxInstance node = std::move(parse.Instances[at]);
			for (const uint32_t child : childrenOf[at]) {
				node.Children.push_back(Detach(parse, childrenOf, child));
			}
			return node;
		}

		// Turns the flat list and its parent links into the tree that leaves here.
		//
		// **The depth walk is the cycle check.** A chain that has not reached a
		// root within `MAXIMUM_ROBLOX_DEPTH` steps is either deeper than this
		// reads or a loop, and both are the same answer - the file is refused,
		// rather than a subtree being quietly dropped or the assembly recursing
		// until the stack runs out.
		bool Assemble(Parse &parse, RobloxModel &out, std::string &failure) {
			std::vector<std::vector<uint32_t>> childrenOf(parse.Instances.size());
			for (uint32_t index = 0; index < parse.Instances.size(); index++) {
				const uint32_t parent = parse.ParentOf[index];
				if (parent != Parse::NO_PARENT) {
					childrenOf[parent].push_back(index);
				}
			}

			for (uint32_t index = 0; index < parse.Instances.size(); index++) {
				uint32_t steps = 0;
				uint32_t at = index;
				while (parse.ParentOf[at] != Parse::NO_PARENT) {
					at = parse.ParentOf[at];
					steps++;
					if (steps > MAXIMUM_ROBLOX_DEPTH) {
						failure = "rbxm: the parent table nests deeper than this reads, or loops";
						return false;
					}
				}
			}

			for (uint32_t index = 0; index < parse.Instances.size(); index++) {
				if (parse.ParentOf[index] == Parse::NO_PARENT) {
					out.Roots.push_back(Detach(parse, childrenOf, index));
				}
			}
			return true;
		}
	}

	bool ReadRobloxModel(std::span<const std::byte> bytes, RobloxModel &out, std::string &failure) {
		// The signature is eight readable bytes and six that are not, and the
		// second half is the half that matters: it is what a text editor or an
		// FTP client in text mode would have mangled, which is exactly the
		// accident it was chosen to catch.
		static constexpr std::string_view MAGIC = "<roblox!\x89\xff\x0d\x0a\x1a\x0a";

		if (bytes.size() < MAGIC.size() || std::memcmp(bytes.data(), MAGIC.data(), MAGIC.size()) != 0) {
			failure = "rbxm: wrong signature - an XML .rbxmx or a mangled copy reads like this";
			return false;
		}

		ModelCursor cursor(bytes);
		cursor.Skip(MAGIC.size());

		const uint16_t version = cursor.Half();
		if (version != 0) {
			failure = "rbxm: version " + std::to_string(version) + ", and this reads version 0";
			return false;
		}

		// The class and instance counts the header states are a claim about what
		// the chunks below will say, and the chunks are what this believes. They
		// are read so the cursor lands in the right place and checked so that a
		// file promising four billion instances is refused here rather than
		// after the first allocation.
		const int32_t declaredClasses = static_cast<int32_t>(cursor.Word());
		const int32_t declaredInstances = static_cast<int32_t>(cursor.Word());
		cursor.Skip(8);
		if (cursor.Failed()) {
			failure = "rbxm: the file ends inside its own header";
			return false;
		}
		if (declaredClasses < 0 || declaredInstances < 0 ||
			static_cast<uint32_t>(declaredInstances) > MAXIMUM_ROBLOX_INSTANCES) {
			failure = "rbxm: the header states a count this will not read";
			return false;
		}

		RobloxModel model;
		Parse parse;
		parse.Notes = &model.Notes;

		std::vector<std::byte> inflated;
		bool ended = false;

		while (!ended) {
			const std::span<const std::byte> name = cursor.Take(4);
			const uint32_t compressed = cursor.Word();
			const uint64_t uncompressed = cursor.Word();
			cursor.Skip(4);
			if (cursor.Failed()) {
				failure = "rbxm: the file ends where a chunk header should be";
				return false;
			}

			if (uncompressed > MAXIMUM_CHUNK_BYTES) {
				failure = "rbxm: a chunk states a size larger than this will inflate";
				return false;
			}

			const std::span<const std::byte> stored =
				cursor.Take(compressed == 0 ? uncompressed : compressed);
			if (cursor.Failed()) {
				failure = "rbxm: a chunk claims more bytes than the file holds";
				return false;
			}

			std::span<const std::byte> payload = stored;
			if (compressed != 0) {
				// **Which compressor is a property of the bytes, not of a flag.**
				// The chunk header says how long the payload is and nothing about
				// what produced it, so Zstandard's frame magic is the only thing
				// that separates the two - and a file written by an older Studio
				// has no magic and is LZ4.
				static constexpr std::byte ZSTD_MAGIC[4] = {
					std::byte{0x28}, std::byte{0xB5}, std::byte{0x2F}, std::byte{0xFD}
				};

				const bool zstd = stored.size() >= 4 && std::memcmp(stored.data(), ZSTD_MAGIC, 4) == 0;
				const bool inflatedOk = zstd ? DecodeZstd(stored, static_cast<size_t>(uncompressed), inflated)
											 : DecodeLz4(stored, static_cast<size_t>(uncompressed), inflated);

				if (!inflatedOk) {
					failure = zstd ? "rbxm: a Zstandard chunk did not inflate to the size it states"
								   : "rbxm: an LZ4 chunk did not inflate to the size it states";
					return false;
				}
				payload = inflated;
			}

			ModelCursor inner(payload);
			const std::string_view tag(reinterpret_cast<const char *>(name.data()), name.size());

			if (tag == "INST") {
				if (!ReadInstances(inner, parse, failure)) {
					return false;
				}
			} else if (tag == "PROP") {
				if (!ReadProperties(inner, parse, failure)) {
					return false;
				}
			} else if (tag == "PRNT") {
				if (!ReadParents(inner, parse, failure)) {
					return false;
				}
			} else if (tag == "SSTR") {
				inner.Skip(4);
				const int64_t declared = static_cast<int32_t>(inner.Word());
				if (inner.Failed() || declared < 0) {
					failure = "rbxm: the shared string table ends inside its own header";
					return false;
				}
				for (int64_t index = 0; index < declared; index++) {
					// Sixteen bytes of hash nothing here checks - it identifies
					// the entry to a writer deduplicating, and a reader has the
					// table in front of it.
					inner.Skip(16);
					parse.SharedStrings.push_back(inner.Text());
					if (inner.Failed()) {
						failure = "rbxm: the shared string table claims more entries than it holds";
						return false;
					}
				}
			} else if (tag == std::string_view("END\0", 4)) {
				ended = true;
			} else if (tag != "META") {
				// **Skipped rather than refused, because a chunk carries its own
				// length.** The format is Roblox's to extend and nothing after an
				// unknown chunk depends on its bytes, so this is the one place
				// where reading past something is the right answer.
				//
				// `META` is excluded from the note rather than from the skip: it
				// is document-level settings - whether Studio welds on move -
				// and holds no instance, so reporting it would be a line on every
				// file Studio has ever written.
				parse.Note("a " + std::string(tag) + " chunk is one this reader skips");
			}
		}

		if (!Assemble(parse, model, failure)) {
			return false;
		}

		if (!model.Notes.empty()) {
			ENGINE_WARN(
				"rbxm: {} root(s) read with {} thing(s) this reader could not keep",
				model.Roots.size(),
				model.Notes.size()
			);
		} else {
			ENGINE_DEBUG("rbxm: {} root(s) read whole", model.Roots.size());
		}

		out = std::move(model);
		return true;
	}
}
