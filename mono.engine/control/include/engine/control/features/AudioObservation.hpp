#pragma once

// MCP handoff for copied audio_observation/v1 records. The product registers
// the L12 bridge; this L13 feature never reaches into a mixer or a device.
// @tier L13 · shared

#include <engine/control/DataFactoryReadFence.hpp>
#include <engine/control/Surface.hpp>
#include <engine/script/DataAudioObservationBridge.hpp>
#include <engine/script/DataSceneService.hpp>
#include <engine/world/Universe.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::control {
	using nlohmann::json;

	namespace audio_observation_detail {
		inline constexpr size_t MAXIMUM_READ_BYTES = script::MAX_AUDIO_OBSERVATION_JSON_BYTES;

		inline std::string Error(std::string_view code, std::string_view detail) {
			return std::string(code) + ": " + std::string(detail);
		}

		inline bool Text(
			const json &value, std::string_view field, size_t limit, std::string &out, std::string &failure
		) {
			if (!value.is_string()) {
				failure = Error("validation_failed", std::string(field) + " must be a string");
				return false;
			}
			out = value.get<std::string>();
			if (out.empty() || out.size() > limit || out.find('\0') != std::string::npos) {
				failure = Error("validation_failed", std::string(field) + " is outside its byte bound");
				return false;
			}
			return true;
		}

		inline bool UInt(const json &value, std::string_view field, uint64_t &out, std::string &failure) {
			if (!value.is_number_unsigned()) {
				failure = Error("validation_failed", std::string(field) + " must be an unsigned integer");
				return false;
			}
			out = value.get<uint64_t>();
			return true;
		}

		inline bool BridgeText(std::string_view value, bool nonempty = true) {
			return script::IsDataAudioObservationText(
				value, script::MAX_AUDIO_OBSERVATION_STRING_BYTES, nonempty
			);
		}

		inline bool Status(std::string_view value) {
			return value == "ok" || value == "unavailable" || value == "resource_limit" ||
				   value == "invalid_data";
		}

		inline bool FitsSurfaceContent(const json &value) {
			return value.dump(2).size() <= script::MAX_AUDIO_OBSERVATION_JSON_BYTES;
		}

		inline bool Only(
			const json &value,
			std::initializer_list<std::string_view> allowed,
			bool requireRevision,
			std::string &failure
		) {
			if (!value.is_object()) {
				failure = Error("validation_failed", "arguments must be an object");
				return false;
			}
			for (auto item = value.begin(); item != value.end(); ++item) {
				if (std::find(allowed.begin(), allowed.end(), std::string_view(item.key())) ==
						allowed.end() ||
					(!requireRevision && data_factory_read_fence::IsExpectedRevisionField(item.key()))) {
					failure = Error("validation_failed", "unknown argument: " + item.key());
					return false;
				}
			}
			return true;
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

		inline json Record(const script::DataAudioObservation &observation) {
			json chunks = json::array();
			for (const auto &chunk : observation.Waveform.Chunks)
				chunks.push_back(
					{{"sha256", chunk.Sha256}, {"byte_begin", chunk.ByteBegin}, {"byte_end", chunk.ByteEnd}}
				);
			json sources = json::array();
			for (const auto &source : observation.Sources)
				sources.push_back(
					{{"source_id", source.SourceId},
					 {"kind", source.Kind},
					 {"gain", source.Gain},
					 {"pan", source.Pan},
					 {"muted", source.Muted},
					 {"playing", source.Playing},
					 {"looping", source.Looping},
					 {"present", source.Present}}
				);
			json events = json::array();
			for (const auto &event : observation.Events)
				events.push_back(
					{{"event_id", event.EventId},
					 {"kind", event.Kind},
					 {"source_id", event.SourceId},
					 {"related_source_id",
					  event.RelatedSourceId.empty() ? json(nullptr) : json(event.RelatedSourceId)},
					 {"requested_sample", event.RequestedSample},
					 {"applied_sample", event.AppliedSample},
					 {"sample_offset_frames", event.SampleOffsetFrames},
					 {"timing", event.Timing},
					 {"finish_provenance",
					  event.FinishProvenance.empty() ? json(nullptr) : json(event.FinishProvenance)}}
				);
			json missing = json::array();
			for (const auto &item : observation.Missing)
				missing.push_back({{"field", item.Field}, {"reason", item.Reason}});
			return {
				{"schema", script::AUDIO_OBSERVATION_SCHEMA},
				{"observation_id", observation.ObservationId},
				{"world",
				 {{"tick", observation.World.Tick},
				  {"time_ns", observation.World.TimeNanoseconds},
				  {"epoch", observation.World.Epoch},
				  {"version", observation.World.Version},
				  {"tick_interval",
				   {{"numerator_ns", observation.World.TickInterval.NumeratorNanoseconds},
					{"denominator", observation.World.TickInterval.Denominator}}}}},
				{"sample_begin", observation.SampleBegin},
				{"sample_end", observation.SampleEnd},
				{"sample_rate_hz", observation.SampleRateHz},
				{"channels", observation.Channels},
				{"channel_layout", observation.ChannelLayout},
				{"sample_type", "float32"},
				{"interleaved", true},
				{"waveform",
				 {{"available", observation.Waveform.Available},
				  {"unavailable_reason",
				   observation.Waveform.Available ? json(nullptr)
												  : json(observation.Waveform.UnavailableReason)},
				  {"resource_id",
				   observation.Waveform.Available ? json(observation.Waveform.ResourceId) : json(nullptr)},
				  {"sha256",
				   observation.Waveform.Available ? json(observation.Waveform.Sha256) : json(nullptr)},
				  {"byte_length",
				   observation.Waveform.Available ? json(observation.Waveform.ByteLength) : json(nullptr)},
				  {"chunks", std::move(chunks)}}},
				{"sources", std::move(sources)},
				{"events", std::move(events)},
				{"missing", std::move(missing)}
			};
		}

		inline json Schema(world::DataFactorySession *session) {
			json properties{
				{"instance_id",
				 {{"type", "string"},
				  {"minLength", 1},
				  {"maxLength", script::MAX_AUDIO_OBSERVATION_ID_BYTES}}}
			};
			json required = json::array({"instance_id"});
			if (session != nullptr) {
				properties["expected_tick"] = {{"type", "integer"}, {"minimum", 0}};
				properties["expected_world_epoch"] = {{"type", "integer"}, {"minimum", 0}};
				properties["expected_world_version"] = {{"type", "integer"}, {"minimum", 0}};
				required.push_back("expected_tick");
				required.push_back("expected_world_epoch");
				required.push_back("expected_world_version");
			}
			return {
				{"type", "object"},
				{"additionalProperties", false},
				{"properties", std::move(properties)},
				{"required", std::move(required)}
			};
		}
	}

	// Deliberately a free installer until a product registers the feature in its
	// own surface list. That keeps this lower-layer slice independent of
	// Surface.hpp and of any product's audio ownership.
	inline void AddDataAudioObservationTools(
		Surface &surface,
		world::Universe &universe,
		std::shared_ptr<script::DataAudioObservationBridge> bridge,
		world::DataFactorySession *session = nullptr
	) {
		using namespace audio_observation_detail;
		if (!bridge) return;
		world::Universe *worlds = session != nullptr ? &session->UniverseOf() : &universe;
		surface.AddResource(
			Resource{
				"atomic://metadata/audio_observation/v1",
				"audio_observation/v1 metadata",
				"Metadata for the copied sample-aligned audio record. Waveform bytes stay in "
				"digest-addressed "
				"chunks fetched through get_audio_waveform_chunk.",
				"application/json",
				[](std::string &) {
					return json{
						{"schema", script::AUDIO_OBSERVATION_SCHEMA},
						{"contract", "datafactories-docs/audio_observation.py"},
						{"waveform", "external_digest_addressed_chunks"},
						{"sample_type", "float32"},
						{"interleaved", true},
						{"limits",
						 {{"json_bytes", script::MAX_AUDIO_OBSERVATION_JSON_BYTES},
						  {"string_bytes", script::MAX_AUDIO_OBSERVATION_STRING_BYTES},
						  {"id_bytes", script::MAX_AUDIO_OBSERVATION_ID_BYTES},
						  {"sources", script::MAX_AUDIO_OBSERVATION_SOURCES},
						  {"events", script::MAX_AUDIO_OBSERVATION_EVENTS},
						  {"chunks", script::MAX_AUDIO_OBSERVATION_CHUNKS},
						  {"missing", script::MAX_AUDIO_OBSERVATION_MISSING}}},
						{"waveform_handoff",
						 {{"tool", "get_audio_waveform_chunk"},
						  {"sha256", "lowercase_hex_64"},
						  {"range", "half_open_byte_begin_byte_end"},
						  {"lifetime", "host_owned_immutable_until_expired"}}}
					}.dump();
				},
			}
		);
		surface.Add(
			Tool{
				"get_audio_observation",
				"Returns one bounded audio_observation/v1 record. Waveform bytes remain external "
				"digest-addressed chunks.",
				[session] { return Schema(session); },
				[worlds, bridge, session](const json &arguments, std::string &failure) -> json {
					if (!Only(
							arguments,
							{"instance_id",
							 "expected_tick",
							 "expected_world_epoch",
							 "expected_world_version"},
							session != nullptr,
							failure
						))
						return nullptr;
					std::string instance;
					const auto field = arguments.find("instance_id");
					if (field == arguments.end()) {
						failure = Error("validation_failed", "instance_id is required");
						return nullptr;
					}
					if (!Text(
							*field, "instance_id", script::MAX_AUDIO_OBSERVATION_ID_BYTES, instance, failure
						))
						return nullptr;
					json fence;
					if (!data_factory_read_fence::Validate(session, instance, arguments, fence, failure))
						return fence;
					if (!worlds->Find(core::Name(instance)).IsValid()) {
						failure = Error("not_found", "no scene has that instance_id");
						return nullptr;
					}
					script::DataAudioObservationBridgeResult result;
					std::string detail;
					if (!bridge->Capture(instance, result, detail)) {
						if (!BridgeText(detail, false)) {
							failure =
								Error("invalid_bridge_reply", "capture bridge returned an invalid detail");
							return nullptr;
						}
						failure = Error(
							"audio_observation_unavailable",
							detail.empty() ? "capture bridge refused the request" : detail
						);
						return nullptr;
					}
					if (!Status(result.Status) || !BridgeText(result.Status) ||
						!BridgeText(result.Detail, false)) {
						failure = Error(
							"invalid_bridge_reply", "capture bridge returned an invalid status or detail"
						);
						return nullptr;
					}
					if (result.Status != "ok") return {{"status", result.Status}, {"detail", result.Detail}};
					if (!script::ValidateDataAudioObservation(result.Observation, detail)) {
						failure = Error("invalid_audio_observation", detail);
						return nullptr;
					}
					json record = Record(result.Observation);
					if (!FitsSurfaceContent(record)) {
						failure = Error("resource_limit", "audio observation exceeds its JSON byte bound");
						return nullptr;
					}
					return record;
				},
			}
		);
		surface.Add(
			Tool{
				"get_audio_waveform_chunk",
				"Copies a bounded byte range from one digest-addressed waveform chunk returned by "
				"get_audio_observation.",
				[session] {
					json properties{
						{"instance_id", {{"type", "string"}}},
						{"resource_id", {{"type", "string"}}},
						{"sha256", {{"type", "string"}, {"pattern", "^[0-9a-f]{64}$"}}},
						{"byte_begin", {{"type", "integer"}, {"minimum", 0}}},
						{"byte_end", {{"type", "integer"}, {"minimum", 1}}},
					};
					json required =
						json::array({"instance_id", "resource_id", "sha256", "byte_begin", "byte_end"});
					if (session != nullptr) {
						properties["expected_tick"] = {{"type", "integer"}, {"minimum", 0}};
						properties["expected_world_epoch"] = {{"type", "integer"}, {"minimum", 0}};
						properties["expected_world_version"] = {{"type", "integer"}, {"minimum", 0}};
						required.push_back("expected_tick");
						required.push_back("expected_world_epoch");
						required.push_back("expected_world_version");
					}
					return json{
						{"type", "object"},
						{"additionalProperties", false},
						{"properties", std::move(properties)},
						{"required", std::move(required)}
					};
				},
				[worlds, bridge, session](const json &arguments, std::string &failure) -> json {
					if (!Only(
							arguments,
							{"instance_id",
							 "resource_id",
							 "sha256",
							 "byte_begin",
							 "byte_end",
							 "expected_tick",
							 "expected_world_epoch",
							 "expected_world_version"},
							session != nullptr,
							failure
						))
						return nullptr;
					std::string instance, resource, digest;
					uint64_t begin = 0, end = 0;
					if (!Text(
							arguments.value("instance_id", json{}),
							"instance_id",
							script::MAX_AUDIO_OBSERVATION_ID_BYTES,
							instance,
							failure
						) ||
						!Text(
							arguments.value("resource_id", json{}),
							"resource_id",
							script::MAX_AUDIO_OBSERVATION_ID_BYTES,
							resource,
							failure
						) ||
						!Text(arguments.value("sha256", json{}), "sha256", 64, digest, failure) ||
						!UInt(arguments.value("byte_begin", json{}), "byte_begin", begin, failure) ||
						!UInt(arguments.value("byte_end", json{}), "byte_end", end, failure))
						return nullptr;
					json fence;
					if (!data_factory_read_fence::Validate(session, instance, arguments, fence, failure))
						return fence;
					if (!script::IsDataAudioObservationDigest(digest)) {
						failure =
							Error("validation_failed", "sha256 must be 64 lowercase hexadecimal characters");
						return nullptr;
					}
					if (end <= begin || end - begin > MAXIMUM_READ_BYTES ||
						!worlds->Find(core::Name(instance)).IsValid()) {
						failure = Error("validation_failed", "waveform range or instance_id is invalid");
						return nullptr;
					}
					std::vector<std::byte> bytes;
					std::string detail;
					if (!bridge->ReadWaveform(
							instance, resource, digest, begin, static_cast<size_t>(end - begin), bytes, detail
						) ||
						bytes.size() != end - begin) {
						if (!BridgeText(detail, false)) {
							failure =
								Error("invalid_bridge_reply", "waveform bridge returned an invalid detail");
							return nullptr;
						}
						failure = Error(
							"waveform_unavailable",
							detail.empty() ? "bridge did not return the requested chunk range" : detail
						);
						return nullptr;
					}
					json reply{
						{"resource_id", resource},
						{"sha256", digest},
						{"byte_begin", begin},
						{"byte_end", end},
						{"base64", Base64(bytes)}
					};
					if (!FitsSurfaceContent(reply)) {
						failure = Error("resource_limit", "waveform response exceeds its JSON byte bound");
						return nullptr;
					}
					return reply;
				},
			}
		);
	}

	namespace features {
		inline Feature DataAudioObservation(
			world::Universe &universe,
			std::shared_ptr<script::DataAudioObservationBridge> bridge,
			world::DataFactorySession *session = nullptr
		) {
			return Feature{
				"audio_observation", [&universe, bridge = std::move(bridge), session](Surface &surface) {
					AddDataAudioObservationTools(surface, universe, bridge, session);
				}
			};
		}
	}
}
