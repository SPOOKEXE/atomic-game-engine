#include "Importers.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <nlohmann/json.hpp>
#include <vector>

namespace engine::bake {

	namespace {
		using nlohmann::json;

		// A 4x4 column-major transform, which is glTF's own convention.
		//
		// Written out rather than reaching for `glm`, because `glm` is a vendor
		// and this module's whole claim is that nothing it exposes says what
		// produced it. Sixteen floats and two multiplies is not worth widening
		// a dependency for.
		struct Matrix {
			std::array<float, 16> Values{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

			float At(int row, int column) const {
				return Values[static_cast<size_t>(column) * 4 + row];
			}
			float &At(int row, int column) {
				return Values[static_cast<size_t>(column) * 4 + row];
			}
		};

		Matrix Multiply(const Matrix &left, const Matrix &right) {
			Matrix result;
			for (int row = 0; row < 4; row++) {
				for (int column = 0; column < 4; column++) {
					float total = 0.0f;
					for (int index = 0; index < 4; index++) {
						total += left.At(row, index) * right.At(index, column);
					}
					result.At(row, column) = total;
				}
			}
			return result;
		}

		// The transform a node declares, either as a matrix or as translation,
		// rotation and scale. glTF allows one or the other and never both.
		Matrix TransformOf(const json &node) {
			if (node.contains("matrix") && node["matrix"].is_array() && node["matrix"].size() == 16) {
				Matrix matrix;
				for (size_t index = 0; index < 16; index++) {
					matrix.Values[index] = node["matrix"][index].get<float>();
				}
				return matrix;
			}

			float translation[3] = {0, 0, 0};
			float rotation[4] = {0, 0, 0, 1};
			float scale[3] = {1, 1, 1};

			if (node.contains("translation") && node["translation"].size() == 3) {
				for (size_t index = 0; index < 3; index++) {
					translation[index] = node["translation"][index].get<float>();
				}
			}
			if (node.contains("rotation") && node["rotation"].size() == 4) {
				for (size_t index = 0; index < 4; index++) {
					rotation[index] = node["rotation"][index].get<float>();
				}
			}
			if (node.contains("scale") && node["scale"].size() == 3) {
				for (size_t index = 0; index < 3; index++) {
					scale[index] = node["scale"][index].get<float>();
				}
			}

			// Quaternion to rotation matrix, then scale on the right so it stays
			// a scale rather than becoming a shear — `render::ToGpu` folds a
			// half-extent in for the same reason and states it the same way.
			const float x = rotation[0], y = rotation[1], z = rotation[2], w = rotation[3];
			Matrix matrix;
			matrix.At(0, 0) = (1 - 2 * (y * y + z * z)) * scale[0];
			matrix.At(1, 0) = (2 * (x * y + z * w)) * scale[0];
			matrix.At(2, 0) = (2 * (x * z - y * w)) * scale[0];
			matrix.At(0, 1) = (2 * (x * y - z * w)) * scale[1];
			matrix.At(1, 1) = (1 - 2 * (x * x + z * z)) * scale[1];
			matrix.At(2, 1) = (2 * (y * z + x * w)) * scale[1];
			matrix.At(0, 2) = (2 * (x * z + y * w)) * scale[2];
			matrix.At(1, 2) = (2 * (y * z - x * w)) * scale[2];
			matrix.At(2, 2) = (1 - 2 * (x * x + y * y)) * scale[2];
			matrix.At(0, 3) = translation[0];
			matrix.At(1, 3) = translation[1];
			matrix.At(2, 3) = translation[2];
			return matrix;
		}

		// The matrix a normal is transformed by, which is not the one a position
		// is.
		//
		// **The inverse transpose of the upper 3x3, and the reason it cannot be
		// skipped is non-uniform scale.** A node scaled twice as wide as it is
		// tall tilts every normal that is not on an axis, and the result is a
		// model lit as if its surfaces faced somewhere else — which reads as a
		// lighting bug rather than as an import one. A singular matrix falls
		// back to the identity, because a node scaled to zero has no normals
		// worth arguing about.
		Matrix NormalMatrixOf(const Matrix &transform) {
			const float a = transform.At(0, 0), b = transform.At(0, 1), c = transform.At(0, 2);
			const float d = transform.At(1, 0), e = transform.At(1, 1), f = transform.At(1, 2);
			const float g = transform.At(2, 0), h = transform.At(2, 1), i = transform.At(2, 2);

			const float determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
			Matrix result;
			if (std::abs(determinant) < 1e-12f) {
				return result;
			}

			const float inverse = 1.0f / determinant;
			// The transpose of the inverse, written directly: entry (row,
			// column) of the transposed inverse is the cofactor at (row, column).
			result.At(0, 0) = (e * i - f * h) * inverse;
			result.At(0, 1) = (f * g - d * i) * inverse;
			result.At(0, 2) = (d * h - e * g) * inverse;
			result.At(1, 0) = (c * h - b * i) * inverse;
			result.At(1, 1) = (a * i - c * g) * inverse;
			result.At(1, 2) = (b * g - a * h) * inverse;
			result.At(2, 0) = (b * f - c * e) * inverse;
			result.At(2, 1) = (c * d - a * f) * inverse;
			result.At(2, 2) = (a * e - b * d) * inverse;
			return result;
		}

