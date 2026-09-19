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
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <nlohmann/json.hpp>
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
			auto view = [&](size_t offset, size_t length, uint32_t target) {
				if (!document.contains("bufferViews")) document["bufferViews"] = json::array();
				document["bufferViews"].push_back(
					{{"buffer", 0}, {"byteOffset", offset}, {"byteLength", length}, {"target", target}}
				);
				return document["bufferViews"].size() - 1;
			};
			auto accessor = [&](size_t bufferView,
								uint32_t component,
								size_t count,
								const char *type,
								json minimum = nullptr,
								json maximum = nullptr) {
				json value{
					{"bufferView", bufferView}, {"componentType", component}, {"count", count}, {"type", type}
				};
				if (!minimum.is_null()) value["min"] = std::move(minimum);
				if (!maximum.is_null()) value["max"] = std::move(maximum);
				if (!document.contains("accessors")) document["accessors"] = json::array();
				document["accessors"].push_back(std::move(value));
				return document["accessors"].size() - 1;
			};
			for (const auto &node : source.Nodes) {
				const auto &mesh = source.Meshes[node.Mesh].Data;
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
				const size_t indices = accessor(indexView, 5125, mesh.Indices.size(), "SCALAR");
				if (!document.contains("materials")) document["materials"] = json::array();
				const size_t material = document["materials"].size();
				document["materials"].push_back(
					{{"pbrMetallicRoughness",
					  {{"baseColorFactor",
						{node.Material.BaseColour.R,
						 node.Material.BaseColour.G,
						 node.Material.BaseColour.B,
						 node.Material.Alpha}},
					   {"metallicFactor", 0.0},
					   {"roughnessFactor", 1.0}}},
					 {"alphaMode",
					  node.Material.AlphaMode == script::GltfExportAlphaMode::Blend ? "BLEND" : "OPAQUE"}}
				);
				if (!document.contains("meshes")) document["meshes"] = json::array();
				const size_t gltfMesh = document["meshes"].size();
				document["meshes"].push_back(
					{{"name", source.Meshes[node.Mesh].Name},
					 {"primitives",
					  {{{"attributes",
						 {{"POSITION", position}, {"NORMAL", normal}, {"TEXCOORD_0", texcoord}}},
						{"indices", indices},
						{"material", material},
						{"mode", 4}}}}}
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
			if (12 + 8 + text.size() + binaryChunkBytes > MAX_GLTF_EXPORT_REPLY_BYTES) {
				failure = "gltf export exceeds the 40 KiB inline response limit";
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
		world::DataFactorySession *session
	) {
		world::Universe *worlds = session != nullptr ? &session->UniverseOf() : &universe;
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
			"Exports one fenced scene as a bounded glTF 2.0 GLB. The v1 subset contains exact built-in and "
			"EditableMesh geometry, transforms, base-colour factors and perspective cameras. It reports "
			"streamed mesh assets and unavailable textures explicitly.",
			schema,
			[worlds, session](const json &arguments, std::string &failure) -> json {
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
					CaptureGltfSceneExport(store, captured, failure);
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
				return {
					{"status", "ok"},
					{"schema_version", "gltf-scene/v1"},
					{"format", "glb"},
					{"encoding", "base64"},
					{"data", Base64(bytes)},
					{"byte_length", bytes.size()},
					{"hash_algorithm", "blake3-256"},
					{"hash", hash.ToHex()},
					{"tick", captured.Tick},
					{"coordinate_system", "right_handed_y_up"},
					{"units", "metres"},
					{"unavailable", std::move(unavailable)},
					{"inline_byte_limit", MAX_GLTF_EXPORT_REPLY_BYTES}
				};
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
			world::DataFactorySession *session = nullptr
		) {
			return Feature{"data_scene", [&universe, bridge = std::move(bridge), session](Surface &surface) {
							   surface.AddDataSceneTools(universe, bridge, session);
						   }};
		}
	}
}
