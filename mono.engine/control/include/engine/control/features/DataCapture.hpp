#pragma once

// The bounded MCP adapter for a host-installed script capture bridge.
// Capture bytes remain in the bridge until explicit release; this surface only
// serializes a caller-selected range as base64.

#include <engine/control/Surface.hpp>
#include <engine/script/DataCaptureBridge.hpp>
#include <engine/world/DataFactory.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine::control {

	using nlohmann::json;

	namespace data_capture_detail {
		inline constexpr size_t MAXIMUM_ID = 128;
		inline constexpr size_t MAXIMUM_CHANNELS = 3;
		inline constexpr size_t MAXIMUM_RANGE_BYTES = 1024 * 1024;
		inline constexpr size_t MAXIMUM_LEDGER_ENTRIES = 256;

		struct LedgerEntry {
			std::string Arguments;
			json Result;
			std::string Failure;
		};
		struct Ledger {
			std::unordered_map<std::string, LedgerEntry> Entries;
			std::deque<std::string> Order;
		};

		inline std::string Error(std::string_view code, std::string_view detail) {
			return std::string(code) + ": " + std::string(detail);
		}

		inline bool Text(const json &value, std::string_view name, std::string &out, std::string &failure) {
			if (!value.is_string()) {
				failure = Error("validation_failed", std::string(name) + " must be a string");
				return false;
			}
			out = value.get<std::string>();
			if (out.empty() || out.size() > MAXIMUM_ID || out.find('\0') != std::string::npos) {
				failure = Error("validation_failed", std::string(name) + " must contain 1 to 128 bytes");
				return false;
			}
			return true;
		}

		inline bool UInt(const json &value, std::string_view name, uint64_t &out, std::string &failure) {
			if (!value.is_number_unsigned()) {
				failure = Error("validation_failed", std::string(name) + " must be an unsigned integer");
				return false;
			}
			out = value.get<uint64_t>();
			return true;
		}

		inline bool Field(const json &values, std::string_view name, const json *&out, std::string &failure) {
			const auto found = values.find(std::string(name));
			if (found == values.end()) {
				failure = Error("validation_failed", std::string(name) + " is required");
				return false;
			}
			out = &*found;
			return true;
		}

		inline bool
		Only(const json &values, std::initializer_list<std::string_view> names, std::string &failure) {
			if (!values.is_object()) {
				failure = Error("validation_failed", "arguments must be an object");
				return false;
			}
			for (auto field = values.begin(); field != values.end(); ++field) {
				if (std::find(names.begin(), names.end(), std::string_view(field.key())) == names.end()) {
					failure = Error("validation_failed", "unknown argument: " + field.key());
					return false;
				}
			}
			return true;
		}

		inline bool Versions(
			world::DataFactorySession &session,
			std::string_view instance,
			const json &values,
			std::string &failure
		) {
			const json *field = nullptr;
			uint64_t tick = 0, epoch = 0, version = 0;
			if (!Field(values, "expected_tick", field, failure) ||
				!UInt(*field, "expected_tick", tick, failure) ||
				!Field(values, "expected_world_epoch", field, failure) ||
				!UInt(*field, "expected_world_epoch", epoch, failure) ||
				!Field(values, "expected_world_version", field, failure) ||
				!UInt(*field, "expected_world_version", version, failure))
				return false;
			const world::DataFactoryReply current = session.Inspect(instance);
			if (current.Status != world::DataFactoryStatus::Ok) {
				failure = Error(world::Describe(current.Status), current.Detail);
				return false;
			}
			if (current.Clock.Tick != tick || current.WorldEpoch != epoch ||
				current.WorldVersion != version) {
				failure = Error("version_conflict", "capture requires the current completed world version");
				return false;
			}
			return true;
		}

		inline json VersionReply(world::DataFactorySession &session, std::string_view instance) {
			const world::DataFactoryReply current = session.Inspect(instance);
			return {
				{"instance_id", std::string(instance)},
				{"current_tick", current.Clock.Tick},
				{"current_world_epoch", current.WorldEpoch},
				{"current_world_version", current.WorldVersion}
			};
		}

		inline void Store(
			Ledger &ledger, const std::string &id, std::string arguments, json result, std::string failure
		) {
			ledger.Order.push_back(id);
			ledger.Entries.emplace(
				id, LedgerEntry{std::move(arguments), std::move(result), std::move(failure)}
			);
			while (ledger.Order.size() > MAXIMUM_LEDGER_ENTRIES) {
				ledger.Entries.erase(ledger.Order.front());
				ledger.Order.pop_front();
			}
		}

		inline std::string Base64(std::span<const std::byte> bytes) {
			static constexpr std::array alphabet{'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K',
												 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V',
												 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f', 'g',
												 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r',
												 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '0', '1', '2',
												 '3', '4', '5', '6', '7', '8', '9', '+', '/'};
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

		inline json Plane(const script::DataCaptureBridgePlane &plane, std::string_view snapshot) {
			const uint64_t byteSize = static_cast<uint64_t>(plane.RowStride) * plane.Height;
			json shape{plane.Height, plane.Width};
			std::string dtype = plane.Scalar;
			json packing = nullptr;
			if (plane.Channel == "rgb_linear_hdr") {
				shape.push_back(4);
				dtype = "float16";
			} else if (plane.Channel == "linear_depth") {
				dtype = "float32";
			} else if (plane.Channel == "shading_normal") {
				dtype = "uint32";
				packing = "UNorm10A2";
			}
			return {
				{"channel", plane.Channel},
				{"status", plane.Status},
				{"snapshot_id", snapshot},
				{"resource_id", plane.Resource},
				{"source_resource", plane.SourceResource},
				{"byte_size", byteSize},
				{"digest", plane.Hash},
				{"hash_algorithm", plane.HashAlgorithm},
				{"shape", std::move(shape)},
				{"dtype", dtype},
				{"packing", std::move(packing)},
				{"row_stride", plane.RowStride},
				{"colour_space", plane.ColourSpace},
				{"origin", plane.Origin}
			};
		}

		inline json PollReply(uint64_t ticket, const script::DataCaptureBridgePoll &reply) {
			json planes = json::array();
			for (const auto &plane : reply.Planes)
				planes.push_back(Plane(plane, reply.SnapshotId));
			return {
				{"ticket", ticket},
				{"status", reply.Status},
				{"snapshot_id", reply.SnapshotId},
				{"capture_frame", reply.CaptureFrame},
				{"camera",
				 {{"available", reply.HasCamera},
				  {"world_from_camera", reply.WorldFromCamera},
				  {"projection_available", reply.HasProjection},
				  {"projection", reply.Projection},
				  {"vertical_field_of_view_radians", reply.VerticalFieldOfViewRadians},
				  {"near_metres", reply.NearMetres},
				  {"far_metres", reply.FarMetres},
				  {"coordinate_convention", reply.CoordinateConvention}}},
				{"planes", std::move(planes)}
			};
		}

		inline json Schema(json properties, json required) {
			return {
				{"type", "object"},
				{"additionalProperties", false},
				{"properties", std::move(properties)},
				{"required", std::move(required)}
			};
		}
	}

	inline void Surface::AddDataCaptureTools(
		world::DataFactorySession &session, std::shared_ptr<script::DataCaptureBridge> bridge
	) {
		using namespace data_capture_detail;
		if (!bridge) return;
		SetDataCaptureAvailabilityProvider([bridge] {
			const script::DataCaptureBridgeCapabilities capabilities = bridge->Capabilities();
			return DataCaptureAvailability{
				.Available = capabilities.Available,
				.Channels = capabilities.Channels,
				.Detail = capabilities.Detail,
			};
		});
		auto ledger = std::make_shared<Ledger>();

		Add(Tool{
			"capture",
			"Queues one calibrated render capture from a paused world's exact completed snapshot.",
			[] {
				return Schema(
					{{"instance_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
					 {"snapshot_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
					 {"pipeline", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
					 {"capture_node", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
					 {"view_slot", {{"type", "integer"}, {"minimum", 0}}},
					 {"channels",
					  {{"type", "array"},
					   {"minItems", 1},
					   {"maxItems", MAXIMUM_CHANNELS},
					   {"items", {{"type", "string"}}}}},
					 {"temporal_history", {{"type", "string"}, {"enum", {"preserve"}}}},
					 {"operation_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
					 {"expected_tick", {{"type", "integer"}, {"minimum", 0}}},
					 {"expected_world_epoch", {{"type", "integer"}, {"minimum", 0}}},
					 {"expected_world_version", {{"type", "integer"}, {"minimum", 0}}}},
					{"instance_id",
					 "snapshot_id",
					 "pipeline",
					 "capture_node",
					 "view_slot",
					 "channels",
					 "temporal_history",
					 "operation_id",
					 "expected_tick",
					 "expected_world_epoch",
					 "expected_world_version"}
				);
			},
			[&session, bridge, ledger](const json &values, std::string &failure) -> json {
				if (!Only(
						values,
						{"instance_id",
						 "snapshot_id",
						 "pipeline",
						 "capture_node",
						 "view_slot",
						 "channels",
						 "temporal_history",
						 "operation_id",
						 "expected_tick",
						 "expected_world_epoch",
						 "expected_world_version"},
						failure
					))
					return nullptr;
				const json *field = nullptr;
				script::DataCaptureBridgeRequest request;
				std::string operation;
				uint64_t slot = 0;
				if (!Field(values, "instance_id", field, failure) ||
					!Text(*field, "instance_id", request.InstanceId, failure) ||
					!Field(values, "snapshot_id", field, failure) ||
					!Text(*field, "snapshot_id", request.SnapshotId, failure) ||
					!Field(values, "pipeline", field, failure) ||
					!Text(*field, "pipeline", request.Pipeline, failure) ||
					!Field(values, "capture_node", field, failure) ||
					!Text(*field, "capture_node", request.CaptureNode, failure) ||
					!Field(values, "view_slot", field, failure) ||
					!UInt(*field, "view_slot", slot, failure) ||
					!Field(values, "temporal_history", field, failure) ||
					!Text(*field, "temporal_history", request.TemporalHistory, failure) ||
					!Field(values, "operation_id", field, failure) ||
					!Text(*field, "operation_id", operation, failure))
					return nullptr;
				request.ViewSlot = slot;
				if (request.TemporalHistory != "preserve") {
					failure =
						Error("capability_unsupported", "this host supports only preserve temporal history");
					return nullptr;
				}
				if (!Field(values, "channels", field, failure) || !field->is_array() || field->empty() ||
					field->size() > MAXIMUM_CHANNELS) {
					failure = Error("validation_failed", "channels must contain 1 to 3 names");
					return nullptr;
				}
				for (const json &channel : *field) {
					std::string name;
					if (!Text(channel, "channel", name, failure)) return nullptr;
					request.Channels.push_back(std::move(name));
				}
				json normalized = values;
				normalized["tool"] = "capture";
				const std::string normalizedText = normalized.dump();
				if (const auto prior = ledger->Entries.find(operation); prior != ledger->Entries.end()) {
					if (prior->second.Arguments != normalizedText) {
						failure = Error(
							"operation_id_conflict", "operation_id was already used with different arguments"
						);
						return nullptr;
					}
					failure = prior->second.Failure;
					return prior->second.Result;
				}
				if (!Versions(session, request.InstanceId, values, failure)) {
					json result = VersionReply(session, request.InstanceId);
					Store(*ledger, operation, normalizedText, result, failure);
					return result;
				}
				uint64_t ticket = 0;
				std::string detail;
				if (!bridge->Queue(request.InstanceId, request, ticket, detail)) {
					failure = Error("capture_refused", detail);
					json result = VersionReply(session, request.InstanceId);
					Store(*ledger, operation, normalizedText, result, failure);
					return result;
				}
				json result{
					{"status", "queued"},
					{"ticket", ticket},
					{"instance_id", request.InstanceId},
					{"snapshot_id", request.SnapshotId}
				};
				Store(*ledger, operation, normalizedText, result, {});
				return result;
			},
		});

		Add(Tool{
			"poll_capture",
			"Reads capture metadata without consuming its retained plane bytes.",
			[] {
				return Schema(
					{{"instance_id", {{"type", "string"}}},
					 {"ticket", {{"type", "integer"}, {"minimum", 1}}}},
					{"instance_id", "ticket"}
				);
			},
			[bridge](const json &values, std::string &failure) -> json {
				if (!Only(values, {"instance_id", "ticket"}, failure)) return nullptr;
				const json *field = nullptr;
				std::string instance;
				uint64_t ticket = 0;
				if (!Field(values, "instance_id", field, failure) ||
					!Text(*field, "instance_id", instance, failure) ||
					!Field(values, "ticket", field, failure) || !UInt(*field, "ticket", ticket, failure))
					return nullptr;
				if (ticket == 0) {
					failure = Error("validation_failed", "ticket must be positive");
					return nullptr;
				}
				script::DataCaptureBridgePoll reply;
				std::string detail;
				if (!bridge->Poll(instance, ticket, reply, detail)) {
					failure = Error("capture_not_found", detail);
					return nullptr;
				}
				return PollReply(ticket, reply);
			},
		});

		Add(Tool{
			"get_resource",
			"Returns up to one MiB of one retained capture resource as base64.",
			[] {
				return Schema(
					{{"id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
					 {"options", {{"type", "object"}}}},
					{"id", "options"}
				);
			},
			[bridge](const json &values, std::string &failure) -> json {
				if (!Only(values, {"id", "options"}, failure)) return nullptr;
				const json *field = nullptr;
				std::string resource;
				if (!Field(values, "id", field, failure) || !Text(*field, "id", resource, failure) ||
					!Field(values, "options", field, failure) || !field->is_object()) {
					if (failure.empty()) failure = Error("validation_failed", "options must be an object");
					return nullptr;
				}
				const json &options = *field;
				if (!Only(options, {"instance_id", "ticket", "offset", "max_bytes"}, failure)) return nullptr;
				std::string instance;
				uint64_t ticket = 0, offset = 0, maximum = 0;
				if (!Field(options, "instance_id", field, failure) ||
					!Text(*field, "instance_id", instance, failure) ||
					!Field(options, "ticket", field, failure) || !UInt(*field, "ticket", ticket, failure) ||
					!Field(options, "offset", field, failure) || !UInt(*field, "offset", offset, failure) ||
					!Field(options, "max_bytes", field, failure) ||
					!UInt(*field, "max_bytes", maximum, failure))
					return nullptr;
				if (ticket == 0 || maximum > MAXIMUM_RANGE_BYTES ||
					offset > std::numeric_limits<size_t>::max() ||
					maximum > std::numeric_limits<size_t>::max()) {
					failure = Error("validation_failed", "max_bytes must be at most 1048576");
					return nullptr;
				}
				std::vector<std::byte> bytes;
				std::string detail;
				if (!bridge->ReadPlane(
						instance,
						ticket,
						resource,
						static_cast<size_t>(offset),
						static_cast<size_t>(maximum),
						bytes,
						detail
					)) {
					failure = Error("resource_unavailable", detail);
					return nullptr;
				}
				return {
					{"id", resource},
					{"ticket", ticket},
					{"offset", offset},
					{"byte_size", bytes.size()},
					{"encoding", "base64"},
					{"data", Base64(bytes)}
				};
			},
		});

		auto action = [&session, bridge, ledger](const char *name, bool release) {
			return Tool{
				name,
				release ? "Releases one terminal capture and its retained bytes."
						: "Cancels one pending capture.",
				[] {
					return Schema(
						{{"instance_id", {{"type", "string"}}},
						 {"ticket", {{"type", "integer"}, {"minimum", 1}}},
						 {"operation_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}}},
						{"instance_id", "ticket", "operation_id"}
					);
				},
				[&session, bridge, ledger, name, release](const json &values, std::string &failure) -> json {
					if (!Only(values, {"instance_id", "ticket", "operation_id"}, failure)) return nullptr;
					const json *field = nullptr;
					std::string instance, operation;
					uint64_t ticket = 0;
					if (!Field(values, "instance_id", field, failure) ||
						!Text(*field, "instance_id", instance, failure) ||
						!Field(values, "ticket", field, failure) ||
						!UInt(*field, "ticket", ticket, failure) ||
						!Field(values, "operation_id", field, failure) ||
						!Text(*field, "operation_id", operation, failure))
						return nullptr;
					if (ticket == 0) {
						failure = Error("validation_failed", "ticket must be positive");
						return nullptr;
					}
					json normalized = values;
					normalized["tool"] = name;
					const std::string normalizedText = normalized.dump();
					if (const auto prior = ledger->Entries.find(operation); prior != ledger->Entries.end()) {
						if (prior->second.Arguments != normalizedText) {
							failure = Error(
								"operation_id_conflict",
								"operation_id was already used with different arguments"
							);
							return nullptr;
						}
						failure = prior->second.Failure;
						return prior->second.Result;
					}
					std::string detail;
					bool ok = true;
					if (release)
						ok = bridge->Release(instance, ticket, detail);
					else
						bridge->Cancel(instance, ticket);
					json result{
						{"status", ok ? (release ? "released" : "cancel_requested") : "refused"},
						{"ticket", ticket},
						{"instance_id", instance}
					};
					if (!ok) failure = Error("capture_refused", detail);
					Store(*ledger, operation, normalizedText, result, failure);
					return result;
				}
			};
		};
		Add(action("release_capture", true));
		Add(action("cancel_capture", false));
	}
}

namespace engine::control::features {
	inline Feature
	DataCapture(world::DataFactorySession &session, std::shared_ptr<script::DataCaptureBridge> bridge) {
		return Feature{"data_capture", [&session, bridge = std::move(bridge)](Surface &surface) {
						   surface.AddDataCaptureTools(session, bridge);
					   }};
	}
}