		void TransformPoint(const Matrix &matrix, float point[3]) {
			const float x = point[0], y = point[1], z = point[2];
			point[0] = matrix.At(0, 0) * x + matrix.At(0, 1) * y + matrix.At(0, 2) * z + matrix.At(0, 3);
			point[1] = matrix.At(1, 0) * x + matrix.At(1, 1) * y + matrix.At(1, 2) * z + matrix.At(1, 3);
			point[2] = matrix.At(2, 0) * x + matrix.At(2, 1) * y + matrix.At(2, 2) * z + matrix.At(2, 3);
		}

		void TransformDirection(const Matrix &matrix, float direction[3]) {
			const float x = direction[0], y = direction[1], z = direction[2];
			direction[0] = matrix.At(0, 0) * x + matrix.At(0, 1) * y + matrix.At(0, 2) * z;
			direction[1] = matrix.At(1, 0) * x + matrix.At(1, 1) * y + matrix.At(1, 2) * z;
			direction[2] = matrix.At(2, 0) * x + matrix.At(2, 1) * y + matrix.At(2, 2) * z;
		}

		// glTF component types, as the specification numbers them.
		enum : uint32_t {
			BYTE = 5120,
			UNSIGNED_BYTE = 5121,
			SHORT = 5122,
			UNSIGNED_SHORT = 5123,
			UNSIGNED_INT = 5125,
			FLOAT = 5126,
		};

		uint32_t SizeOfComponent(uint32_t componentType) {
			switch (componentType) {
			case BYTE:
			case UNSIGNED_BYTE:
				return 1;
			case SHORT:
			case UNSIGNED_SHORT:
				return 2;
			case UNSIGNED_INT:
			case FLOAT:
				return 4;
			default:
				return 0;
			}
		}

		uint32_t ComponentsOfType(std::string_view type) {
			if (type == "SCALAR") {
				return 1;
			}
			if (type == "VEC2") {
				return 2;
			}
			if (type == "VEC3") {
				return 3;
			}
			if (type == "VEC4") {
				return 4;
			}
			if (type == "MAT4") {
				return 16;
			}
			return 0;
		}

		// Everything needed to walk one accessor, resolved once.
		struct Accessor {
			const std::byte *Data = nullptr;
			size_t Stride = 0;
			uint32_t Count = 0;
			uint32_t Components = 0;
			uint32_t ComponentType = 0;
			bool Normalised = false;
		};

