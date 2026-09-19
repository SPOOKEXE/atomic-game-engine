#pragma once

// Read-only MCP access to the script module's data-scene observations.
//
// @tier L13 · shared

#include <engine/assets/ContentHash.hpp>
#include <engine/control/DataFactoryReadFence.hpp>
#include <engine/control/Surface.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/script/DataCaptureBridge.hpp>
#include <engine/script/DataSceneService.hpp>
#include <engine/script/GltfSceneExport.hpp>
#include <engine/world/Universe.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>

namespace engine::control {

	using nlohmann::json;

	namespace data_scene_detail {
		inline constexpr size_t MAXIMUM_DEPTH = 16;
		inline constexpr size_t MAXIMUM_RESULT_BYTES = script::MAX_DATA_SCENE_JSON_RESPONSE_BYTES;
		// Script numbers are doubles. Keep a whole value as an integer only while
		// every signed integer in this range is represented exactly by that double.
		inline constexpr double MAXIMUM_EXACT_JSON_INTEGER = 9'007'199'254'740'991.0;

		inline bool Only(const json &value, std::initializer_list<const char *> names, std::string &failure) {
			if (!value.is_object()) {
				failure = "arguments must be an object";
				return false;
			}
			for (const auto &[name, ignored] : value.items()) {
				(void)ignored;
				bool known = false;
				for (const char *allowed : names)
					known = known || name == allowed;
				if (!known) {
					failure = "unknown argument '" + name + "'";
					return false;
				}
			}
			return true;
		}

		inline bool Options(
			const json &value,
			std::initializer_list<const char *> names,
			bool requireRevision,
			std::string &failure
		) {
			if (!value.is_object()) {
				failure = "options must be an object";
				return false;
			}
			for (const auto &[name, ignored] : value.items()) {
				(void)ignored;
				if (requireRevision && data_factory_read_fence::IsExpectedRevisionField(name)) continue;
				bool known = false;
				for (const char *allowed : names)
					known = known || name == allowed;
				if (!known) {
					failure = "unknown option '" + name + "'";
					return false;
				}
			}
			return true;
		}

		inline bool Spend(size_t &bytes, size_t amount) {
			if (amount > MAXIMUM_RESULT_BYTES - bytes) return false;
			bytes += amount;
			return true;
		}

		inline bool Finite(float value) {
			return std::isfinite(value);
		}
		inline bool Vector(const json &value, core::Vector3 &out) {
			if (!value.is_array() || value.size() != 3) return false;
			for (const auto &item : value)
				if (!item.is_number()) return false;
			out = {value[0].get<float>(), value[1].get<float>(), value[2].get<float>()};
			return Finite(out.X) && Finite(out.Y) && Finite(out.Z);
		}

		inline bool
		JsonValue(const script::ScriptValue &source, json &destination, size_t depth, size_t &bytes) {
			if (depth > MAXIMUM_DEPTH) return false;
			switch (source.Tag) {
			case script::ValueTag::Nil:
				if (!Spend(bytes, 4)) return false;
				destination = nullptr;
				return true;
			case script::ValueTag::False:
			case script::ValueTag::True:
				if (!Spend(bytes, 5)) return false;
				destination = source.Boolean;
				return true;
			case script::ValueTag::Number:
				if (!std::isfinite(source.Number) || !Spend(bytes, 32)) return false;
				if (std::trunc(source.Number) == source.Number &&
					source.Number >= -MAXIMUM_EXACT_JSON_INTEGER &&
					source.Number <= MAXIMUM_EXACT_JSON_INTEGER)
					destination = static_cast<std::int64_t>(source.Number);
				else
					destination = source.Number;
				return true;
			case script::ValueTag::String:
				if (!Spend(bytes, source.Text.size() + 2)) return false;
				destination = source.Text;
				return true;
			case script::ValueTag::Array: {
				destination = json::array();
				for (const script::ScriptValue &item : source.Items) {
					json converted;
					if (!Spend(bytes, 1) || !JsonValue(item, converted, depth + 1, bytes)) return false;
					destination.push_back(std::move(converted));
				}
				return true;
			}
			case script::ValueTag::Map: {
				destination = json::object();
				for (const auto &[name, item] : source.Entries) {
					json converted;
					if (!Spend(bytes, name.size() + 4) || !JsonValue(item, converted, depth + 1, bytes) ||
						destination.contains(name))
						return false;
					destination[name] = std::move(converted);
				}
				return true;
			}
			case script::ValueTag::Vector3:
				if (!Finite(source.Vector.X) || !Finite(source.Vector.Y) || !Finite(source.Vector.Z) ||
					!Spend(bytes, 96))
					return false;
				destination = json{{"x", source.Vector.X}, {"y", source.Vector.Y}, {"z", source.Vector.Z}};
				return true;
			case script::ValueTag::Color3:
				if (!Finite(source.Colour.R) || !Finite(source.Colour.G) || !Finite(source.Colour.B) ||
					!Spend(bytes, 96))
					return false;
				destination = json{{"r", source.Colour.R}, {"g", source.Colour.G}, {"b", source.Colour.B}};
				return true;
			case script::ValueTag::CFrame:
				if (!Finite(source.Frame.Position.X) || !Finite(source.Frame.Position.Y) ||
					!Finite(source.Frame.Position.Z) || !Finite(source.Frame.QuaternionX) ||
					!Finite(source.Frame.QuaternionY) || !Finite(source.Frame.QuaternionZ) ||
					!Finite(source.Frame.QuaternionW) || !Spend(bytes, 192))
					return false;
				destination = json{
					{"position", {source.Frame.Position.X, source.Frame.Position.Y, source.Frame.Position.Z}},
					{"rotation",
					 {source.Frame.QuaternionX,
					  source.Frame.QuaternionY,
					  source.Frame.QuaternionZ,
					  source.Frame.QuaternionW}}
				};
				return true;
			}
			return false;
		}

		inline json Result(const script::DataSceneResult &result, std::string &failure) {
			json converted;
			size_t bytes = 0;
			if (!JsonValue(result.Value, converted, 0, bytes)) {
				failure = "data-scene result exceeds the safe response limit";
				return nullptr;
			}
			if (converted.dump().size() > MAXIMUM_RESULT_BYTES) {
				failure = "data-scene result exceeds the safe response limit";
				return nullptr;
			}
			if (std::strcmp(result.Status, "ok") != 0) failure = converted.dump();
			return converted;
		}

		inline world::WorldId
		World(world::Universe &universe, std::string_view instance, std::string &failure) {
			const world::WorldId id = universe.Find(core::Name(instance));
			if (!id.IsValid()) failure = "no scene called '" + std::string(instance) + "'";
			return id;
		}

		inline constexpr size_t MAX_GLTF_EXPORT_REPLY_BYTES = 40u * 1024u;
		inline constexpr size_t MAX_GLTF_EXPORT_BYTES = 320u * 1024u * 1024u;
		inline constexpr size_t MAX_GLTF_EXPORT_RESOURCES = 2;
		inline constexpr size_t MAX_GLTF_EXPORT_READ_BYTES = 1024u * 1024u;

		struct GltfResource {
			std::string Id;
			std::string Instance;
			uint64_t Tick = 0;
			uint64_t WorldEpoch = 0;
			uint64_t WorldVersion = 0;
			std::string Hash;
			std::vector<std::byte> Bytes;
		};

		struct GltfResources {
			std::mutex Mutex;
			uint64_t NextId = 1;
			std::vector<GltfResource> Entries;
		};

		inline constexpr size_t MAX_RAW_SCENE_BYTES = 320u * 1024u * 1024u;
		inline constexpr size_t MAX_RAW_SCENE_RESOURCES = 2;
		inline constexpr size_t MAX_RAW_SCENE_READ_BYTES = 1024u * 1024u;

		struct RawSceneResource {
			std::string Id;
			std::string Instance;
			uint64_t Tick = 0;
			uint64_t WorldEpoch = 0;
			uint64_t WorldVersion = 0;
			std::string Hash;
			std::vector<std::byte> Bytes;
		};

		struct RawSceneResources {
			std::mutex Mutex;
			uint64_t NextId = 1;
			std::vector<RawSceneResource> Entries;
		};

		inline void RawWord(std::vector<std::byte> &out, uint32_t value) {
			for (size_t byte = 0; byte < 4; ++byte)
				out.push_back(static_cast<std::byte>(value >> (byte * 8)));
		}

		inline void RawShort(std::vector<std::byte> &out, uint16_t value) {
			out.push_back(static_cast<std::byte>(value));
			out.push_back(static_cast<std::byte>(value >> 8));
		}

		inline void RawFloat(std::vector<std::byte> &out, float value) {
			RawWord(out, std::bit_cast<uint32_t>(value));
		}

