#include "Importers.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <string_view>
#include <vector>

namespace engine::bake {

	namespace {
		// One `f` corner, as OBJ spells it: `v`, `v/vt`, `v//vn` or `v/vt/vn`.
		struct Corner {
			int32_t Position = 0;
			int32_t TexCoord = 0;
			int32_t Normal = 0;

			bool operator<(const Corner &other) const {
				if (Position != other.Position) {
					return Position < other.Position;
				}
				if (TexCoord != other.TexCoord) {
					return TexCoord < other.TexCoord;
				}
				return Normal < other.Normal;
			}
		};

		std::string_view Trim(std::string_view text) {
			while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) {
				text.remove_prefix(1);
			}
			while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
				text.remove_suffix(1);
			}
			return text;
		}

		// Splits on runs of whitespace, which is what OBJ's own grammar means by
		// a separator.
		std::vector<std::string_view> Fields(std::string_view line) {
			std::vector<std::string_view> fields;
			size_t start = 0;
			while (start < line.size()) {
				while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) {
					start++;
				}
				size_t end = start;
				while (end < line.size() && line[end] != ' ' && line[end] != '\t') {
					end++;
				}
				if (end > start) {
					fields.push_back(line.substr(start, end - start));
				}
				start = end;
			}
			return fields;
		}

		bool ParseFloat(std::string_view text, float &out) {
			// `from_chars` rather than `strtof`, because the latter is
			// locale-dependent: a machine set to a comma decimal separator
			// would silently read `1.5` as `1`, and every model imported on it
			// would be the wrong shape.
			const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
			return result.ec == std::errc() && std::isfinite(out);
		}

		bool ParseIndex(std::string_view text, int32_t &out) {
			if (text.empty()) {
				out = 0;
				return true;
			}
			const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
			return result.ec == std::errc();
		}

		// Resolves OBJ's one-based, optionally negative index against a list.
		//
		// **Negative means "counting back from what has been read so far"**,
		// which is the half of the format everybody forgets: a generator that
		// emits negative indices produces a file that reads as garbage under a
		// parser that only handles positive ones, and the garbage still forms
		// triangles.
		bool ResolveIndex(int32_t value, size_t available, size_t &out) {
			if (value > 0) {
				if (static_cast<size_t>(value) > available) {
					return false;
				}
				out = static_cast<size_t>(value) - 1;
				return true;
			}
			if (value < 0) {
				const size_t back = static_cast<size_t>(-static_cast<int64_t>(value));
				if (back > available) {
					return false;
				}
				out = available - back;
				return true;
			}
			return false;
		}
	}

	bool ReadObj(std::span<const std::byte> bytes, ImportedModel &out, std::string &failure) {
		const std::string_view text(reinterpret_cast<const char *>(bytes.data()), bytes.size());

		std::vector<std::array<float, 3>> positions;
		std::vector<std::array<float, 2>> texCoords;
		std::vector<std::array<float, 3>> normals;

		ImportedModel imported;
		std::map<Corner, uint32_t> emitted;

		std::string currentMaterial;
		bool haveGroup = false;

		// Starts a new submesh, closing whichever one was open.
		const auto openGroup = [&imported, &currentMaterial, &haveGroup](const std::string &material) {
			if (haveGroup && !imported.Mesh.Submeshes.empty()) {
				assets::Submesh &previous = imported.Mesh.Submeshes.back();
				previous.IndexCount =
					static_cast<uint32_t>(imported.Mesh.Indices.size()) - previous.FirstIndex;
				if (previous.IndexCount == 0) {
					imported.Mesh.Submeshes.pop_back();
				}
			}
			assets::Submesh submesh;
			submesh.FirstIndex = static_cast<uint32_t>(imported.Mesh.Indices.size());
			submesh.Material = material;
			imported.Mesh.Submeshes.push_back(submesh);
			currentMaterial = material;
			haveGroup = true;
		};

		size_t lineStart = 0;
		while (lineStart <= text.size()) {
			const size_t lineEnd = std::min(text.find('\n', lineStart), text.size());
			const std::string_view line = Trim(text.substr(lineStart, lineEnd - lineStart));
			lineStart = lineEnd + 1;

			if (line.empty() || line.front() == '#') {
				continue;
			}

			const std::vector<std::string_view> fields = Fields(line);
			if (fields.empty()) {
				continue;
			}

			if (fields[0] == "v" && fields.size() >= 4) {
				std::array<float, 3> position{};
				if (!ParseFloat(fields[1], position[0]) || !ParseFloat(fields[2], position[1]) ||
					!ParseFloat(fields[3], position[2])) {
					failure = "obj: malformed vertex";
					return false;
				}
				if (positions.size() >= MAXIMUM_IMPORTED_VERTICES) {
					failure = "obj: more vertices than the format can hold";
					return false;
				}
				positions.push_back(position);
			} else if (fields[0] == "vt" && fields.size() >= 3) {
				std::array<float, 2> coordinate{};
				if (!ParseFloat(fields[1], coordinate[0]) || !ParseFloat(fields[2], coordinate[1])) {
					failure = "obj: malformed texture coordinate";
					return false;
				}

				// **OBJ's `v` runs up the image and every other format here runs
				// it down.** Flipped once, at import, because a renderer that
				// flipped would flip the formats that are already right —
				// `assets::MeshVertex::TexCoord` states the convention.
				coordinate[1] = 1.0f - coordinate[1];
				texCoords.push_back(coordinate);
			} else if (fields[0] == "vn" && fields.size() >= 4) {
				std::array<float, 3> normal{};
				if (!ParseFloat(fields[1], normal[0]) || !ParseFloat(fields[2], normal[1]) ||
					!ParseFloat(fields[3], normal[2])) {
					failure = "obj: malformed normal";
					return false;
				}
				normals.push_back(normal);
			} else if (fields[0] == "usemtl") {
				openGroup(fields.size() >= 2 ? std::string(fields[1]) : std::string());
			} else if (fields[0] == "f" && fields.size() >= 4) {
				if (!haveGroup) {
					openGroup(currentMaterial);
				}

				std::vector<uint32_t> corners;
				corners.reserve(fields.size() - 1);

				for (size_t field = 1; field < fields.size(); field++) {
					std::string_view spec = fields[field];
					Corner corner;

					const size_t firstSlash = spec.find('/');
					if (firstSlash == std::string_view::npos) {
						if (!ParseIndex(spec, corner.Position)) {
							failure = "obj: malformed face";
							return false;
						}
					} else {
						if (!ParseIndex(spec.substr(0, firstSlash), corner.Position)) {
							failure = "obj: malformed face";
							return false;
						}
						const std::string_view rest = spec.substr(firstSlash + 1);
						const size_t secondSlash = rest.find('/');
						if (secondSlash == std::string_view::npos) {
							if (!ParseIndex(rest, corner.TexCoord)) {
								failure = "obj: malformed face";
								return false;
							}
						} else {
							if (!ParseIndex(rest.substr(0, secondSlash), corner.TexCoord) ||
								!ParseIndex(rest.substr(secondSlash + 1), corner.Normal)) {
								failure = "obj: malformed face";
								return false;
							}
						}
					}

					// **Resolved before the lookup, not after.** OBJ lets one
					// corner be spelled `1/1/1` or `-4/-4/-1` in the same file,
					// and those are the same vertex — so a cache keyed on what
					// was *written* emits it twice, splits the normal across
					// the copies and leaves a visible seam down a surface that
					// should be smooth.
					size_t positionIndex = 0;
					if (!ResolveIndex(corner.Position, positions.size(), positionIndex)) {
						failure = "obj: face names a vertex that is not there";
						return false;
					}

					size_t resolvedTexCoord = 0;
					const bool haveTexCoord =
						corner.TexCoord != 0 &&
						ResolveIndex(corner.TexCoord, texCoords.size(), resolvedTexCoord);

					size_t resolvedNormal = 0;
					const bool haveNormal =
						corner.Normal != 0 && ResolveIndex(corner.Normal, normals.size(), resolvedNormal);

					const Corner resolved{
						static_cast<int32_t>(positionIndex),
						haveTexCoord ? static_cast<int32_t>(resolvedTexCoord) + 1 : 0,
						haveNormal ? static_cast<int32_t>(resolvedNormal) + 1 : 0,
					};

					const auto existing = emitted.find(resolved);
					if (existing != emitted.end()) {
						corners.push_back(existing->second);
						continue;
					}

					assets::MeshVertex vertex{};
					vertex.Position[0] = positions[positionIndex][0];
					vertex.Position[1] = positions[positionIndex][1];
					vertex.Position[2] = positions[positionIndex][2];

					if (haveTexCoord) {
						vertex.TexCoord[0] = texCoords[resolvedTexCoord][0];
						vertex.TexCoord[1] = texCoords[resolvedTexCoord][1];
					}
					if (haveNormal) {
						vertex.Normal[0] = normals[resolvedNormal][0];
						vertex.Normal[1] = normals[resolvedNormal][1];
						vertex.Normal[2] = normals[resolvedNormal][2];
					}

					const uint32_t index = static_cast<uint32_t>(imported.Mesh.Vertices.size());
					imported.Mesh.Vertices.push_back(vertex);
					emitted.emplace(resolved, index);
					corners.push_back(index);
				}

				// **A fan, which is right for a convex polygon and wrong for a
				// concave one.** OBJ allows both and carries nothing that says
				// which; a real triangulator needs the polygon's plane and a
				// winding test, and almost every exporter emits convex faces.
				// Stated rather than hidden, because a concave face imports as
				// a shape with a bite out of it.
				for (size_t corner = 2; corner < corners.size(); corner++) {
					imported.Mesh.Indices.push_back(corners[0]);
					imported.Mesh.Indices.push_back(corners[corner - 1]);
					imported.Mesh.Indices.push_back(corners[corner]);
				}

				if (imported.Mesh.Indices.size() > MAXIMUM_IMPORTED_INDICES) {
					failure = "obj: more indices than the format can hold";
					return false;
				}
			}
		}

		if (haveGroup && !imported.Mesh.Submeshes.empty()) {
			assets::Submesh &last = imported.Mesh.Submeshes.back();
			last.IndexCount = static_cast<uint32_t>(imported.Mesh.Indices.size()) - last.FirstIndex;
			if (last.IndexCount == 0) {
				imported.Mesh.Submeshes.pop_back();
			}
		}

		if (imported.Mesh.Vertices.empty() || imported.Mesh.Indices.empty()) {
			failure = "obj: no triangles";
			return false;
		}

		// An OBJ with no `vn` lines has zero normals everywhere, which shades
		// black. Filling them in is the one thing an importer can do here that
		// the file cannot.
		const bool anyNormal = std::any_of(
			imported.Mesh.Vertices.begin(),
			imported.Mesh.Vertices.end(),
			[](const assets::MeshVertex &vertex) {
				return vertex.Normal[0] != 0.0f || vertex.Normal[1] != 0.0f || vertex.Normal[2] != 0.0f;
			}
		);
		if (!anyNormal) {
			SmoothNormals(imported.Mesh);
		}

		imported.Mesh.ComputeBounds();
		if (!imported.Mesh.IsValid()) {
			failure = "obj: the imported mesh is not valid geometry";
			return false;
		}

		out = std::move(imported);
		return true;
	}
}