		// Resolves an accessor against the buffer, checking every offset and
		// length against what is actually there.
		//
		// **Sparse accessors are refused rather than half-honoured**: an
		// accessor with a sparse override whose base is read alone produces the
		// mesh before the override, which is geometry that is subtly and
		// invisibly wrong.
		bool Resolve(
			const json &document,
			size_t index,
			std::span<const std::byte> buffer,
			Accessor &out,
			std::string &failure
		) {
			if (!document.contains("accessors") || index >= document["accessors"].size()) {
				failure = "gltf: accessor index out of range";
				return false;
			}
			const json &accessor = document["accessors"][index];

			if (accessor.contains("sparse")) {
				failure = "gltf: sparse accessors are not supported";
				return false;
			}

			out.Count = accessor.value("count", 0u);
			out.ComponentType = accessor.value("componentType", 0u);
			out.Components = ComponentsOfType(accessor.value("type", std::string()));
			out.Normalised = accessor.value("normalized", false);

			const uint32_t componentBytes = SizeOfComponent(out.ComponentType);
			if (componentBytes == 0 || out.Components == 0) {
				failure = "gltf: unknown accessor component type";
				return false;
			}
			if (out.Count == 0 || out.Count > MAXIMUM_IMPORTED_INDICES) {
				failure = "gltf: accessor count is zero or implausible";
				return false;
			}

			// An accessor with no buffer view reads as zeroes by the
			// specification. Refused instead: it only appears with sparse
			// accessors, which are already refused above.
			if (!accessor.contains("bufferView")) {
				failure = "gltf: accessor without a buffer view is not supported";
				return false;
			}

			const size_t viewIndex = accessor["bufferView"].get<size_t>();
			if (!document.contains("bufferViews") || viewIndex >= document["bufferViews"].size()) {
				failure = "gltf: buffer view index out of range";
				return false;
			}
			const json &view = document["bufferViews"][viewIndex];

			const uint64_t viewOffset = view.value("byteOffset", 0ull);
			const uint64_t viewLength = view.value("byteLength", 0ull);
			const uint64_t accessorOffset = accessor.value("byteOffset", 0ull);
			const uint64_t elementBytes = static_cast<uint64_t>(componentBytes) * out.Components;
			const uint64_t stride =
				view.value("byteStride", 0ull) != 0 ? view["byteStride"].get<uint64_t>() : elementBytes;

			if (stride < elementBytes) {
				failure = "gltf: buffer view stride is smaller than one element";
				return false;
			}
			if (viewOffset + viewLength > buffer.size()) {
				failure = "gltf: buffer view runs past the end of the buffer";
				return false;
			}

			// The last element's end, computed from the stride, checked in 64
			// bits against the view. This is the whole bounds check: an
			// accessor that passes it cannot read outside its view whatever the
			// counts say.
			const uint64_t span = accessorOffset + stride * (out.Count - 1) + elementBytes;
			if (span > viewLength) {
				failure = "gltf: accessor runs past the end of its buffer view";
				return false;
			}

			out.Data = buffer.data() + viewOffset + accessorOffset;
			out.Stride = static_cast<size_t>(stride);
			return true;
		}

		// Reads one component of one element as a float, applying the
		// normalisation glTF declares for integer attributes.
		float ReadFloatComponent(const Accessor &accessor, uint32_t element, uint32_t component) {
			const std::byte *base = accessor.Data + static_cast<size_t>(element) * accessor.Stride +
									static_cast<size_t>(component) * SizeOfComponent(accessor.ComponentType);

			switch (accessor.ComponentType) {
			case FLOAT: {
				float value = 0.0f;
				std::memcpy(&value, base, sizeof(float));
				return value;
			}
			case UNSIGNED_BYTE: {
				const uint8_t value = static_cast<uint8_t>(*base);
				return accessor.Normalised ? static_cast<float>(value) / 255.0f : static_cast<float>(value);
			}
			case UNSIGNED_SHORT: {
				uint16_t value = 0;
				std::memcpy(&value, base, sizeof(uint16_t));
				return accessor.Normalised ? static_cast<float>(value) / 65535.0f : static_cast<float>(value);
			}
			case BYTE: {
				const int8_t value = static_cast<int8_t>(*base);
				return accessor.Normalised ? std::max(static_cast<float>(value) / 127.0f, -1.0f)
										   : static_cast<float>(value);
			}
			case SHORT: {
				int16_t value = 0;
				std::memcpy(&value, base, sizeof(int16_t));
				return accessor.Normalised ? std::max(static_cast<float>(value) / 32767.0f, -1.0f)
										   : static_cast<float>(value);
			}
			case UNSIGNED_INT: {
				uint32_t value = 0;
				std::memcpy(&value, base, sizeof(uint32_t));
				return static_cast<float>(value);
			}
			default:
				return 0.0f;
			}
		}

		uint32_t ReadIndexComponent(const Accessor &accessor, uint32_t element) {
			const std::byte *base = accessor.Data + static_cast<size_t>(element) * accessor.Stride;
			switch (accessor.ComponentType) {
			case UNSIGNED_BYTE:
				return static_cast<uint8_t>(*base);
			case UNSIGNED_SHORT: {
				uint16_t value = 0;
				std::memcpy(&value, base, sizeof(uint16_t));
				return value;
			}
			case UNSIGNED_INT: {
				uint32_t value = 0;
				std::memcpy(&value, base, sizeof(uint32_t));
				return value;
			}
			default:
				return 0;
			}
		}

		// One primitive of one node, flattened.
		struct Piece {
			size_t MeshIndex = 0;
			size_t PrimitiveIndex = 0;
			Matrix Transform;
			bool Skinned = false;
		};

