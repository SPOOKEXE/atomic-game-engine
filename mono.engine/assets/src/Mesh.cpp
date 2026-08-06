#include <engine/assets/Mesh.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::assets {

	namespace {
		// How many bytes one vertex occupies on disk.
		//
		// Written out rather than `sizeof(MeshVertex)`, because the file layout
		// must not follow the struct: a padding byte or a fourth component
		// added to the struct would change every published mesh's meaning with
		// nothing to notice. Eight floats, written one at a time by
		// `ByteWriter`, is the format.
		constexpr uint64_t VERTEX_BYTES = 8 * sizeof(float);

		// Whether every coordinate of a vertex is a real number.
		//
		// A NaN in a position poisons the bounds fold — every comparison
		// against it is false, so the box silently stops covering the mesh —
		// and a NaN in a normal produces a black fragment that reads as a
		// lighting bug. Both are much cheaper to refuse at the boundary than to
		// find on a screen.
		bool Finite(const MeshVertex &vertex) {
			for (int axis = 0; axis < 3; axis++) {
				if (!std::isfinite(vertex.Position[axis]) || !std::isfinite(vertex.Normal[axis])) {
					return false;
				}
			}
			return std::isfinite(vertex.TexCoord[0]) && std::isfinite(vertex.TexCoord[1]);
		}
	}

	bool MeshData::IsValid() const {
		if (Vertices.empty() || Indices.empty()) {
			return false;
		}
		if (Indices.size() % 3 != 0) {
			return false;
		}

		const uint64_t vertexCount = static_cast<uint64_t>(Vertices.size());
		for (const uint32_t index : Indices) {
			if (static_cast<uint64_t>(index) >= vertexCount) {
				return false;
			}
		}

		const uint64_t indexCount = static_cast<uint64_t>(Indices.size());
		for (const Submesh &submesh : Submeshes) {
			if (submesh.IndexCount % 3 != 0) {
				return false;
			}

			// Summed in 64 bits, so a first index and a count that each fit in
			// 32 cannot wrap past the end and land back inside the buffer.
			const uint64_t end =
				static_cast<uint64_t>(submesh.FirstIndex) + static_cast<uint64_t>(submesh.IndexCount);
			if (end > indexCount) {
				return false;
			}
			if (submesh.Material.size() > Mesh::MAXIMUM_MATERIAL_BYTES) {
				return false;
			}
			if (submesh.Texture.size() > Mesh::MAXIMUM_MATERIAL_BYTES) {
				return false;
			}
			for (const float channel : submesh.BaseColour) {
				// Non-finite reaches a shader as a black or a white fragment
				// depending on the blend, and negative light is not a thing.
				if (!std::isfinite(channel) || channel < 0.0f) {
					return false;
				}
			}
		}

		for (const MeshVertex &vertex : Vertices) {
			if (!Finite(vertex)) {
				return false;
			}
		}
		return true;
	}

	void MeshData::ComputeBounds() {
		if (Vertices.empty()) {
			Minimum = core::Vector3();
			Maximum = core::Vector3();
			return;
		}

		float minimum[3] = {
			std::numeric_limits<float>::infinity(),
			std::numeric_limits<float>::infinity(),
			std::numeric_limits<float>::infinity(),
		};
		float maximum[3] = {
			-std::numeric_limits<float>::infinity(),
			-std::numeric_limits<float>::infinity(),
			-std::numeric_limits<float>::infinity(),
		};

		for (const MeshVertex &vertex : Vertices) {
			for (int axis = 0; axis < 3; axis++) {
				minimum[axis] = std::min(minimum[axis], vertex.Position[axis]);
				maximum[axis] = std::max(maximum[axis], vertex.Position[axis]);
			}
		}

		Minimum = core::Vector3(minimum[0], minimum[1], minimum[2]);
		Maximum = core::Vector3(maximum[0], maximum[1], maximum[2]);
	}

	bool Mesh::Write(core::ByteWriter &writer, const MeshData &data) {
		if (!data.IsValid()) {
			return false;
		}
		if (data.Vertices.size() > MAXIMUM_VERTICES || data.Indices.size() > MAXIMUM_INDICES) {
			return false;
		}
		if (data.Submeshes.size() > MAXIMUM_SUBMESHES) {
			return false;
		}

		writer.WriteUInt32(MAGIC);
		writer.WriteUInt16(VERSION);
		writer.WriteUInt32(static_cast<uint32_t>(data.Vertices.size()));
		writer.WriteUInt32(static_cast<uint32_t>(data.Indices.size()));
		writer.WriteUInt32(static_cast<uint32_t>(data.Submeshes.size()));

		for (const MeshVertex &vertex : data.Vertices) {
			for (int axis = 0; axis < 3; axis++) {
				writer.WriteFloat(vertex.Position[axis]);
			}
			for (int axis = 0; axis < 3; axis++) {
				writer.WriteFloat(vertex.Normal[axis]);
			}
			writer.WriteFloat(vertex.TexCoord[0]);
			writer.WriteFloat(vertex.TexCoord[1]);
		}

		for (const uint32_t index : data.Indices) {
			writer.WriteUInt32(index);
		}

		for (const Submesh &submesh : data.Submeshes) {
			writer.WriteUInt32(submesh.FirstIndex);
			writer.WriteUInt32(submesh.IndexCount);
			writer.WriteString(submesh.Material);
			writer.WriteString(submesh.Texture);
			for (const float channel : submesh.BaseColour) {
				writer.WriteFloat(channel);
			}
		}

		// No bounds are written. See `MeshData::Minimum`: a stored bound is the
		// one field of this format an attacker would get to choose, and the
		// vertices already say what it is.
		return true;
	}

	bool Mesh::Read(core::ByteReader &reader, MeshData &out) {
		if (reader.ReadUInt32() != MAGIC) {
			return false;
		}
		if (reader.ReadUInt16() != VERSION) {
			return false;
		}

		const uint32_t vertexCount = reader.ReadUInt32();
		const uint32_t indexCount = reader.ReadUInt32();
		const uint32_t submeshCount = reader.ReadUInt32();

		if (reader.Failed()) {
			return false;
		}
		if (vertexCount == 0 || vertexCount > MAXIMUM_VERTICES) {
			return false;
		}
		if (indexCount == 0 || indexCount > MAXIMUM_INDICES || indexCount % 3 != 0) {
			return false;
		}
		if (submeshCount > MAXIMUM_SUBMESHES) {
			return false;
		}

		// **The three counts are checked against the bytes actually present
		// before a single element is reserved.** Without this a header claiming
		// four million vertices over a forty-byte file is a 128 MB allocation,
		// which is the decompression bomb `Texture::Read` refuses in its own
		// terms. The submeshes are not included: each carries a variable-length
		// name, so they are bounded by their count and their name ceiling
		// instead, and the reader refuses past the end regardless.
		const uint64_t fixed = static_cast<uint64_t>(vertexCount) * VERTEX_BYTES +
							   static_cast<uint64_t>(indexCount) * sizeof(uint32_t);
		if (fixed > static_cast<uint64_t>(reader.Remaining())) {
			return false;
		}

		MeshData parsed;
		parsed.Vertices.resize(vertexCount);
		for (MeshVertex &vertex : parsed.Vertices) {
			for (int axis = 0; axis < 3; axis++) {
				vertex.Position[axis] = reader.ReadFloat();
			}
			for (int axis = 0; axis < 3; axis++) {
				vertex.Normal[axis] = reader.ReadFloat();
			}
			vertex.TexCoord[0] = reader.ReadFloat();
			vertex.TexCoord[1] = reader.ReadFloat();
		}

		parsed.Indices.resize(indexCount);
		for (uint32_t &index : parsed.Indices) {
			index = reader.ReadUInt32();
		}

		parsed.Submeshes.resize(submeshCount);
		for (Submesh &submesh : parsed.Submeshes) {
			submesh.FirstIndex = reader.ReadUInt32();
			submesh.IndexCount = reader.ReadUInt32();

			const std::string_view material = reader.ReadString();
			if (material.size() > MAXIMUM_MATERIAL_BYTES) {
				return false;
			}
			submesh.Material.assign(material);

			const std::string_view texture = reader.ReadString();
			if (texture.size() > MAXIMUM_MATERIAL_BYTES) {
				return false;
			}
			submesh.Texture.assign(texture);

			for (float &channel : submesh.BaseColour) {
				channel = reader.ReadFloat();
			}
		}

		if (reader.Failed()) {
			return false;
		}

		// `IsValid` is what checks that every index names a vertex and every
		// run names indices — the two things a consumer would otherwise read
		// past the end of. Run after parsing rather than during it because the
		// vertex count is only known to be *correct* once the vertices are
		// actually there.
		if (!parsed.IsValid()) {
			return false;
		}

		parsed.ComputeBounds();

		// Moved into `out` only once everything has been checked, for
		// `Texture::Read`'s reason.
		out = std::move(parsed);
		return true;
	}
}
