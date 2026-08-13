#include "Extension.hpp"
#include "Importers.hpp"

#include <engine/bake/Model.hpp>

#include <algorithm>
#include <cmath>
#include <string>

namespace engine::bake {

	ModelFormat ModelFormatOfBytes(std::span<const std::byte> bytes) {
		if (bytes.size() >= 4) {
			const char *front = reinterpret_cast<const char *>(bytes.data());
			if (front[0] == 'g' && front[1] == 'l' && front[2] == 'T' && front[3] == 'F') {
				return ModelFormat::Gltf;
			}
			if (front[0] == 'P' && front[1] == 'M' && front[2] == 'X' && front[3] == ' ') {
				return ModelFormat::Pmx;
			}
		}

		// A `.gltf` is JSON, and JSON starting with `{` is the only thing that
		// distinguishes it from any other text file. Accepted here because the
		// parse itself is the real check — a text file that is not glTF fails on
		// "no nodes" with a message that says so.
		for (const std::byte byte : bytes) {
			const char character = static_cast<char>(byte);
			if (character == ' ' || character == '\t' || character == '\r' || character == '\n') {
				continue;
			}
			return character == '{' ? ModelFormat::Gltf : ModelFormat::Unknown;
		}
		return ModelFormat::Unknown;
	}

	ModelFormat ModelFormatOfName(std::string_view name) {
		const std::string extension = ExtensionOf(name);
		if (extension == "glb" || extension == "gltf") {
			return ModelFormat::Gltf;
		}
		if (extension == "obj") {
			return ModelFormat::Obj;
		}
		if (extension == "pmx") {
			return ModelFormat::Pmx;
		}
		return ModelFormat::Unknown;
	}

	std::string_view Describe(ModelFormat format) {
		switch (format) {
		case ModelFormat::Gltf:
			return "gltf";
		case ModelFormat::Obj:
			return "obj";
		case ModelFormat::Pmx:
			return "pmx";
		case ModelFormat::Unknown:
			break;
		}
		return "unknown";
	}

	bool ReadModel(
		ModelFormat format, std::span<const std::byte> bytes, ImportedModel &out, std::string &failure
	) {
		switch (format) {
		case ModelFormat::Gltf:
			return ReadGltf(bytes, out, failure);
		case ModelFormat::Obj:
			return ReadObj(bytes, out, failure);
		case ModelFormat::Pmx:
			return ReadPmx(bytes, out, failure);
		case ModelFormat::Unknown:
			break;
		}
		failure = "model: not a format this reads";
		return false;
	}

	bool FitMesh(assets::MeshData &mesh, float size) {
		if (size <= 0.0f || !std::isfinite(size) || mesh.Vertices.empty()) {
			return false;
		}

		mesh.ComputeBounds();
		const float extent[3] = {
			mesh.Maximum.X - mesh.Minimum.X,
			mesh.Maximum.Y - mesh.Minimum.Y,
			mesh.Maximum.Z - mesh.Minimum.Z,
		};
		const float longest = std::max({extent[0], extent[1], extent[2]});
		if (longest <= 0.0f) {
			// Every vertex at one point. Scaled by `size / 0` it would become
			// infinities, which `MeshData::IsValid` would then refuse — with
			// nothing to say that the *input* was the degenerate thing.
			return false;
		}

		const float scale = size / longest;
		const float centre[3] = {
			(mesh.Maximum.X + mesh.Minimum.X) * 0.5f,
			(mesh.Maximum.Y + mesh.Minimum.Y) * 0.5f,
			(mesh.Maximum.Z + mesh.Minimum.Z) * 0.5f,
		};

		for (assets::MeshVertex &vertex : mesh.Vertices) {
			for (int axis = 0; axis < 3; axis++) {
				vertex.Position[axis] = (vertex.Position[axis] - centre[axis]) * scale;
			}
		}

		// **The normals are left alone, and that is correct rather than lazy.**
		// A uniform scale about a point does not rotate anything, so every
		// normal still points where it did. A per-axis scale would, which is
		// why this only ever does uniform ones.
		mesh.ComputeBounds();
		return true;
	}

	void SmoothNormals(assets::MeshData &mesh) {
		for (assets::MeshVertex &vertex : mesh.Vertices) {
			vertex.Normal[0] = vertex.Normal[1] = vertex.Normal[2] = 0.0f;
		}

		for (size_t triangle = 0; triangle + 2 < mesh.Indices.size(); triangle += 3) {
			const uint32_t a = mesh.Indices[triangle];
			const uint32_t b = mesh.Indices[triangle + 1];
			const uint32_t c = mesh.Indices[triangle + 2];
			if (a >= mesh.Vertices.size() || b >= mesh.Vertices.size() || c >= mesh.Vertices.size()) {
				continue;
			}

			const float *first = mesh.Vertices[a].Position;
			const float *second = mesh.Vertices[b].Position;
			const float *third = mesh.Vertices[c].Position;

			const float edge1[3] = {second[0] - first[0], second[1] - first[1], second[2] - first[2]};
			const float edge2[3] = {third[0] - first[0], third[1] - first[1], third[2] - first[2]};

			// **Not normalised before accumulating**, which is what makes this
			// area-weighted: the cross product's length is twice the triangle's
			// area, so a large face contributes proportionally more than a
			// sliver at the same corner.
			const float cross[3] = {
				edge1[1] * edge2[2] - edge1[2] * edge2[1],
				edge1[2] * edge2[0] - edge1[0] * edge2[2],
				edge1[0] * edge2[1] - edge1[1] * edge2[0],
			};

			for (const uint32_t corner : {a, b, c}) {
				for (int axis = 0; axis < 3; axis++) {
					mesh.Vertices[corner].Normal[axis] += cross[axis];
				}
			}
		}

		for (assets::MeshVertex &vertex : mesh.Vertices) {
			const float length = std::sqrt(
				vertex.Normal[0] * vertex.Normal[0] + vertex.Normal[1] * vertex.Normal[1] +
				vertex.Normal[2] * vertex.Normal[2]
			);
			if (length > 1e-12f) {
				for (float &component : vertex.Normal) {
					component /= length;
				}
			} else {
				// A vertex no triangle uses, or one where opposite faces
				// cancelled exactly. Given an axis rather than left at zero,
				// because a zero normal shades black and reads as a hole.
				vertex.Normal[0] = 0.0f;
				vertex.Normal[1] = 1.0f;
				vertex.Normal[2] = 0.0f;
			}
		}
	}
}