		inline const char *RawTextureFormat(assets::TextureFormat format) {
			switch (format) {
			case assets::TextureFormat::RGBA8:
			case assets::TextureFormat::RGBA8_LINEAR:
				return "rgba8_unorm";
			case assets::TextureFormat::R8:
				return "r8_unorm";
			}
			return "unknown";
		}

		inline json RawMaterial(const script::GltfExportMaterial &material) {
			const auto source = [](std::optional<size_t> index) -> json {
				return index ? json(*index) : json(nullptr);
			};
			return {
				{"base_colour", {material.BaseColour.R, material.BaseColour.G, material.BaseColour.B}},
				{"alpha", material.Alpha},
				{"alpha_mode",
				 material.AlphaMode == script::GltfExportAlphaMode::Blend  ? "blend"
				 : material.AlphaMode == script::GltfExportAlphaMode::Mask ? "mask"
																		   : "opaque"},
				{"alpha_cutoff", material.AlphaCutoff},
				{"roughness", material.RoughnessFactor},
				{"metalness", material.MetalnessFactor},
				{"emissive_factor",
				 {material.EmissiveFactor.R, material.EmissiveFactor.G, material.EmissiveFactor.B}},
				{"pixelated", material.Pixelated},
				{"source_colour_texture", source(material.SourceColourTexture)},
				{"source_normal_texture", source(material.SourceNormalTexture)},
				{"source_occlusion_texture", source(material.SourceOcclusionTexture)},
				{"source_emissive_texture", source(material.SourceEmissiveTexture)},
				{"source_roughness_texture", source(material.SourceRoughnessTexture)},
				{"source_metalness_texture", source(material.SourceMetalnessTexture)},
				{"source_height_texture", source(material.SourceHeightTexture)},
				{"source_alpha_mode", material.SourceAlphaMode},
				{"source_shader", material.SourceShader.empty() ? json(nullptr) : json(material.SourceShader)}
			};
		}

		inline bool
		RawScene(const script::GltfSceneExport &source, std::vector<std::byte> &bytes, json &manifest) {
			bytes.clear();
			manifest = {
				{"coordinate_system", "right_handed_y_up"},
				{"units", "metres"},
				{"meshes", json::array()},
					{"textures", json::array()},
					{"nodes", json::array()},
					{"cameras", json::array()},
					{"lights", json::array()}
			};
			for (const auto &mesh : source.Meshes) {
				const uint64_t meshBytes = static_cast<uint64_t>(mesh.Data.Vertices.size()) * 48 +
										   static_cast<uint64_t>(mesh.Data.Indices.size()) * 4;
				if (meshBytes > MAX_RAW_SCENE_BYTES - bytes.size()) return false;
				const size_t vertexOffset = bytes.size();
				for (const assets::MeshVertex &vertex : mesh.Data.Vertices) {
					for (float value : vertex.Position)
						RawFloat(bytes, value);
					for (float value : vertex.Normal)
						RawFloat(bytes, value);
					for (float value : vertex.TexCoord)
						RawFloat(bytes, value);
					for (uint16_t value : vertex.Joints)
						RawShort(bytes, value);
					for (uint16_t value : vertex.Weights)
						RawShort(bytes, value);
				}
				const size_t indexOffset = bytes.size();
				for (uint32_t index : mesh.Data.Indices)
					RawWord(bytes, index);
				if (bytes.size() > MAX_RAW_SCENE_BYTES) return false;
				json runs = json::array();
				for (const assets::Submesh &run : mesh.Data.Submeshes)
					runs.push_back(
						{{"first_index", run.FirstIndex},
						 {"index_count", run.IndexCount},
						 {"material", run.Material},
						 {"texture", run.Texture},
						 {"base_colour", run.BaseColour}}
					);
				manifest["meshes"].push_back(
					{{"name", mesh.Name},
					 {"joint_count", mesh.Data.JointCount},
					 {"vertex_section",
					  {{"byte_offset", vertexOffset},
					   {"byte_length", indexOffset - vertexOffset},
					   {"count", mesh.Data.Vertices.size()},
					   {"stride", 48},
					   {"endianness", "little"},
					   {"attributes",
						json::array(
							{{{"name", "position"},
							  {"format", "float32"},
							  {"components", 3},
							  {"byte_offset", 0}},
							 {{"name", "normal"},
							  {"format", "float32"},
							  {"components", 3},
							  {"byte_offset", 12}},
							 {{"name", "texcoord0"},
							  {"format", "float32"},
							  {"components", 2},
							  {"byte_offset", 24}},
							 {{"name", "joints0"},
							  {"format", "uint16"},
							  {"components", 4},
							  {"byte_offset", 32}},
							 {{"name", "weights0"},
							  {"format", "unorm16"},
							  {"components", 4},
							  {"byte_offset", 40}}}
						)}}},
					 {"index_section",
					  {{"byte_offset", indexOffset},
					   {"byte_length", bytes.size() - indexOffset},
					   {"count", mesh.Data.Indices.size()},
					   {"format", "uint32"},
					   {"endianness", "little"}}},
					 {"submeshes", std::move(runs)}}
				);
			}
			for (const auto &texture : source.SourceTextures) {
				if (texture.Pixels.size() > MAX_RAW_SCENE_BYTES - bytes.size()) return false;
				const size_t offset = bytes.size();
				bytes.insert(bytes.end(), texture.Pixels.begin(), texture.Pixels.end());
				manifest["textures"].push_back(
					{{"name", texture.Name},
					 {"byte_offset", offset},
					 {"byte_length", texture.Pixels.size()},
					 {"width", texture.Width},
					 {"height", texture.Height},
					 {"format", RawTextureFormat(texture.Format)},
					 {"color_space", assets::IsSRGB(texture.Format) ? "srgb" : "linear"},
					 {"row_order", "top_to_bottom"},
					 {"alpha", texture.Format == assets::TextureFormat::R8 ? "none" : "straight"}}
				);
			}
			for (const auto &node : source.Nodes) {
				json sourceRuns = json::array();
				for (std::optional<size_t> texture : node.SourceSubmeshColourTextures)
					sourceRuns.push_back(texture ? json(*texture) : json(nullptr));
				manifest["nodes"].push_back(
					{{"stable_id", node.StableId},
					 {"name", node.Name},
					 {"mesh", node.Mesh},
					 {"visible", node.Visible},
					 {"position", {node.Frame.Position.X, node.Frame.Position.Y, node.Frame.Position.Z}},
					 {"rotation",
					  {node.Frame.QuaternionX,
					   node.Frame.QuaternionY,
					   node.Frame.QuaternionZ,
					   node.Frame.QuaternionW}},
					 {"scale", {node.Scale.X, node.Scale.Y, node.Scale.Z}},
					 {"material", RawMaterial(node.Material)},
					 {"submesh_colour_textures", std::move(sourceRuns)}}
				);
			}
			for (const auto &camera : source.Cameras)
				manifest["cameras"].push_back(
					{{"stable_id", camera.StableId},
					 {"name", camera.Name},
					 {"position",
					  {camera.Frame.Position.X, camera.Frame.Position.Y, camera.Frame.Position.Z}},
					 {"rotation",
					  {camera.Frame.QuaternionX,
					   camera.Frame.QuaternionY,
					   camera.Frame.QuaternionZ,
					   camera.Frame.QuaternionW}},
					 {"field_of_view_radians", camera.FieldOfViewRadians},
					 {"near_plane_metres", camera.NearPlaneMetres},
					 {"far_plane_metres", camera.FarPlaneMetres}}
					);
			for (const auto &light : source.Lights)
				manifest["lights"].push_back(
					{{"stable_id", light.StableId},
					 {"name", light.Name},
					 {"kind", "point"},
					 {"position", {light.Position.X, light.Position.Y, light.Position.Z}},
					 {"colour", {light.Colour.R, light.Colour.G, light.Colour.B}},
					 {"brightness", light.Brightness},
					 {"brightness_unit", "engine_brightness"},
					 {"range_metres", light.Range}}
				);
			return manifest.dump().size() <= MAXIMUM_RESULT_BYTES;
		}

		inline bool
		GltfText(const json &value, std::string_view name, std::string &out, std::string &failure) {
			if (!value.is_string() || value.get_ref<const std::string &>().empty() ||
				value.get_ref<const std::string &>().size() > 256 ||
				value.get_ref<const std::string &>().find('\0') != std::string::npos) {
				failure = std::string(name) + " must be text of at most 256 bytes";
				return false;
			}
			out = value.get<std::string>();
			return true;
		}

