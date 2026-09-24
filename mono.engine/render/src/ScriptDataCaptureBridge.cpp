#include "CaptureRecordValidation.hpp"
#include "DataCaptureCompact.hpp"
#include "DataCapturePacking.hpp"

#include <engine/ecs/Attributes.hpp>
#include <engine/render/DataFactoryHookBind.hpp>
#include <engine/render/ScriptDataCaptureBridge.hpp>
#include <engine/scene/Components.hpp>
#include <engine/script/DataSceneService.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace engine::render {
	struct ScriptDataCaptureBridge::HookState {
		std::unordered_map<uint64_t, ConnectionHandle> Connections;
		std::unordered_map<uint64_t, BatchHandle> Batches;
	};

	ScriptDataCaptureBridge::ScriptDataCaptureBridge(world::DataFactorySession &session, Renderer &renderer)
		: Session(session), RendererRef(renderer) {
		RefreshCapabilities();
	}

	namespace {
		constexpr size_t MAX_CAPTURE_CHANNELS = 17;
		constexpr size_t MAX_CAPTURE_TICKETS = 6;
		constexpr size_t RETAINED_BYTE_LIMIT = 64 * 1024 * 1024;

		// A requested inline sidecar must fit before a capture ticket is issued. Large
		// worlds can still be captured using get_scene_snapshot and ranged reads.
		bool SidecarFits(
			world::DataFactorySession &session,
			const script::DataCaptureBridgeRequest &request,
			std::string &detail
		) {
			if (!request.IncludeSceneData) return true;
			const world::WorldId worldId = session.UniverseOf().Find(core::Name(request.InstanceId));
			if (!worldId.IsValid()) return true;
			bool fits = false;
			const world::WorldStatus status = session.UniverseOf().Enter(worldId, [&](ecs::Store &store) {
				const script::DataSceneResult scene = script::GetSceneSnapshot(store);
				size_t bytes = 0;
				fits = scene.Status == std::string_view("ok") &&
					   script::DataSceneJsonResponseBudget(scene.Value, bytes);
			});
			if (status != world::WorldStatus::Ok) return true;
			if (fits) return true;
			detail = "inline scene sidecar exceeds 64 KiB or is unavailable; use get_scene_snapshot "
					 "and include_scene_data=false";
			return false;
		}

		bool Text(std::string_view value, size_t limit = 256) {
			return !value.empty() && value.size() <= limit && value.find('\0') == std::string_view::npos;
		}

		std::optional<DataCaptureChannel> Channel(std::string_view name) {
			if (name == "rgb_linear_hdr") return DataCaptureChannel::RgbLinearHdr;
			if (name == "linear_depth") return DataCaptureChannel::LinearDepth;
			if (name == "shading_normal") return DataCaptureChannel::ShadingNormal;
			if (name == "pbr_albedo") return DataCaptureChannel::PbrAlbedo;
			if (name == "pbr_material") return DataCaptureChannel::PbrMaterial;
			if (name == "pbr_emissive") return DataCaptureChannel::PbrEmissive;
			if (name == "pbr_specular") return DataCaptureChannel::PbrSpecular;
			if (name == "pbr_transmission") return DataCaptureChannel::PbrTransmission;
			if (name == "mesh_uv") return DataCaptureChannel::MeshUv;
			if (name == "ambient_occlusion") return DataCaptureChannel::AmbientOcclusion;
			if (name == "object_ids") return DataCaptureChannel::ObjectIds;
			if (name == "semantic_ids") return DataCaptureChannel::SemanticMask;
			if (name == "part_ids") return DataCaptureChannel::PartMask;
			if (name == "first_surface_validity") return DataCaptureChannel::FirstSurfaceValidity;
			if (name == "second_surface_depth") return DataCaptureChannel::SecondSurfaceDepth;
			if (name == "second_surface_validity") return DataCaptureChannel::SecondSurfaceValidity;
			if (name == "motion_vectors") return DataCaptureChannel::MotionVectors;
			if (name == "directional_response") return DataCaptureChannel::DirectionalResponse;
			if (name == "shadow_visibility") return DataCaptureChannel::ShadowVisibility;
			if (name == "local_light_contribution") return DataCaptureChannel::LocalLightContribution;
			if (name == "local_light_shadow_visibility")
				return DataCaptureChannel::LocalLightShadowVisibility;
			if (name == "packed_gpu") return DataCaptureChannel::PackedGpu;
			return std::nullopt;
		}

		bool Valid(std::string_view instanceId, const script::DataCaptureBridgeRequest &request) {
			if (!Text(instanceId) || request.InstanceId != instanceId || !Text(request.SnapshotId) ||
				!Text(request.Pipeline) || !Text(request.CaptureNode) ||
				!Text(request.CameraId, script::MAX_DATA_SCENE_ID_BYTES) || request.Channels.empty() ||
				request.Channels.size() > MAX_CAPTURE_CHANNELS || request.TemporalHistory != "preserve" ||
				(request.StorageProfile != "lossless" && request.StorageProfile != "training_compact") ||
				(request.NoiseMode != "none" && request.NoiseMode != "gaussian") ||
				!std::isfinite(request.NoiseSigma) || request.NoiseSigma < 0.0 || request.NoiseSigma > 64.0 ||
				(request.NoiseMode == "none" && (request.NoiseSeed != 0 || request.NoiseSigma != 0.0)) ||
				request.ViewSlot > std::numeric_limits<size_t>::max() ||
				request.LocalLightIds.size() > MAX_DATA_CAPTURE_LOCAL_LIGHT_IDS)
				return false;
			for (size_t index = 0; index < request.LocalLightIds.size(); ++index) {
				if (!Text(request.LocalLightIds[index], script::MAX_DATA_SCENE_ID_BYTES)) return false;
				for (size_t previous = 0; previous < index; ++previous)
					if (request.LocalLightIds[previous] == request.LocalLightIds[index]) return false;
			}
			const bool requested =
				std::ranges::find(request.Channels, "local_light_contribution") != request.Channels.end() ||
				std::ranges::find(request.Channels, "local_light_shadow_visibility") !=
					request.Channels.end();
			if (requested != !request.LocalLightIds.empty()) return false;
			for (size_t first = 0; first < request.Channels.size(); ++first) {
				if (!Text(request.Channels[first], 64) || !Channel(request.Channels[first])) return false;
				for (size_t second = first + 1; second < request.Channels.size(); ++second)
					if (request.Channels[first] == request.Channels[second]) return false;
			}
			if (request.PackedPlanes.size() > script::MAX_DATA_CAPTURE_PACKED_PLANES) return false;
			for (size_t first = 0; first < request.PackedPlanes.size(); ++first) {
				const auto &packed = request.PackedPlanes[first];
				if (!Text(packed.Name, 64) ||
					std::ranges::find(request.Channels, packed.Name) != request.Channels.end())
					return false;
				for (const auto &component : packed.Components)
					if (!Text(component.SourceChannel, 64) || component.SourceComponent > 3 ||
						std::ranges::find(request.Channels, component.SourceChannel) ==
							request.Channels.end())
						return false;
				for (size_t second = first + 1; second < request.PackedPlanes.size(); ++second)
					if (packed.Name == request.PackedPlanes[second].Name) return false;
			}
			const bool secondDepth =
				std::ranges::find(request.Channels, "second_surface_depth") != request.Channels.end();
			const bool secondValidity =
				std::ranges::find(request.Channels, "second_surface_validity") != request.Channels.end();
			return secondDepth == secondValidity &&
				   (request.NoiseMode != "gaussian" ||
					std::ranges::find(request.Channels, "rgb_linear_hdr") != request.Channels.end());
		}

		std::optional<script::DataCaptureBridgeNoise>
		ApplyNoise(DataCapturePlane &plane, const script::DataCaptureBridgeRequest &request) {
			if (request.NoiseMode != "gaussian" || plane.Channel != DataCaptureChannel::RgbLinearHdr ||
				plane.Status != DataCaptureStatus::Ready)
				return std::nullopt;
			if (plane.Scalar != DataCaptureScalar::Float16 ||
				plane.ColourSpace != DataCaptureColourSpace::Linear) {
				plane.Status = DataCaptureStatus::Unsupported;
				plane.Bytes.clear();
				plane.Hash = {};
				plane.Width = 0;
				plane.Height = 0;
				plane.RowStride = 0;
				plane.Scalar = DataCaptureScalar::Unknown;
				plane.ColourSpace = DataCaptureColourSpace::Unknown;
				plane.Provenance = "unavailable/gaussian_noise_requires_linear_rgba16f/v1";
				return std::nullopt;
			}
			data_capture_compact::RgbNoiseResult result;
			if (!data_capture_compact::ApplyGaussianRgbFloat16(
					plane.Bytes,
					plane.Width,
					plane.Height,
					plane.RowStride,
					request.NoiseSeed,
					request.NoiseSigma,
					result
				)) {
				plane.Status = DataCaptureStatus::Unsupported;
				plane.Bytes.clear();
				plane.Hash = {};
				plane.Width = 0;
				plane.Height = 0;
				plane.RowStride = 0;
				plane.Scalar = DataCaptureScalar::Unknown;
				plane.ColourSpace = DataCaptureColourSpace::Unknown;
				plane.Provenance = "unavailable/gaussian_noise_source_layout/v1";
				return std::nullopt;
			}
			plane.Hash = assets::Hasher::Of(plane.Bytes);
			return script::DataCaptureBridgeNoise{
				.Mode = "gaussian",
				.Algorithm = "xorshift64star_clt12_q17/v2",
				.Seed = request.NoiseSeed,
				.Sigma = request.NoiseSigma == 0.0 ? 0.0 : request.NoiseSigma,
				.SigmaQuantization = "binary64_to_q24_round_to_nearest_ties_to_even/v1",
				.EffectiveSigmaQ24 = result.EffectiveSigmaQ24,
				.EffectiveSigma = result.EffectiveSigma,
				.SeedStatePolicy = "zero_maps_to_0x9e3779b97f4a7c15_else_direct/v1",
				.Order = "after_storage_profile/v1",
				.ClampPolicy = "finite_rgb_clamped_to_binary16_range[-65504,65504]",
				.AlphaPolicy = "preserve_exact_binary16",
				.ValueClassification = std::move(result.ValueClassification),
				.MaximumAbsoluteError = result.MaximumAbsoluteError,
			};
		}

		bool PipelineMatches(std::string_view requested, const View &view) {
			const std::string_view runtime = view.Pipeline.Text();
			if (requested == runtime) return true;
			const std::string suffix = "#" + std::to_string(view.World);
			return runtime.ends_with(suffix) &&
				   requested == runtime.substr(0, runtime.size() - suffix.size());
		}

		bool ResolveNamedCamera(
			world::DataFactorySession &session, View &view, std::string_view cameraId, std::string &detail
		) {
			if (cameraId == "current_view") return true;
			size_t matchingIdentities = 0;
			std::optional<scene::Camera> selectedCamera;
			std::optional<core::CFrame> selectedFrame;
			std::optional<ecs::Entity> selectedEntity;
			uint64_t storeIdentity = 0;
			const world::WorldStatus status =
				session.UniverseOf().Enter(session.UniverseOf().Find(view.WorldName), [&](ecs::Store &store) {
					storeIdentity = store.Identity();
					store.EachEntity([&](ecs::Entity entity) {
						ecs::AttributeValue identity;
						if (!ecs::GetAttribute(store, entity, core::Name("DataFactoryId"), identity) ||
							identity.Type != ecs::PropertyType::String || identity.String != cameraId)
							return;
						matchingIdentities++;
						const scene::Camera *camera = store.Get<scene::Camera>(entity);
						const scene::Transform *transform = store.Get<scene::Transform>(entity);
						if (camera && transform) {
							selectedCamera = *camera;
							selectedFrame = transform->Frame;
							selectedEntity = entity;
						}
					});
				});
			if (status != world::WorldStatus::Ok)
				detail = "capture world is unavailable";
			else if (matchingIdentities != 1)
				detail = "named camera id is not unique or absent";
			else if (!selectedCamera || !selectedFrame)
				detail = "named camera id does not identify a camera";
			if (status != world::WorldStatus::Ok || matchingIdentities != 1 || !selectedCamera ||
				!selectedFrame)
				return false;
			view.CameraFrame = *selectedFrame;
			view.Camera = *selectedCamera;
			// A named camera has its own producer lineage. The store incarnation and
			// entity generation prevent a restored or replaced camera from inheriting it.
			view.CameraTemporalId = "named/" + std::string(view.WorldName.Text()) + "/" +
									std::to_string(storeIdentity) + "/" + std::to_string(selectedEntity->Id);
			if (view.CameraTemporalId.size() > 256) view.CameraTemporalId.clear();
			return true;
		}

		const char *Status(DataCaptureStatus status) {
			switch (status) {
			case DataCaptureStatus::Pending:
				return "pending";
			case DataCaptureStatus::Ready:
				return "ready";
			case DataCaptureStatus::Partial:
				return "partial";
			case DataCaptureStatus::Unsupported:
				return "unsupported";
			case DataCaptureStatus::Invalid:
				return "invalid";
			case DataCaptureStatus::Failed:
				return "failed";
			case DataCaptureStatus::Cancelled:
				return "cancelled";
			}
			return "invalid";
		}
		const char *MutationStatusText(ViewMutationStatus status) {
			switch (status) {
			case ViewMutationStatus::Pending:
			case ViewMutationStatus::AppliedAwaitingRestore:
				return "pending";
			case ViewMutationStatus::Applied:
				return "applied";
			case ViewMutationStatus::Cancelled:
				return "cancelled";
			case ViewMutationStatus::Stale:
				return "stale";
			case ViewMutationStatus::Invalid:
				return "invalid";
			}
			return "invalid";
		}

		const char *Scalar(DataCaptureScalar scalar) {
			switch (scalar) {
			case DataCaptureScalar::Float16:
				return "float16";
			case DataCaptureScalar::Float32:
				return "float32";
			case DataCaptureScalar::UInt32:
				return "uint32";
			case DataCaptureScalar::UNorm8:
				return "unorm8";
			case DataCaptureScalar::UNorm10A2:
				return "unorm10a2";
			case DataCaptureScalar::Unknown:
				return "unknown";
			}
			return "unknown";
		}

		const char *Encoding(DataCaptureScalar scalar) {
			switch (scalar) {
			case DataCaptureScalar::Float16:
				return "ieee754_binary16_le";
			case DataCaptureScalar::Float32:
				return "ieee754_binary32_le";
			case DataCaptureScalar::UInt32:
				return "uint32_le";
			case DataCaptureScalar::UNorm8:
				return "unorm8";
			case DataCaptureScalar::UNorm10A2:
				return "unorm10a2_le";
			case DataCaptureScalar::Unknown:
				return "unknown";
			}
			return "unknown";
		}

		bool CompactDepthPlane(
			DataCapturePlane &plane,
			std::optional<double> &maximumError,
			std::string &valueClassification,
			std::string &rejection
		) {
			if (std::endian::native != std::endian::little || plane.Scalar != DataCaptureScalar::Float32) {
				rejection = "unsupported_source_layout";
				return false;
			}
			std::vector<std::byte> compact;
			if (!data_capture_compact::CompactFloat32DepthBytes(
					plane.Bytes,
					plane.Width,
					plane.Height,
					plane.RowStride,
					compact,
					maximumError,
					valueClassification,
					rejection
				))
				return false;
			plane.RowStride = plane.Width * 2;
			plane.Scalar = DataCaptureScalar::Float16;
			plane.Bytes = std::move(compact);
			plane.Hash = assets::Hasher::Of(plane.Bytes);
			return true;
		}

		const char *Packing(DataCaptureChannel channel, DataCaptureScalar scalar) {
			return channel == DataCaptureChannel::AmbientOcclusion ||
						   channel == DataCaptureChannel::ShadowVisibility ||
						   channel == DataCaptureChannel::LocalLightShadowVisibility ||
						   channel == DataCaptureChannel::FirstSurfaceValidity ||
						   channel == DataCaptureChannel::SecondSurfaceValidity
					   ? "unorm8"
				   : scalar == DataCaptureScalar::UNorm8 ? "rgba8_unorm"
				   : channel == DataCaptureChannel::MeshUv || channel == DataCaptureChannel::MotionVectors
					   ? "rg16_float"
				   : scalar == DataCaptureScalar::UNorm10A2 ? "unorm10a2"
				   : (channel == DataCaptureChannel::PackedGpu ||
					  channel == DataCaptureChannel::DirectionalResponse)
					   ? "rgba32_float"
					   : "";
		}

		bool Compactible(DataCaptureChannel channel) {
			return channel == DataCaptureChannel::LinearDepth ||
				   channel == DataCaptureChannel::SecondSurfaceDepth;
		}

		void CompactUnavailable(DataCapturePlane &plane, std::string_view reason) {
			const DataCaptureChannel channel = plane.Channel;
			const core::Name node = plane.CaptureNode;
			plane = {};
			plane.Channel = channel;
			plane.CaptureNode = node;
			plane.Status = DataCaptureStatus::Unsupported;
			plane.Provenance = "unavailable/training_compact_" + std::string(reason) + "/v1";
		}

		const char *ColourSpace(DataCaptureColourSpace colourSpace) {
			switch (colourSpace) {
			case DataCaptureColourSpace::Linear:
				return "linear";
			case DataCaptureColourSpace::SRGB:
				return "srgb";
			case DataCaptureColourSpace::NotApplicable:
				return "not_applicable";
			case DataCaptureColourSpace::Unknown:
				return "unknown";
			}
			return "unknown";
		}

		const char *Provenance(DataCaptureChannel channel) {
			return channel == DataCaptureChannel::AmbientOcclusion
					   ? "ssao_estimator_visibility_factor_not_ground_truth"
				   : channel == DataCaptureChannel::DirectionalResponse
					   ? "directional_response_rgb_is_unshadowed_linear_radiance_alpha_is_visibility_factor"
				   : channel == DataCaptureChannel::ShadowVisibility
					   ? "directional_shadow_and_portal_beam_visibility_factor_quantized_to_unorm8"
					   : "";
		}

		const char *SourceState(AmbientOcclusionSourceState state) {
			switch (state) {
			case AmbientOcclusionSourceState::Estimated:
				return "estimated";
			case AmbientOcclusionSourceState::ClearedDisabled:
				return "cleared_disabled";
			case AmbientOcclusionSourceState::ClearedNoPass:
				return "cleared_no_pass";
			case AmbientOcclusionSourceState::Unavailable:
				return "unavailable";
			}
			return "unavailable";
		}

		std::optional<script::DataCaptureBridgeAmbientOcclusion>
		CopyAmbientOcclusion(const std::optional<AmbientOcclusionProvenance> &source) {
			if (!source) return std::nullopt;
			const auto denoiser = source->Denoiser == std::optional(AmbientOcclusionDenoiser::None)
									  ? std::optional<std::string>("none")
									  : std::nullopt;
			const auto temporalHistory =
				source->TemporalHistory == std::optional(AmbientOcclusionTemporalHistory::Disabled)
					? std::optional<std::string>("none")
					: std::nullopt;
			const auto backgroundClassification =
				source->BackgroundClassification ? std::optional<std::string>("unavailable") : std::nullopt;
			return script::DataCaptureBridgeAmbientOcclusion{
				.SourceState = SourceState(source->SourceState),
				.ProducerFrame = source->ProducerFrame,
				.Enabled = source->Enabled,
				.SampleCount = source->SampleCount,
				.RadiusWorldUnits = source->RadiusWorldUnits
										? std::optional<double>(*source->RadiusWorldUnits)
										: std::nullopt,
				.Denoiser = std::move(denoiser),
				.TemporalHistory = std::move(temporalHistory),
				.BackgroundValue =
					source->BackgroundValue ? std::optional<double>(*source->BackgroundValue) : std::nullopt,
				.BackgroundClassification = std::move(backgroundClassification)
			};
		}

		std::string ResourceId(uint64_t ticket, DataCaptureChannel channel, std::string_view lightId = {}) {
			std::string resource =
				"capture/" + std::to_string(ticket) + "/" + std::string(DataCaptureChannelName(channel));
			if (!lightId.empty()) resource += "/" + std::string(lightId);
			return resource;
		}

		std::string PackedResourceId(uint64_t ticket, std::string_view name) {
			return "capture/" + std::to_string(ticket) + "/packed/" + std::string(name);
		}

		std::string CoordinateConvention(const DataCaptureCameraConvention &camera) {
			return std::string(camera.RightHandedWorld ? "right_handed_world" : "left_handed_world") + "," +
				   (camera.CameraLooksNegativeZ ? "camera_negative_z" : "camera_positive_z") + "," +
				   (camera.ClipYUp ? "clip_y_up" : "clip_y_down") + "," +
				   (camera.DepthZeroToOne ? "depth_zero_to_one" : "depth_negative_one_to_one") + "," +
				   (camera.ProjectionIsColumnMajor ? "column_major_projection" : "row_major_projection") +
				   ",metres_per_world_unit=" + std::to_string(camera.MetresPerWorldUnit);
		}

		void CopyCamera(const DataCapturePoll &source, script::DataCaptureBridgePoll &destination) {
			destination.HasCamera = true;
			for (size_t index = 0; index < source.CameraPose.WorldFromCamera.size(); ++index)
				destination.WorldFromCamera[index] = source.CameraPose.WorldFromCamera[index];
			destination.HasProjection = source.CameraPose.ProjectionAvailable;
			if (destination.HasProjection)
				for (size_t index = 0; index < source.CameraPose.Projection.size(); ++index)
					destination.Projection[index] = source.CameraPose.Projection[index];
			destination.VerticalFieldOfViewRadians = source.CameraPose.VerticalFieldOfViewRadians;
			destination.NearMetres = source.CameraPose.NearPlaneMetres;
			destination.FarMetres = source.CameraPose.FarPlaneMetres;
			destination.CropLeft = source.CameraPose.CropLeft;
			destination.CropTop = source.CameraPose.CropTop;
			destination.CropWidth = source.CameraPose.CropWidth;
			destination.CropHeight = source.CameraPose.CropHeight;
			destination.CoordinateConvention = CoordinateConvention(source.Camera);
		}
	}

	ScriptDataCaptureBridge::~ScriptDataCaptureBridge() {
		std::vector<ConnectionHandle> connections;
		std::vector<MutationHandle> mutations;
		{
			std::lock_guard lock(Mutex);
			if (Hooks)
				for (const auto &[id, connection] : Hooks->Connections)
					connections.push_back(connection);
			for (const auto &[ticket, entry] : Mutations) {
				(void)ticket;
				if (entry.Handle.IsValid()) mutations.push_back(entry.Handle);
			}
			Mutations.clear();
		}
		RendererRef.Hooks().DisconnectHooks(connections);
		for (MutationHandle mutation : mutations) {
			RendererRef.Hooks().DiscardViewMutation(mutation);
		}
	}

	script::DataCaptureBridgeCapabilities ScriptDataCaptureBridge::Capabilities() const {
		std::lock_guard lock(Mutex);
		return {
			.Available = CaptureAvailable,
			.Channels =
				{"rgb_linear_hdr",
				 "linear_depth",
				 "shading_normal",
				 "pbr_albedo",
				 "pbr_material",
				 "pbr_emissive",
				 "pbr_specular",
				 "pbr_transmission",
				 "mesh_uv",
				 "ambient_occlusion",
				 "object_ids",
				 "semantic_ids",
				 "part_ids",
				 "first_surface_validity",
				 "second_surface_depth",
				 "second_surface_validity",
				 "motion_vectors",
				 "directional_response",
				 "shadow_visibility",
				 "local_light_contribution",
				 "local_light_shadow_visibility",
				 "packed_gpu"},
			.StorageProfiles = {"lossless", "training_compact"},
			.TrainingCompactLimitations =
				{"linear_depth=float32_to_float16_le",
				 "second_surface_depth=float32_to_float16_le",
				 "finite_overflow_above_65504_rejected",
				 "infinity_and_nan_classes_preserved",
				 "source_descriptor_preserved"},
			.NoiseLimitations =
				{"gaussian=rgb_linear_hdr_only",
				 "algorithm=xorshift64star_clt12_q17/v2",
				 "seed_state=zero_maps_to_0x9e3779b97f4a7c15_else_direct/v1",
				 "sigma=finite_0_to_64",
				 "sigma_quantization=binary64_to_q24_round_to_nearest_ties_to_even/v1",
				 "colour_lanes=rgb_only_alpha_exact",
				 "order=after_storage_profile/v1",
				 "labels_depth_normals_sidecars=never_modified"},
			.HookRecords = HookCapabilities,
			.MaximumHooks = static_cast<uint32_t>(MAX_DATA_FACTORY_HOOKS),
			.MaximumConnections = static_cast<uint32_t>(MAX_DATA_FACTORY_CONNECTIONS),
			.MaximumBatches = static_cast<uint32_t>(MAX_DATA_FACTORY_BATCHES),
			.MaximumCaptureTickets = static_cast<uint32_t>(MAX_CAPTURE_TICKETS),
			.MaximumReadbackNodes = static_cast<uint32_t>(MAX_DATA_FACTORY_READBACK_NODES),
			.MaximumRetainedBytes = MAX_DATA_FACTORY_RETAINED_BYTES,
			.MaximumPendingPumps = MAX_DATA_FACTORY_PENDING_PUMPS,
			.NamedCameraSelection = true,
			.SameFrameMultiCamera = true,
			.MaximumSameFrameCameraViews = static_cast<uint32_t>(MAX_CAPTURE_TICKETS),
			.MaximumCameraIdBytes = static_cast<uint32_t>(script::MAX_DATA_SCENE_ID_BYTES),
			.MaximumLocalLightIds = static_cast<uint32_t>(MAX_DATA_CAPTURE_LOCAL_LIGHT_IDS),
			.Detail = CaptureAvailable ? "requires a declared compatible capture node"
									   : "renderer is not ready for capture"
		};
	}

	void ScriptDataCaptureBridge::RefreshCapabilities() {
		const bool available = RendererRef.Backend().Device != nullptr;
		std::vector<script::DataCaptureBridgeHookCapability> hooks;
		for (const RenderHookCapability &hook : RendererRef.Hooks().DescribeHooks()) {
			script::DataCaptureBridgeHookCapability record;
			record.Name = hook.Name.Text();
			record.SchemaVersion = hook.SchemaVersion;
			record.NodeKind = hook.NodeKind.Text();
			record.Required = hook.Required;
			record.Access =
				hook.Access == RenderHookAccess::SynchronousMutation ? "synchronous_mutation" : "observation";
			record.Channels.reserve(hook.ChannelCount);
			for (size_t index = 0; index < hook.ChannelCount; ++index)
				record.Channels.emplace_back(DataCaptureChannelName(hook.Channels[index]));
			for (size_t index = 0; index < hook.MutatedFieldCount; ++index) {
				switch (hook.MutatedFields[index]) {
				case RenderHookMutatedField::CameraFrame:
					record.MutatedFields.emplace_back("camera_frame");
					break;
				case RenderHookMutatedField::Camera:
					record.MutatedFields.emplace_back("camera");
					break;
				case RenderHookMutatedField::Projection:
					record.MutatedFields.emplace_back("projection");
					break;
				}
			}
			hooks.push_back(std::move(record));
		}
		std::lock_guard lock(Mutex);
		CaptureAvailable = available;
		HookCapabilities = std::move(hooks);
	}

	bool ScriptDataCaptureBridge::Queue(
		std::string_view instanceId,
		const script::DataCaptureBridgeRequest &request,
		uint64_t &ticket,
		std::string &detail
	) {
		if (!Valid(instanceId, request)) {
			detail = "invalid capture request";
			return false;
		}
		if (!SidecarFits(Session, request, detail)) return false;
		std::lock_guard lock(Mutex);
		if (Entries.size() >= MAX_CAPTURE_TICKETS) {
			detail = "capture queue is full; release a terminal capture";
			return false;
		}
		if (NextTicket == 0) {
			detail = "capture ticket space exhausted";
			return false;
		}
		ticket = NextTicket++;
		Entry entry;
		entry.Request = request;
		entry.Reply.StorageProfile = request.StorageProfile;
		Entries.emplace(ticket, std::move(entry));
		detail.clear();
		return true;
	}

	bool ScriptDataCaptureBridge::QueueGroup(
		std::string_view instanceId,
		std::span<const script::DataCaptureBridgeRequest> requests,
		std::span<uint64_t> tickets,
		std::string &detail
	) {
		std::fill(tickets.begin(), tickets.end(), uint64_t{0});
		if (requests.size() < 2 || requests.size() != tickets.size()) {
			detail = "same-frame capture group needs matching request and ticket counts of at least two";
			return false;
		}
		for (const auto &request : requests)
			if (!SidecarFits(Session, request, detail)) return false;
		std::lock_guard lock(Mutex);
		if (requests.size() > MAX_CAPTURE_TICKETS - Entries.size() || NextTicket == 0 || NextGroup == 0) {
			detail = Entries.size() >= MAX_CAPTURE_TICKETS
						 ? "capture queue is full; release a terminal capture"
						 : "capture ticket space exhausted";
			return false;
		}
		const script::DataCaptureBridgeRequest &first = requests.front();
		if (first.ViewSlot != 0) {
			detail = "same-frame capture groups require logical view slot 0";
			return false;
		}
		for (size_t index = 0; index < requests.size(); ++index) {
			const auto &request = requests[index];
			if (!Valid(instanceId, request) || request.SnapshotId != first.SnapshotId ||
				request.Pipeline != first.Pipeline || request.CaptureNode != first.CaptureNode ||
				request.ViewSlot != first.ViewSlot) {
				detail = "same-frame capture group must share instance, snapshot, pipeline, capture node, "
						 "and view slot";
				return false;
			}
			for (size_t earlier = 0; earlier < index; ++earlier)
				if (request.CameraId == requests[earlier].CameraId) {
					detail = "same-frame capture group requires unique camera ids";
					return false;
				}
		}
		const uint64_t group = NextGroup++;
		for (size_t index = 0; index < requests.size(); ++index) {
			const uint64_t ticket = NextTicket++;
			Entry entry;
			entry.Request = requests[index];
			entry.Reply.StorageProfile = requests[index].StorageProfile;
			entry.Group = group;
			Entries.emplace(ticket, std::move(entry));
			tickets[index] = ticket;
		}
		detail.clear();
		return true;
	}

	bool ScriptDataCaptureBridge::QueueViewCameraMutation(
		std::string_view instanceId,
		const script::ViewCameraMutationRequest &request,
		uint64_t &ticket,
		std::string &detail
	) {
		if (!Text(instanceId) || request.InstanceId != instanceId || !Text(request.SnapshotId) ||
			!Text(request.Pipeline) || request.PipelineRevision == 0 ||
			(!request.CameraFrame && !request.Lens && !request.Projection)) {
			detail = "invalid view.camera patch";
			return false;
		}
		std::lock_guard lock(Mutex);
		if (Mutations.size() >= MAX_DATA_FACTORY_BATCHES) {
			detail = "view.camera capacity reached";
			return false;
		}
		if (NextTicket == 0) {
			detail = "view.camera ticket space exhausted";
			return false;
		}
		for (const auto &[existingTicket, existing] : Mutations) {
			(void)existingTicket;
			const auto &identity = existing.Request;
			if (!existing.CancelRequested && identity.InstanceId == request.InstanceId &&
				identity.SnapshotId == request.SnapshotId && identity.Pipeline == request.Pipeline &&
				identity.PipelineRevision == request.PipelineRevision &&
				identity.ViewSlot == request.ViewSlot) {
				detail = "view.camera patch already pending for this identity";
				return false;
			}
		}
		ticket = NextTicket++;
		Mutations.emplace(
			ticket,
			MutationEntry{
				.Request = request,
				.Handle = {},
				.Status = ViewMutationStatus::Pending,
				.Detail = {},
				.CancelRequested = false
			}
		);
		detail = "queued";
		return true;
	}

	void ScriptDataCaptureBridge::CancelViewCameraMutation(std::string_view instanceId, uint64_t ticket) {
		std::lock_guard lock(Mutex);
		if (const auto entry = Mutations.find(ticket);
			entry != Mutations.end() && entry->second.Request.InstanceId == instanceId)
			entry->second.CancelRequested = true;
	}

	bool ScriptDataCaptureBridge::PollViewCameraMutation(
		std::string_view instanceId,
		uint64_t ticket,
		script::ViewCameraMutationPoll &poll,
		std::string &detail
	) {
		std::lock_guard lock(Mutex);
		const auto entry = Mutations.find(ticket);
		if (entry == Mutations.end() || entry->second.Request.InstanceId != instanceId) {
			detail = "unknown view.camera ticket";
			return false;
		}
		poll.Status = MutationStatusText(entry->second.Status);
		poll.Terminal = entry->second.Status != ViewMutationStatus::Pending &&
						entry->second.Status != ViewMutationStatus::AppliedAwaitingRestore &&
						!entry->second.Handle.IsValid();
		poll.Detail = entry->second.Detail;
		if (poll.Terminal) Mutations.erase(entry);
		detail.clear();
		return true;
	}

	bool ScriptDataCaptureBridge::Poll(
		std::string_view instanceId, uint64_t ticket, script::DataCaptureBridgePoll &poll, std::string &detail
	) {
		std::lock_guard lock(Mutex);
		const auto found = Entries.find(ticket);
		if (found == Entries.end() || found->second.Request.InstanceId != instanceId) {
			detail = "unknown capture ticket";
			return false;
		}
		poll = found->second.Reply;
		if (!found->second.Terminal) {
			poll.Status = "pending";
			poll.SnapshotId = found->second.Request.SnapshotId;
			poll.StorageProfile = found->second.Request.StorageProfile;
		}
		detail = found->second.Detail;
		return true;
	}

	bool ScriptDataCaptureBridge::ReadPlane(
		std::string_view instanceId,
		uint64_t ticket,
		std::string_view resource,
		size_t offset,
		size_t maximumBytes,
		std::vector<std::byte> &bytes,
		std::string &detail
	) {
		std::lock_guard lock(Mutex);
		const auto found = Entries.find(ticket);
		if (found == Entries.end() || found->second.Request.InstanceId != instanceId) {
			detail = "unknown capture ticket";
			return false;
		}
		if (!found->second.Terminal) {
			detail = "capture is pending";
			return false;
		}
		const auto plane = found->second.PlaneBytes.find(std::string(resource));
		if (plane == found->second.PlaneBytes.end()) {
			detail = "unknown capture resource";
			return false;
		}
		if (offset > plane->second.size()) {
			detail = "capture offset exceeds plane size";
			return false;
		}
		const size_t count = std::min(maximumBytes, plane->second.size() - offset);
		bytes.assign(
			plane->second.begin() + static_cast<std::ptrdiff_t>(offset),
			plane->second.begin() + static_cast<std::ptrdiff_t>(offset + count)
		);
		found->second.Reply.Profile.TransferBytes += count;
		++found->second.Reply.Profile.TransferOperations;
		detail.clear();
		return true;
	}

	bool ScriptDataCaptureBridge::Release(std::string_view instanceId, uint64_t ticket, std::string &detail) {
		std::lock_guard lock(Mutex);
		const auto found = Entries.find(ticket);
		if (found == Entries.end() || found->second.Request.InstanceId != instanceId) {
			detail = "unknown capture ticket";
			return false;
		}
		if (!found->second.Terminal) {
			detail = "capture is pending";
			return false;
		}
		for (const auto &plane : found->second.PlaneBytes)
			RetainedBytes -= plane.second.size();
		RetainedBytes -= found->second.SceneSidecarBytes;
		Entries.erase(found);
		detail.clear();
		return true;
	}

	void ScriptDataCaptureBridge::Cancel(std::string_view instanceId, uint64_t ticket) {
		std::lock_guard lock(Mutex);
		const auto found = Entries.find(ticket);
		if (found == Entries.end() || found->second.Request.InstanceId != instanceId ||
			found->second.Terminal)
			return;
		const uint64_t group = found->second.Group;
		for (auto &[candidate, entry] : Entries) {
			if (entry.Request.InstanceId != instanceId || entry.Terminal ||
				(group == 0 ? candidate != ticket : entry.Group != group))
				continue;
			entry.CancelRequested = true;
		}
	}

	bool ScriptDataCaptureBridge::TeardownInstance(std::string_view instanceId, std::string &detail) {
		if (!Text(instanceId)) {
			detail = "invalid capture instance";
			return false;
		}
		std::vector<ConnectionHandle> connections;
		std::vector<MutationHandle> mutations;
		{
			std::lock_guard lock(Mutex);
			for (auto entry = Entries.begin(); entry != Entries.end();) {
				if (entry->second.Request.InstanceId != instanceId) {
					++entry;
					continue;
				}
				if (Hooks) {
					if (auto owner = Hooks->Connections.find(entry->first);
						owner != Hooks->Connections.end()) {
						connections.push_back(owner->second);
						Hooks->Connections.erase(owner);
					}
					Hooks->Batches.erase(entry->first);
				}
				for (const auto &[resource, bytes] : entry->second.PlaneBytes)
					RetainedBytes -= bytes.size();
				RetainedBytes -= entry->second.SceneSidecarBytes;
				entry = Entries.erase(entry);
			}
			for (auto entry = Mutations.begin(); entry != Mutations.end();) {
				if (entry->second.Request.InstanceId != instanceId) {
					++entry;
					continue;
				}
				if (entry->second.Handle.IsValid()) mutations.push_back(entry->second.Handle);
				entry = Mutations.erase(entry);
			}
		}
		RendererRef.Hooks().DisconnectHooks(connections);
		for (MutationHandle mutation : mutations) {
			RendererRef.Hooks().DiscardViewMutation(mutation);
		}
		detail.clear();
		return true;
	}

	bool ScriptDataCaptureBridge::PrepareBatch(
		const View &source, std::vector<View> &views, PreparedBatch *prepared
	) {
		views.clear();
		if (prepared != nullptr) prepared->Views.clear();
		RefreshCapabilities();
		if (source.Slot != 0 || source.CaptureGroup != 0 ||
			!RendererRef.ResolvePipelineIdentity(source.Pipeline))
			return false;

		std::vector<PendingRequest> pending;
		uint64_t group = 0;
		{
			std::lock_guard lock(Mutex);
			for (const auto &[ticket, entry] : Entries) {
				if (entry.Group == 0 || entry.PhysicalViewSlot || entry.Preparing || entry.CancelRequested ||
					entry.Terminal || entry.Request.InstanceId != source.WorldName.Text() ||
					!PipelineMatches(entry.Request.Pipeline, source) || entry.Request.ViewSlot != source.Slot)
					continue;
				if (group == 0 || entry.Group < group) group = entry.Group;
			}
			if (group == 0) return false;
			for (const auto &[ticket, entry] : Entries)
				if (entry.Group == group) pending.push_back({.Id = ticket, .Request = entry.Request});
		}
		std::sort(
			pending.begin(), pending.end(), [](const PendingRequest &left, const PendingRequest &right) {
				return left.Id < right.Id;
			}
		);
		if (pending.size() < 2 || pending.size() > MAX_CAPTURE_TICKETS) return false;

		const world::DataFactoryReply barrier =
			Session.RenderSnapshotBarrier(source.WorldName.Text(), pending.front().Request.SnapshotId);
		if (barrier.Status != world::DataFactoryStatus::Ok) {
			std::lock_guard lock(Mutex);
			for (const PendingRequest &request : pending)
				if (auto entry = Entries.find(request.Id); entry != Entries.end()) {
					entry->second.Reply.Status = "stale_snapshot";
					entry->second.Reply.SnapshotId = request.Request.SnapshotId;
					entry->second.Detail = barrier.Detail;
					entry->second.Terminal = true;
				}
			return false;
		}

		std::vector<View> built;
		built.reserve(pending.size());
		for (size_t index = 0; index < pending.size(); ++index) {
			View capture = source;
			capture.Slot = index + 1;
			capture.CaptureGroup = group;
			capture.SnapshotId = pending[index].Request.SnapshotId;
			std::string detail;
			if (!ResolveNamedCamera(Session, capture, pending[index].Request.CameraId, detail)) {
				std::lock_guard lock(Mutex);
				for (const PendingRequest &request : pending)
					if (auto entry = Entries.find(request.Id); entry != Entries.end()) {
						entry->second.Reply.Status = "invalid";
						entry->second.Reply.SnapshotId = request.Request.SnapshotId;
						entry->second.Detail = detail;
						entry->second.Terminal = true;
					}
				return false;
			}
			built.push_back(std::move(capture));
		}
		{
			std::lock_guard lock(Mutex);
			for (size_t index = 0; index < pending.size(); ++index) {
				auto entry = Entries.find(pending[index].Id);
				if (entry == Entries.end() || entry->second.Group != group || entry->second.CancelRequested ||
					entry->second.Terminal) {
					return false;
				}
			}
			for (size_t index = 0; index < pending.size(); ++index) {
				auto entry = Entries.find(pending[index].Id);
				entry->second.PhysicalViewSlot = index + 1;
			}
		}
		views = std::move(built);
		if (prepared != nullptr) {
			prepared->Views.resize(pending.size());
			for (size_t index = 0; index < pending.size(); ++index)
				prepared->Views[index].Captures.push_back(pending[index].Id);
		}
		return true;
	}

	void ScriptDataCaptureBridge::AbortPreparedBatch(const PreparedBatch &prepared) {
		for (const PreparedView &view : prepared.Views)
			AbortPreparedView(view);
	}

	bool ScriptDataCaptureBridge::PrepareView(View &view, PreparedView *prepared) {
		if (prepared != nullptr) *prepared = {};
		RefreshCapabilities();
		if (!Hooks) Hooks = std::make_unique<HookState>();
		const auto pipeline = RendererRef.ResolvePipelineIdentity(view.Pipeline);
		if (!pipeline) return false;
		const std::string originalSnapshot = view.SnapshotId;
		bool namedCameraApplied = false;
		std::vector<PendingRequest> pending;
		std::vector<std::pair<uint64_t, std::string>> mutationSnapshots;
		std::vector<std::pair<uint64_t, MutationHandle>> armedPendingMutations;
		std::string selectedSnapshot = view.SnapshotId;
		{
			std::lock_guard lock(Mutex);
			for (auto &[id, entry] : Entries) {
				if (Hooks->Batches.contains(id) || entry.Preparing || entry.CancelRequested ||
					entry.Terminal || entry.Request.InstanceId != view.WorldName.Text() ||
					entry.Group != view.CaptureGroup || !PipelineMatches(entry.Request.Pipeline, view) ||
					entry.PhysicalViewSlot.value_or(entry.Request.ViewSlot) != view.Slot)
					continue;
				pending.push_back({.Id = id, .Request = entry.Request});
			}
			std::sort(
				pending.begin(), pending.end(), [](const PendingRequest &left, const PendingRequest &right) {
					return left.Id < right.Id;
				}
			);
			for (const auto &[ticket, entry] : Mutations) {
				const auto &request = entry.Request;
				if (request.InstanceId == view.WorldName.Text() &&
					request.Pipeline == pipeline->Name.Text() && request.ViewSlot == view.Slot &&
					((entry.Status == ViewMutationStatus::Pending && !entry.CancelRequested) ||
					 entry.Status == ViewMutationStatus::AppliedAwaitingRestore))
					mutationSnapshots.emplace_back(ticket, request.SnapshotId);
			}
			std::sort(mutationSnapshots.begin(), mutationSnapshots.end());
			if (selectedSnapshot.empty()) {
				const bool captureFirst =
					!pending.empty() &&
					(mutationSnapshots.empty() || pending.front().Id < mutationSnapshots.front().first);
				if (captureFirst)
					selectedSnapshot = pending.front().Request.SnapshotId;
				else if (!mutationSnapshots.empty())
					selectedSnapshot = mutationSnapshots.front().second;
			}
			std::erase_if(pending, [&](const PendingRequest &request) {
				return request.Request.SnapshotId != selectedSnapshot;
			});
			const std::string selectedCamera =
				pending.empty() ? "current_view" : pending.front().Request.CameraId;
			std::erase_if(pending, [&](const PendingRequest &request) {
				return request.Request.CameraId != selectedCamera;
			});
			if (prepared != nullptr)
				for (const PendingRequest &request : pending)
					prepared->Captures.push_back(request.Id);
			for (const auto &[ticket, entry] : Mutations) {
				const auto &request = entry.Request;
				if (entry.Status == ViewMutationStatus::Pending && entry.Handle.IsValid() &&
					!entry.CancelRequested && request.InstanceId == view.WorldName.Text() &&
					request.Pipeline == pipeline->Name.Text() && request.ViewSlot == view.Slot &&
					request.SnapshotId == selectedSnapshot)
					armedPendingMutations.emplace_back(ticket, entry.Handle);
			}
		}
		bool armedSnapshotCurrent = true;
		for (const auto &[ticket, handle] : armedPendingMutations) {
			const world::DataFactoryReply barrier =
				Session.RenderSnapshotBarrier(view.WorldName.Text(), selectedSnapshot);
			if (barrier.Status == world::DataFactoryStatus::Ok) continue;
			RendererRef.Hooks().Cancel(handle);
			RendererRef.Hooks().ReleaseViewMutation(handle);
			std::lock_guard lock(Mutex);
			if (auto entry = Mutations.find(ticket); entry != Mutations.end() &&
													 entry->second.Handle.Slot == handle.Slot &&
													 entry->second.Handle.Generation == handle.Generation) {
				entry->second.Handle = {};
				if (entry->second.CancelRequested) {
					entry->second.Status = ViewMutationStatus::Cancelled;
					entry->second.Detail.clear();
				} else {
					entry->second.Status = ViewMutationStatus::Stale;
					entry->second.Detail = barrier.Detail;
				}
			}
			armedSnapshotCurrent = false;
		}
		if (!armedSnapshotCurrent) return false;
		if (!pending.empty()) {
			const world::DataFactoryReply barrier =
				Session.RenderSnapshotBarrier(view.WorldName.Text(), pending.front().Request.SnapshotId);
			if (barrier.Status != world::DataFactoryStatus::Ok) {
				std::lock_guard lock(Mutex);
				for (const PendingRequest &request : pending)
					if (auto entry = Entries.find(request.Id); entry != Entries.end()) {
						entry->second.Reply.Status = "stale_snapshot";
						entry->second.Reply.SnapshotId = request.Request.SnapshotId;
						entry->second.Detail = barrier.Detail;
						entry->second.Terminal = true;
					}
				return false;
			}
			std::string cameraDetail;
			if (!ResolveNamedCamera(Session, view, pending.front().Request.CameraId, cameraDetail)) {
				view.SnapshotId = originalSnapshot;
				std::lock_guard lock(Mutex);
				for (const PendingRequest &request : pending)
					if (auto entry = Entries.find(request.Id); entry != Entries.end()) {
						entry->second.Reply.Status = "invalid";
						entry->second.Reply.SnapshotId = request.Request.SnapshotId;
						entry->second.Detail = cameraDetail;
						entry->second.Terminal = true;
					}
				return false;
			}
			namedCameraApplied = pending.front().Request.CameraId != "current_view";
		}
		if (!selectedSnapshot.empty()) view.SnapshotId = selectedSnapshot;
		{
			std::lock_guard lock(Mutex);
			for (const PendingRequest &request : pending)
				if (auto entry = Entries.find(request.Id);
					entry != Entries.end() && !entry->second.CancelRequested && !entry->second.Terminal)
					entry->second.Preparing = true;
		}

		for (const PendingRequest &pendingRequest : pending) {
			const world::DataFactoryReply barrier = Session.RenderSnapshotBarrier(
				pendingRequest.Request.InstanceId, pendingRequest.Request.SnapshotId
			);
			if (barrier.Status != world::DataFactoryStatus::Ok) {
				std::lock_guard lock(Mutex);
				if (auto entry = Entries.find(pendingRequest.Id);
					entry != Entries.end() && !entry->second.CancelRequested) {
					entry->second.Reply.Status = "stale_snapshot";
					entry->second.Reply.SnapshotId = pendingRequest.Request.SnapshotId;
					entry->second.Detail = barrier.Detail;
					entry->second.Terminal = true;
					entry->second.Preparing = false;
				}
				continue;
			}
			std::optional<script::DataCaptureBridgeSceneSidecar> sceneSidecar;
			size_t sceneSidecarBytes = 0;
			if (pendingRequest.Request.IncludeSceneData) {
				script::DataSceneResult scene;
				const world::WorldStatus entered = Session.UniverseOf().Enter(
					Session.UniverseOf().Find(core::Name(pendingRequest.Request.InstanceId)),
					[&](ecs::Store &store) { scene = script::GetSceneSnapshot(store); }
				);
				if (entered != world::WorldStatus::Ok || scene.Status != std::string_view("ok") ||
					!script::DataSceneJsonResponseBudget(scene.Value, sceneSidecarBytes)) {
					std::lock_guard lock(Mutex);
					if (auto entry = Entries.find(pendingRequest.Id);
						entry != Entries.end() && !entry->second.CancelRequested) {
						entry->second.Reply.Status = "failed";
						entry->second.Reply.SnapshotId = pendingRequest.Request.SnapshotId;
						entry->second.Detail = "scene sidecar is unavailable or exceeds its byte limit";
						entry->second.Terminal = true;
						entry->second.Preparing = false;
					}
					continue;
				}
				sceneSidecar = script::DataCaptureBridgeSceneSidecar{
					.SnapshotId = pendingRequest.Request.SnapshotId,
					.Tick = barrier.Clock.Tick,
					.WorldEpoch = barrier.WorldEpoch,
					.WorldVersion = barrier.WorldVersion,
					.StorageProfile = pendingRequest.Request.StorageProfile,
					.Scene = std::move(scene.Value),
				};
			}
			bool releaseHook = false;
			{
				std::lock_guard lock(Mutex);
				const auto entry = Entries.find(pendingRequest.Id);
				if (entry == Entries.end() || entry->second.CancelRequested || entry->second.Terminal)
					continue;
			}
			DataCaptureRequest request{
				.SnapshotId = pendingRequest.Request.SnapshotId,
				.Pipeline = view.Pipeline,
				.CaptureNode = core::Name(pendingRequest.Request.CaptureNode),
				.ViewSlot = view.Slot,
				.Channels = {},
				.ObjectLabels = {view.ObjectLabels.begin(), view.ObjectLabels.end()},
				.SemanticLabels = {view.SemanticLabels.begin(), view.SemanticLabels.end()},
				.PartLabels = {view.PartLabels.begin(), view.PartLabels.end()},
				.LocalLightIds = pendingRequest.Request.LocalLightIds,
			};
			for (const std::string &name : pendingRequest.Request.Channels) {
				const auto channel = Channel(name);
				if (!channel) {
					std::lock_guard lock(Mutex);
					if (auto entry = Entries.find(pendingRequest.Id); entry != Entries.end()) {
						entry->second.Reply.Status = "invalid";
						entry->second.Reply.SnapshotId = pendingRequest.Request.SnapshotId;
						entry->second.Detail = "unknown capture channel";
						entry->second.Terminal = true;
						entry->second.Preparing = false;
					}
					continue;
				}
				request.Channels.push_back(*channel);
			}
			if (request.Channels.size() != pendingRequest.Request.Channels.size()) continue;
			const bool wantsObjectIds =
				std::ranges::find(request.Channels, DataCaptureChannel::ObjectIds) != request.Channels.end();
			const bool wantsSemantic =
				std::ranges::find(request.Channels, DataCaptureChannel::SemanticMask) !=
				request.Channels.end();
			const bool wantsPart =
				std::ranges::find(request.Channels, DataCaptureChannel::PartMask) != request.Channels.end();
			if (wantsObjectIds && !view.ObjectLabelsValid) {
				std::lock_guard lock(Mutex);
				if (auto entry = Entries.find(pendingRequest.Id); entry != Entries.end()) {
					entry->second.Reply.Status = "invalid";
					entry->second.Reply.SnapshotId = pendingRequest.Request.SnapshotId;
					entry->second.Detail = "object ids require unique bounded DataFactoryId values";
					entry->second.Terminal = true;
					entry->second.Preparing = false;
				}
				continue;
			}
			if ((wantsSemantic && !view.SemanticLabelsValid) || (wantsPart && !view.PartLabelsValid)) {
				std::lock_guard lock(Mutex);
				if (auto entry = Entries.find(pendingRequest.Id); entry != Entries.end()) {
					entry->second.Reply.Status = "invalid";
					entry->second.Reply.SnapshotId = pendingRequest.Request.SnapshotId;
					entry->second.Detail = "semantic or part ids require unique bounded authored labels";
					entry->second.Terminal = true;
					entry->second.Preparing = false;
				}
				continue;
			}
			std::array<HookHandle, MAX_CAPTURE_CHANNELS> hookHandles{};
			for (size_t index = 0; index < request.Channels.size(); ++index)
				hookHandles[index] = RendererRef.Hooks().FindHook(
					core::Name("data_capture." + std::string(DataCaptureChannelName(request.Channels[index])))
				);
			const auto described = RendererRef.DescribePipeline(
				view.Pipeline,
				view.Target != nullptr ? view.Target->Width : 1,
				view.Target != nullptr ? view.Target->Height : 1
			);
			const ConnectHooksResult connection =
				described ? RendererRef.Hooks().ConnectHooks(
								{.Session = {.WorldName = pendingRequest.Request.InstanceId},
								 .Pipeline = view.Pipeline,
								 .PipelineRevision = described->Revision,
								 .ViewSlot = view.Slot,
								 .CaptureNode = request.CaptureNode,
								 .ViewWidth = view.Target != nullptr ? view.Target->Width : 1,
								 .ViewHeight = view.Target != nullptr ? view.Target->Height : 1},
								std::span(hookHandles).first(request.Channels.size())
							)
						  : ConnectHooksResult{.Status = HookBindStatus::Stale, .Connection = {}};
			const auto batch = connection.Status == HookBindStatus::Ok
								   ? RendererRef.Hooks().ArmDataCapture(connection.Connection, request)
								   : std::optional<BatchHandle>{};
			const RenderObservationContext observation{
				.Pipeline = view.Pipeline,
				.Node = request.CaptureNode,
				.WorldName = view.WorldName,
				.ViewSlot = view.Slot,
				.SnapshotId = request.SnapshotId,
				.PipelineRevision = described ? described->Revision : 0,
				.Camera = {},
			};
			const CallHooksResult queued =
				batch ? RendererRef.Hooks().CallHooks(connection.Connection, *batch, observation)
					  : CallHooksResult{.Status = HookBindStatus::Backpressured, .Batch = {}};
			{
				std::lock_guard lock(Mutex);
				auto entry = Entries.find(pendingRequest.Id);
				if (entry == Entries.end() || entry->second.CancelRequested || entry->second.Terminal) {
					releaseHook = true;
				} else if (queued.Status != HookBindStatus::Ok) {
					releaseHook = true;
					entry->second.Reply.Status = "failed";
					entry->second.Reply.SnapshotId = pendingRequest.Request.SnapshotId;
					entry->second.Detail = "renderer capture hook backpressured";
					entry->second.Terminal = true;
					entry->second.Preparing = false;
				} else if (RetainedBytes > RETAINED_BYTE_LIMIT ||
						   sceneSidecarBytes > RETAINED_BYTE_LIMIT - RetainedBytes) {
					releaseHook = true;
					entry->second.Reply.Status = "failed";
					entry->second.Reply.SnapshotId = pendingRequest.Request.SnapshotId;
					entry->second.Detail = "scene sidecar exceeds retained byte quota";
					entry->second.Terminal = true;
					entry->second.Preparing = false;
				} else {
					entry->second.SceneSidecar = std::move(sceneSidecar);
					entry->second.SceneSidecarBytes = sceneSidecarBytes;
					RetainedBytes += sceneSidecarBytes;
					Hooks->Connections.emplace(pendingRequest.Id, connection.Connection);
					Hooks->Batches.emplace(pendingRequest.Id, *batch);
					entry->second.Preparing = false;
					view.SnapshotId = request.SnapshotId;
				}
			}
			if (releaseHook) {
				if (batch) RendererRef.Hooks().Cancel(*batch);
				if (connection.Status == HookBindStatus::Ok)
					RendererRef.Hooks().DisconnectHooks(std::array{connection.Connection});
			}
		}

		std::vector<std::pair<uint64_t, script::ViewCameraMutationRequest>> mutations;
		std::vector<MutationHandle> cancelledMutations;
		{
			std::lock_guard lock(Mutex);
			for (auto &[ticket, entry] : Mutations) {
				(void)ticket;
				if (entry.CancelRequested) continue;
				const auto &request = entry.Request;
				if (entry.Status == ViewMutationStatus::Pending && !entry.Handle.IsValid() &&
					request.InstanceId == view.WorldName.Text() &&
					request.Pipeline == pipeline->Name.Text() && request.ViewSlot == view.Slot &&
					request.SnapshotId == view.SnapshotId)
					mutations.emplace_back(ticket, request);
			}
		}
		std::sort(mutations.begin(), mutations.end(), [](const auto &left, const auto &right) {
			return left.first < right.first;
		});
		if (prepared != nullptr)
			for (const auto &[ticket, request] : mutations)
				prepared->CameraMutations.push_back(ticket);
		for (MutationHandle handle : cancelledMutations)
			RendererRef.Hooks().Cancel(handle);
		for (const auto &[ticket, request] : mutations) {
			const auto current = RendererRef.ResolvePipelineIdentity(core::Name(request.Pipeline));
			const world::DataFactoryReply barrier =
				Session.RenderSnapshotBarrier(request.InstanceId, request.SnapshotId);
			if (!current || current->Name.Text() != request.Pipeline ||
				current->Revision != request.PipelineRevision ||
				barrier.Status != world::DataFactoryStatus::Ok) {
				std::lock_guard lock(Mutex);
				if (auto entry = Mutations.find(ticket);
					entry != Mutations.end() && !entry->second.CancelRequested) {
					entry->second.Status = ViewMutationStatus::Stale;
					entry->second.Detail = current ? barrier.Detail : "render pipeline revision changed";
				}
				continue;
			}
			ViewCameraPatch patch;
			patch.Identity = {
				.WorldName = std::string(view.WorldName.Text()),
				.SnapshotId = view.SnapshotId,
				.Pipeline = core::Name(request.Pipeline),
				.PipelineRevision = request.PipelineRevision,
				.ViewSlot = view.Slot,
			};
			if (request.CameraFrame) {
				const auto &frame = *request.CameraFrame;
				patch.CameraFrame = core::CFrame(
					core::Vector3{frame[0], frame[1], frame[2]},
					glm::quat(frame[6], frame[3], frame[4], frame[5])
				);
			}
			if (request.Lens) {
				const auto &lens = *request.Lens;
				patch.Camera = view.Camera;
				patch.Camera->FieldOfViewRadians = lens.FieldOfViewRadians;
				patch.Camera->NearPlane = lens.NearPlane;
				patch.Camera->FarPlane = lens.FarPlane;
			}
			if (request.Projection) {
				glm::mat4 projection;
				for (size_t column = 0; column < 4; ++column)
					for (size_t row = 0; row < 4; ++row)
						projection[column][row] = (*request.Projection)[column * 4 + row];
				patch.Projection = projection;
			}
			const HookHandle hook = RendererRef.Hooks().FindHook(core::Name("view.camera"));
			const ArmViewMutationResult armed = RendererRef.Hooks().ArmViewMutation(hook, std::move(patch));
			bool cancelArmed = false;
			{
				std::lock_guard lock(Mutex);
				if (const auto entry = Mutations.find(ticket); entry != Mutations.end()) {
					if (entry->second.CancelRequested) {
						entry->second.Handle = armed.Mutation;
						cancelArmed = armed.Mutation.IsValid();
					} else if (armed.Status == HookBindStatus::Ok) {
						entry->second.Handle = armed.Mutation;
					} else {
						entry->second.Status = armed.Status == HookBindStatus::Stale
												   ? ViewMutationStatus::Stale
												   : ViewMutationStatus::Invalid;
						entry->second.Detail = "renderer refused view.camera patch";
					}
				}
			}
			if (cancelArmed) RendererRef.Hooks().Cancel(armed.Mutation);
		}
		return namedCameraApplied;
	}

	void ScriptDataCaptureBridge::AbortPreparedView(const PreparedView &prepared) {
		std::vector<ConnectionHandle> connections;
		std::vector<MutationHandle> mutations;
		{
			std::lock_guard lock(Mutex);
			for (const uint64_t ticket : prepared.Captures) {
				auto found = Entries.find(ticket);
				if (found == Entries.end()) continue;
				auto &[entryTicket, entry] = *found;
				entry.CancelRequested = true;
				if (Hooks) {
					if (const auto connection = Hooks->Connections.find(entryTicket);
						connection != Hooks->Connections.end()) {
						connections.push_back(connection->second);
						Hooks->Connections.erase(connection);
					}
					Hooks->Batches.erase(entryTicket);
				}
			}
			for (const uint64_t ticket : prepared.CameraMutations) {
				auto found = Mutations.find(ticket);
				if (found == Mutations.end()) continue;
				auto &[mutationTicket, entry] = *found;
				entry.CancelRequested = true;
				if (entry.Handle.IsValid()) mutations.push_back(entry.Handle);
			}
		}
		RendererRef.Hooks().DisconnectHooks(connections);
		for (MutationHandle mutation : mutations)
			RendererRef.Hooks().Cancel(mutation);
		Pump();
	}

	void ScriptDataCaptureBridge::Pump() {
		RefreshCapabilities();
		std::vector<ConnectionHandle> cancelled;
		std::vector<MutationHandle> cancelledMutations;
		{
			std::lock_guard lock(Mutex);
			for (auto &[id, entry] : Entries) {
				if (!entry.CancelRequested || entry.Terminal) continue;
				if (Hooks) {
					if (auto connection = Hooks->Connections.find(id);
						connection != Hooks->Connections.end()) {
						cancelled.push_back(connection->second);
						Hooks->Connections.erase(connection);
						Hooks->Batches.erase(id);
					}
				}
				entry.Reply.Status = "cancelled";
				entry.Reply.SnapshotId = entry.Request.SnapshotId;
				entry.Detail.clear();
				entry.Terminal = true;
				entry.Preparing = false;
			}
			for (auto &[ticket, entry] : Mutations) {
				(void)ticket;
				if (!entry.CancelRequested) continue;
				if (entry.Handle.IsValid())
					cancelledMutations.push_back(entry.Handle);
				else if (entry.Status == ViewMutationStatus::Pending) {
					entry.Status = ViewMutationStatus::Cancelled;
					entry.Detail.clear();
				}
			}
		}
		if (Hooks) RendererRef.Hooks().DisconnectHooks(cancelled);
		for (MutationHandle handle : cancelledMutations)
			RendererRef.Hooks().Cancel(handle);
		RendererRef.Hooks().Pump();
		std::vector<std::pair<uint64_t, MutationHandle>> mutationPolling;
		{
			std::lock_guard lock(Mutex);
			for (const auto &[ticket, entry] : Mutations)
				if (entry.Handle.IsValid()) mutationPolling.emplace_back(ticket, entry.Handle);
		}
		for (const auto &[ticket, handle] : mutationPolling) {
			const ViewMutationPoll state = RendererRef.Hooks().PollViewMutation(handle);
			if (state.Status == ViewMutationStatus::Pending ||
				state.Status == ViewMutationStatus::AppliedAwaitingRestore) {
				std::lock_guard lock(Mutex);
				if (auto entry = Mutations.find(ticket);
					entry != Mutations.end() && entry->second.Handle.Slot == handle.Slot &&
					entry->second.Handle.Generation == handle.Generation) {
					entry->second.Status = state.Status;
				}
				continue;
			}
			RendererRef.Hooks().ReleaseViewMutation(handle);
			std::lock_guard lock(Mutex);
			if (auto entry = Mutations.find(ticket); entry != Mutations.end() &&
													 entry->second.Handle.Slot == handle.Slot &&
													 entry->second.Handle.Generation == handle.Generation) {
				entry->second.Status = state.Status;
				entry->second.Handle = {};
				if (state.Status != ViewMutationStatus::Applied)
					entry->second.Detail = "renderer did not apply view.camera patch";
			}
		}
		if (!Hooks) return;

		std::vector<std::pair<uint64_t, BatchHandle>> polling;
		std::vector<ConnectionHandle> completedConnections;
		{
			std::lock_guard lock(Mutex);
			for (const auto &entry : Hooks->Batches)
				polling.emplace_back(entry.first, entry.second);
		}
		for (const auto &ticket : polling) {
			auto bundle = RendererRef.Hooks().TakeCompleted(ticket.second);
			if (!bundle) continue;
			DataCapturePoll captured = std::move(bundle->Capture);
			const auto finalizationStart = std::chrono::steady_clock::now();
			DataCaptureTicket validationTicket;
			std::string storageProfile;
			script::DataCaptureBridgeRequest storedRequest;
			{
				std::lock_guard lock(Mutex);
				const auto entry = Entries.find(ticket.first);
				if (entry == Entries.end()) continue;
				validationTicket.SnapshotId = entry->second.Request.SnapshotId;
				validationTicket.CaptureNode = core::Name(entry->second.Request.CaptureNode);
				storageProfile = entry->second.Request.StorageProfile;
				storedRequest = entry->second.Request;
				for (const std::string &name : entry->second.Request.Channels) {
					const auto channel = Channel(name);
					if (!channel) continue;
					if (*channel == DataCaptureChannel::LocalLightContribution ||
						*channel == DataCaptureChannel::LocalLightShadowVisibility)
						for (const std::string &lightId : entry->second.Request.LocalLightIds) {
							validationTicket.Channels.push_back(*channel);
							validationTicket.LightIds.push_back(lightId);
						}
					else {
						validationTicket.Channels.push_back(*channel);
						validationTicket.LightIds.emplace_back();
					}
				}
				Hooks->Batches.erase(ticket.first);
				if (const auto connection = Hooks->Connections.find(ticket.first);
					connection != Hooks->Connections.end()) {
					completedConnections.push_back(connection->second);
					Hooks->Connections.erase(connection);
				}
			}
			capture_record_validation::State validation;
			bool malformedPlane = false;
			for (const DataCapturePlane &plane : captured.Planes) {
				if (!capture_record_validation::Plane(validationTicket, plane, ticket.first, validation)) {
					malformedPlane = true;
					break;
				}
			}
			struct SourceDescriptor {
				std::string Resource;
				std::string Hash;
				uint32_t Width = 0;
				uint32_t Height = 0;
				uint32_t RowStride = 0;
				size_t ByteSize = 0;
				DataCaptureScalar Scalar = DataCaptureScalar::Unknown;
				DataCaptureColourSpace ColourSpace = DataCaptureColourSpace::Unknown;
				DataCaptureOrigin Origin = DataCaptureOrigin::TopLeft;
				std::string Provenance;
			};
			std::vector<SourceDescriptor> sources;
			sources.reserve(captured.Planes.size());
			for (const DataCapturePlane &plane : captured.Planes)
				sources.push_back(
					{std::string(plane.Resource.Text()),
					 plane.Hash.ToHex(),
					 plane.Width,
					 plane.Height,
					 plane.RowStride,
					 plane.Bytes.size(),
					 plane.Scalar,
					 plane.ColourSpace,
					 plane.Origin,
					 plane.Provenance}
				);
			std::vector<bool> compacted(captured.Planes.size());
			std::vector<std::optional<double>> compactErrors(captured.Planes.size());
			std::vector<std::string> classifications(captured.Planes.size(), "not_inspected");
			std::vector<std::optional<script::DataCaptureBridgeNoise>> noises(captured.Planes.size());
			if (!malformedPlane && storageProfile == "training_compact" &&
				(captured.Status == DataCaptureStatus::Ready ||
				 captured.Status == DataCaptureStatus::Partial)) {
				bool rejectSecondSurfacePair = false;
				for (size_t index = 0; index < captured.Planes.size(); ++index) {
					DataCapturePlane &plane = captured.Planes[index];
					if (plane.Status != DataCaptureStatus::Ready || !Compactible(plane.Channel)) continue;
					std::optional<double> error;
					std::string rejection;
					if (CompactDepthPlane(plane, error, classifications[index], rejection)) {
						compacted[index] = true;
						compactErrors[index] = error;
						continue;
					}
					rejectSecondSurfacePair =
						rejectSecondSurfacePair || plane.Channel == DataCaptureChannel::SecondSurfaceDepth;
					CompactUnavailable(plane, rejection);
				}
				if (rejectSecondSurfacePair)
					for (DataCapturePlane &plane : captured.Planes)
						if (plane.Channel == DataCaptureChannel::SecondSurfaceValidity)
							CompactUnavailable(plane, "second_surface_depth_rejected");
			}
			// Storage transforms first. The RGB noise then operates on exactly the
			// bytes this ticket retains, while every source descriptor stays native.
			if (!malformedPlane && (captured.Status == DataCaptureStatus::Ready ||
									captured.Status == DataCaptureStatus::Partial))
				for (size_t index = 0; index < captured.Planes.size(); ++index)
					noises[index] = ApplyNoise(captured.Planes[index], storedRequest);
			const size_t readyPlanes = std::count_if(
				captured.Planes.begin(), captured.Planes.end(), [](const DataCapturePlane &plane) {
					return plane.Status == DataCaptureStatus::Ready;
				}
			);
			if ((storageProfile == "training_compact" || storedRequest.NoiseMode != "none") &&
				!malformedPlane &&
				(captured.Status == DataCaptureStatus::Ready ||
				 captured.Status == DataCaptureStatus::Partial)) {
				captured.Status = readyPlanes == captured.Planes.size() ? DataCaptureStatus::Ready
								  : readyPlanes != 0					? DataCaptureStatus::Partial
																		: DataCaptureStatus::Unsupported;
			}
			script::DataCaptureBridgePoll reply;
			reply.Status = Status(captured.Status);
			reply.StorageProfile = storageProfile;
			reply.SnapshotId = captured.SnapshotId;
			reply.CaptureFrame = captured.CaptureFrame;
			reply.Profile.CpuReadbackNanoseconds = captured.CpuReadbackNanoseconds;
			reply.Profile.HostReadbackReservedCapacityBytes = captured.HostReadbackReservedCapacityBytes;
			reply.Profile.DeviceReadbackStagingReservedCapacityBytes =
				captured.DeviceReadbackStagingReservedCapacityBytes;
			reply.Profile.GpuNanoseconds = captured.GpuNanoseconds;
			reply.Profile.GpuTimingReason = captured.GpuTimingReason;
			for (const SourceDescriptor &source : sources)
				if (source.ByteSize != 0) {
					reply.Profile.SourceBytes += source.ByteSize;
					reply.Profile.ReadbackBytes += source.ByteSize;
					++reply.Profile.SourceOperations;
					++reply.Profile.ReadbackOperations;
				}
			if (captured.Status == DataCaptureStatus::Ready || captured.Status == DataCaptureStatus::Partial)
				CopyCamera(captured, reply);
			std::unordered_map<std::string, std::vector<std::byte>> bytes;
			struct PackedOutput {
				script::DataCaptureBridgePackedPlane Definition;
				data_capture_packing::Result Image;
				std::string Rejection;
			};
			std::vector<PackedOutput> packedOutputs;
			packedOutputs.reserve(storedRequest.PackedPlanes.size());
			size_t nativeBytes = 0;
			for (const DataCapturePlane &plane : captured.Planes) {
				if (nativeBytes > RETAINED_BYTE_LIMIT ||
					plane.Bytes.size() > RETAINED_BYTE_LIMIT - nativeBytes) {
					nativeBytes = RETAINED_BYTE_LIMIT + 1;
					break;
				}
				nativeBytes += plane.Bytes.size();
			}
			size_t packedBudget = nativeBytes <= RETAINED_BYTE_LIMIT ? RETAINED_BYTE_LIMIT - nativeBytes : 0;
			for (const script::DataCaptureBridgePackedPlane &definition : storedRequest.PackedPlanes) {
				std::array<const DataCapturePlane *, 4> sourcePlanes{};
				bool missingSource = false;
				bool compactedDepth = false;
				for (size_t lane = 0; lane < definition.Components.size(); ++lane) {
					const auto match =
						std::ranges::find_if(captured.Planes, [&](const DataCapturePlane &plane) {
							return definition.Components[lane].SourceChannel ==
								   DataCaptureChannelName(plane.Channel);
						});
					if (match == captured.Planes.end()) {
						missingSource = true;
						break;
					}
					sourcePlanes[lane] = &*match;
					if (match->Channel == DataCaptureChannel::LinearDepth &&
						match->Scalar != DataCaptureScalar::Float32)
						compactedDepth = true;
				}
				PackedOutput output{.Definition = definition, .Image = {}, .Rejection = {}};
				const size_t pixels =
					sourcePlanes[0] == nullptr
						? 0
						: static_cast<size_t>(sourcePlanes[0]->Width) * sourcePlanes[0]->Height;
				if (missingSource)
					output.Rejection = "requested_source_missing";
				else if (compactedDepth)
					output.Rejection = "linear_depth_requires_float32_source";
				else if (pixels > SIZE_MAX / 16 || pixels * 16 > packedBudget)
					output.Rejection = "retained_byte_quota";
				else
					(void)data_capture_packing::PackRgba32F(
						sourcePlanes, definition.Components, output.Image, output.Rejection
					);
				if (output.Rejection.empty()) packedBudget -= output.Image.Bytes.size();
				packedOutputs.push_back(std::move(output));
			}
			if (std::ranges::any_of(
					packedOutputs, [](const PackedOutput &output) { return !output.Rejection.empty(); }
				) &&
				reply.Status == "ready")
				reply.Status = "partial";
			bool objectIdsReady = false;
			bool semanticIdsReady = false;
			bool partIdsReady = false;
			size_t totalBytes = 0;
			for (size_t index = 0; index < captured.Planes.size(); ++index) {
				DataCapturePlane &plane = captured.Planes[index];
				const std::string resource = ResourceId(ticket.first, plane.Channel, plane.LightId);
				const std::string channel(DataCaptureChannelName(plane.Channel));
				const std::string &source = sources[index].Resource;
				const std::string hash = plane.Hash.ToHex();
				reply.Planes.push_back(
					{.Channel = channel,
					 .LightId = plane.LightId,
					 .Status = Status(plane.Status),
					 .Resource = resource,
					 .SourceResource = source,
					 .HashAlgorithm = "blake3-256",
					 .Hash = hash,
					 .Width = plane.Width,
					 .Height = plane.Height,
					 .RowStride = plane.RowStride,
					 .ByteSize = plane.Bytes.size(),
					 .Scalar = Scalar(plane.Scalar),
					 .SourceScalar = Scalar(sources[index].Scalar),
					 .SourceHash = std::move(sources[index].Hash),
					 .SourceWidth = sources[index].Width,
					 .SourceHeight = sources[index].Height,
					 .SourceRowStride = sources[index].RowStride,
					 .SourceByteSize = sources[index].ByteSize,
					 .SourceEncoding = Encoding(sources[index].Scalar),
					 .SourceColourSpace = ColourSpace(sources[index].ColourSpace),
					 .SourceOrigin = "top_left",
					 .SourcePacking = Packing(plane.Channel, sources[index].Scalar),
					 .SourceProvenance = std::move(sources[index].Provenance),
					 .ValueClassification = classifications[index],
					 .Encoding = Encoding(plane.Scalar),
					 .MaximumAbsoluteError = compacted[index] ? compactErrors[index] : std::nullopt,
					 .ColourSpace = ColourSpace(plane.ColourSpace),
					 .Origin = "top_left",
					 .Packing = Packing(plane.Channel, plane.Scalar),
					 .Provenance = plane.Provenance.empty() ? Provenance(plane.Channel) : plane.Provenance,
					 .AmbientOcclusion = CopyAmbientOcclusion(plane.AmbientOcclusion),
					 .Noise = std::move(noises[index]),
					 .PreviousCameraMotionFrame = plane.PreviousCameraMotionFrame,
					 .Packed = {},
					 .Resampling = {}}
				);
				if (!plane.Bytes.empty()) {
					if (totalBytes > RETAINED_BYTE_LIMIT ||
						plane.Bytes.size() > RETAINED_BYTE_LIMIT - totalBytes)
						totalBytes = RETAINED_BYTE_LIMIT + 1;
					else
						totalBytes += plane.Bytes.size();
					bytes.emplace(resource, std::move(plane.Bytes));
				}
				if (plane.Status == DataCaptureStatus::Ready) {
					objectIdsReady = objectIdsReady || plane.Channel == DataCaptureChannel::ObjectIds;
					semanticIdsReady = semanticIdsReady || plane.Channel == DataCaptureChannel::SemanticMask;
					partIdsReady = partIdsReady || plane.Channel == DataCaptureChannel::PartMask;
				}
			}
			for (PackedOutput &output : packedOutputs) {
				const std::string resource = PackedResourceId(ticket.first, output.Definition.Name);
				const bool ready = output.Rejection.empty();
				const assets::ContentHash hash =
					ready ? assets::Hasher::Of(output.Image.Bytes) : assets::ContentHash{};
				reply.Planes.push_back(
					{.Channel = "packed/" + output.Definition.Name,
					 .Status = ready ? "ready" : "unsupported",
					 .Resource = resource,
					 .SourceResource = {},
					 .HashAlgorithm = "blake3-256",
					 .Hash = hash.ToHex(),
					 .Width = output.Image.Width,
					 .Height = output.Image.Height,
					 .RowStride = output.Image.Width * 16,
					 .ByteSize = output.Image.Bytes.size(),
					 .Scalar = "float32",
					 .SourceScalar = "mixed",
					 .SourceHash = {},
					 .SourceEncoding = "mixed",
					 .SourceColourSpace = "mixed",
					 .SourceOrigin = "top_left",
					 .SourcePacking = "explicit_component_mapping",
					 .SourceProvenance = "completed_capture_planes/v1",
					 .ValueClassification = "not_inspected",
					 .Encoding = "ieee754_binary32_le",
					 .MaximumAbsoluteError = {},
					 .ColourSpace = "not_applicable",
					 .Origin = "top_left",
					 .Packing = "rgba32_float",
					 .Provenance = ready ? "derived/explicit_channel_pack_rgba32f/v1"
										 : "unavailable/explicit_channel_pack_" + output.Rejection + "/v1",
					 .AmbientOcclusion = {},
					 .Noise = {},
					 .PreviousCameraMotionFrame = {},
					 .Packed = output.Definition,
					 .Resampling = "pixel_center_nearest/v1"}
				);
				if (!ready) continue;
				if (totalBytes > RETAINED_BYTE_LIMIT ||
					output.Image.Bytes.size() > RETAINED_BYTE_LIMIT - totalBytes)
					totalBytes = RETAINED_BYTE_LIMIT + 1;
				else
					totalBytes += output.Image.Bytes.size();
				bytes.emplace(resource, std::move(output.Image.Bytes));
			}
			if (objectIdsReady)
				for (const DataCaptureObjectLabel &label : captured.ObjectLabels)
					reply.ObjectLabels.push_back({label.Label, label.StableId});
			if (semanticIdsReady)
				for (const DataCaptureSemanticLabel &label : captured.SemanticLabels)
					reply.SemanticLabels.push_back({label.Label, label.StableId});
			if (partIdsReady)
				for (const DataCapturePartLabel &label : captured.PartLabels)
					reply.PartLabels.push_back({label.Label, label.StableId});
			reply.Profile.RetainedBytes = totalBytes;
			reply.Profile.RetainedOperations = static_cast<uint64_t>(bytes.size());
			const uint64_t finalizationNanoseconds =
				static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
										  std::chrono::steady_clock::now() - finalizationStart
				)
										  .count());
			reply.Profile.CpuFinalizationNanoseconds = finalizationNanoseconds;
			if (finalizationNanoseconds != 0)
				reply.Profile.FinalizationBytesPerSecond = static_cast<double>(totalBytes) * 1'000'000'000.0 /
														   static_cast<double>(finalizationNanoseconds);
			const bool coherentStatus =
				(captured.Status == DataCaptureStatus::Ready &&
				 readyPlanes == validationTicket.Channels.size() &&
				 validation.Channels.size() == validationTicket.Channels.size()) ||
				(captured.Status == DataCaptureStatus::Partial && readyPlanes > 0 &&
				 readyPlanes < validationTicket.Channels.size() &&
				 validation.Channels.size() == validationTicket.Channels.size()) ||
				((captured.Status == DataCaptureStatus::Unsupported ||
				  captured.Status == DataCaptureStatus::Invalid ||
				  captured.Status == DataCaptureStatus::Failed ||
				  captured.Status == DataCaptureStatus::Cancelled) &&
				 readyPlanes == 0 && validation.Channels.size() == validationTicket.Channels.size());
			malformedPlane = malformedPlane || !coherentStatus;
			{
				std::lock_guard lock(Mutex);
				const auto entry = Entries.find(ticket.first);
				if (entry != Entries.end() && !entry->second.Terminal) {
					if (entry->second.SceneSidecarBytes != 0) {
						reply.Profile.RetainedBytes += entry->second.SceneSidecarBytes;
						++reply.Profile.RetainedOperations;
					}
					entry->second.Reply.Profile = reply.Profile;
					if (entry->second.CancelRequested) {
						entry->second.Reply.Status = "cancelled";
						entry->second.Reply.SnapshotId = entry->second.Request.SnapshotId;
						entry->second.Reply.Profile.RetainedBytes = entry->second.SceneSidecarBytes;
						entry->second.Reply.Profile.RetainedOperations =
							entry->second.SceneSidecarBytes == 0 ? 0 : 1;
						entry->second.Detail.clear();
					} else if (captured.SnapshotId != entry->second.Request.SnapshotId) {
						entry->second.Reply.Status = "stale_snapshot";
						entry->second.Reply.SnapshotId = entry->second.Request.SnapshotId;
						entry->second.Reply.Profile.RetainedBytes = entry->second.SceneSidecarBytes;
						entry->second.Reply.Profile.RetainedOperations =
							entry->second.SceneSidecarBytes == 0 ? 0 : 1;
						entry->second.Detail = "renderer returned a different snapshot";
					} else if (malformedPlane) {
						entry->second.Reply.Status = "failed";
						entry->second.Reply.SnapshotId = captured.SnapshotId;
						entry->second.Reply.Profile.RetainedBytes = entry->second.SceneSidecarBytes;
						entry->second.Reply.Profile.RetainedOperations =
							entry->second.SceneSidecarBytes == 0 ? 0 : 1;
						entry->second.Detail = "renderer returned a malformed capture plane";
					} else if (RetainedBytes > RETAINED_BYTE_LIMIT ||
							   totalBytes > RETAINED_BYTE_LIMIT - RetainedBytes) {
						entry->second.Reply.Status = "failed";
						entry->second.Reply.SnapshotId = captured.SnapshotId;
						entry->second.Reply.Profile.RetainedBytes = entry->second.SceneSidecarBytes;
						entry->second.Reply.Profile.RetainedOperations =
							entry->second.SceneSidecarBytes == 0 ? 0 : 1;
						entry->second.Detail = "capture exceeds retained byte quota";
					} else {
						RetainedBytes += totalBytes;
						if (captured.Status == DataCaptureStatus::Ready ||
							captured.Status == DataCaptureStatus::Partial) {
							// Reply becomes the sidecar owner. The entry retains only its
							// reserved-byte marker until Release or Teardown.
							reply.SceneSidecar = std::move(entry->second.SceneSidecar);
						}
						entry->second.Reply = std::move(reply);
						entry->second.PlaneBytes = std::move(bytes);
						entry->second.Detail.clear();
					}
					entry->second.Terminal = true;
				}
			}
		}
		RendererRef.Hooks().DisconnectHooks(completedConnections);
	}

	void ScriptDataCaptureBridge::CancelPending() {
		std::vector<MutationHandle> mutations;
		{
			std::lock_guard lock(Mutex);
			for (auto &[ticket, entry] : Entries) {
				(void)ticket;
				if (!entry.Terminal) entry.CancelRequested = true;
			}
			for (auto &[ticket, entry] : Mutations) {
				(void)ticket;
				if (entry.Status != ViewMutationStatus::Pending &&
					entry.Status != ViewMutationStatus::AppliedAwaitingRestore)
					continue;
				if (!entry.Handle.IsValid()) {
					entry.Status = ViewMutationStatus::Cancelled;
					continue;
				}
				mutations.push_back(entry.Handle);
				entry.Handle = {};
				entry.Status = ViewMutationStatus::Cancelled;
				entry.CancelRequested = false;
			}
		}
		// Applied patches normally wait for a following view to restore. Shutdown
		// has no following view, so retire those handles through the teardown path.
		for (MutationHandle mutation : mutations)
			RendererRef.Hooks().DiscardViewMutation(mutation);
	}

	bool ScriptDataCaptureBridge::HasPending() const {
		std::lock_guard lock(Mutex);
		const bool captures = std::any_of(Entries.begin(), Entries.end(), [](const auto &entry) {
			return !entry.second.Terminal;
		});
		return captures || std::any_of(Mutations.begin(), Mutations.end(), [](const auto &entry) {
				   return entry.second.Status == ViewMutationStatus::Pending ||
						  entry.second.Status == ViewMutationStatus::AppliedAwaitingRestore;
			   });
	}

	bool ScriptDataCaptureBridge::HasOutstanding() const {
		std::lock_guard lock(Mutex);
		return !Entries.empty() || !Mutations.empty();
	}

	bool ScriptDataCaptureBridge::DiscardTerminal() {
		std::lock_guard lock(Mutex);
		if (std::any_of(
				Entries.begin(), Entries.end(), [](const auto &entry) { return !entry.second.Terminal; }
			) ||
			std::any_of(Mutations.begin(), Mutations.end(), [](const auto &entry) {
				return entry.second.Status == ViewMutationStatus::Pending ||
					   entry.second.Status == ViewMutationStatus::AppliedAwaitingRestore ||
					   entry.second.Handle.IsValid();
			}))
			return false;
		Entries.clear();
		Mutations.clear();
		RetainedBytes = 0;
		return true;
	}
}
