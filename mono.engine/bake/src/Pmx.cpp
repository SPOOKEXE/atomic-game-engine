#include "Importers.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace engine::bake {

	namespace {
		// A cursor over the file that cannot read past the end.
		//
		// PMX is a densely packed binary format with variable-width index
		// fields decided by a header byte, so a hand-rolled `offset +=` at each
		// field is exactly the shape that walks off the end of a truncated
		// file. Every read goes through this and every one of them is bounded.
		class Cursor {
		  public:
			explicit Cursor(std::span<const std::byte> bytes) : Bytes(bytes) {}

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

			float Real() {
				const uint32_t bits = Word();
				float value = 0.0f;
				std::memcpy(&value, &bits, sizeof(float));
				return value;
			}

			// An index of the width the header declared, sign-extended.
			//
			// **Negative means "none"** in PMX — a material with no texture
			// carries -1 — so the sign has to survive the widening or every
			// untextured material picks up texture zero.
			int32_t Index(uint8_t width) {
				switch (width) {
				case 1:
					return static_cast<int8_t>(Byte());
				case 2:
					return static_cast<int16_t>(Half());
				case 4:
					return static_cast<int32_t>(Word());
				default:
					Broken = true;
					return -1;
				}
			}

			// A vertex index, which is unsigned at widths 1 and 2 and signed at
			// 4. That asymmetry is the format's, not a mistake here: a
			// one-byte vertex index addresses 0..255 and a four-byte one is
			// declared signed so it can carry the -1 that means "none"
			// elsewhere.
			int64_t VertexIndex(uint8_t width) {
				switch (width) {
				case 1:
					return Byte();
				case 2:
					return Half();
				case 4:
					return static_cast<int32_t>(Word());
				default:
					Broken = true;
					return -1;
				}
			}

			// A length-prefixed string, converted to UTF-8.
			std::string Text(bool utf8) {
				const uint32_t length = Word();
				if (Broken || length > Remaining()) {
					Broken = true;
					return {};
				}

				const std::span<const std::byte> raw = Bytes.subspan(Offset, length);
				Offset += length;

				if (utf8) {
					return std::string(reinterpret_cast<const char *>(raw.data()), raw.size());
				}

				// UTF-16LE to UTF-8. Written out because the alternative is
				// `std::wstring_convert`, which is deprecated, and because the
				// texture paths in a PMX are routinely Chinese or Japanese —
				// mangling them means every texture reference misses.
				std::string result;
				for (size_t index = 0; index + 1 < raw.size(); index += 2) {
					uint32_t code =
						static_cast<uint32_t>(raw[index]) | (static_cast<uint32_t>(raw[index + 1]) << 8);

					if (code >= 0xD800 && code <= 0xDBFF && index + 3 < raw.size()) {
						const uint32_t low = static_cast<uint32_t>(raw[index + 2]) |
											 (static_cast<uint32_t>(raw[index + 3]) << 8);
						if (low >= 0xDC00 && low <= 0xDFFF) {
							code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
							index += 2;
						}
					}

					if (code < 0x80) {
						result.push_back(static_cast<char>(code));
					} else if (code < 0x800) {
						result.push_back(static_cast<char>(0xC0 | (code >> 6)));
						result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
					} else if (code < 0x10000) {
						result.push_back(static_cast<char>(0xE0 | (code >> 12)));
						result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
						result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
					} else {
						result.push_back(static_cast<char>(0xF0 | (code >> 18)));
						result.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
						result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
						result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
					}
				}
				return result;
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

		// Backslashes to forward slashes, because PMX is a Windows format and
		// every path in one is spelled the Windows way.
		std::string Normalised(std::string path) {
			std::replace(path.begin(), path.end(), '\\', '/');
			return path;
		}
	}

	bool ReadPmx(std::span<const std::byte> bytes, ImportedModel &out, std::string &failure) {
		if (bytes.size() < 9 || static_cast<char>(bytes[0]) != 'P' || static_cast<char>(bytes[1]) != 'M' ||
			static_cast<char>(bytes[2]) != 'X' || static_cast<char>(bytes[3]) != ' ') {
			failure = "pmx: wrong signature";
			return false;
		}

		Cursor cursor(bytes);
		cursor.Skip(4);

		const float version = cursor.Real();
		if (version < 2.0f || version >= 3.0f) {
			failure = "pmx: only version 2 is supported";
			return false;
		}

		const uint8_t globalCount = cursor.Byte();
		if (globalCount < 8) {
			failure = "pmx: malformed globals";
			return false;
		}

		const uint8_t encoding = cursor.Byte();
		const uint8_t additionalVectors = cursor.Byte();
		const uint8_t vertexIndexWidth = cursor.Byte();
		const uint8_t textureIndexWidth = cursor.Byte();
		const uint8_t materialIndexWidth = cursor.Byte();
		const uint8_t boneIndexWidth = cursor.Byte();
		const uint8_t morphIndexWidth = cursor.Byte();
		const uint8_t rigidBodyIndexWidth = cursor.Byte();
		cursor.Skip(globalCount - 8u);

		const bool utf8 = encoding == 1;
		if (encoding > 1 || additionalVectors > 4) {
			failure = "pmx: unsupported text encoding or additional vector count";
			return false;
		}

		// The four model names. Read and dropped: what a model calls itself is
		// not what the engine calls it — the publisher's asset name is, and
		// `AGENTS.md` rule 4 puts that decision in one place rather than in
		// whatever a modeller typed.
		for (int index = 0; index < 4; index++) {
			cursor.Text(utf8);
		}
		if (cursor.Failed()) {
			failure = "pmx: truncated header";
			return false;
		}

		const uint32_t vertexCount = cursor.Word();
		if (cursor.Failed() || vertexCount == 0 || vertexCount > MAXIMUM_IMPORTED_VERTICES) {
			failure = "pmx: vertex count is zero or implausible";
			return false;
		}

		// The smallest a vertex can be: position, normal, uv, an index and an
		// edge scale. Checked before the loop so a count of four million over a
		// short file costs a comparison rather than the allocation.
		constexpr uint64_t SMALLEST_VERTEX_BYTES = 8 * sizeof(float) + 2;
		if (static_cast<uint64_t>(vertexCount) * SMALLEST_VERTEX_BYTES > cursor.Remaining()) {
			failure = "pmx: vertices run past the end of the file";
			return false;
		}

		ImportedModel imported;
		imported.Mesh.Vertices.resize(vertexCount);

		for (assets::MeshVertex &vertex : imported.Mesh.Vertices) {
			for (int axis = 0; axis < 3; axis++) {
				vertex.Position[axis] = cursor.Real();
			}
			for (int axis = 0; axis < 3; axis++) {
				vertex.Normal[axis] = cursor.Real();
			}
			vertex.TexCoord[0] = cursor.Real();
			vertex.TexCoord[1] = cursor.Real();

			// **PMX is left-handed and this engine is not.** Mirroring Z is the
			// conversion, and it is why the triangle winding is reversed below:
			// a mirror turns every counter-clockwise face clockwise, so a model
			// imported without the reversal is inside-out and looks like the
			// renderer has lost its culling.
			vertex.Position[2] = -vertex.Position[2];
			vertex.Normal[2] = -vertex.Normal[2];

			cursor.Skip(static_cast<uint64_t>(additionalVectors) * 4 * sizeof(float));

			// The weight deform. Skipped by width rather than parsed: there are
			// no skeletons in the engine yet, and reading bone indices to throw
			// them away would be work with no consumer.
			const uint8_t deform = cursor.Byte();
			switch (deform) {
			case 0: // BDEF1
				cursor.Index(boneIndexWidth);
				break;
			case 1: // BDEF2
				cursor.Index(boneIndexWidth);
				cursor.Index(boneIndexWidth);
				cursor.Skip(sizeof(float));
				break;
			case 2: // BDEF4
			case 4: // QDEF
				for (int bone = 0; bone < 4; bone++) {
					cursor.Index(boneIndexWidth);
				}
				cursor.Skip(4 * sizeof(float));
				break;
			case 3: // SDEF
				cursor.Index(boneIndexWidth);
				cursor.Index(boneIndexWidth);
				cursor.Skip(sizeof(float) + 9 * sizeof(float));
				break;
			default:
				failure = "pmx: unknown weight deform type";
				return false;
			}

			cursor.Skip(sizeof(float)); // edge scale

			if (cursor.Failed()) {
				failure = "pmx: truncated vertex data";
				return false;
			}
		}

		const uint32_t indexCount = cursor.Word();
		if (cursor.Failed() || indexCount == 0 || indexCount % 3 != 0 ||
			indexCount > MAXIMUM_IMPORTED_INDICES) {
			failure = "pmx: face count is zero, not whole triangles, or implausible";
			return false;
		}
		if (static_cast<uint64_t>(indexCount) * vertexIndexWidth > cursor.Remaining()) {
			failure = "pmx: faces run past the end of the file";
			return false;
		}

		imported.Mesh.Indices.resize(indexCount);
		for (uint32_t triangle = 0; triangle < indexCount; triangle += 3) {
			int64_t corners[3];
			for (int corner = 0; corner < 3; corner++) {
				corners[corner] = cursor.VertexIndex(vertexIndexWidth);
				if (corners[corner] < 0 || corners[corner] >= static_cast<int64_t>(vertexCount)) {
					failure = "pmx: face names a vertex that is not there";
					return false;
				}
			}

			// Reversed, to undo what mirroring Z did to the winding.
			imported.Mesh.Indices[triangle] = static_cast<uint32_t>(corners[0]);
			imported.Mesh.Indices[triangle + 1] = static_cast<uint32_t>(corners[2]);
			imported.Mesh.Indices[triangle + 2] = static_cast<uint32_t>(corners[1]);
		}
		if (cursor.Failed()) {
			failure = "pmx: truncated face data";
			return false;
		}

		const uint32_t textureCount = cursor.Word();
		if (cursor.Failed() || textureCount > 4096) {
			failure = "pmx: texture count is implausible";
			return false;
		}

		std::vector<std::string> texturePaths;
		texturePaths.reserve(textureCount);
		for (uint32_t index = 0; index < textureCount; index++) {
			texturePaths.push_back(Normalised(cursor.Text(utf8)));
			if (cursor.Failed()) {
				failure = "pmx: truncated texture table";
				return false;
			}
		}

		const uint32_t materialCount = cursor.Word();
		if (cursor.Failed() || materialCount == 0 || materialCount > assets::Mesh::MAXIMUM_SUBMESHES) {
			failure = "pmx: material count is zero or past what a mesh can hold";
			return false;
		}

		uint32_t consumed = 0;
		for (uint32_t index = 0; index < materialCount; index++) {
			assets::Submesh submesh;
			submesh.Material = cursor.Text(utf8);
			cursor.Text(utf8); // the English name, which nothing here uses

			for (int channel = 0; channel < 4; channel++) {
				submesh.BaseColour[channel] = cursor.Real();
			}
			cursor.Skip(3 * sizeof(float) + sizeof(float)); // specular and its strength
			cursor.Skip(3 * sizeof(float));					// ambient
			cursor.Byte();									// drawing flags
			cursor.Skip(4 * sizeof(float) + sizeof(float)); // edge colour and scale

			const int32_t textureIndex = cursor.Index(textureIndexWidth);
			cursor.Index(textureIndexWidth); // the environment map
			cursor.Byte();					 // environment blend mode

			const uint8_t toonReference = cursor.Byte();
			if (toonReference == 0) {
				cursor.Index(textureIndexWidth);
			} else {
				cursor.Byte();
			}

			cursor.Text(utf8); // the memo

			const uint32_t surfaceCount = cursor.Word();
			if (cursor.Failed()) {
				failure = "pmx: truncated material table";
				return false;
			}
			if (surfaceCount % 3 != 0 || static_cast<uint64_t>(consumed) + surfaceCount > indexCount) {
				failure = "pmx: material surface counts do not add up to the faces";
				return false;
			}

			if (textureIndex >= 0 && static_cast<size_t>(textureIndex) < texturePaths.size()) {
				submesh.Texture = texturePaths[static_cast<size_t>(textureIndex)];
			}

			// A material's base colour is trusted only as far as the format
			// allows: PMX writes diffuse alpha and some models put a number
			// outside zero-to-one there, which would be refused by
			// `MeshData::IsValid` after a full parse.
			for (float &channel : submesh.BaseColour) {
				if (!std::isfinite(channel) || channel < 0.0f) {
					channel = 1.0f;
				}
			}

			submesh.FirstIndex = consumed;
			submesh.IndexCount = surfaceCount;
			consumed += surfaceCount;

			if (surfaceCount != 0) {
				imported.Mesh.Submeshes.push_back(std::move(submesh));
			}
		}

		// Bones, morphs and rigid bodies follow and none of them is read: there
		// is nothing in the engine that consumes a skeleton or a physics proxy
		// out of a model file yet. The widths are named above so that the day
		// there is, the cursor already knows how to step over them.
		(void)materialIndexWidth;
		(void)morphIndexWidth;
		(void)rigidBodyIndexWidth;

		for (const std::string &path : texturePaths) {
			if (!path.empty()) {
				imported.Textures.push_back(path);
			}
		}

		imported.Mesh.ComputeBounds();
		if (!imported.Mesh.IsValid()) {
			failure = "pmx: the imported mesh is not valid geometry";
			return false;
		}

		out = std::move(imported);
		return true;
	}
}