		inline std::string Base64(std::span<const std::byte> bytes) {
			static constexpr std::string_view alphabet =
				"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
			std::string encoded;
			encoded.reserve((bytes.size() + 2) / 3 * 4);
			for (size_t offset = 0; offset < bytes.size(); offset += 3) {
				const uint32_t first = std::to_integer<unsigned char>(bytes[offset]);
				const uint32_t second =
					offset + 1 < bytes.size() ? std::to_integer<unsigned char>(bytes[offset + 1]) : 0;
				const uint32_t third =
					offset + 2 < bytes.size() ? std::to_integer<unsigned char>(bytes[offset + 2]) : 0;
				const uint32_t group = first << 16 | second << 8 | third;
				encoded.push_back(alphabet[(group >> 18) & 63]);
				encoded.push_back(alphabet[(group >> 12) & 63]);
				encoded.push_back(offset + 1 < bytes.size() ? alphabet[(group >> 6) & 63] : '=');
				encoded.push_back(offset + 2 < bytes.size() ? alphabet[group & 63] : '=');
			}
			return encoded;
		}

		inline void Word(std::vector<std::byte> &out, uint32_t value) {
			for (size_t byte = 0; byte < 4; ++byte)
				out.push_back(static_cast<std::byte>(value >> (byte * 8)));
		}

		inline size_t Binary(std::vector<std::byte> &out, std::span<const std::byte> bytes) {
			while (out.size() % 4 != 0)
				out.push_back(std::byte{});
			const size_t offset = out.size();
			out.insert(out.end(), bytes.begin(), bytes.end());
			return offset;
		}

		template <class Type>
		inline size_t BinaryValues(std::vector<std::byte> &out, std::span<Type> values) {
			return Binary(out, std::as_bytes(values));
		}

		inline void PngWord(std::vector<std::byte> &out, uint32_t value) {
			for (int shift = 24; shift >= 0; shift -= 8)
				out.push_back(static_cast<std::byte>(value >> shift));
		}

		inline void
		PngChunk(std::vector<std::byte> &out, const char (&type)[5], std::span<const std::byte> payload) {
			static const auto crcTable = [] {
				std::array<uint32_t, 256> table{};
				for (uint32_t index = 0; index < table.size(); ++index) {
					uint32_t value = index;
					for (size_t bit = 0; bit < 8; ++bit)
						value = (value >> 1) ^ ((value & 1) != 0 ? 0xEDB88320u : 0u);
					table[index] = value;
				}
				return table;
			}();
			PngWord(out, static_cast<uint32_t>(payload.size()));
			uint32_t crc = 0xFFFFFFFFu;
			for (size_t index = 0; index < 4; ++index) {
				const auto value = static_cast<uint8_t>(type[index]);
				out.push_back(static_cast<std::byte>(value));
				crc = (crc >> 8) ^ crcTable[(crc ^ value) & 0xFFu];
			}
			for (const std::byte value : payload) {
				out.push_back(value);
				crc = (crc >> 8) ^ crcTable[(crc ^ std::to_integer<uint8_t>(value)) & 0xFFu];
			}
			PngWord(out, crc ^ 0xFFFFFFFFu);
		}

		inline bool Png(const script::GltfExportTexture &image, std::vector<std::byte> &out) {
			const uint64_t pixels = static_cast<uint64_t>(image.Width) * image.Height;
			if (image.Width == 0 || image.Height == 0 || pixels > script::MAX_GLTF_EXPORT_TEXTURE_BYTES / 4 ||
				image.Pixels.size() != pixels * 4)
				return false;
			std::vector<std::byte> rows;
			rows.reserve(image.Pixels.size() + image.Height);
			for (uint32_t row = 0; row < image.Height; ++row) {
				rows.push_back(std::byte{});
				const size_t begin = static_cast<size_t>(row) * image.Width * 4;
				for (size_t pixel = begin; pixel < begin + static_cast<size_t>(image.Width) * 4; ++pixel)
					rows.push_back(static_cast<std::byte>(image.Pixels[pixel]));
			}
			std::vector<std::byte> compressed{std::byte{0x78}, std::byte{0x01}};
			for (size_t offset = 0; offset < rows.size();) {
				const uint16_t length = static_cast<uint16_t>(std::min<size_t>(65535, rows.size() - offset));
				compressed.push_back(offset + length == rows.size() ? std::byte{1} : std::byte{});
				compressed.push_back(static_cast<std::byte>(length));
				compressed.push_back(static_cast<std::byte>(length >> 8));
				compressed.push_back(static_cast<std::byte>(~length));
				compressed.push_back(static_cast<std::byte>((~length) >> 8));
				compressed.insert(compressed.end(), rows.begin() + offset, rows.begin() + offset + length);
				offset += length;
			}
			uint32_t a = 1, b = 0;
			for (const std::byte value : rows) {
				a = (a + std::to_integer<uint8_t>(value)) % 65521;
				b = (b + a) % 65521;
			}
			PngWord(compressed, (b << 16) | a);
			out = {
				std::byte{0x89},
				std::byte{0x50},
				std::byte{0x4E},
				std::byte{0x47},
				std::byte{0x0D},
				std::byte{0x0A},
				std::byte{0x1A},
				std::byte{0x0A}
			};
			std::vector<std::byte> header;
			PngWord(header, image.Width);
			PngWord(header, image.Height);
			header.insert(header.end(), {std::byte{8}, std::byte{6}, std::byte{}, std::byte{}, std::byte{}});
			PngChunk(out, "IHDR", header);
			PngChunk(out, "IDAT", compressed);
			PngChunk(out, "IEND", {});
			return true;
		}

