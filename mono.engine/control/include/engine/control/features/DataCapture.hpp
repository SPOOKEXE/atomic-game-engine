#pragma once

// The bounded MCP adapter for a host-installed script capture bridge.
// Capture bytes remain in the bridge until explicit release; this surface only
// serializes a caller-selected range as base64.

#include <engine/control/DataFactoryOperationLedger.hpp>
#include <engine/control/Surface.hpp>
#include <engine/control/features/DataScene.hpp>
#include <engine/script/DataCaptureBridge.hpp>
#include <engine/world/DataFactory.hpp>

#include <algorithm>
#include <array>
#include <cmath>
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
		// Maximum UTF-8 byte length for instance, snapshot, and capture ticket identifiers.
		inline constexpr size_t MAXIMUM_ID = 128;
		// Maximum byte length for profile and pipeline names supplied in a capture request.
		inline constexpr size_t MAXIMUM_OPTION_TEXT = 256;
		// Maximum byte length for one requested render channel name.
		inline constexpr size_t MAXIMUM_CHANNEL_NAME = 64;
		// Largest channel set accepted for one capture ticket.
		inline constexpr size_t MAXIMUM_CHANNELS = 15;
		// Per-light contribution uses one graph response target per selected light.
		inline constexpr size_t MAXIMUM_LOCAL_LIGHT_IDS = 4;
		// ScriptDataCaptureBridge owns six retained ticket slots. The MCP bound must
		// match that admission limit rather than the separate hook-batch limit.
		inline constexpr size_t MAXIMUM_MULTICAMERA_VIEWS = 6;
		// Largest byte range copied from a retained plane in one MCP response.
		inline constexpr size_t MAXIMUM_RANGE_BYTES = 1024 * 1024;

		// Joins a machine-readable failure code and its bridge supplied detail.
		inline std::string Error(std::string_view code, std::string_view detail) {
			return std::string(code) + ": " + std::string(detail);
		}

		// Reads a nonempty, NUL-free identifier bounded by MAXIMUM_ID bytes.
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

		// Reads a nonempty, NUL-free option string using its field-specific byte limit.
		inline bool OptionText(
			const json &value, std::string_view name, size_t maximum, std::string &out, std::string &failure
		) {
			if (!value.is_string()) {
				failure = Error("validation_failed", std::string(name) + " must be a string");
				return false;
			}
			out = value.get<std::string>();
			if (out.empty() || out.size() > maximum || out.find('\0') != std::string::npos) {
				failure = Error(
					"validation_failed",
					std::string(name) + " must contain 1 to " + std::to_string(maximum) + " bytes"
				);
				return false;
			}
			return true;
		}

		// Reads an unsigned JSON number without narrowing it.
		inline bool UInt(const json &value, std::string_view name, uint64_t &out, std::string &failure);
		// Borrows a required member from the request object for later type validation.
		inline bool Field(const json &values, std::string_view name, const json *&out, std::string &failure);
		// Rejects fields outside the named wire request shape.
		inline bool
		Only(const json &values, std::initializer_list<std::string_view> names, std::string &failure);

		// Validates one camera's base profile and writes the bridge-owned capture request.
		inline bool CaptureBundleOptions(
			const json &options, script::DataCaptureBridgeRequest &request, std::string &failure
		) {
			if (!Only(
					options,
					{"schema_version",
					 "channels",
					 "camera_id",
					 "pipeline",
					 "capture_node",
					 "view_slot",
					 "temporal_history",
					 "storage_profile",
					 "output",
					 "include_scene_data",
					 "include_exact_masks",
					 "coordinate_space",
					 "noise_mode",
					 "noise_seed",
					 "noise_sigma",
					 "local_light_ids"},
					failure
				))
				return false;
			const json *field = nullptr;
			std::string schema, camera, history, storage, output, coordinate, noise;
			uint64_t slot = 0;
			uint64_t noiseSeed = 0;
			double noiseSigma = 0.0;
			if (!Field(options, "schema_version", field, failure) ||
				!OptionText(*field, "options.schema_version", MAXIMUM_OPTION_TEXT, schema, failure) ||
				!Field(options, "camera_id", field, failure) ||
				!OptionText(*field, "options.camera_id", MAXIMUM_OPTION_TEXT, camera, failure) ||
				!Field(options, "pipeline", field, failure) ||
				!OptionText(*field, "options.pipeline", MAXIMUM_OPTION_TEXT, request.Pipeline, failure) ||
				!Field(options, "capture_node", field, failure) ||
				!OptionText(
					*field, "options.capture_node", MAXIMUM_OPTION_TEXT, request.CaptureNode, failure
				) ||
				!Field(options, "view_slot", field, failure) ||
				!UInt(*field, "options.view_slot", slot, failure) ||
				!Field(options, "temporal_history", field, failure) ||
				!OptionText(*field, "options.temporal_history", MAXIMUM_OPTION_TEXT, history, failure) ||
				!Field(options, "storage_profile", field, failure) ||
				!OptionText(*field, "options.storage_profile", MAXIMUM_OPTION_TEXT, storage, failure) ||
				!Field(options, "output", field, failure) ||
				!OptionText(*field, "options.output", MAXIMUM_OPTION_TEXT, output, failure) ||
				!Field(options, "coordinate_space", field, failure) ||
				!OptionText(*field, "options.coordinate_space", MAXIMUM_OPTION_TEXT, coordinate, failure) ||
				!Field(options, "noise_mode", field, failure) ||
				!OptionText(*field, "options.noise_mode", MAXIMUM_OPTION_TEXT, noise, failure) ||
				!Field(options, "noise_seed", field, failure) ||
				!UInt(*field, "options.noise_seed", noiseSeed, failure) ||
				!Field(options, "noise_sigma", field, failure))
				return false;
			if (!field->is_number() || !std::isfinite(noiseSigma = field->get<double>()) ||
				noiseSigma < 0.0 || noiseSigma > 64.0) {
				failure =
					Error("validation_failed", "options.noise_sigma must be a finite number from 0 to 64");
				return false;
			}
			if (slot > std::numeric_limits<uint32_t>::max()) {
				failure = Error("validation_failed", "options.view_slot must fit uint32");
				return false;
			}
			if (schema != "data-scene-options/v1" || history != "preserve" ||
				(storage != "lossless" && storage != "training_compact") || output != "raw_planes" ||
				coordinate != "world_camera_image" || (noise != "none" && noise != "gaussian") ||
				(noise == "none" && (noiseSeed != 0 || noiseSigma != 0.0))) {
				failure = Error(
					"capability_unsupported", "this host supports only the data-scene-options/v1 base profile"
				);
				return false;
			}
			request.ViewSlot = slot;
			request.CameraId = std::move(camera);
			request.StorageProfile = std::move(storage);
			request.NoiseMode = std::move(noise);
			request.NoiseSeed = noiseSeed;
			request.NoiseSigma = noiseSigma;
			if (!Field(options, "include_scene_data", field, failure)) return false;
			if (!field->is_boolean()) {
				failure = Error("validation_failed", "options.include_scene_data must be a boolean");
				return false;
			}
			request.IncludeSceneData = options.at("include_scene_data").get<bool>();
			if (!Field(options, "include_exact_masks", field, failure)) return false;
			if (!field->is_boolean()) {
				failure = Error("validation_failed", "options.include_exact_masks must be a boolean");
				return false;
			}
			if (options.at("include_exact_masks").get<bool>()) {
				failure = Error("capability_unsupported", "bundle exact mask assembly is not implemented");
				return false;
			}
			if (!Field(options, "channels", field, failure) || !field->is_array() || field->empty() ||
				field->size() > MAXIMUM_CHANNELS) {
				failure = Error("validation_failed", "options.channels must contain 1 to 15 names");
				return false;
			}
			bool hasSecondSurfaceDepth = false;
			bool hasSecondSurfaceValidity = false;
			for (const json &channel : *field) {
				std::string name;
				if (!OptionText(channel, "options.channel", MAXIMUM_CHANNEL_NAME, name, failure))
					return false;
				if (std::find(request.Channels.begin(), request.Channels.end(), name) !=
					request.Channels.end()) {
					failure = Error("validation_failed", "options.channels must not contain duplicates");
					return false;
				}
				hasSecondSurfaceDepth = hasSecondSurfaceDepth || name == "second_surface_depth";
				hasSecondSurfaceValidity = hasSecondSurfaceValidity || name == "second_surface_validity";
				request.Channels.push_back(std::move(name));
			}
			if (hasSecondSurfaceDepth != hasSecondSurfaceValidity) {
				failure = Error(
					"validation_failed",
					"second_surface_depth and second_surface_validity must be requested together"
				);
				return false;
			}
			if (const auto localLights = options.find("local_light_ids"); localLights != options.end()) {
				if (!localLights->is_array() || localLights->empty() ||
					localLights->size() > MAXIMUM_LOCAL_LIGHT_IDS) {
					failure = Error("validation_failed", "local_light_ids must contain 1 to 4 ids");
					return false;
				}
				for (const json &light : *localLights) {
					std::string id;
					if (!OptionText(light, "local_light_ids", MAXIMUM_ID, id, failure)) return false;
					if (std::find(request.LocalLightIds.begin(), request.LocalLightIds.end(), id) !=
						request.LocalLightIds.end()) {
						failure = Error("validation_failed", "local_light_ids must be unique");
						return false;
					}
					request.LocalLightIds.push_back(std::move(id));
				}
			}
			const bool localLightChannel =
				std::find(request.Channels.begin(), request.Channels.end(), "local_light_contribution") !=
					request.Channels.end() ||
				std::find(
					request.Channels.begin(), request.Channels.end(), "local_light_shadow_visibility"
				) != request.Channels.end();
			if (localLightChannel != !request.LocalLightIds.empty()) {
				failure = Error(
					"validation_failed", "local_light_ids must accompany a local-light capture channel"
				);
				return false;
			}
			if (request.NoiseMode == "gaussian" &&
				std::find(request.Channels.begin(), request.Channels.end(), "rgb_linear_hdr") ==
					request.Channels.end()) {
				failure = Error("capability_unsupported", "gaussian noise requires rgb_linear_hdr");
				return false;
			}
			request.TemporalHistory = std::move(history);
			return true;
		}

		// Expands distinct camera and view-slot pairs into independently retainable capture requests.
		inline bool MultiCameraOptions(
			const json &options,
			const json &cameras,
			std::vector<script::DataCaptureBridgeRequest> &requests,
			std::string &failure
		) {
			if (!Only(
					options,
					{"schema_version",
					 "channels",
					 "pipeline",
					 "capture_node",
					 "temporal_history",
					 "storage_profile",
					 "output",
					 "include_scene_data",
					 "include_exact_masks",
					 "coordinate_space",
					 "noise_mode",
					 "noise_seed",
					 "noise_sigma",
					 "local_light_ids"},
					failure
				))
				return false;
			if (!cameras.is_array() || cameras.size() < 2 || cameras.size() > MAXIMUM_MULTICAMERA_VIEWS) {
				failure = Error("validation_failed", "cameras must contain 2 to 6 views");
				return false;
			}
			std::vector<std::string> cameraIds;
			requests.clear();
			requests.reserve(cameras.size());
			for (const json &camera : cameras) {
				if (!Only(camera, {"camera_id", "view_slot"}, failure)) return false;
				const json *field = nullptr;
				std::string cameraId;
				uint64_t viewSlot = 0;
				if (!Field(camera, "camera_id", field, failure) ||
					!OptionText(*field, "camera.camera_id", MAXIMUM_OPTION_TEXT, cameraId, failure) ||
					!Field(camera, "view_slot", field, failure) ||
					!UInt(*field, "camera.view_slot", viewSlot, failure) ||
					viewSlot > std::numeric_limits<uint32_t>::max()) {
					if (failure.empty())
						failure = Error("validation_failed", "camera.view_slot must fit uint32");
					return false;
				}
				if (std::find(cameraIds.begin(), cameraIds.end(), cameraId) != cameraIds.end()) {
					failure = Error("validation_failed", "cameras must use distinct camera_id values");
					return false;
				}
				json oneOptions = options;
				oneOptions["camera_id"] = cameraId;
				oneOptions["view_slot"] = viewSlot;
				script::DataCaptureBridgeRequest request;
				if (!CaptureBundleOptions(oneOptions, request, failure)) return false;
				cameraIds.push_back(std::move(cameraId));
				requests.push_back(std::move(request));
			}
			return true;
		}

		// Reads an unsigned JSON number without narrowing it.
		inline bool UInt(const json &value, std::string_view name, uint64_t &out, std::string &failure) {
			if (!value.is_number_unsigned()) {
				failure = Error("validation_failed", std::string(name) + " must be an unsigned integer");
				return false;
			}
			out = value.get<uint64_t>();
			return true;
		}

		// Borrows a required member from the request object for later type validation.
		inline bool Field(const json &values, std::string_view name, const json *&out, std::string &failure) {
			const auto found = values.find(std::string(name));
			if (found == values.end()) {
				failure = Error("validation_failed", std::string(name) + " is required");
				return false;
			}
			out = &*found;
			return true;
		}

		// Rejects fields outside the named wire request shape.
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

		// Ensures the requested capture observes the current completed tick, epoch, and world version.
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

		// Reports the lifecycle revision callers must echo before submitting a capture.
		inline json VersionReply(world::DataFactorySession &session, std::string_view instance) {
			const world::DataFactoryReply current = session.Inspect(instance);
			return {
				{"instance_id", std::string(instance)},
				{"current_tick", current.Clock.Tick},
				{"current_world_epoch", current.WorldEpoch},
				{"current_world_version", current.WorldVersion}
			};
		}

		// Encodes retained binary bytes for the JSON-only MCP transport.
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

		// Converts bridge plane metadata into the public record while retaining source provenance.
		inline json Plane(const script::DataCaptureBridgePlane &plane, std::string_view snapshot) {
			const uint64_t byteSize = plane.ByteSize;
			json shape{plane.Height, plane.Width};
			std::string dtype = plane.Scalar;
			json packing = plane.Packing.empty() ? json(nullptr) : json(plane.Packing);
			if (plane.Channel == "rgb_linear_hdr") {
				shape.push_back(4);
				dtype = "float16";
			} else if (plane.Packed || plane.Channel == "packed_gpu") {
				shape.push_back(4);
				dtype = "float32";
			} else if (plane.Channel == "shading_normal") {
				dtype = "uint32";
				packing = "UNorm10A2";
			} else if (plane.Channel == "mesh_uv" || plane.Channel == "motion_vectors") {
				shape.push_back(2);
				dtype = "float16";
			}
			const auto ambientOcclusion =
				[](const std::optional<script::DataCaptureBridgeAmbientOcclusion> &value) {
					if (!value) return json(nullptr);
					const auto nullable = [](const auto &field) -> json {
						return field ? json(*field) : json(nullptr);
					};
					return json{
						{"schema_version", "ssao-provenance/v1"},
						{"source_state", value->SourceState},
						{"producer_frame", nullable(value->ProducerFrame)},
						{"enabled", nullable(value->Enabled)},
						{"sample_count", nullable(value->SampleCount)},
						{"radius_world_units", nullable(value->RadiusWorldUnits)},
						{"denoiser", nullable(value->Denoiser)},
						{"temporal_history", nullable(value->TemporalHistory)},
						{"background_value", nullable(value->BackgroundValue)},
						{"background_classification", nullable(value->BackgroundClassification)}
					};
				};
			const auto noise = [](const std::optional<script::DataCaptureBridgeNoise> &value) -> json {
				if (!value) return nullptr;
				return {
					{"mode", value->Mode},
					{"algorithm", value->Algorithm},
					{"seed", value->Seed},
					{"sigma", value->Sigma},
					{"sigma_quantization", value->SigmaQuantization},
					{"effective_sigma_q24", value->EffectiveSigmaQ24},
					{"effective_sigma", value->EffectiveSigma},
					{"seed_state_policy", value->SeedStatePolicy},
					{"order", value->Order},
					{"clamp_policy", value->ClampPolicy},
					{"alpha_policy", value->AlphaPolicy},
					{"value_classification", value->ValueClassification},
					{"maximum_absolute_error",
					 value->MaximumAbsoluteError ? json(*value->MaximumAbsoluteError) : json(nullptr)},
				};
			};
			json packed = nullptr;
			if (plane.Packed) {
				packed = {
					{"schema_version", "data-capture-packed-rgba32f/v1"}, {"components", json::array()}
				};
				for (const auto &component : plane.Packed->Components)
					packed["components"].push_back(
						{{"source_channel", component.SourceChannel},
						 {"source_component", component.SourceComponent}}
					);
			}
			return {
				{"channel", plane.Channel},
				{"light_id", plane.LightId.empty() ? json(nullptr) : json(plane.LightId)},
				{"status", plane.Status},
				{"snapshot_id", snapshot},
				{"resource_id", plane.Resource},
				{"source_resource", plane.SourceResource},
				{"byte_size", byteSize},
				{"digest", plane.Hash},
				{"hash_algorithm", plane.HashAlgorithm},
				{"shape", std::move(shape)},
				{"dtype", dtype},
				{"source_dtype", plane.SourceScalar},
				{"source_hash", plane.SourceHash},
				{"source_width", plane.SourceWidth},
				{"source_height", plane.SourceHeight},
				{"source_row_stride", plane.SourceRowStride},
				{"source_byte_size", plane.SourceByteSize},
				{"source_encoding", plane.SourceEncoding},
				{"source_color_space", plane.SourceColourSpace},
				{"source_origin", plane.SourceOrigin},
				{"source_packing", plane.SourcePacking},
				{"source_provenance", plane.SourceProvenance},
				{"value_classification", plane.ValueClassification},
				{"encoding", plane.Encoding},
				{"maximum_absolute_error",
				 plane.MaximumAbsoluteError ? json(*plane.MaximumAbsoluteError) : json(nullptr)},
				{"packing", std::move(packing)},
				{"provenance", plane.Provenance.empty() ? json(nullptr) : json(plane.Provenance)},
				{"ambient_occlusion", ambientOcclusion(plane.AmbientOcclusion)},
				{"noise", noise(plane.Noise)},
				{"packed", std::move(packed)},
				{"resampling", plane.Resampling.empty() ? json(nullptr) : json(plane.Resampling)},
				{"previous_camera_motion_frame",
				 plane.PreviousCameraMotionFrame ? json(*plane.PreviousCameraMotionFrame) : json(nullptr)},
				{"row_stride", plane.RowStride},
				{"colour_space", plane.ColourSpace},
				{"origin", plane.Origin}
			};
		}

		// Serializes a ticket poll, including only scene sidecars that fit the response budget.
		inline json PollReply(uint64_t ticket, const script::DataCaptureBridgePoll &reply) {
			json planes = json::array();
			for (const auto &plane : reply.Planes)
				planes.push_back(Plane(plane, reply.SnapshotId));
			json sceneSidecar = nullptr;
			if (reply.SceneSidecar) {
				json scene;
				size_t budgetBytes = 0;
				size_t responseBytes = 0;
				if (script::DataSceneJsonResponseBudget(reply.SceneSidecar->Scene, budgetBytes) &&
					data_scene_detail::JsonValue(reply.SceneSidecar->Scene, scene, 0, responseBytes) &&
					scene.dump().size() <= data_scene_detail::MAXIMUM_RESULT_BYTES) {
					sceneSidecar = {
						{"schema_version", "data-capture-scene-sidecar/v1"},
						{"snapshot_id", reply.SceneSidecar->SnapshotId},
						{"lifecycle",
						 {{"tick", reply.SceneSidecar->Tick},
						  {"world_epoch", reply.SceneSidecar->WorldEpoch},
						  {"world_version", reply.SceneSidecar->WorldVersion}}},
						{"storage_profile", reply.SceneSidecar->StorageProfile},
						{"scene", std::move(scene)},
					};
				}
			}
			return {
				{"ticket", ticket},
				{"status", reply.Status},
				{"snapshot_id", reply.SnapshotId},
				{"capture_frame", reply.CaptureFrame},
				{"storage_profile", reply.StorageProfile},
				{"profile",
				 {{"source_bytes", reply.Profile.SourceBytes},
				  {"retained_bytes", reply.Profile.RetainedBytes},
				  {"readback_bytes", reply.Profile.ReadbackBytes},
				  {"transfer_bytes", reply.Profile.TransferBytes},
				  {"source_operations", reply.Profile.SourceOperations},
				  {"retained_operations", reply.Profile.RetainedOperations},
				  {"readback_operations", reply.Profile.ReadbackOperations},
				  {"transfer_operations", reply.Profile.TransferOperations},
				  {"cpu_readback_nanoseconds",
				   reply.Profile.CpuReadbackNanoseconds ? json(*reply.Profile.CpuReadbackNanoseconds)
														: json(nullptr)},
				  {"cpu_finalization_nanoseconds",
				   reply.Profile.CpuFinalizationNanoseconds ? json(*reply.Profile.CpuFinalizationNanoseconds)
															: json(nullptr)},
				  {"finalization_bytes_per_second",
				   reply.Profile.FinalizationBytesPerSecond ? json(*reply.Profile.FinalizationBytesPerSecond)
															: json(nullptr)},
				  {"gpu_nanoseconds",
				   reply.Profile.GpuNanoseconds ? json(*reply.Profile.GpuNanoseconds) : json(nullptr)},
				  {"gpu_timing_reason", reply.Profile.GpuTimingReason},
				  {"host_readback_reserved_capacity_bytes",
				   reply.Profile.HostReadbackReservedCapacityBytes
					   ? json(*reply.Profile.HostReadbackReservedCapacityBytes)
					   : json(nullptr)},
				  {"device_readback_staging_reserved_capacity_bytes",
				   reply.Profile.DeviceReadbackStagingReservedCapacityBytes
					   ? json(*reply.Profile.DeviceReadbackStagingReservedCapacityBytes)
					   : json(nullptr)},
				  {"allocation_bytes",
				   reply.Profile.AllocationBytes ? json(*reply.Profile.AllocationBytes) : json(nullptr)},
				  {"peak_allocation_bytes",
				   reply.Profile.PeakAllocationBytes ? json(*reply.Profile.PeakAllocationBytes)
													 : json(nullptr)},
				  {"allocation_reason", reply.Profile.AllocationReason}}},
				{"camera",
				 {{"available", reply.HasCamera},
				  {"world_from_camera", reply.WorldFromCamera},
				  {"projection_available", reply.HasProjection},
				  {"projection", reply.Projection},
				  {"vertical_field_of_view_radians", reply.VerticalFieldOfViewRadians},
				  {"near_metres", reply.NearMetres},
				  {"far_metres", reply.FarMetres},
				  {"crop", {reply.CropLeft, reply.CropTop, reply.CropWidth, reply.CropHeight}},
				  {"crop_convention", reply.CropConvention},
				  {"lens_distortion_available", reply.LensDistortionAvailable},
				  {"lens_distortion_reason", reply.LensDistortionReason},
				  {"jitter_available", reply.JitterAvailable},
				  {"jitter_policy", reply.JitterPolicy},
				  {"coordinate_convention", reply.CoordinateConvention}}},
				{"planes", std::move(planes)},
				{"object_labels",
				 [&] {
					 json labels = json::array();
					 for (const auto &label : reply.ObjectLabels)
						 labels.push_back({{"label", label.Label}, {"stable_id", label.StableId}});
					 return labels;
				 }()},
				{"semantic_labels",
				 [&] {
					 json labels = json::array();
					 for (const auto &label : reply.SemanticLabels)
						 labels.push_back({{"label", label.Label}, {"stable_id", label.StableId}});
					 return labels;
				 }()},
				{"part_labels",
				 [&] {
					 json labels = json::array();
					 for (const auto &label : reply.PartLabels)
						 labels.push_back({{"label", label.Label}, {"stable_id", label.StableId}});
					 return labels;
				 }()},
				{"scene_sidecar", std::move(sceneSidecar)}
			};
		}

		// Describes one explicit four-lane RGBA32F capture output for tool discovery.
		inline json PackedPlaneSchema() {
			json component{{"type", "object"}, {"additionalProperties", false}};
			component["required"] = {"source_channel", "source_component"};
			component["properties"] = {
				{"source_channel",
				 {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_CHANNEL_NAME}}},
				{"source_component", {{"type", "integer"}, {"minimum", 0}, {"maximum", 3}}},
			};
			json result{{"type", "object"}, {"additionalProperties", false}};
			result["required"] = {"name", "components"};
			result["properties"] = {
				{"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_CHANNEL_NAME}}},
				{"components",
				 {{"type", "array"}, {"minItems", 4}, {"maxItems", 4}, {"items", std::move(component)}}},
			};
			return result;
		}

		// Wraps tool-specific fields in the closed JSON object schema used by this surface.
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
		auto ledger = DataFactoryOperations();

		Add(Tool{
			"capture",
			"Queues one calibrated render capture from a paused world's exact completed snapshot.",
			[] {
				return Schema(
					{{"instance_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
					 {"snapshot_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
					 {"pipeline", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
					 {"capture_node", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
					 {"camera_id",
					  {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_OPTION_TEXT}}},
					 {"view_slot", {{"type", "integer"}, {"minimum", 0}}},
					 {"channels",
					  {{"type", "array"},
					   {"minItems", 1},
					   {"maxItems", MAXIMUM_CHANNELS},
					   {"items", {{"type", "string"}}}}},
					 {"local_light_ids",
					  {{"type", "array"},
					   {"minItems", 1},
					   {"maxItems", MAXIMUM_LOCAL_LIGHT_IDS},
					   {"items", {{"type", "string"}}}}},
					 {"packed_planes",
					  {{"type", "array"},
					   {"maxItems", script::MAX_DATA_CAPTURE_PACKED_PLANES},
					   {"items", PackedPlaneSchema()}}},
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
						 "camera_id",
						 "view_slot",
						 "channels",
						 "local_light_ids",
						 "packed_planes",
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
				if (const auto camera = values.find("camera_id"); camera != values.end()) {
					if (!OptionText(*camera, "camera_id", MAXIMUM_OPTION_TEXT, request.CameraId, failure))
						return nullptr;
					if (request.CameraId != "current_view" && !bridge->Capabilities().NamedCameraSelection) {
						failure = Error("capability_unsupported", "named camera selection is unavailable");
						return nullptr;
					}
				}
				if (request.TemporalHistory != "preserve") {
					failure =
						Error("capability_unsupported", "this host supports only preserve temporal history");
					return nullptr;
				}
				if (!Field(values, "channels", field, failure) || !field->is_array() || field->empty() ||
					field->size() > MAXIMUM_CHANNELS) {
					failure = Error("validation_failed", "channels must contain 1 to 15 names");
					return nullptr;
				}
				for (const json &channel : *field) {
					std::string name;
					if (!Text(channel, "channel", name, failure)) return nullptr;
					request.Channels.push_back(std::move(name));
				}
				if (const auto localLights = values.find("local_light_ids"); localLights != values.end()) {
					if (!localLights->is_array() || localLights->empty() ||
						localLights->size() > MAXIMUM_LOCAL_LIGHT_IDS) {
						failure = Error("validation_failed", "local_light_ids must contain 1 to 4 ids");
						return nullptr;
					}
					for (const json &light : *localLights) {
						std::string id;
						if (!Text(light, "local_light_ids", id, failure)) return nullptr;
						if (std::find(request.LocalLightIds.begin(), request.LocalLightIds.end(), id) !=
							request.LocalLightIds.end()) {
							failure = Error("validation_failed", "local_light_ids must be unique");
							return nullptr;
						}
						request.LocalLightIds.push_back(std::move(id));
					}
				}
				const bool localLightChannel =
					std::find(request.Channels.begin(), request.Channels.end(), "local_light_contribution") !=
						request.Channels.end() ||
					std::find(
						request.Channels.begin(), request.Channels.end(), "local_light_shadow_visibility"
					) != request.Channels.end();
				if (localLightChannel != !request.LocalLightIds.empty()) {
					failure = Error(
						"validation_failed", "local_light_ids must accompany a local-light capture channel"
					);
					return nullptr;
				}
				if (const auto packed = values.find("packed_planes"); packed != values.end()) {
					if (!packed->is_array() || packed->size() > script::MAX_DATA_CAPTURE_PACKED_PLANES) {
						failure =
							Error("validation_failed", "packed_planes must contain at most three outputs");
						return nullptr;
					}
					for (const json &entry : *packed) {
						if (!entry.is_object() || !Only(entry, {"name", "components"}, failure))
							return nullptr;
						const json *name = nullptr;
						const json *components = nullptr;
						if (!Field(entry, "name", name, failure) ||
							!Field(entry, "components", components, failure) || !components->is_array() ||
							components->size() != 4) {
							failure = Error("validation_failed", "packed plane requires four components");
							return nullptr;
						}
						script::DataCaptureBridgePackedPlane definition;
						if (!OptionText(
								*name, "packed_planes.name", MAXIMUM_CHANNEL_NAME, definition.Name, failure
							))
							return nullptr;
						for (size_t lane = 0; lane < definition.Components.size(); ++lane) {
							const json &component = (*components)[lane];
							const json *source = nullptr;
							const json *sourceComponent = nullptr;
							uint64_t index = 0;
							if (!component.is_object() ||
								!Only(component, {"source_channel", "source_component"}, failure) ||
								!Field(component, "source_channel", source, failure) ||
								!OptionText(
									*source,
									"packed_planes.source_channel",
									MAXIMUM_CHANNEL_NAME,
									definition.Components[lane].SourceChannel,
									failure
								) ||
								!Field(component, "source_component", sourceComponent, failure) ||
								!UInt(*sourceComponent, "packed_planes.source_component", index, failure) ||
								index > 3) {
								failure =
									Error("validation_failed", "packed source component must be 0 through 3");
								return nullptr;
							}
							definition.Components[lane].SourceComponent = static_cast<uint8_t>(index);
						}
						request.PackedPlanes.push_back(std::move(definition));
					}
				}
				json normalized = values;
				normalized["tool"] = "capture";
				const std::string normalizedText = normalized.dump();
				json replay;
				const auto prior = ledger->Replay("capture", operation, normalizedText, replay, failure);
				if (prior == DataFactoryOperationReplay::Conflict) return nullptr;
				if (prior == DataFactoryOperationReplay::Replay) return replay;
				if (!Versions(session, request.InstanceId, values, failure)) {
					json result = VersionReply(session, request.InstanceId);
					ledger->Store("capture", operation, normalizedText, result, failure);
					return result;
				}
				if (request.CameraId != "current_view" && !bridge->Capabilities().NamedCameraSelection) {
					failure = Error("capability_unsupported", "named camera selection is unavailable");
					json result = VersionReply(session, request.InstanceId);
					ledger->Store("capture", operation, normalizedText, result, failure);
					return result;
				}
				uint64_t ticket = 0;
				std::string detail;
				if (!bridge->Queue(request.InstanceId, request, ticket, detail)) {
					failure = Error("capture_refused", detail);
					json result = VersionReply(session, request.InstanceId);
					ledger->Store("capture", operation, normalizedText, result, failure);
					return result;
				}
				json result{
					{"status", "queued"},
					{"ticket", ticket},
					{"instance_id", request.InstanceId},
					{"snapshot_id", request.SnapshotId}
				};
				ledger->Store("capture", operation, normalizedText, result, {});
				return result;
			},
		});

		Add(Tool{
			"capture_bundle",
			"Queues one data-scene-options/v1 bundle from a paused world's exact completed snapshot.",
			[] {
				const json optionProperties{
					{"schema_version",
					 {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_OPTION_TEXT}}},
					{"channels",
					 {{"type", "array"},
					  {"minItems", 1},
					  {"maxItems", MAXIMUM_CHANNELS},
					  {"items",
					   {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_CHANNEL_NAME}}}}},
					{"local_light_ids",
					 {{"type", "array"},
					  {"minItems", 1},
					  {"maxItems", MAXIMUM_LOCAL_LIGHT_IDS},
					  {"items", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}}}},
					{"camera_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_OPTION_TEXT}}},
					{"pipeline", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_OPTION_TEXT}}},
					{"capture_node",
					 {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_OPTION_TEXT}}},
					{"view_slot",
					 {{"type", "integer"},
					  {"minimum", 0},
					  {"maximum", std::numeric_limits<uint32_t>::max()}}},
					{"temporal_history",
					 {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_OPTION_TEXT}}},
					{"storage_profile",
					 {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_OPTION_TEXT}}},
					{"output", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_OPTION_TEXT}}},
					{"include_scene_data", {{"type", "boolean"}}},
					{"include_exact_masks", {{"type", "boolean"}}},
					{"coordinate_space",
					 {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_OPTION_TEXT}}},
					{"noise_mode",
					 {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_OPTION_TEXT}}},
					{"noise_seed", {{"type", "integer"}, {"minimum", 0}}},
					{"noise_sigma", {{"type", "number"}, {"minimum", 0}, {"maximum", 64}}},
				};
				return Schema(
					{{"instance_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
					 {"snapshot_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
					 {"options",
					  {{"type", "object"},
					   {"properties", optionProperties},
					   {"required",
						json::array(
							{"schema_version",
							 "channels",
							 "camera_id",
							 "pipeline",
							 "capture_node",
							 "view_slot",
							 "temporal_history",
							 "storage_profile",
							 "output",
							 "include_scene_data",
							 "include_exact_masks",
							 "coordinate_space",
							 "noise_mode",
							 "noise_seed",
							 "noise_sigma"}
						)},
					   {"additionalProperties", false}}},
					 {"operation_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
					 {"expected_tick", {{"type", "integer"}, {"minimum", 0}}},
					 {"expected_world_epoch", {{"type", "integer"}, {"minimum", 0}}},
					 {"expected_world_version", {{"type", "integer"}, {"minimum", 0}}}},
					{"instance_id",
					 "snapshot_id",
					 "options",
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
						 "options",
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
				if (!Field(values, "instance_id", field, failure) ||
					!Text(*field, "instance_id", request.InstanceId, failure) ||
					!Field(values, "snapshot_id", field, failure) ||
					!Text(*field, "snapshot_id", request.SnapshotId, failure) ||
					!Field(values, "operation_id", field, failure) ||
					!Text(*field, "operation_id", operation, failure) ||
					!Field(values, "options", field, failure) ||
					!CaptureBundleOptions(*field, request, failure))
					return nullptr;
				json normalized = values;
				normalized["tool"] = "capture_bundle";
				const std::string normalizedText = normalized.dump();
				json replay;
				const auto prior =
					ledger->Replay("capture_bundle", operation, normalizedText, replay, failure);
				if (prior == DataFactoryOperationReplay::Conflict) return nullptr;
				if (prior == DataFactoryOperationReplay::Replay) return replay;
				if (!Versions(session, request.InstanceId, values, failure)) {
					json result = VersionReply(session, request.InstanceId);
					ledger->Store("capture_bundle", operation, normalizedText, result, failure);
					return result;
				}
				if (request.CameraId != "current_view" && !bridge->Capabilities().NamedCameraSelection) {
					failure = Error("capability_unsupported", "named camera selection is unavailable");
					json result = VersionReply(session, request.InstanceId);
					ledger->Store("capture_bundle", operation, normalizedText, result, failure);
					return result;
				}
				uint64_t ticket = 0;
				std::string detail;
				if (!bridge->Queue(request.InstanceId, request, ticket, detail)) {
					failure = Error("capture_refused", detail);
					json result = VersionReply(session, request.InstanceId);
					ledger->Store("capture_bundle", operation, normalizedText, result, failure);
					return result;
				}
				json result{
					{"status", "queued"},
					{"ticket", ticket},
					{"instance_id", request.InstanceId},
					{"snapshot_id", request.SnapshotId},
				};
				ledger->Store("capture_bundle", operation, normalizedText, result, {});
				return result;
			},
		});

		Add(Tool{
			"capture_multi_camera",
			"Queues 2 to 6 named camera captures in one renderer frame from one exact paused world "
			"snapshot. The shared renderer frame does not make simulation stepping atomic.",
			[] {
				const json optionProperties{
					{"schema_version", {{"type", "string"}}},
					{"channels", {{"type", "array"}, {"minItems", 1}, {"maxItems", MAXIMUM_CHANNELS}}},
					{"pipeline", {{"type", "string"}}},
					{"capture_node", {{"type", "string"}}},
					{"temporal_history", {{"type", "string"}}},
					{"storage_profile", {{"type", "string"}}},
					{"output", {{"type", "string"}}},
					{"include_scene_data", {{"type", "boolean"}}},
					{"include_exact_masks", {{"type", "boolean"}}},
					{"coordinate_space", {{"type", "string"}}},
					{"noise_mode", {{"type", "string"}}},
					{"noise_seed", {{"type", "integer"}, {"minimum", 0}}},
					{"noise_sigma", {{"type", "number"}, {"minimum", 0}, {"maximum", 64}}},
				};
				return Schema(
					{{"instance_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
					 {"snapshot_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
					 {"cameras",
					  {{"type", "array"},
					   {"minItems", 2},
					   {"maxItems", MAXIMUM_MULTICAMERA_VIEWS},
					   {"items", {{"type", "object"}}}}},
					 {"options",
					  {{"type", "object"},
					   {"properties", optionProperties},
					   {"additionalProperties", false}}},
					 {"operation_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
					 {"expected_tick", {{"type", "integer"}, {"minimum", 0}}},
					 {"expected_world_epoch", {{"type", "integer"}, {"minimum", 0}}},
					 {"expected_world_version", {{"type", "integer"}, {"minimum", 0}}}},
					{"instance_id",
					 "snapshot_id",
					 "cameras",
					 "options",
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
						 "cameras",
						 "options",
						 "operation_id",
						 "expected_tick",
						 "expected_world_epoch",
						 "expected_world_version"},
						failure
					))
					return nullptr;
				const json *field = nullptr;
				std::string instance, snapshot, operation;
				std::vector<script::DataCaptureBridgeRequest> requests;
				if (!Field(values, "instance_id", field, failure) ||
					!Text(*field, "instance_id", instance, failure) ||
					!Field(values, "snapshot_id", field, failure) ||
					!Text(*field, "snapshot_id", snapshot, failure) ||
					!Field(values, "operation_id", field, failure) ||
					!Text(*field, "operation_id", operation, failure) ||
					!Field(values, "options", field, failure) || !Field(values, "cameras", field, failure))
					return nullptr;
				const json &cameras = *field;
				const json &options = values.at("options");
				if (!MultiCameraOptions(options, cameras, requests, failure)) return nullptr;
				for (script::DataCaptureBridgeRequest &request : requests) {
					request.InstanceId = instance;
					request.SnapshotId = snapshot;
				}
				json normalized = values;
				normalized["tool"] = "capture_multi_camera";
				const std::string normalizedText = normalized.dump();
				json replay;
				const auto prior =
					ledger->Replay("capture_multi_camera", operation, normalizedText, replay, failure);
				if (prior == DataFactoryOperationReplay::Conflict) return nullptr;
				if (prior == DataFactoryOperationReplay::Replay) return replay;
				if (!Versions(session, instance, values, failure)) {
					json result = VersionReply(session, instance);
					ledger->Store("capture_multi_camera", operation, normalizedText, result, failure);
					return result;
				}
				const script::DataCaptureBridgeCapabilities capabilities = bridge->Capabilities();
				if (!capabilities.SameFrameMultiCamera) {
					failure =
						Error("capability_unsupported", "same-frame multi-camera capture is unavailable");
					json result = VersionReply(session, instance);
					ledger->Store("capture_multi_camera", operation, normalizedText, result, failure);
					return result;
				}
				if (requests.size() > capabilities.MaximumCaptureTickets ||
					requests.size() > capabilities.MaximumSameFrameCameraViews) {
					failure = Error(
						"capability_unsupported", "requested camera count exceeds same-frame camera capacity"
					);
					json result = VersionReply(session, instance);
					ledger->Store("capture_multi_camera", operation, normalizedText, result, failure);
					return result;
				}
				for (const script::DataCaptureBridgeRequest &request : requests) {
					if (request.CameraId != "current_view" && !capabilities.NamedCameraSelection) {
						failure = Error("capability_unsupported", "named camera selection is unavailable");
						json result = VersionReply(session, instance);
						ledger->Store("capture_multi_camera", operation, normalizedText, result, failure);
						return result;
					}
				}
				std::vector<uint64_t> tickets(requests.size());
				std::string detail;
				if (!bridge->QueueGroup(instance, requests, tickets, detail)) {
					failure = Error(
						"capture_refused", detail.empty() ? "capture bridge refused camera group" : detail
					);
					json result = VersionReply(session, instance);
					ledger->Store("capture_multi_camera", operation, normalizedText, result, failure);
					return result;
				}
				json captures = json::array();
				for (size_t index = 0; index < requests.size(); ++index) {
					const uint64_t ticket = tickets[index];
					if (ticket == 0 || std::find(tickets.begin(), tickets.begin() + index, ticket) !=
										   tickets.begin() + index) {
						for (const uint64_t queued : tickets)
							if (queued != 0) bridge->Cancel(instance, queued);
						failure = Error("capture_refused", "capture bridge returned an invalid ticket");
						json result = VersionReply(session, instance);
						ledger->Store("capture_multi_camera", operation, normalizedText, result, failure);
						return result;
					}
					const script::DataCaptureBridgeRequest &request = requests[index];
					captures.push_back(
						{{"ticket", ticket}, {"camera_id", request.CameraId}, {"view_slot", request.ViewSlot}}
					);
				}
				json result{
					{"status", "coordinated"},
					{"atomic", false},
					{"atomicity", "same_renderer_frame_not_simulation_atomic"},
					{"same_renderer_frame", true},
					{"instance_id", instance},
					{"snapshot_id", snapshot},
					{"captures", std::move(captures)},
				};
				ledger->Store("capture_multi_camera", operation, normalizedText, result, {});
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
			"submit_view_camera_mutation",
			"Queues one owned synchronous view.camera patch for an exact paused snapshot and pipeline "
			"revision.",
			[] {
				return Schema(
					{{"instance_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
					 {"snapshot_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
					 {"pipeline", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
					 {"pipeline_revision", {{"type", "integer"}, {"minimum", 1}}},
					 {"view_slot", {{"type", "integer"}, {"minimum", 0}}},
					 {"camera_frame", {{"type", "array"}, {"minItems", 7}, {"maxItems", 7}}},
					 {"camera", {{"type", "object"}}},
					 {"projection", {{"type", "array"}, {"minItems", 16}, {"maxItems", 16}}}},
					{"instance_id", "snapshot_id", "pipeline", "pipeline_revision", "view_slot"}
				);
			},
			[bridge](const json &values, std::string &failure) -> json {
				if (!Only(
						values,
						{"instance_id",
						 "snapshot_id",
						 "pipeline",
						 "pipeline_revision",
						 "view_slot",
						 "camera_frame",
						 "camera",
						 "projection"},
						failure
					))
					return nullptr;
				const json *field = nullptr;
				script::ViewCameraMutationRequest request;
				uint64_t revision = 0, slot = 0;
				if (!Field(values, "instance_id", field, failure) ||
					!Text(*field, "instance_id", request.InstanceId, failure) ||
					!Field(values, "snapshot_id", field, failure) ||
					!Text(*field, "snapshot_id", request.SnapshotId, failure) ||
					!Field(values, "pipeline", field, failure) ||
					!Text(*field, "pipeline", request.Pipeline, failure) ||
					!Field(values, "pipeline_revision", field, failure) ||
					!UInt(*field, "pipeline_revision", revision, failure) ||
					!Field(values, "view_slot", field, failure) ||
					!UInt(*field, "view_slot", slot, failure) || revision == 0)
					return nullptr;
				request.PipelineRevision = revision;
				request.ViewSlot = slot;
				if (const auto frame = values.find("camera_frame"); frame != values.end()) {
					if (!frame->is_array() || frame->size() != 7) {
						failure =
							Error("validation_failed", "camera_frame must contain seven finite numbers");
						return nullptr;
					}
					std::array<float, 7> copied{};
					for (size_t index = 0; index < copied.size(); ++index) {
						const double value =
							(*frame)[index].is_number() ? (*frame)[index].get<double>() : 0.0;
						if (!(*frame)[index].is_number() || !std::isfinite(value) ||
							std::abs(value) > std::numeric_limits<float>::max()) {
							failure = Error(
								"validation_failed", "camera_frame must contain seven finite float values"
							);
							return nullptr;
						}
						copied[index] = static_cast<float>(value);
					}
					request.CameraFrame = copied;
				}
				if (const auto camera = values.find("camera"); camera != values.end()) {
					if (!camera->is_object() ||
						!Only(*camera, {"field_of_view_radians", "near_plane", "far_plane"}, failure))
						return nullptr;
					script::ViewCameraMutationRequest::Camera copied;
					for (const auto &[name, target] : std::array<std::pair<const char *, float *>, 3>{
							 {{"field_of_view_radians", &copied.FieldOfViewRadians},
							  {"near_plane", &copied.NearPlane},
							  {"far_plane", &copied.FarPlane}}
						 }) {
						const auto value = camera->find(name);
						const double number =
							value != camera->end() && value->is_number() ? value->get<double>() : 0.0;
						if (value == camera->end() || !value->is_number() || !std::isfinite(number) ||
							std::abs(number) > std::numeric_limits<float>::max()) {
							failure = Error("validation_failed", "camera values must be finite floats");
							return nullptr;
						}
						*target = static_cast<float>(number);
					}
					request.Lens = copied;
				}
				if (const auto projection = values.find("projection"); projection != values.end()) {
					if (!projection->is_array() || projection->size() != 16) {
						failure =
							Error("validation_failed", "projection must contain sixteen finite numbers");
						return nullptr;
					}
					std::array<float, 16> copied{};
					for (size_t index = 0; index < copied.size(); ++index) {
						const double value =
							(*projection)[index].is_number() ? (*projection)[index].get<double>() : 0.0;
						if (!(*projection)[index].is_number() || !std::isfinite(value) ||
							std::abs(value) > std::numeric_limits<float>::max()) {
							failure = Error(
								"validation_failed", "projection must contain sixteen finite float values"
							);
							return nullptr;
						}
						copied[index] = static_cast<float>(value);
					}
					request.Projection = copied;
				}
				uint64_t ticket = 0;
				std::string detail;
				if (!bridge->QueueViewCameraMutation(request.InstanceId, request, ticket, detail)) {
					failure = Error("view_camera_refused", detail);
					return nullptr;
				}
				return {
					{"status", "queued"},
					{"ticket", ticket},
					{"instance_id", request.InstanceId},
					{"snapshot_id", request.SnapshotId}
				};
			},
		});

		Add(Tool{
			"cancel_view_camera_mutation",
			"Cancels one queued view.camera mutation.",
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
					!Field(values, "ticket", field, failure) || !UInt(*field, "ticket", ticket, failure) ||
					ticket == 0)
					return nullptr;
				bridge->CancelViewCameraMutation(instance, ticket);
				return {{"status", "cancellation_requested"}, {"ticket", ticket}};
			},
		});

		Add(Tool{
			"poll_view_camera_mutation",
			"Returns a view.camera mutation status and releases a terminal mutation ticket.",
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
					!Field(values, "ticket", field, failure) || !UInt(*field, "ticket", ticket, failure) ||
					ticket == 0)
					return nullptr;
				script::ViewCameraMutationPoll poll;
				std::string detail;
				if (!bridge->PollViewCameraMutation(instance, ticket, poll, detail)) {
					failure = Error("view_camera_not_found", detail);
					return nullptr;
				}
				return {
					{"status", poll.Status},
					{"terminal", poll.Terminal},
					{"detail", poll.Detail},
					{"ticket", ticket}
				};
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

		auto action = [bridge, ledger](const char *name, bool release) {
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
				[bridge, ledger, name, release](const json &values, std::string &failure) -> json {
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
					json replay;
					const auto prior = ledger->Replay(name, operation, normalizedText, replay, failure);
					if (prior == DataFactoryOperationReplay::Conflict) return nullptr;
					if (prior == DataFactoryOperationReplay::Replay) return replay;
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
					ledger->Store(name, operation, normalizedText, result, failure);
					return result;
				}
			};
		};
		Add(action("release_capture", true));
		Add(action("cancel_capture", false));
	}
}

namespace engine::control::features {
	// Installs the ticketed capture tools backed by the supplied product-owned bridge.
	inline Feature
	DataCapture(world::DataFactorySession &session, std::shared_ptr<script::DataCaptureBridge> bridge) {
		return Feature{"data_capture", [&session, bridge = std::move(bridge)](Surface &surface) {
						   surface.AddDataCaptureTools(session, bridge);
					   }};
	}
}