		// Walks the scene, recording every mesh primitive with the transform it
		// ends up under.
		//
		// **A skinned node contributes the identity rather than its own
		// transform**, which is the specification's rule and is easy to get
		// wrong in exactly the way that produces a model lying on its side: a
		// skinned mesh's vertices are in the skin's space, and the node's
		// transform is what the *skeleton* is placed by. The fox this was built
		// against is skinned, so this is not a corner case.
		void Walk(
			const json &document,
			size_t nodeIndex,
			const Matrix &parent,
			std::vector<Piece> &pieces,
			std::vector<bool> &visited
		) {
			if (nodeIndex >= visited.size() || visited[nodeIndex]) {
				// A node reachable twice, or a cycle. glTF forbids both, and a
				// file that has one would otherwise recurse until the stack
				// ended.
				return;
			}
			visited[nodeIndex] = true;

			const json &node = document["nodes"][nodeIndex];
			const Matrix here = Multiply(parent, TransformOf(node));

			if (node.contains("mesh")) {
				const size_t meshIndex = node["mesh"].get<size_t>();
				if (document.contains("meshes") && meshIndex < document["meshes"].size()) {
					const json &mesh = document["meshes"][meshIndex];
					if (mesh.contains("primitives")) {
						for (size_t primitive = 0; primitive < mesh["primitives"].size(); primitive++) {
							Piece piece;
							piece.MeshIndex = meshIndex;
							piece.PrimitiveIndex = primitive;
							piece.Skinned = node.contains("skin");
							piece.Transform = piece.Skinned ? Matrix{} : here;
							pieces.push_back(piece);
						}
					}
				}
			}

			if (node.contains("children")) {
				for (const json &child : node["children"]) {
					Walk(document, child.get<size_t>(), here, pieces, visited);
				}
			}
		}
	}