		inline bool
		Gltf(const script::GltfSceneExport &source, std::vector<std::byte> &out, std::string &failure) {
			json document{
				{"asset", {{"version", "2.0"}, {"generator", "Atomic gltf-scene/v1"}}},
				{"scene", 0},
				{"scenes", json::array({json::object()})}
			};
			json nodes = json::array();
			json unavailable = json::array();
			for (const auto &entry : source.Unavailable)
				unavailable.push_back(
					{{"stable_id", entry.StableId}, {"feature", entry.Feature}, {"reason", entry.Reason}}
				);
			document["extras"] = {
				{"schema_version", "gltf-scene/v1"},
				{"tick", source.Tick},
				{"coordinate_system", "right_handed_y_up"},
				{"units", "metres"},
				{"unavailable", std::move(unavailable)}
			};
			std::vector<std::byte> binary;
			auto view = [&](size_t offset, size_t length, std::optional<uint32_t> target) {
				if (!document.contains("bufferViews")) document["bufferViews"] = json::array();
				json descriptor{{"buffer", 0}, {"byteOffset", offset}, {"byteLength", length}};
				if (target) descriptor["target"] = *target;
				document["bufferViews"].push_back(std::move(descriptor));
				return document["bufferViews"].size() - 1;
			};
			auto accessor = [&](size_t bufferView,
								uint32_t component,
								size_t count,
								const char *type,
								json minimum = nullptr,
								json maximum = nullptr,
								size_t byteOffset = 0) {
				json value{
					{"bufferView", bufferView}, {"componentType", component}, {"count", count}, {"type", type}
				};
				if (!minimum.is_null()) value["min"] = std::move(minimum);
				if (!maximum.is_null()) value["max"] = std::move(maximum);
				if (byteOffset != 0) value["byteOffset"] = byteOffset;
				if (!document.contains("accessors")) document["accessors"] = json::array();
				document["accessors"].push_back(std::move(value));
				return document["accessors"].size() - 1;
			};
			if (!source.Textures.empty()) {
				document["images"] = json::array();
				document["textures"] = json::array();
				document["samplers"] = json::array(
					{{{"magFilter", 9729}, {"minFilter", 9729}, {"wrapS", 10497}, {"wrapT", 10497}},
					 {{"magFilter", 9728}, {"minFilter", 9728}, {"wrapS", 10497}, {"wrapT", 10497}}}
				);
				for (const auto &texture : source.Textures) {
					std::vector<std::byte> png;
					if (!Png(texture, png)) {
						failure = "gltf export texture is invalid";
						return false;
					}
					const size_t bufferView = view(Binary(binary, png), png.size(), {});
					const size_t image = document["images"].size();
					document["images"].push_back(
						{{"name", texture.Name}, {"bufferView", bufferView}, {"mimeType", "image/png"}}
					);
					for (size_t sampler = 0; sampler < 2; ++sampler)
						document["textures"].push_back({{"source", image}, {"sampler", sampler}});
				}
			}
			for (const auto &node : source.Nodes) {
				if (node.Mesh >= source.Meshes.size()) {
					failure = "gltf export mesh index is invalid";
					return false;
				}
				for (const auto &index :
					 {node.Material.ColourTexture,
					  node.Material.NormalTexture,
					  node.Material.OcclusionTexture,
					  node.Material.EmissiveTexture,
					  node.Material.MetallicRoughnessTexture}) {
					if (index && *index >= source.Textures.size()) {
						failure = "gltf export texture index is invalid";
						return false;
					}
				}
				const auto textureRef = [&](std::optional<size_t> index) {
					return json{{"index", *index * 2 + static_cast<size_t>(node.Material.Pixelated)}};
				};
				const auto &mesh = source.Meshes[node.Mesh].Data;
				if (!node.SubmeshColourTextures.empty() &&
					node.SubmeshColourTextures.size() != mesh.Submeshes.size()) {
					failure = "gltf export submesh texture count is invalid";
					return false;
				}
				for (const auto &index : node.SubmeshColourTextures) {
					if (index && *index >= source.Textures.size()) {
						failure = "gltf export submesh texture index is invalid";
						return false;
					}
				}
				std::vector<float> positions, normals, texcoords;
				positions.reserve(mesh.Vertices.size() * 3);
				normals.reserve(mesh.Vertices.size() * 3);
				texcoords.reserve(mesh.Vertices.size() * 2);
				for (const auto &vertex : mesh.Vertices) {
					positions.insert(
						positions.end(), {vertex.Position[0], vertex.Position[1], vertex.Position[2]}
					);
					normals.insert(normals.end(), {vertex.Normal[0], vertex.Normal[1], vertex.Normal[2]});
					texcoords.insert(texcoords.end(), {vertex.TexCoord[0], vertex.TexCoord[1]});
				}
				const size_t positionView =
					view(BinaryValues(binary, std::span(positions)), positions.size() * sizeof(float), 34962);
				const size_t normalView =
					view(BinaryValues(binary, std::span(normals)), normals.size() * sizeof(float), 34962);
				const size_t texcoordView =
					view(BinaryValues(binary, std::span(texcoords)), texcoords.size() * sizeof(float), 34962);
				const size_t indexView = view(
					BinaryValues(binary, std::span(mesh.Indices)),
					mesh.Indices.size() * sizeof(uint32_t),
					34963
				);
				const size_t position = accessor(
					positionView,
					5126,
					mesh.Vertices.size(),
					"VEC3",
					{mesh.Minimum.X, mesh.Minimum.Y, mesh.Minimum.Z},
					{mesh.Maximum.X, mesh.Maximum.Y, mesh.Maximum.Z}
				);
				const size_t normal = accessor(normalView, 5126, mesh.Vertices.size(), "VEC3");
				const size_t texcoord = accessor(texcoordView, 5126, mesh.Vertices.size(), "VEC2");
				if (!document.contains("materials")) document["materials"] = json::array();
				json pbr{
					{"baseColorFactor",
					 {node.Material.BaseColour.R,
					  node.Material.BaseColour.G,
					  node.Material.BaseColour.B,
					  node.Material.Alpha}},
					{"metallicFactor", node.Material.MetalnessFactor},
					{"roughnessFactor", node.Material.RoughnessFactor}
				};
				if (node.Material.ColourTexture)
					pbr["baseColorTexture"] = textureRef(node.Material.ColourTexture);
				if (node.Material.MetallicRoughnessTexture)
					pbr["metallicRoughnessTexture"] = textureRef(node.Material.MetallicRoughnessTexture);
				json materialRecord{{"pbrMetallicRoughness", std::move(pbr)}};
				switch (node.Material.AlphaMode) {
				case script::GltfExportAlphaMode::Opaque:
					materialRecord["alphaMode"] = "OPAQUE";
					break;
				case script::GltfExportAlphaMode::Mask:
					materialRecord["alphaMode"] = "MASK";
					materialRecord["alphaCutoff"] = node.Material.AlphaCutoff;
					break;
				case script::GltfExportAlphaMode::Blend:
					materialRecord["alphaMode"] = "BLEND";
					break;
				}
				if (node.Material.NormalTexture)
					materialRecord["normalTexture"] = textureRef(node.Material.NormalTexture);
				if (node.Material.OcclusionTexture)
					materialRecord["occlusionTexture"] = textureRef(node.Material.OcclusionTexture);
				if (node.Material.EmissiveTexture) {
					materialRecord["emissiveTexture"] = textureRef(node.Material.EmissiveTexture);
					materialRecord["emissiveFactor"] = {
						node.Material.EmissiveFactor.R,
						node.Material.EmissiveFactor.G,
						node.Material.EmissiveFactor.B
					};
				}
				json primitives = json::array();
				const auto primitive = [&](uint32_t first,
										   uint32_t count,
										   const assets::Submesh *run,
										   std::optional<size_t> runTexture) {
					json ownMaterial = materialRecord;
					if (runTexture)
						ownMaterial["pbrMetallicRoughness"]["baseColorTexture"] = textureRef(runTexture);
					if (run != nullptr) {
						ownMaterial["pbrMetallicRoughness"]["baseColorFactor"] = {
							node.Material.BaseColour.R * run->BaseColour[0],
							node.Material.BaseColour.G * run->BaseColour[1],
							node.Material.BaseColour.B * run->BaseColour[2],
							node.Material.Alpha * run->BaseColour[3]
						};
						if (ownMaterial["alphaMode"] == "OPAQUE" && run->BaseColour[3] < 1.0f)
							ownMaterial["alphaMode"] = "BLEND";
						if (!run->Material.empty()) ownMaterial["name"] = run->Material;
					}
					const size_t materialIndex = document["materials"].size();
					document["materials"].push_back(std::move(ownMaterial));
					const size_t indices = accessor(
						indexView, 5125, count, "SCALAR", nullptr, nullptr, static_cast<size_t>(first) * 4
					);
					primitives.push_back(
						{{"attributes",
						  {{"POSITION", position}, {"NORMAL", normal}, {"TEXCOORD_0", texcoord}}},
						 {"indices", indices},
						 {"material", materialIndex},
						 {"mode", 4}}
					);
				};
				if (mesh.Submeshes.empty()) {
					primitive(0, static_cast<uint32_t>(mesh.Indices.size()), nullptr, {});
				} else {
					for (size_t index = 0; index < mesh.Submeshes.size(); ++index) {
						const auto &run = mesh.Submeshes[index];
						primitive(
							run.FirstIndex,
							run.IndexCount,
							&run,
							node.SubmeshColourTextures.empty() ? std::optional<size_t>{}
															   : node.SubmeshColourTextures[index]
						);
					}
				}
				if (!document.contains("meshes")) document["meshes"] = json::array();
				const size_t gltfMesh = document["meshes"].size();
				document["meshes"].push_back(
					{{"name", source.Meshes[node.Mesh].Name}, {"primitives", std::move(primitives)}}
				);
				nodes.push_back(
					{{"name", node.Name},
					 {"mesh", gltfMesh},
					 {"translation", {node.Frame.Position.X, node.Frame.Position.Y, node.Frame.Position.Z}},
					 {"rotation",
					  {node.Frame.QuaternionX,
					   node.Frame.QuaternionY,
					   node.Frame.QuaternionZ,
					   node.Frame.QuaternionW}},
					 {"scale", {node.Scale.X, node.Scale.Y, node.Scale.Z}},
					 {"extras", {{"engine_stable_id", node.StableId}}}}
				);
			}
			for (const auto &camera : source.Cameras) {
				if (!document.contains("cameras")) document["cameras"] = json::array();
				const size_t gltfCamera = document["cameras"].size();
				document["cameras"].push_back(
					{{"name", camera.Name},
					 {"type", "perspective"},
					 {"perspective",
					  {{"yfov", camera.FieldOfViewRadians},
					   {"znear", camera.NearPlaneMetres},
					   {"zfar", camera.FarPlaneMetres}}}}
				);
				nodes.push_back(
					{{"name", camera.Name},
					 {"camera", gltfCamera},
					 {"translation",
					  {camera.Frame.Position.X, camera.Frame.Position.Y, camera.Frame.Position.Z}},
					 {"rotation",
					  {camera.Frame.QuaternionX,
					   camera.Frame.QuaternionY,
					   camera.Frame.QuaternionZ,
					   camera.Frame.QuaternionW}},
					 {"extras", {{"engine_stable_id", camera.StableId}}}}
				);
			}
			if (!nodes.empty()) {
				document["nodes"] = std::move(nodes);
				document["scenes"][0]["nodes"] = json::array();
				for (size_t index = 0; index < document["nodes"].size(); ++index)
					document["scenes"][0]["nodes"].push_back(index);
			}
			if (!binary.empty()) {
				while (binary.size() % 4 != 0)
					binary.push_back(std::byte{});
				document["buffers"] = {{{"byteLength", binary.size()}}};
			}
			std::string text = document.dump();
			while (text.size() % 4 != 0)
				text.push_back(' ');
			const size_t binaryChunkBytes = binary.empty() ? 0 : 8 + binary.size();
			if (12 + 8 + text.size() + binaryChunkBytes > MAX_GLTF_EXPORT_BYTES) {
				failure = "gltf export exceeds the 320 MiB resource limit";
				return false;
			}
			out.clear();
			out.reserve(12 + 8 + text.size() + binaryChunkBytes);
			Word(out, 0x46546C67);
			Word(out, 2);
			Word(out, static_cast<uint32_t>(12 + 8 + text.size() + binaryChunkBytes));
			Word(out, static_cast<uint32_t>(text.size()));
			Word(out, 0x4E4F534A);
			out.insert(
				out.end(),
				reinterpret_cast<const std::byte *>(text.data()),
				reinterpret_cast<const std::byte *>(text.data() + text.size())
			);
			if (!binary.empty()) {
				Word(out, static_cast<uint32_t>(binary.size()));
				Word(out, 0x004E4942);
				out.insert(out.end(), binary.begin(), binary.end());
			}
			return true;
		}
	}

