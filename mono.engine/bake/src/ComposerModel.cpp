#include <engine/bake/ComposerModel.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <sstream>

namespace engine::bake {
	namespace {
		using Vec = glm::dvec3;
		bool Fail(std::string &failure, std::string text) {
			failure = "composer model: " + std::move(text);
			return false;
		}
		bool Finite(Vec value) {
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) &&
				   std::abs(value.x) <= std::numeric_limits<float>::max() &&
				   std::abs(value.y) <= std::numeric_limits<float>::max() &&
				   std::abs(value.z) <= std::numeric_limits<float>::max();
		}
		std::array<float, 3> Point(Vec value) {
			return {static_cast<float>(value.x), static_cast<float>(value.y), static_cast<float>(value.z)};
		}
		Vec Axis(Vec p, uint8_t axis) {
			return axis == 1 ? Vec{p.x, p.z, -p.y} : axis == 2 ? Vec{p.x, -p.z, p.y} : p;
		}
		assets::MeshVertex Vertex(Vec position, Vec normal, glm::dvec2 uv) {
			assets::MeshVertex result{};
			const auto p = Point(position), n = Point(normal);
			std::copy(p.begin(), p.end(), result.Position);
			std::copy(n.begin(), n.end(), result.Normal);
			result.TexCoord[0] = static_cast<float>(uv.x);
			result.TexCoord[1] = static_cast<float>(uv.y);
			return result;
		}
		bool Number(std::string_view text, double &out) {
			const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
			return result.ec == std::errc{} && result.ptr == text.data() + text.size() && std::isfinite(out);
		}
		std::vector<std::string_view> Fields(std::string_view line) {
			std::vector<std::string_view> result;
			while (!line.empty()) {
				const auto first = line.find_first_not_of(" \t\r");
				if (first == line.npos) break;
				line.remove_prefix(first);
				const auto ending = line.find_first_of(" \t\r");
				result.push_back(line.substr(0, ending));
				if (ending == line.npos) break;
				line.remove_prefix(ending);
			}
			return result;
		}
		bool Position(const nlohmann::json &value, Vec &out) {
			if (!value.is_array() || value.size() != 3) return false;
			for (size_t i = 0; i < 3; i++) {
				if (!value[i].is_number()) return false;
				out[static_cast<int>(i)] = value[i].get<double>();
			}
			return Finite(out);
		}
		bool AddBytes(uint64_t &used, uint64_t bytes, uint64_t maximum) {
			if (bytes > maximum - used) return false;
			used += bytes;
			return true;
		}
	}
	bool ReadComposerObj(
		std::span<const std::byte> bytes,
		double scale,
		uint8_t axis,
		ComposerModel &out,
		std::string &failure,
		uint64_t maximumBytes
	) {
		if (bytes.size() > 8 * 1024 * 1024 || bytes.size() > maximumBytes || !std::isfinite(scale) ||
			axis > 2)
			return Fail(failure, "OBJ input, scale or axis is outside its bounds");
		struct Corner {
			size_t Position = 0, UV = 0;
		};
		struct Face {
			std::vector<Corner> Corners;
			size_t Part = 0;
		};
		std::vector<Vec> positions{Vec{0}}, normals;
		std::vector<glm::dvec2> uvs{glm::dvec2{0}};
		std::vector<Face> faces;
		ComposerModel parsed;
		parsed.Parts.push_back({});
		uint64_t used = sizeof(Vec) + sizeof(glm::dvec2) + sizeof(ComposerModelPart);
		if (used > maximumBytes) return Fail(failure, "OBJ import exceeds its memory budget");
		std::string_view text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
		while (!text.empty()) {
			const auto newline = text.find('\n');
			const auto fields = Fields(text.substr(0, newline));
			if (newline == text.npos)
				text = {};
			else
				text.remove_prefix(newline + 1);
			if (fields.empty() || fields[0].starts_with('#')) continue;
			if (fields[0] == "v" || fields[0] == "vn") {
				Vec value{};
				if (fields.size() < 4 || !Number(fields[1], value.x) || !Number(fields[2], value.y) ||
					!Number(fields[3], value.z))
					return Fail(failure, "malformed OBJ vector");
				if (!AddBytes(used, sizeof(Vec), maximumBytes) || positions.size() + normals.size() > 65536)
					return Fail(failure, "OBJ vector budget exceeded");
				if (fields[0] == "v")
					positions.push_back(Axis(value * scale, axis));
				else
					normals.push_back(value);
			} else if (fields[0] == "vt") {
				glm::dvec2 value{};
				if (fields.size() < 3 || !Number(fields[1], value.x) || !Number(fields[2], value.y))
					return Fail(failure, "malformed OBJ UV");
				if (!AddBytes(used, sizeof(glm::dvec2), maximumBytes) || uvs.size() > 65536)
					return Fail(failure, "OBJ UV budget exceeded");
				uvs.push_back(value);
			} else if (fields[0] == "usemtl") {
				if (fields.size() != 2 || fields[1].size() > 255)
					return Fail(failure, "invalid OBJ material name");
				if (parsed.Parts.back().Material.empty() && parsed.Parts.size() == 1 && faces.empty())
					parsed.Parts.back().Material = fields[1];
				else {
					if (!AddBytes(used, sizeof(ComposerModelPart) + fields[1].size(), maximumBytes) ||
						parsed.Parts.size() >= 4096)
						return Fail(failure, "OBJ material count budget exceeded");
					parsed.Parts.push_back({std::string(fields[1]), {}, std::nullopt});
				}
			} else if (fields[0] == "f") {
				if (fields.size() < 4 || fields.size() > 5)
					return Fail(failure, "OBJ faces require triangles or quads");
				const size_t vertices = (fields.size() - 3) * 3;
				if (!AddBytes(
						used,
						sizeof(Face) + (fields.size() - 1) * sizeof(Corner) +
							vertices * sizeof(assets::MeshVertex) +
							(fields.size() - 1) * sizeof(parsed.Edges[0]),
						maximumBytes
					) ||
					faces.size() >= 16384)
					return Fail(failure, "OBJ face budget exceeded");
				Face face;
				face.Part = parsed.Parts.size() - 1;
				for (size_t index = 1; index < fields.size(); index++) {
					auto word = fields[index];
					const auto slash = word.find('/');
					const auto positionWord = word.substr(0, slash);
					int64_t position = 0;
					const auto p = std::from_chars(
						positionWord.data(), positionWord.data() + positionWord.size(), position
					);
					if (p.ec != std::errc{} || p.ptr != positionWord.data() + positionWord.size() ||
						position <= 0 || static_cast<uint64_t>(position) >= positions.size())
						return Fail(failure, "OBJ vertex index is invalid");
					size_t uv = 0;
					if (slash != word.npos) {
						word.remove_prefix(slash + 1);
						const auto end = word.find('/');
						word = word.substr(0, end);
						if (!word.empty()) {
							int64_t value = 0;
							const auto t = std::from_chars(word.data(), word.data() + word.size(), value);
							if (t.ec != std::errc{} || t.ptr != word.data() + word.size() || value < 0 ||
								static_cast<uint64_t>(value) >= uvs.size())
								return Fail(failure, "OBJ UV index is invalid");
							uv = static_cast<size_t>(value);
						}
					}
					face.Corners.push_back({static_cast<size_t>(position), uv});
				}
				faces.push_back(std::move(face));
			}
		}
		if (faces.empty()) return Fail(failure, "OBJ has no drawable source faces");
		Vec centre{};
		for (auto p : positions)
			centre += p;
		centre /= static_cast<double>(positions.size());
		for (auto &p : positions)
			p -= centre;
		size_t vertexCount = 0;
		for (const auto &face : faces) {
			auto &part = parsed.Parts[face.Part];
			const Vec normal = glm::cross(
				positions[face.Corners[1].Position] - positions[face.Corners[0].Position],
				positions[face.Corners[2].Position] - positions[face.Corners[0].Position]
			);
			for (size_t triangle = 1; triangle + 1 < face.Corners.size(); triangle++)
				for (const size_t corner : {size_t{0}, triangle + 1, triangle}) {
					const auto &c = face.Corners[corner];
					if (!Finite(positions[c.Position]) || !Finite(normal))
						return Fail(failure, "OBJ geometry exceeds float vertex range");
					part.Vertices.push_back(Vertex(positions[c.Position], normal, uvs[c.UV]));
					if (++vertexCount > 65536) return Fail(failure, "OBJ emitted vertex count exceeded");
				}
			for (size_t i = 0; i < face.Corners.size(); i++)
				parsed.Edges.push_back(
					{Point(positions[face.Corners[i].Position]),
					 Point(positions[face.Corners[(i + 1) % face.Corners.size()].Position])}
				);
		}
		out = std::move(parsed);
		return true;
	}

	bool ReadComposerElementJson(
		std::span<const std::byte> bytes,
		double scale,
		uint8_t axis,
		ComposerModel &out,
		std::string &failure,
		uint64_t maximumBytes
	) {
		if (bytes.size() > 4 * 1024 * 1024 || bytes.size() > maximumBytes || !std::isfinite(scale) ||
			axis > 1)
			return Fail(failure, "element JSON size, scale or axis is outside its bounds");
		size_t depth = 0, nodes = 0;
		const auto callback = [&](int nesting, nlohmann::json::parse_event_t event, nlohmann::json &) {
			if (event == nlohmann::json::parse_event_t::object_start ||
				event == nlohmann::json::parse_event_t::array_start) {
				depth = std::max(depth, static_cast<size_t>(std::max(nesting, 0)));
				nodes++;
			}
			return depth <= 64 && nodes <= 65536;
		};
		const auto json = nlohmann::json::parse(
			reinterpret_cast<const char *>(bytes.data()),
			reinterpret_cast<const char *>(bytes.data() + bytes.size()),
			callback,
			false
		);
		if (json.is_discarded() || depth > 64 || nodes > 65536 || !json.is_object() ||
			!json.contains("elements") || !json["elements"].is_array())
			return Fail(failure, "invalid or unbounded element JSON");
		double textureWidth = 16, textureHeight = 16;
		if (json.contains("textureWidth")) {
			if (!json["textureWidth"].is_number()) return Fail(failure, "invalid texture width");
			textureWidth = json["textureWidth"].get<double>();
		}
		if (json.contains("textureHeight")) {
			if (!json["textureHeight"].is_number()) return Fail(failure, "invalid texture height");
			textureHeight = json["textureHeight"].get<double>();
		}
		if (!std::isfinite(textureWidth) || !std::isfinite(textureHeight) || textureWidth <= 0 ||
			textureHeight <= 0)
			return Fail(failure, "invalid texture dimensions");
		ComposerModel parsed;
		uint64_t used = 0;
		size_t elements = 0;
		size_t vertexCount = 0;
		struct FaceDef {
			const char *Name;
			Vec Normal;
			std::array<std::array<int, 3>, 6> Corners;
			std::array<std::array<int, 2>, 6> UV;
		};
		const std::array<FaceDef, 6> definitions{
			{{"up",
			  {0, 1, 0},
			  {{{0, 1, 0}, {1, 1, 1}, {0, 1, 1}, {1, 1, 0}, {1, 1, 1}, {0, 1, 0}}},
			  {{{0, 0}, {1, 1}, {0, 1}, {1, 0}, {1, 1}, {0, 0}}}},
			 {"down",
			  {0, -1, 0},
			  {{{0, 0, 0}, {0, 0, 1}, {1, 0, 1}, {1, 0, 0}, {0, 0, 0}, {1, 0, 1}}},
			  {{{0, 0}, {0, 1}, {1, 1}, {1, 0}, {0, 0}, {1, 1}}}},
			 {"east",
			  {1, 0, 0},
			  {{{1, 0, 0}, {1, 0, 1}, {1, 1, 1}, {1, 1, 0}, {1, 0, 0}, {1, 1, 1}}},
			  {{{1, 0}, {0, 0}, {0, 1}, {1, 1}, {1, 0}, {0, 1}}}},
			 {"west",
			  {-1, 0, 0},
			  {{{0, 0, 0}, {0, 1, 1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 1}, {0, 0, 0}}},
			  {{{1, 0}, {0, 1}, {0, 0}, {1, 1}, {0, 1}, {1, 0}}}},
			 {"north",
			  {0, 0, -1},
			  {{{0, 0, 0}, {1, 1, 0}, {0, 1, 0}, {1, 0, 0}, {1, 1, 0}, {0, 0, 0}}},
			  {{{0, 0}, {1, 1}, {0, 1}, {1, 0}, {1, 1}, {0, 0}}}},
			 {"south",
			  {0, 0, 1},
			  {{{0, 0, 1}, {0, 1, 1}, {1, 1, 1}, {1, 0, 1}, {0, 0, 1}, {1, 1, 1}}},
			  {{{0, 0}, {0, 1}, {1, 1}, {1, 0}, {0, 0}, {1, 1}}}}}
		};
		const auto read = [&](auto &&self,
							  const nlohmann::json &element,
							  const glm::dmat4 &parent,
							  size_t nesting) -> bool {
			if (nesting > 64 || ++elements > 4096 || !element.is_object() || !element.contains("from") ||
				!element.contains("to") || !element.contains("faces") || !element["faces"].is_object())
				return Fail(failure, "invalid element shape or count");
			Vec from{}, to{}, origin{};
			if (!Position(element["from"], from) || !Position(element["to"], to))
				return Fail(failure, "invalid element bounds");
			if (element.contains("rotationOrigin") && !Position(element["rotationOrigin"], origin))
				return Fail(failure, "invalid element rotation origin");
			std::array<double, 3> rotation{};
			for (size_t i = 0; i < 3; i++) {
				const auto name = std::array{"rotationX", "rotationY", "rotationZ"}[i];
				if (element.contains(name)) {
					if (!element[name].is_number()) return Fail(failure, "invalid element rotation");
					rotation[i] = element[name].get<double>();
					if (!std::isfinite(rotation[i])) return Fail(failure, "nonfinite element rotation");
				}
			}
			const auto local = glm::translate(glm::dmat4{1}, origin) *
							   glm::rotate(glm::dmat4{1}, glm::radians(-rotation[0]), Vec{1, 0, 0}) *
							   glm::rotate(glm::dmat4{1}, glm::radians(-rotation[1]), Vec{0, 1, 0}) *
							   glm::rotate(glm::dmat4{1}, glm::radians(-rotation[2]), Vec{0, 0, 1}) *
							   glm::translate(glm::dmat4{1}, from - origin);
			const auto transform = parent * local;
			const Vec extent = to - from;
			const auto map = [&](Vec position, bool normal = false) {
				Vec mapped = Vec{transform * glm::dvec4{position, normal ? 0.0 : 1.0}};
				if (axis == 1) mapped = {mapped.x, -mapped.z, mapped.y};
				return normal ? mapped : mapped * scale;
			};
			for (const auto &face : definitions) {
				if (!element["faces"].contains(face.Name)) continue;
				const auto &value = element["faces"][face.Name];
				if (!value.is_object()) return Fail(failure, "invalid element face");
				if (value.contains("enabled")) {
					if (!value["enabled"].is_boolean()) return Fail(failure, "invalid face enabled value");
					if (!value["enabled"].get<bool>()) continue;
				}
				if (!value.contains("uv") || !value["uv"].is_array() || value["uv"].size() != 4 ||
					!value.contains("texture") || !value["texture"].is_string())
					return Fail(failure, "element face has no UV rectangle or material");
				if (!AddBytes(
						used,
						sizeof(ComposerModelPart) + 6 * sizeof(assets::MeshVertex) +
							4 * sizeof(parsed.Edges[0]),
						maximumBytes
					))
					return Fail(failure, "element geometry exceeds its memory budget");
				std::array<double, 4> uv{};
				for (size_t i = 0; i < 4; i++) {
					if (!value["uv"][i].is_number()) return Fail(failure, "invalid face UV");
					uv[i] = value["uv"][i].get<double>() / (i % 2 ? textureHeight : textureWidth);
					if (!std::isfinite(uv[i])) return Fail(failure, "nonfinite face UV");
				}
				if (std::string_view(face.Name) == "north") {
					uv[0] = 1 - uv[0];
					uv[2] = 1 - uv[2];
				}
				if (vertexCount > 65536 - 6) return Fail(failure, "element emitted vertex count exceeded");
				vertexCount += 6;
				ComposerModelPart part;
				const glm::dmat4 basis =
					glm::scale(glm::dmat4{1}, Vec{scale}) *
					(axis == 1 ? glm::rotate(glm::dmat4{1}, glm::radians(90.0), Vec{1, 0, 0})
							   : glm::dmat4{1});
				const glm::dmat4 sourceMatrix = basis * transform;
				part.LocalMatrix.emplace();
				for (size_t column = 0; column < 4; column++)
					for (size_t row = 0; row < 4; row++) {
						const double coefficient =
							sourceMatrix[static_cast<int>(column)][static_cast<int>(row)];
						if (!std::isfinite(coefficient)) return Fail(failure, "element matrix is not finite");
						(*part.LocalMatrix)[column * 4 + row] = coefficient;
					}
				part.Material = value["texture"].get<std::string>();
				if (part.Material.size() > 255 || !AddBytes(used, part.Material.size(), maximumBytes))
					return Fail(failure, "element material name exceeds its budget");
				while (!part.Material.empty() && part.Material.front() == '#')
					part.Material.erase(0, 1);
				for (size_t i = 0; i < 6; i++) {
					const auto corner = face.Corners[i];
					const auto sample = face.UV[i];
					const Vec p{extent.x * corner[0], extent.y * corner[1], extent.z * corner[2]};
					const Vec n = face.Normal;
					if (!Finite(p) || !Finite(n)) return Fail(failure, "transformed element is not finite");
					part.Vertices.push_back(Vertex(p, n, {uv[sample[0] ? 2 : 0], uv[sample[1] ? 3 : 1]}));
				}
				const std::array<size_t, 4> perimeter = std::string_view(face.Name) == "up" ||
																std::string_view(face.Name) == "west" ||
																std::string_view(face.Name) == "north"
															? std::array<size_t, 4>{0, 2, 1, 3}
															: std::array<size_t, 4>{0, 1, 2, 3};
				for (size_t i = 0; i < 4; i++) {
					const auto &a = part.Vertices[perimeter[i]], &b = part.Vertices[perimeter[(i + 1) % 4]];
					parsed.Edges.push_back(
						{Point(map(Vec{a.Position[0], a.Position[1], a.Position[2]})),
						 Point(map(Vec{b.Position[0], b.Position[1], b.Position[2]}))}
					);
				}
				parsed.Parts.push_back(std::move(part));
			}
			if (element.contains("children")) {
				if (!element["children"].is_array()) return Fail(failure, "invalid child elements");
				for (const auto &child : element["children"])
					if (!self(self, child, transform, nesting + 1)) return false;
			}
			return true;
		};
		for (const auto &element : json["elements"])
			if (!read(read, element, glm::dmat4{1}, 0)) return false;
		if (parsed.Parts.empty()) return Fail(failure, "element JSON has no enabled faces");
		out = std::move(parsed);
		return true;
	}
}