	bool ReadGltf(std::span<const std::byte> bytes, ImportedModel &out, std::string &failure) {
		std::span<const std::byte> jsonBytes = bytes;
		std::span<const std::byte> binary;

		// GLB: a twelve-byte header then length-prefixed chunks. The JSON chunk
		// is first by the specification and the binary chunk is optional.
		if (bytes.size() >= 12 && static_cast<char>(bytes[0]) == 'g' && static_cast<char>(bytes[1]) == 'l' &&
			static_cast<char>(bytes[2]) == 'T' && static_cast<char>(bytes[3]) == 'F') {
			const auto word = [&bytes](size_t offset) {
				return static_cast<uint32_t>(bytes[offset]) |
					   (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
					   (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
					   (static_cast<uint32_t>(bytes[offset + 3]) << 24);
			};

			if (word(4) != 2) {
				failure = "gltf: only version 2 is supported";
				return false;
			}

			bool haveJson = false;
			size_t offset = 12;
			while (offset + 8 <= bytes.size()) {
				const uint64_t length = word(offset);
				const uint32_t type = word(offset + 4);
				if (offset + 8 + length > bytes.size()) {
					failure = "gltf: chunk runs past the end of the file";
					return false;
				}

				if (type == 0x4E4F534A) {
					jsonBytes = bytes.subspan(offset + 8, static_cast<size_t>(length));
					haveJson = true;
				} else if (type == 0x004E4942) {
					binary = bytes.subspan(offset + 8, static_cast<size_t>(length));
				}
				offset += 8 + static_cast<size_t>(length);
			}

			if (!haveJson) {
				failure = "gltf: no json chunk";
				return false;
			}
		}

		json document;
		try {
			document = json::parse(
				reinterpret_cast<const char *>(jsonBytes.data()),
				reinterpret_cast<const char *>(jsonBytes.data()) + jsonBytes.size()
			);
		} catch (const json::exception &error) {
			// Caught because malformed JSON is an ordinary property of an input
			// file and the caller is a bake tool that names the file and
			// carries on to the next.
			failure = std::string("gltf: ") + error.what();
			return false;
		}

		if (!document.is_object() || !document.contains("nodes") || !document["nodes"].is_array()) {
			failure = "gltf: no nodes";
			return false;
		}

		// External buffers are refused. Following a `uri` would mean this module
		// reading a file, and it has no filesystem by design — the seam is the
		// caller's.
		if (document.contains("buffers")) {
			for (const json &buffer : document["buffers"]) {
				if (buffer.contains("uri")) {
					failure = "gltf: external buffers are not supported, use a .glb";
					return false;
				}
			}
		}

		std::vector<Piece> pieces;
		std::vector<bool> visited(document["nodes"].size(), false);

		if (document.contains("scenes") && document["scenes"].is_array() && !document["scenes"].empty()) {
			const size_t sceneIndex = document.value("scene", 0u);
			const json &scene = document["scenes"][std::min(sceneIndex, document["scenes"].size() - 1)];
			if (scene.contains("nodes")) {
				for (const json &node : scene["nodes"]) {
					Walk(document, node.get<size_t>(), Matrix{}, pieces, visited);
				}
			}
		} else {
			// No scene at all is legal and means "the nodes are the scene".
			for (size_t index = 0; index < document["nodes"].size(); index++) {
				Walk(document, index, Matrix{}, pieces, visited);
			}
		}

		if (pieces.empty()) {
			failure = "gltf: no mesh in the scene";
			return false;
		}

		ImportedModel imported;

		// Material index to where its texture landed in `Textures`, so a
		// texture used by three materials is named once.
		std::vector<std::string> textureNames;

		for (const Piece &piece : pieces) {
			const json &primitive = document["meshes"][piece.MeshIndex]["primitives"][piece.PrimitiveIndex];

			// Mode 4 is triangles. The strip and fan modes are a different
			// index expansion and the line and point modes are not geometry
			// this renders, so both are skipped rather than mis-expanded.
			if (primitive.value("mode", 4u) != 4u) {
				continue;
			}
			if (!primitive.contains("attributes") || !primitive["attributes"].contains("POSITION")) {
				continue;
			}

			Accessor positions;
			if (!Resolve(
					document, primitive["attributes"]["POSITION"].get<size_t>(), binary, positions, failure
				)) {
				return false;
			}

			Accessor normals;
			const bool haveNormals =
				primitive["attributes"].contains("NORMAL") &&
				Resolve(document, primitive["attributes"]["NORMAL"].get<size_t>(), binary, normals, failure);

			Accessor texCoords;
			const bool haveTexCoords =
				primitive["attributes"].contains("TEXCOORD_0") &&
				Resolve(
					document, primitive["attributes"]["TEXCOORD_0"].get<size_t>(), binary, texCoords, failure
				);

			const uint32_t firstVertex = static_cast<uint32_t>(imported.Mesh.Vertices.size());
			if (static_cast<uint64_t>(firstVertex) + positions.Count > MAXIMUM_IMPORTED_VERTICES) {
				failure = "gltf: model has more vertices than the format can hold";
				return false;
			}

			const Matrix normalMatrix = NormalMatrixOf(piece.Transform);

			for (uint32_t index = 0; index < positions.Count; index++) {
				assets::MeshVertex vertex{};
				for (uint32_t axis = 0; axis < 3; axis++) {
					vertex.Position[axis] = ReadFloatComponent(positions, index, axis);
				}
				TransformPoint(piece.Transform, vertex.Position);

				if (haveNormals && index < normals.Count) {
					for (uint32_t axis = 0; axis < 3; axis++) {
						vertex.Normal[axis] = ReadFloatComponent(normals, index, axis);
					}
					TransformDirection(normalMatrix, vertex.Normal);

					const float length = std::sqrt(
						vertex.Normal[0] * vertex.Normal[0] + vertex.Normal[1] * vertex.Normal[1] +
						vertex.Normal[2] * vertex.Normal[2]
					);
					if (length > 1e-8f) {
						for (float &component : vertex.Normal) {
							component /= length;
						}
					}
				}

				if (haveTexCoords && index < texCoords.Count) {
					vertex.TexCoord[0] = ReadFloatComponent(texCoords, index, 0);
					vertex.TexCoord[1] = ReadFloatComponent(texCoords, index, 1);
				}

				imported.Mesh.Vertices.push_back(vertex);
			}

			const uint32_t firstIndex = static_cast<uint32_t>(imported.Mesh.Indices.size());

			if (primitive.contains("indices")) {
				Accessor indices;
				if (!Resolve(document, primitive["indices"].get<size_t>(), binary, indices, failure)) {
					return false;
				}
				if (indices.ComponentType != UNSIGNED_BYTE && indices.ComponentType != UNSIGNED_SHORT &&
					indices.ComponentType != UNSIGNED_INT) {
					failure = "gltf: index accessor is not an unsigned integer type";
					return false;
				}

				for (uint32_t index = 0; index < indices.Count; index++) {
					const uint32_t value = ReadIndexComponent(indices, index);
					if (value >= positions.Count) {
						failure = "gltf: index past the end of its primitive";
						return false;
					}
					imported.Mesh.Indices.push_back(firstVertex + value);
				}
			} else {
				// No index accessor means the vertices are already in draw
				// order.
				for (uint32_t index = 0; index < positions.Count; index++) {
					imported.Mesh.Indices.push_back(firstVertex + index);
				}
			}

			// A run that is not whole triangles would be refused by
			// `MeshData::IsValid` later with nothing to say which primitive did
			// it, so it is trimmed here where the name is still to hand.
			while ((imported.Mesh.Indices.size() - firstIndex) % 3 != 0) {
				imported.Mesh.Indices.pop_back();
			}
			if (imported.Mesh.Indices.size() == firstIndex) {
				continue;
			}
			if (imported.Mesh.Indices.size() > MAXIMUM_IMPORTED_INDICES) {
				failure = "gltf: model has more indices than the format can hold";
				return false;
			}

			assets::Submesh submesh;
			submesh.FirstIndex = firstIndex;
			submesh.IndexCount = static_cast<uint32_t>(imported.Mesh.Indices.size()) - firstIndex;

			if (primitive.contains("material") && document.contains("materials")) {
				const size_t materialIndex = primitive["material"].get<size_t>();
				if (materialIndex < document["materials"].size()) {
					const json &material = document["materials"][materialIndex];
					submesh.Material = material.value("name", std::string());

					if (material.contains("pbrMetallicRoughness")) {
						const json &pbr = material["pbrMetallicRoughness"];
						if (pbr.contains("baseColorFactor") && pbr["baseColorFactor"].size() == 4) {
							for (size_t channel = 0; channel < 4; channel++) {
								submesh.BaseColour[channel] = pbr["baseColorFactor"][channel].get<float>();
							}
						}

						if (pbr.contains("baseColorTexture") && pbr["baseColorTexture"].contains("index") &&
							document.contains("textures")) {
							const size_t textureIndex = pbr["baseColorTexture"]["index"].get<size_t>();
							if (textureIndex < document["textures"].size() &&
								document["textures"][textureIndex].contains("source") &&
								document.contains("images")) {
								const size_t imageIndex =
									document["textures"][textureIndex]["source"].get<size_t>();
								if (imageIndex < document["images"].size()) {
									const json &image = document["images"][imageIndex];

									if (image.contains("uri")) {
										submesh.Texture = image["uri"].get<std::string>();
									} else if (image.contains("bufferView")) {
										const size_t viewIndex = image["bufferView"].get<size_t>();
										if (document.contains("bufferViews") &&
											viewIndex < document["bufferViews"].size()) {
											const json &view = document["bufferViews"][viewIndex];
											const uint64_t viewOffset = view.value("byteOffset", 0ull);
											const uint64_t viewLength = view.value("byteLength", 0ull);

											if (viewOffset + viewLength <= binary.size()) {
												const std::string name =
													image.contains("name")
														? image["name"].get<std::string>()
														: "image" + std::to_string(imageIndex);

												const auto existing = std::find_if(
													imported.Embedded.begin(),
													imported.Embedded.end(),
													[&name](const EmbeddedImage &candidate) {
														return candidate.Name == name;
													}
												);
												if (existing == imported.Embedded.end()) {
													EmbeddedImage embedded;
													embedded.Name = name;
													const std::span<const std::byte> slice = binary.subspan(
														static_cast<size_t>(viewOffset),
														static_cast<size_t>(viewLength)
													);
													embedded.Bytes.assign(slice.begin(), slice.end());
													imported.Embedded.push_back(std::move(embedded));
												}
												submesh.Texture = name;
											}
										}
									}
								}
							}
						}
					}
				}
			}

			if (!submesh.Texture.empty() &&
				std::find(textureNames.begin(), textureNames.end(), submesh.Texture) == textureNames.end()) {
				textureNames.push_back(submesh.Texture);
			}
			imported.Mesh.Submeshes.push_back(std::move(submesh));
		}

		if (imported.Mesh.Vertices.empty() || imported.Mesh.Indices.empty()) {
			failure = "gltf: no triangles";
			return false;
		}

		imported.Textures = std::move(textureNames);
		imported.Mesh.ComputeBounds();

		if (!imported.Mesh.IsValid()) {
			failure = "gltf: the imported mesh is not valid geometry";
			return false;
		}

		out = std::move(imported);
		return true;
	}
}