	inline void Surface::AddDataSceneTools(
		world::Universe &universe,
		std::shared_ptr<script::DataCaptureBridge> bridge,
		world::DataFactorySession *session,
		script::GltfMeshSource meshSource,
		script::GltfTextureSource textureSource
	) {
		world::Universe *worlds = session != nullptr ? &session->UniverseOf() : &universe;
		auto gltfResources = std::make_shared<data_scene_detail::GltfResources>();
		auto rawSceneResources = std::make_shared<data_scene_detail::RawSceneResources>();
		auto schema = [session] {
			json options{{"type", "object"}};
			if (session != nullptr) {
				options["properties"] = {
					{"expected_tick", {{"type", "integer"}, {"minimum", 0}}},
					{"expected_world_epoch", {{"type", "integer"}, {"minimum", 0}}},
					{"expected_world_version", {{"type", "integer"}, {"minimum", 0}}},
				};
				options["required"] =
					json::array({"expected_tick", "expected_world_epoch", "expected_world_version"});
			}
			return json{
				{"type", "object"},
				{"properties",
				 json{
					 {"instance_id", json{{"type", "string"}, {"minLength", 1}}},
					 {"options", std::move(options)}
				 }},
				{"required", json::array({"instance_id", "options"})},
				{"additionalProperties", false}
			};
		};
		Add(Tool{
			"get_scene_snapshot",
			"A bounded read-only snapshot of one scene, using stable authored ids.",
			schema,
			[worlds, session](const json &arguments, std::string &failure) -> json {
				using namespace data_scene_detail;
				if (!Only(arguments, {"instance_id", "options"}, failure)) return nullptr;
				if (!arguments.contains("options") || !arguments["options"].is_object()) {
					failure = "options must be an object";
					return nullptr;
				}
				if (!Options(arguments["options"], {"limit"}, session != nullptr, failure)) return nullptr;
				std::string instance;
				if (!data_factory_read_fence::InstanceId(arguments, instance, failure)) return nullptr;
				json fence;
				if (!data_factory_read_fence::Validate(
						session, instance, arguments["options"], fence, failure
					))
					return fence;
				const world::WorldId id = World(*worlds, instance, failure);
				if (!failure.empty()) return nullptr;
				size_t limit = script::MAX_DATA_SCENE_ENTITIES;
				if (arguments["options"].contains("limit")) {
					const json &value = arguments["options"]["limit"];
					if (!value.is_number_unsigned() ||
						value.get<uint64_t>() > script::MAX_DATA_SCENE_ENTITIES) {
						failure = "options.limit must be an integer from 0 through 10000";
						return nullptr;
					}
					limit = value.get<size_t>();
				}
				json out;
				const world::WorldStatus status = worlds->Enter(id, [&](ecs::Store &store) {
					out = Result(script::GetSceneSnapshot(store, limit), failure);
				});
				if (status != world::WorldStatus::Ok && failure.empty()) failure = "scene is unavailable";
				return out;
			}
		});

		Add(Tool{
			"export_gltf_scene",
			"Exports one fenced scene as a bounded glTF 2.0 GLB. Results above 40 KiB use a retained "
			"resource with ranged reads and explicit release. The v1 subset contains built-in, EditableMesh "
			"and bounded resident mesh geometry, source image maps, material runs and perspective cameras. "
			"Unavailable source facts are listed explicitly.",
			schema,
			[worlds, session, gltfResources, meshSource, textureSource](
				const json &arguments, std::string &failure
			) -> json {
				using namespace data_scene_detail;
				if (!Only(arguments, {"instance_id", "options"}, failure) || !arguments.contains("options") ||
					!arguments["options"].is_object() ||
					!Options(arguments["options"], {}, session != nullptr, failure))
					return nullptr;
				std::string instance;
				if (!data_factory_read_fence::InstanceId(arguments, instance, failure)) return nullptr;
				json fence;
				if (!data_factory_read_fence::Validate(
						session, instance, arguments["options"], fence, failure
					))
					return fence;
				const world::WorldId id = World(*worlds, instance, failure);
				if (!failure.empty()) return nullptr;
				script::GltfSceneExport captured;
				const world::WorldStatus status = worlds->Enter(id, [&](ecs::Store &store) {
					CaptureGltfSceneExport(store, captured, failure, meshSource, textureSource);
				});
				if (status != world::WorldStatus::Ok && failure.empty()) failure = "scene is unavailable";
				if (!failure.empty()) return nullptr;
				std::vector<std::byte> bytes;
				if (!Gltf(captured, bytes, failure)) return nullptr;
				const assets::ContentHash hash = assets::Hasher::Of(bytes);
				json unavailable = json::array();
				for (const auto &entry : captured.Unavailable)
					unavailable.push_back(
						{{"stable_id", entry.StableId}, {"feature", entry.Feature}, {"reason", entry.Reason}}
					);
				json reply{
					{"status", "ok"},
					{"schema_version", "gltf-scene/v1"},
					{"format", "glb"},
					{"byte_length", bytes.size()},
					{"hash_algorithm", "blake3-256"},
					{"hash", hash.ToHex()},
					{"tick", captured.Tick},
					{"coordinate_system", "right_handed_y_up"},
					{"units", "metres"},
					{"unavailable", std::move(unavailable)},
					{"inline_byte_limit", MAX_GLTF_EXPORT_REPLY_BYTES}
				};
				if (bytes.size() <= MAX_GLTF_EXPORT_REPLY_BYTES) {
					reply["encoding"] = "base64";
					reply["data"] = Base64(bytes);
					return reply;
				}
				std::lock_guard lock(gltfResources->Mutex);
				if (gltfResources->Entries.size() == MAX_GLTF_EXPORT_RESOURCES ||
					gltfResources->NextId == 0) {
					failure = "gltf export resource capacity reached";
					return nullptr;
				}
				const std::string resourceId = "gltf/" + std::to_string(gltfResources->NextId++);
				gltfResources->Entries.push_back(
					{.Id = resourceId,
					 .Instance = instance,
					 .Tick = session == nullptr ? captured.Tick
												: arguments["options"]["expected_tick"].get<uint64_t>(),
					 .WorldEpoch = session == nullptr
									   ? 0
									   : arguments["options"]["expected_world_epoch"].get<uint64_t>(),
					 .WorldVersion = session == nullptr
										 ? 0
										 : arguments["options"]["expected_world_version"].get<uint64_t>(),
					 .Hash = hash.ToHex(),
					 .Bytes = std::move(bytes)}
				);
				reply["encoding"] = "resource";
				reply["data"] = nullptr;
				reply["resource_id"] = resourceId;
				reply["resource_byte_limit"] = MAX_GLTF_EXPORT_BYTES;
				return reply;
			}
		});

		Add(Tool{
			"get_gltf_scene_chunk",
			"Returns up to one MiB from a retained glTF export at its original world revision.",
			[schema] {
				json result = schema();
				result["properties"]["resource_id"] = {{"type", "string"}};
				result["properties"]["hash"] = {{"type", "string"}};
				result["properties"]["byte_begin"] = {{"type", "integer"}, {"minimum", 0}};
				result["properties"]["byte_end"] = {{"type", "integer"}, {"minimum", 1}};
				for (const char *field : {"resource_id", "hash", "byte_begin", "byte_end"})
					result["required"].push_back(field);
				return result;
			},
			[worlds, session, gltfResources](const json &arguments, std::string &failure) -> json {
				using namespace data_scene_detail;
				if (!Only(
						arguments,
						{"instance_id", "resource_id", "hash", "byte_begin", "byte_end", "options"},
						failure
					) ||
					!arguments.contains("options") || !arguments["options"].is_object() ||
					!Options(arguments["options"], {}, session != nullptr, failure))
					return nullptr;
				std::string instance, resource, digest;
				if (!data_factory_read_fence::InstanceId(arguments, instance, failure) ||
					!GltfText(arguments.value("resource_id", json{}), "resource_id", resource, failure) ||
					!GltfText(arguments.value("hash", json{}), "hash", digest, failure))
					return nullptr;
				const json &beginValue = arguments.value("byte_begin", json{});
				const json &endValue = arguments.value("byte_end", json{});
				if (!beginValue.is_number_unsigned() || !endValue.is_number_unsigned()) {
					failure = "gltf byte range must use unsigned integers";
					return nullptr;
				}
				const uint64_t begin = beginValue.get<uint64_t>();
				const uint64_t end = endValue.get<uint64_t>();
				if (end <= begin || end - begin > MAX_GLTF_EXPORT_READ_BYTES) {
					failure = "gltf byte range is invalid";
					return nullptr;
				}
				json fence;
				if (!data_factory_read_fence::Validate(
						session, instance, arguments["options"], fence, failure
					))
					return fence;
				if (!worlds->Find(core::Name(instance)).IsValid()) {
					failure = "gltf instance is unavailable";
					return nullptr;
				}
				std::lock_guard lock(gltfResources->Mutex);
				const auto found = std::find_if(
					gltfResources->Entries.begin(),
					gltfResources->Entries.end(),
					[&](const GltfResource &entry) {
						return entry.Id == resource && entry.Instance == instance;
					}
				);
				if (found == gltfResources->Entries.end() || found->Hash != digest ||
					end > found->Bytes.size() ||
					(session != nullptr &&
					 (found->Tick != arguments["options"]["expected_tick"].get<uint64_t>() ||
					  found->WorldEpoch != arguments["options"]["expected_world_epoch"].get<uint64_t>() ||
					  found->WorldVersion !=
						  arguments["options"]["expected_world_version"].get<uint64_t>()))) {
					failure = "gltf resource is unavailable at this revision";
					return nullptr;
				}
				return {
					{"resource_id", resource},
					{"hash", digest},
					{"byte_begin", begin},
					{"byte_end", end},
					{"base64", Base64(std::span(found->Bytes).subspan(begin, end - begin))}
				};
			}
		});

		Add(Tool{
			"release_gltf_scene",
			"Releases one retained glTF export, including after its world revision changes.",
			[] {
				return json{
					{"type", "object"},
					{"properties",
					 {{"instance_id", {{"type", "string"}}}, {"resource_id", {{"type", "string"}}}}},
					{"required", json::array({"instance_id", "resource_id"})},
					{"additionalProperties", false}
				};
			},
			[gltfResources](const json &arguments, std::string &failure) -> json {
				using namespace data_scene_detail;
				if (!Only(arguments, {"instance_id", "resource_id"}, failure)) return nullptr;
				std::string instance, resource;
				if (!data_factory_read_fence::InstanceId(arguments, instance, failure) ||
					!GltfText(arguments.value("resource_id", json{}), "resource_id", resource, failure))
					return nullptr;
				std::lock_guard lock(gltfResources->Mutex);
				const auto found = std::find_if(
					gltfResources->Entries.begin(),
					gltfResources->Entries.end(),
					[&](const GltfResource &entry) {
						return entry.Id == resource && entry.Instance == instance;
					}
				);
				if (found == gltfResources->Entries.end()) {
					failure = "gltf resource is unavailable";
					return nullptr;
				}
				gltfResources->Entries.erase(found);
				return {{"status", "released"}, {"resource_id", resource}};
			}
		});

		Add(Tool{
			"begin_raw_scene_extract",
			"Captures one fenced scene as portable raw mesh and texture sections with an inline manifest.",
			schema,
			[worlds, session, rawSceneResources, meshSource, textureSource](
				const json &arguments, std::string &failure
			) -> json {
				using namespace data_scene_detail;
				if (!Only(arguments, {"instance_id", "options"}, failure) || !arguments.contains("options") ||
					!arguments["options"].is_object() ||
					!Options(arguments["options"], {}, session != nullptr, failure))
					return nullptr;
				std::string instance;
				if (!data_factory_read_fence::InstanceId(arguments, instance, failure)) return nullptr;
				json fence;
				if (!data_factory_read_fence::Validate(
						session, instance, arguments["options"], fence, failure
					))
					return fence;
				const world::WorldId id = World(*worlds, instance, failure);
				if (!failure.empty()) return nullptr;
				script::GltfSceneExport captured;
				const world::WorldStatus status = worlds->Enter(id, [&](ecs::Store &store) {
					CaptureGltfSceneExport(store, captured, failure, meshSource, textureSource, true);
				});
				if (status != world::WorldStatus::Ok && failure.empty())
					failure = "resource_unavailable: scene is unavailable";
				if (!failure.empty()) return nullptr;
				std::vector<std::byte> bytes;
				json manifest;
				if (!RawScene(captured, bytes, manifest)) {
					failure = "resource_too_large: raw scene bytes or manifest exceed the extraction limit";
					return nullptr;
				}
				const assets::ContentHash hash = assets::Hasher::Of(bytes);
				std::lock_guard lock(rawSceneResources->Mutex);
				if (rawSceneResources->Entries.size() == MAX_RAW_SCENE_RESOURCES ||
					rawSceneResources->NextId == 0) {
					failure = "resource_capacity: raw scene resource capacity reached";
					return nullptr;
				}
				const std::string resource = "raw-scene/" + std::to_string(rawSceneResources->NextId);
				const uint64_t tick = session == nullptr
										  ? captured.Tick
										  : arguments["options"]["expected_tick"].get<uint64_t>();
				const uint64_t epoch =
					session == nullptr ? 0 : arguments["options"]["expected_world_epoch"].get<uint64_t>();
				const uint64_t version =
					session == nullptr ? 0 : arguments["options"]["expected_world_version"].get<uint64_t>();
				json unavailable = json::array();
				for (const auto &entry : captured.Unavailable)
					unavailable.push_back(
						{{"stable_id", entry.StableId}, {"feature", entry.Feature}, {"reason", entry.Reason}}
					);
				json reply{
					{"status", "ok"},
					{"schema_version", "raw-scene/v2"},
					{"encoding", "resource"},
					{"resource_id", resource},
					{"instance_id", instance},
					{"tick", tick},
					{"world_epoch", epoch},
					{"world_version", version},
					{"byte_length", bytes.size()},
					{"hash_algorithm", "blake3-256"},
					{"hash", hash.ToHex()},
					{"chunk_byte_limit", MAX_RAW_SCENE_READ_BYTES},
					{"resource_byte_limit", MAX_RAW_SCENE_BYTES},
					{"manifest", std::move(manifest)},
					{"unavailable", std::move(unavailable)}
				};
				if (reply.dump().size() > MAXIMUM_RESULT_BYTES) {
					failure = "resource_too_large: raw scene reply exceeds the response limit";
					return nullptr;
				}
				rawSceneResources->Entries.push_back(
					{resource, instance, tick, epoch, version, hash.ToHex(), std::move(bytes)}
				);
				rawSceneResources->NextId++;
				return reply;
			}
		});

		Add(Tool{
			"get_raw_scene_chunk",
			"Returns up to one MiB from a retained raw scene extract at its original world revision.",
			[schema] {
				json result = schema();
				result["properties"]["resource_id"] = {{"type", "string"}};
				result["properties"]["hash"] = {{"type", "string"}};
				result["properties"]["byte_begin"] = {{"type", "integer"}, {"minimum", 0}};
				result["properties"]["byte_end"] = {{"type", "integer"}, {"minimum", 1}};
				for (const char *field : {"resource_id", "hash", "byte_begin", "byte_end"})
					result["required"].push_back(field);
				return result;
			},
			[worlds, session, rawSceneResources](const json &arguments, std::string &failure) -> json {
				using namespace data_scene_detail;
				if (!Only(
						arguments,
						{"instance_id", "resource_id", "hash", "byte_begin", "byte_end", "options"},
						failure
					) ||
					!arguments.contains("options") || !arguments["options"].is_object() ||
					!Options(arguments["options"], {}, session != nullptr, failure))
					return nullptr;
				std::string instance, resource, digest;
				if (!data_factory_read_fence::InstanceId(arguments, instance, failure) ||
					!GltfText(arguments.value("resource_id", json{}), "resource_id", resource, failure) ||
					!GltfText(arguments.value("hash", json{}), "hash", digest, failure))
					return nullptr;
				const json &beginValue = arguments.value("byte_begin", json{});
				const json &endValue = arguments.value("byte_end", json{});
				if (!beginValue.is_number_unsigned() || !endValue.is_number_unsigned()) {
					failure = "validation_failed: byte range must use unsigned integers";
					return nullptr;
				}
				const uint64_t begin = beginValue.get<uint64_t>(), end = endValue.get<uint64_t>();
				if (end <= begin || end - begin > MAX_RAW_SCENE_READ_BYTES) {
					failure = "validation_failed: byte range is invalid";
					return nullptr;
				}
				json fence;
				if (!data_factory_read_fence::Validate(
						session, instance, arguments["options"], fence, failure
					))
					return fence;
				if (!worlds->Find(core::Name(instance)).IsValid()) {
					failure = "resource_unavailable: scene is unavailable";
					return nullptr;
				}
				std::lock_guard lock(rawSceneResources->Mutex);
				const auto found = std::find_if(
					rawSceneResources->Entries.begin(),
					rawSceneResources->Entries.end(),
					[&](const RawSceneResource &entry) {
						return entry.Id == resource && entry.Instance == instance;
					}
				);
				if (found == rawSceneResources->Entries.end() || found->Hash != digest ||
					end > found->Bytes.size() ||
					(session != nullptr &&
					 (found->Tick != arguments["options"]["expected_tick"].get<uint64_t>() ||
					  found->WorldEpoch != arguments["options"]["expected_world_epoch"].get<uint64_t>() ||
					  found->WorldVersion !=
						  arguments["options"]["expected_world_version"].get<uint64_t>()))) {
					failure = "resource_unavailable: raw scene resource is unavailable at this revision";
					return nullptr;
				}
				return {
					{"resource_id", resource},
					{"hash", digest},
					{"byte_begin", begin},
					{"byte_end", end},
					{"base64", Base64(std::span(found->Bytes).subspan(begin, end - begin))}
				};
			}
		});

		Add(Tool{
			"release_raw_scene_extract",
			"Releases one retained raw scene extract, including after its world revision changes.",
			[] {
				return json{
					{"type", "object"},
					{"properties",
					 {{"instance_id", {{"type", "string"}}}, {"resource_id", {{"type", "string"}}}}},
					{"required", json::array({"instance_id", "resource_id"})},
					{"additionalProperties", false}
				};
			},
			[rawSceneResources](const json &arguments, std::string &failure) -> json {
				using namespace data_scene_detail;
				if (!Only(arguments, {"instance_id", "resource_id"}, failure)) return nullptr;
				std::string instance, resource;
				if (!data_factory_read_fence::InstanceId(arguments, instance, failure) ||
					!GltfText(arguments.value("resource_id", json{}), "resource_id", resource, failure))
					return nullptr;
				std::lock_guard lock(rawSceneResources->Mutex);
				const auto found = std::find_if(
					rawSceneResources->Entries.begin(),
					rawSceneResources->Entries.end(),
					[&](const RawSceneResource &entry) {
						return entry.Id == resource && entry.Instance == instance;
					}
				);
				if (found == rawSceneResources->Entries.end()) {
					failure = "resource_unavailable: raw scene resource is unavailable";
					return nullptr;
				}
				rawSceneResources->Entries.erase(found);
				return {{"status", "released"}, {"resource_id", resource}};
			}
		});

		Add(Tool{
			"get_camera_rendering_data",
			"Camera calibration for a stable authored camera id, or the scene's active camera.",
			schema,
			[worlds, session](const json &arguments, std::string &failure) -> json {
				using namespace data_scene_detail;
				if (!Only(arguments, {"instance_id", "options"}, failure)) return nullptr;
				if (!arguments.contains("options") || !arguments["options"].is_object()) {
					failure = "options must be an object";
					return nullptr;
				}
				if (!Options(
						arguments["options"], {"camera_id", "object_limit"}, session != nullptr, failure
					))
					return nullptr;
				std::string instance;
				if (!data_factory_read_fence::InstanceId(arguments, instance, failure)) return nullptr;
				json fence;
				if (!data_factory_read_fence::Validate(
						session, instance, arguments["options"], fence, failure
					))
					return fence;
				const world::WorldId id = World(*worlds, instance, failure);
				if (!failure.empty()) return nullptr;
				std::string wanted;
				size_t objectLimit = 0;
				if (arguments["options"].contains("object_limit")) {
					const json &value = arguments["options"]["object_limit"];
					if (!value.is_number_unsigned() ||
						value.get<uint64_t>() > script::MAX_CAMERA_OBJECT_OBSERVATIONS) {
						failure = "options.object_limit must be an integer from 0 through 64";
						return nullptr;
					}
					objectLimit = value.get<size_t>();
				}
				if (arguments["options"].contains("camera_id")) {
					if (!arguments["options"]["camera_id"].is_string()) {
						failure = "options.camera_id must be a string";
						return nullptr;
					}
					wanted = arguments["options"]["camera_id"].get<std::string>();
				}
				json out;
				const world::WorldStatus status = worlds->Enter(id, [&](ecs::Store &store) {
					ecs::Entity camera;
					if (wanted.empty()) {
						if (const auto *active = store.Resource<scene::ActiveCamera>())
							camera = active->Entity;
					} else
						store.Each<const ecs::InstanceName>([&](ecs::Entity entity,
																const ecs::InstanceName &) {
							ecs::AttributeValue value;
							if (ecs::GetAttribute(
									store, entity, core::Name(script::DATA_SCENE_ID_ATTRIBUTE), value
								) &&
								value.Type == ecs::PropertyType::String && value.String == wanted) {
								if (camera != ecs::NULL_ENTITY) {
									failure = "camera stable authored id is not unique";
									return;
								}
								camera = entity;
							}
						});
					if (camera == ecs::NULL_ENTITY) {
						failure = wanted.empty() ? "scene has no active camera"
												 : "no camera has that stable authored id";
						return;
					}
					out = Result(script::GetCameraRenderingData(store, camera, objectLimit), failure);
				});
				if (status != world::WorldStatus::Ok && failure.empty()) failure = "scene is unavailable";
				return out;
			}
		});

		const auto observation =
			[worlds, schema, bridge, session](std::string name, std::string description, bool channels) {
				return Tool{
					std::move(name),
					std::move(description),
					schema,
					[worlds, bridge, channels, session](const json &arguments, std::string &failure) -> json {
						using namespace data_scene_detail;
						if (!Only(arguments, {"instance_id", "options"}, failure)) return nullptr;
						if (!arguments.contains("options") || !arguments["options"].is_object()) {
							failure = "options must be an object";
							return nullptr;
						}
						if (!Options(arguments["options"], {}, session != nullptr, failure)) return nullptr;
						std::string instance;
						if (!data_factory_read_fence::InstanceId(arguments, instance, failure))
							return nullptr;
						json fence;
						if (!data_factory_read_fence::Validate(
								session, instance, arguments["options"], fence, failure
							))
							return fence;
						const world::WorldId id = World(*worlds, instance, failure);
						if (!failure.empty()) return nullptr;
						json out;
						const world::WorldStatus status = worlds->Enter(id, [&](ecs::Store &store) {
							out = Result(
								channels ? script::GetCaptureChannels(store, bridge)
										 : script::GetResources(store),
								failure
							);
						});
						if (status != world::WorldStatus::Ok && failure.empty())
							failure = "scene is unavailable";
						return out;
					}
				};
			};
		Add(observation("get_capture_channels", "Capture channel capability metadata for one scene.", true));
		Add(observation("get_resources", "Durable resource metadata for one scene.", false));
		Add(Tool{
			"get_event_narratives",
			"Script-declared, bounded event narratives for one scene.",
			schema,
			[worlds, session](const json &arguments, std::string &failure) -> json {
				using namespace data_scene_detail;
				if (!Only(arguments, {"instance_id", "options"}, failure)) return nullptr;
				if (!arguments.contains("options") ||
					!Options(arguments["options"], {}, session != nullptr, failure))
					return nullptr;
				std::string instance;
				if (!data_factory_read_fence::InstanceId(arguments, instance, failure)) return nullptr;
				json fence;
				if (!data_factory_read_fence::Validate(
						session, instance, arguments["options"], fence, failure
					))
					return fence;
				const world::WorldId id = World(*worlds, instance, failure);
				if (!failure.empty()) return nullptr;
				json out;
				const world::WorldStatus status = worlds->Enter(id, [&](ecs::Store &store) {
					const script::DataSceneResult narratives = script::GetEventNarratives(store);
					if (std::strcmp(narratives.Status, "unavailable") != 0) {
						out = Result(narratives, failure);
						return;
					}
					size_t bytes = 0;
					if (!JsonValue(narratives.Value, out, 0, bytes) ||
						out.dump().size() > MAXIMUM_RESULT_BYTES)
						failure = "data-scene result exceeds the safe response limit";
				});
				if (status != world::WorldStatus::Ok && failure.empty()) failure = "scene is unavailable";
				return out;
			}
		});
		auto querySchema = [session](json properties, json required) {
			return [session, properties = std::move(properties), required = std::move(required)] {
				json optionProperties = properties;
				json optionRequired = required;
				if (session != nullptr) {
					optionProperties["expected_tick"] = {{"type", "integer"}, {"minimum", 0}};
					optionProperties["expected_world_epoch"] = {{"type", "integer"}, {"minimum", 0}};
					optionProperties["expected_world_version"] = {{"type", "integer"}, {"minimum", 0}};
					optionRequired.push_back("expected_tick");
					optionRequired.push_back("expected_world_epoch");
					optionRequired.push_back("expected_world_version");
				}
				return json{
					{"type", "object"},
					{"properties",
					 json{
						 {"instance_id", json{{"type", "string"}, {"minLength", 1}}},
						 {"options",
						  json{
							  {"type", "object"},
							  {"properties", std::move(optionProperties)},
							  {"required", std::move(optionRequired)},
							  {"additionalProperties", false},
						  }},
					 }},
					{"required", json::array({"instance_id", "options"})},
					{"additionalProperties", false},
				};
			};
		};
		const json vectorSchema{
			{"type", "array"},
			{"items", json{{"type", "number"}}},
			{"minItems", 3},
			{"maxItems", 3},
		};
		Add(Tool{
			"get_authored_affordances",
			"Return bounded explicit authored affordances in stable identity order. Geometry and colliders "
			"do not infer semantics.",
			querySchema(
				json{
					{"limit",
					 {{"type", "integer"}, {"minimum", 0}, {"maximum", script::MAX_AUTHORED_AFFORDANCES}}}
				},
				json::array({"limit"})
			),
			[worlds, session](const json &arguments, std::string &failure) -> json {
				using namespace data_scene_detail;
				if (!Only(arguments, {"instance_id", "options"}, failure) || !arguments.contains("options") ||
					!arguments["options"].is_object())
					return nullptr;
				const json &options = arguments["options"];
				if (!Options(options, {"limit"}, session != nullptr, failure) || !options.contains("limit") ||
					!options["limit"].is_number_unsigned() ||
					options["limit"].get<uint64_t>() > script::MAX_AUTHORED_AFFORDANCES) {
					if (failure.empty()) failure = "options.limit must be an integer from 0 through 256";
					return nullptr;
				}
				std::string instance;
				if (!data_factory_read_fence::InstanceId(arguments, instance, failure)) return nullptr;
				json fence;
				if (!data_factory_read_fence::Validate(session, instance, options, fence, failure))
					return fence;
				const world::WorldId id = World(*worlds, instance, failure);
				if (!failure.empty()) return nullptr;
				json out;
				const world::WorldStatus status = worlds->Enter(id, [&](ecs::Store &store) {
					out = Result(
						script::GetAuthoredAffordances(store, options["limit"].get<size_t>()), failure
					);
				});
				if (status != world::WorldStatus::Ok && failure.empty()) failure = "scene is unavailable";
				return out;
			}
		});
		Add(Tool{
			"raycast",
			"Cast a finite ray through one scene's prepared physics colliders and return exact hit metadata "
			"with a stable authored entity id when one exists.",
			querySchema(
				json{
					{"origin", vectorSchema},
					{"direction", vectorSchema},
					{"max_distance_metres",
					 json{{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 100'000}}},
				},
				json::array({"origin", "direction", "max_distance_metres"})
			),
			[worlds, session](const json &arguments, std::string &failure) -> json {
				using namespace data_scene_detail;
				if (!Only(arguments, {"instance_id", "options"}, failure) || !arguments.contains("options") ||
					!arguments["options"].is_object()) {
					if (failure.empty()) failure = "options must be an object";
					return nullptr;
				}
				const json &options = arguments["options"];
				if (!Options(
						options, {"origin", "direction", "max_distance_metres"}, session != nullptr, failure
					))
					return nullptr;
				core::Vector3 origin;
				core::Vector3 direction;
				if (!Vector(options.value("origin", json{}), origin) ||
					!Vector(options.value("direction", json{}), direction) ||
					!options.contains("max_distance_metres") || !options["max_distance_metres"].is_number()) {
					failure = "options requires finite origin, direction and max_distance_metres";
					return nullptr;
				}
				std::string instance;
				if (!data_factory_read_fence::InstanceId(arguments, instance, failure)) return nullptr;
				json fence;
				if (!data_factory_read_fence::Validate(session, instance, options, fence, failure))
					return fence;
				const world::WorldId id = World(*worlds, instance, failure);
				if (!failure.empty()) return nullptr;
				json out;
				const world::WorldStatus status = worlds->Enter(id, [&](ecs::Store &store) {
					out = Result(
						script::Raycast(
							store, {origin, direction, options["max_distance_metres"].get<float>()}
						),
						failure
					);
				});
				if (status != world::WorldStatus::Ok && failure.empty()) failure = "scene is unavailable";
				return out;
			}
		});
		Add(Tool{
			"overlap_aabb",
			"Return stable authored ids for prepared physics colliders overlapping one finite world-space "
			"axis-aligned box.",
			querySchema(
				json{{"minimum", vectorSchema}, {"maximum", vectorSchema}},
				json::array({"minimum", "maximum"})
			),
			[worlds, session](const json &arguments, std::string &failure) -> json {
				using namespace data_scene_detail;
				if (!Only(arguments, {"instance_id", "options"}, failure) || !arguments.contains("options") ||
					!arguments["options"].is_object()) {
					if (failure.empty()) failure = "options must be an object";
					return nullptr;
				}
				const json &options = arguments["options"];
				if (!Options(options, {"minimum", "maximum"}, session != nullptr, failure)) return nullptr;
				core::Vector3 minimum;
				core::Vector3 maximum;
				if (!Vector(options.value("minimum", json{}), minimum) ||
					!Vector(options.value("maximum", json{}), maximum)) {
					failure = "options requires finite minimum and maximum vectors";
					return nullptr;
				}
				std::string instance;
				if (!data_factory_read_fence::InstanceId(arguments, instance, failure)) return nullptr;
				json fence;
				if (!data_factory_read_fence::Validate(session, instance, options, fence, failure))
					return fence;
				const world::WorldId id = World(*worlds, instance, failure);
				if (!failure.empty()) return nullptr;
				json out;
				const world::WorldStatus status = worlds->Enter(id, [&](ecs::Store &store) {
					out = Result(script::OverlapAABB(store, {minimum, maximum}), failure);
				});
				if (status != world::WorldStatus::Ok && failure.empty()) failure = "scene is unavailable";
				return out;
			}
		});
		Add(Tool{
			"overlap_obb",
			"Return stable authored ids for prepared physics colliders overlapping one finite world-space "
			"oriented box.",
			querySchema(
				json{
					{"center", vectorSchema},
					{"orientation_xyzw",
					 json{
						 {"type", "array"},
						 {"items", json{{"type", "number"}}},
						 {"minItems", 4},
						 {"maxItems", 4},
					 }},
					{"half_extent", vectorSchema},
				},
				json::array({"center", "orientation_xyzw", "half_extent"})
			),
			[worlds, session](const json &arguments, std::string &failure) -> json {
				using namespace data_scene_detail;
				if (!Only(arguments, {"instance_id", "options"}, failure) || !arguments.contains("options") ||
					!arguments["options"].is_object()) {
					if (failure.empty()) failure = "options must be an object";
					return nullptr;
				}
				const json &options = arguments["options"];
				if (!Options(
						options, {"center", "orientation_xyzw", "half_extent"}, session != nullptr, failure
					))
					return nullptr;
				core::Vector3 center;
				core::Vector3 halfExtent;
				const json &rotation = options.value("orientation_xyzw", json{});
				if (!Vector(options.value("center", json{}), center) ||
					!Vector(options.value("half_extent", json{}), halfExtent) || !rotation.is_array() ||
					rotation.size() != 4 ||
					std::any_of(rotation.begin(), rotation.end(), [](const json &value) {
						return !value.is_number();
					})) {
					failure = "options requires finite center, orientation_xyzw and half_extent";
					return nullptr;
				}
				core::CFrame frame{center};
				frame.QuaternionX = rotation[0].get<float>();
				frame.QuaternionY = rotation[1].get<float>();
				frame.QuaternionZ = rotation[2].get<float>();
				frame.QuaternionW = rotation[3].get<float>();
				std::string instance;
				if (!data_factory_read_fence::InstanceId(arguments, instance, failure)) return nullptr;
				json fence;
				if (!data_factory_read_fence::Validate(session, instance, options, fence, failure))
					return fence;
				const world::WorldId id = World(*worlds, instance, failure);
				if (!failure.empty()) return nullptr;
				json out;
				const world::WorldStatus status = worlds->Enter(id, [&](ecs::Store &store) {
					out = Result(script::OverlapOBB(store, {frame, halfExtent}), failure);
				});
				if (status != world::WorldStatus::Ok && failure.empty()) failure = "scene is unavailable";
				return out;
			}
		});
	}

	namespace features {
		inline Feature DataScene(
			world::Universe &universe,
			std::shared_ptr<script::DataCaptureBridge> bridge = {},
			world::DataFactorySession *session = nullptr,
			script::GltfMeshSource meshSource = {},
			script::GltfTextureSource textureSource = {}
		) {
			return Feature{
				"data_scene",
				[&universe,
				 bridge = std::move(bridge),
				 session,
				 meshSource = std::move(meshSource),
				 textureSource = std::move(textureSource)](Surface &surface) {
					surface.AddDataSceneTools(universe, bridge, session, meshSource, textureSource);
				}
			};
		}
	}
}
