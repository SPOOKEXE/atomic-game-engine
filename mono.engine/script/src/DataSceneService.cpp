#include <engine/core/Name.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Clock.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Query.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/EditableImage.hpp>
#include <engine/scene/Enums.hpp>
#include <engine/scene/Skinning.hpp>
#include <engine/scene/SurfaceTable.hpp>
#include <engine/script/DataCaptureBridge.hpp>
#include <engine/script/DataCaptureDriver.hpp>
#include <engine/script/DataLifecycleBridge.hpp>
#include <engine/script/DataSceneService.hpp>
#include <engine/script/ScriptCall.hpp>
#include <engine/script/ServiceSurface.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace engine::script {
	namespace {
		ScriptValue String(std::string_view text) {
			ScriptValue value(ValueTag::String);
			value.Text = text;
			return value;
		}

		ScriptValue Number(double number) {
			ScriptValue value(ValueTag::Number);
			value.Number = number;
			return value;
		}

		ScriptValue Boolean(bool boolean) {
			ScriptValue value(boolean ? ValueTag::True : ValueTag::False);
			value.Boolean = boolean;
			return value;
		}

		ScriptValue Frame(const core::CFrame &frame) {
			ScriptValue value(ValueTag::CFrame);
			value.Frame = frame;
			return value;
		}

		ScriptValue Vector(const core::Vector3 &vector) {
			ScriptValue value(ValueTag::Vector3);
			value.Vector = vector;
			return value;
		}

		ScriptValue Colour(const core::Color3 &colour) {
			ScriptValue value(ValueTag::Color3);
			value.Colour = colour;
			return value;
		}

		const char *LightKindName(scene::LightKind kind) {
			switch (kind) {
			case scene::LightKind::Point:
				return "point";
			case scene::LightKind::Spot:
				return "spot";
			case scene::LightKind::Surface:
				return "surface";
			}
			return "unknown";
		}

		ScriptValue Map(std::vector<std::pair<std::string, ScriptValue>> entries) {
			ScriptValue value(ValueTag::Map);
			value.Entries = std::move(entries);
			return value;
		}

		ScriptValue Array(std::vector<ScriptValue> items) {
			ScriptValue value(ValueTag::Array);
			value.Items = std::move(items);
			return value;
		}

		bool StableId(const ecs::Store &store, ecs::Entity entity, std::string &out);

		std::string Decimal(uint64_t value) {
			std::array<char, 32> text{};
			const auto converted = std::to_chars(text.data(), text.data() + text.size(), value);
			return std::string(text.data(), converted.ptr);
		}

		const ScriptValue *Field(const ScriptValue &value, std::string_view name) {
			if (value.Tag != ValueTag::Map) return nullptr;
			for (const auto &[key, item] : value.Entries)
				if (key == name) return &item;
			return nullptr;
		}

		bool
		BoundedStringField(const ScriptValue &value, std::string_view name, size_t limit, std::string &out) {
			const ScriptValue *field = Field(value, name);
			if (field == nullptr || field->Tag != ValueTag::String || field->Text.empty() ||
				field->Text.size() > limit || field->Text.find('\0') != std::string::npos)
				return false;
			out = field->Text;
			return true;
		}

		bool HasOnlyFields(const ScriptValue &value, std::initializer_list<std::string_view> allowed) {
			if (value.Tag != ValueTag::Map) return false;
			for (size_t index = 0; index < value.Entries.size(); index++) {
				const std::string &key = value.Entries[index].first;
				if (key.find('\0') != std::string::npos ||
					std::find(allowed.begin(), allowed.end(), key) == allowed.end())
					return false;
				for (size_t previous = 0; previous < index; previous++)
					if (value.Entries[previous].first == key) return false;
			}
			return true;
		}

		bool Ticket(std::string_view text, uint64_t &out) {
			if (text.empty() || text.size() > 20 || text.find('\0') != std::string_view::npos) return false;
			const auto converted = std::from_chars(text.data(), text.data() + text.size(), out);
			return converted.ec == std::errc{} && converted.ptr == text.data() + text.size();
		}

		bool VectorField(const ScriptValue &value, std::string_view name, core::Vector3 &out) {
			const ScriptValue *field = Field(value, name);
			if (field == nullptr || field->Tag != ValueTag::Vector3 || !std::isfinite(field->Vector.X) ||
				!std::isfinite(field->Vector.Y) || !std::isfinite(field->Vector.Z))
				return false;
			out = field->Vector;
			return true;
		}

		ScriptValue QueryIds(
			const ecs::Store &store, std::span<const ecs::Entity> entities, size_t count, bool overflowed
		) {
			std::vector<ScriptValue> records;
			records.reserve(count);
			for (size_t index = 0; index < count; index++) {
				std::string id;
				if (StableId(store, entities[index], id)) records.push_back(String(id));
			}
			return Map({
				{"status", String("ok")},
				{"provenance", String("physics_exact_collider_query")},
				{"ids", Array(std::move(records))},
				{"overflowed", Boolean(overflowed)},
			});
		}

		DataSceneResult QueryAabb(const ecs::Store &store, const ScriptValue &value) {
			core::Vector3 minimum;
			core::Vector3 maximum;
			if (!HasOnlyFields(value, {"minimum", "maximum"}) || !VectorField(value, "minimum", minimum) ||
				!VectorField(value, "maximum", maximum) || minimum.X > maximum.X || minimum.Y > maximum.Y ||
				minimum.Z > maximum.Z)
				return {"invalid_argument", Map({{"status", String("invalid_aabb_query")}})};
			std::array<ecs::Entity, 64> found;
			const spatial::QueryResult result =
				physics::OverlapBox(store, core::AABB{minimum, maximum}, spatial::LayerMask::All(), found);
			return {"ok", QueryIds(store, found, result.Written, result.Overflowed)};
		}

		DataSceneResult QueryObb(const ecs::Store &store, const ScriptValue &value) {
			const ScriptValue *frame = Field(value, "frame");
			core::Vector3 halfExtent;
			if (!HasOnlyFields(value, {"frame", "half_extent"}) || frame == nullptr ||
				frame->Tag != ValueTag::CFrame || !VectorField(value, "half_extent", halfExtent) ||
				!(halfExtent.X >= 0.0f) || !(halfExtent.Y >= 0.0f) || !(halfExtent.Z >= 0.0f))
				return {"invalid_argument", Map({{"status", String("invalid_obb_query")}})};
			std::array<ecs::Entity, 64> found;
			const spatial::QueryResult result = physics::OverlapOrientedBox(
				store, frame->Frame, halfExtent, spatial::LayerMask::All(), found
			);
			ScriptValue answer = QueryIds(store, found, result.Written, result.Overflowed);
			answer.Entries.emplace_back("provenance", String("physics_exact_oriented_box_query"));
			return {"ok", std::move(answer)};
		}

		DataSceneResult QueryRay(const ecs::Store &store, const ScriptValue &value) {
			core::Vector3 origin;
			core::Vector3 direction;
			const ScriptValue *distance = Field(value, "max_distance_metres");
			if (!HasOnlyFields(value, {"origin", "direction", "max_distance_metres"}) ||
				!VectorField(value, "origin", origin) || !VectorField(value, "direction", direction) ||
				distance == nullptr || distance->Tag != ValueTag::Number ||
				!std::isfinite(distance->Number) || distance->Number <= 0.0 || distance->Number > 100'000.0)
				return {"invalid_argument", Map({{"status", String("invalid_raycast_query")}})};
			const auto hit =
				physics::Raycast(store, core::Ray{origin, direction}, static_cast<float>(distance->Number));
			if (!hit) return {"ok", Map({{"status", String("ok")}, {"hit", Boolean(false)}})};
			std::string id;
			const bool hasId = StableId(store, hit->Owner, id);
			return {
				"ok",
				Map({
					{"status", String("ok")},
					{"provenance", String("physics_exact_collider_raycast")},
					{"hit", Boolean(true)},
					{"id", String(hasId ? id : "")},
					{"has_stable_id", Boolean(hasId)},
					{"distance_metres", Number(hit->Distance)},
					{"position_metres", Vector(hit->Position)},
					{"normal", Vector(hit->Normal)},
				})
			};
		}

		DataSceneResult CaptureUnavailable() {
			return {
				"unsupported",
				Map({
					{"status", String("capability_unsupported")},
					{"feature", String("render_capture")},
					{"channels", Array({})},
					{"reason", String("no runtime-scoped render capture bridge installed")},
				})
			};
		}

		DataSceneResult CaptureChannels(const std::shared_ptr<DataCaptureBridge> &bridge) {
			if (bridge == nullptr) return CaptureUnavailable();
			const DataCaptureBridgeCapabilities capabilities = bridge->Capabilities();
			std::vector<ScriptValue> channels;
			channels.reserve(capabilities.Channels.size());
			for (const std::string &channel : capabilities.Channels)
				channels.push_back(String(channel));
			return {
				capabilities.Available ? "ok" : "unsupported",
				Map({
					{"status", String(capabilities.Available ? "ok" : "capability_unsupported")},
					{"channels", Array(std::move(channels))},
					{"reason", String(capabilities.Detail)},
				})
			};
		}

		DataSceneResult QueueCapture(
			const std::shared_ptr<DataCaptureBridge> &bridge,
			std::string_view worldName,
			const ScriptValue &value
		) {
			if (bridge == nullptr) return CaptureUnavailable();
			DataCaptureBridgeRequest request;
			if (worldName.empty() || worldName.size() > 256 || value.Tag != ValueTag::Map ||
				!BoundedStringField(value, "snapshot_id", 256, request.SnapshotId) ||
				!BoundedStringField(value, "pipeline", 256, request.Pipeline) ||
				!BoundedStringField(value, "capture_node", 256, request.CaptureNode) ||
				!BoundedStringField(value, "temporal_history", 32, request.TemporalHistory)) {
				return {"invalid_argument", Map({{"status", String("invalid_capture_request")}})};
			}
			request.InstanceId = worldName;
			const ScriptValue *slot = Field(value, "view_slot");
			if (slot == nullptr || slot->Tag != ValueTag::Number || !std::isfinite(slot->Number) ||
				!(slot->Number >= 0.0) ||
				slot->Number > static_cast<double>(std::numeric_limits<uint32_t>::max()) ||
				static_cast<double>(static_cast<uint64_t>(slot->Number)) != slot->Number) {
				return {"invalid_argument", Map({{"status", String("invalid_capture_request")}})};
			}
			request.ViewSlot = static_cast<uint64_t>(slot->Number);
			const ScriptValue *channels = Field(value, "channels");
			if (channels == nullptr || channels->Tag != ValueTag::Array || channels->Items.empty() ||
				channels->Items.size() > 12)
				return {"invalid_argument", Map({{"status", String("invalid_capture_request")}})};
			request.Channels.reserve(channels->Items.size());
			for (const ScriptValue &channel : channels->Items) {
				if (channel.Tag != ValueTag::String || channel.Text.empty() || channel.Text.size() > 64)
					return {"invalid_argument", Map({{"status", String("invalid_capture_request")}})};
				if (std::find(request.Channels.begin(), request.Channels.end(), channel.Text) !=
					request.Channels.end())
					return {"invalid_argument", Map({{"status", String("invalid_capture_request")}})};
				request.Channels.push_back(channel.Text);
			}
			uint64_t ticket = 0;
			std::string detail;
			if (!bridge->Queue(worldName, request, ticket, detail))
				return {
					"rejected", Map({{"status", String("capture_rejected")}, {"reason", String(detail)}})
				};
			return {
				"ok",
				Map({
					{"status", String("queued")},
					{"ticket", String(Decimal(ticket))},
					{"detail", String(detail)},
				})
			};
		}

		ScriptValue Matrix(const std::array<double, 16> &matrix) {
			std::vector<ScriptValue> values;
			values.reserve(matrix.size());
			for (double value : matrix)
				values.push_back(Number(value));
			return Array(std::move(values));
		}

		DataSceneResult PollCapture(
			const std::shared_ptr<DataCaptureBridge> &bridge,
			std::string_view worldName,
			std::string_view text
		) {
			if (bridge == nullptr) return CaptureUnavailable();
			uint64_t ticket = 0;
			if (!Ticket(text, ticket))
				return {"invalid_argument", Map({{"status", String("invalid_capture_ticket")}})};
			DataCaptureBridgePoll poll;
			std::string detail;
			if (!bridge->Poll(worldName, ticket, poll, detail))
				return {
					"unknown", Map({{"status", String("unknown_capture_ticket")}, {"reason", String(detail)}})
				};
			std::vector<ScriptValue> planes;
			planes.reserve(poll.Planes.size());
			for (const DataCaptureBridgePlane &plane : poll.Planes) {
				planes.push_back(Map({
					{"channel", String(plane.Channel)},
					{"status", String(plane.Status)},
					{"resource", String(plane.Resource)},
					{"source_resource", String(plane.SourceResource)},
					{"hash_algorithm", String(plane.HashAlgorithm)},
					{"hash", String(plane.Hash)},
					{"width", Number(plane.Width)},
					{"height", Number(plane.Height)},
					{"row_stride", Number(plane.RowStride)},
					{"scalar", String(plane.Scalar)},
					{"color_space", String(plane.ColourSpace)},
					{"origin", String(plane.Origin)},
				}));
			}
			std::vector<std::pair<std::string, ScriptValue>> entries{
				{"status", String(poll.Status)},
				{"snapshot_id", String(poll.SnapshotId)},
				{"capture_frame", String(Decimal(poll.CaptureFrame))},
				{"planes", Array(std::move(planes))},
				{"detail", String(detail)},
			};
			if (poll.HasCamera) {
				std::vector<std::pair<std::string, ScriptValue>> camera{
					{"world_from_camera", Matrix(poll.WorldFromCamera)},
					{"vertical_fov_radians", Number(poll.VerticalFieldOfViewRadians)},
					{"near_metres", Number(poll.NearMetres)},
					{"far_metres", Number(poll.FarMetres)},
					{"crop",
					 Array(
						 {Number(poll.CropLeft),
						  Number(poll.CropTop),
						  Number(poll.CropWidth),
						  Number(poll.CropHeight)}
					 )},
					{"crop_convention", String(poll.CropConvention)},
					{"lens_distortion_available", Boolean(poll.LensDistortionAvailable)},
					{"lens_distortion_reason", String(poll.LensDistortionReason)},
					{"jitter_available", Boolean(poll.JitterAvailable)},
					{"jitter_policy", String(poll.JitterPolicy)},
					{"coordinate_convention", String(poll.CoordinateConvention)},
				};
				if (poll.HasProjection) camera.emplace_back("projection", Matrix(poll.Projection));
				entries.emplace_back("camera", Map(std::move(camera)));
			}
			return {"ok", Map(std::move(entries))};
		}

		DataSceneResult CancelCapture(
			const std::shared_ptr<DataCaptureBridge> &bridge,
			std::string_view worldName,
			std::string_view text
		) {
			if (bridge == nullptr) return CaptureUnavailable();
			uint64_t ticket = 0;
			if (!Ticket(text, ticket))
				return {"invalid_argument", Map({{"status", String("invalid_capture_ticket")}})};
			bridge->Cancel(worldName, ticket);
			return {
				"ok", Map({{"status", String("cancellation_requested")}, {"ticket", String(Decimal(ticket))}})
			};
		}

		void ReadCaptureBuffer(ScriptCall &call) {
			const auto bridge = call.DataCapture();
			if (bridge == nullptr) {
				call.Raise("render capture is unavailable");
			}
			uint64_t ticket = 0;
			if (!Ticket(call.AsString(0), ticket)) call.Raise("invalid capture ticket");
			const std::string resource = call.AsString(1);
			const double requestedOffset = call.AsNumber(2);
			const double requestedMaximum = call.AsNumber(3);
			if (!std::isfinite(requestedOffset) || !std::isfinite(requestedMaximum) ||
				requestedOffset < 0.0 || requestedMaximum <= 0.0 ||
				requestedOffset > static_cast<double>(std::numeric_limits<size_t>::max()) ||
				requestedMaximum > 64.0 * 1024.0 * 1024.0 ||
				static_cast<double>(static_cast<size_t>(requestedOffset)) != requestedOffset ||
				static_cast<double>(static_cast<size_t>(requestedMaximum)) != requestedMaximum) {
				call.Raise("invalid capture byte range");
			}
			std::vector<std::byte> bytes;
			std::string detail;
			if (!bridge->ReadPlane(
					call.World().Name(),
					ticket,
					resource,
					static_cast<size_t>(requestedOffset),
					static_cast<size_t>(requestedMaximum),
					bytes,
					detail
				)) {
				call.Raise((std::string("capture bytes unavailable: ") + detail).c_str());
			}
			call.ReturnBytes(bytes);
		}

		DataSceneResult ReleaseCapture(
			const std::shared_ptr<DataCaptureBridge> &bridge,
			std::string_view worldName,
			std::string_view text
		) {
			if (bridge == nullptr) return CaptureUnavailable();
			uint64_t ticket = 0;
			if (!Ticket(text, ticket))
				return {"invalid_argument", Map({{"status", String("invalid_capture_ticket")}})};
			std::string detail;
			if (!bridge->Release(worldName, ticket, detail))
				return {
					"rejected",
					Map({{"status", String("capture_release_rejected")}, {"reason", String(detail)}})
				};
			return {"ok", Map({{"status", String("released")}, {"ticket", String(Decimal(ticket))}})};
		}

		DataSceneResult LifecycleUnavailable() {
			return {
				"unsupported",
				Map({{"status", String("capability_unsupported")}, {"feature", String("lifecycle")}})
			};
		}

		DataSceneResult QueueLifecycle(
			const std::shared_ptr<DataLifecycleBridge> &bridge,
			std::string_view worldName,
			const ScriptValue &value
		) {
			if (bridge == nullptr) return LifecycleUnavailable();
			DataLifecycleBridgeRequest request;
			if (value.Tag != ValueTag::Map ||
				!BoundedStringField(value, "operation", 32, request.Operation) || worldName.empty() ||
				worldName.size() > 256 || worldName.find('\0') != std::string_view::npos)
				return {"invalid_argument", Map({{"status", String("invalid_lifecycle_request")}})};
			request.InstanceId = worldName;
			const bool inspect = request.Operation == "inspect";
			const bool known = inspect || request.Operation == "pause" || request.Operation == "resume" ||
							   request.Operation == "step" || request.Operation == "snapshot" ||
							   request.Operation == "checkpoint" || request.Operation == "restore";
			if (!known) return {"invalid_argument", Map({{"status", String("invalid_lifecycle_request")}})};
			if (request.Operation == "inspect" && !HasOnlyFields(value, {"operation"}))
				return {"invalid_argument", Map({{"status", String("invalid_lifecycle_request")}})};
			if (request.Operation == "pause" && !HasOnlyFields(
													value,
													{"operation",
													 "operation_id",
													 "expected_tick",
													 "expected_world_epoch",
													 "expected_world_version",
													 "scope"}
												))
				return {"invalid_argument", Map({{"status", String("invalid_lifecycle_request")}})};
			if ((request.Operation == "resume" || request.Operation == "snapshot" ||
				 request.Operation == "checkpoint") &&
				!HasOnlyFields(
					value,
					{"operation",
					 "operation_id",
					 "expected_tick",
					 "expected_world_epoch",
					 "expected_world_version"}
				))
				return {"invalid_argument", Map({{"status", String("invalid_lifecycle_request")}})};
			if (request.Operation == "restore" && !HasOnlyFields(
													  value,
													  {"operation",
													   "operation_id",
													   "expected_tick",
													   "expected_world_epoch",
													   "expected_world_version",
													   "checkpoint_id"}
												  ))
				return {"invalid_argument", Map({{"status", String("invalid_lifecycle_request")}})};
			if (request.Operation == "step" && !HasOnlyFields(
												   value,
												   {"operation",
													"operation_id",
													"expected_tick",
													"expected_world_epoch",
													"expected_world_version",
													"dt_ns"}
											   ))
				return {"invalid_argument", Map({{"status", String("invalid_lifecycle_request")}})};
			if (const ScriptValue *scope = Field(value, "scope"); scope != nullptr) {
				if (scope->Tag != ValueTag::String ||
					(scope->Text != "all_systems" && scope->Text != "physics_only"))
					return {"invalid_argument", Map({{"status", String("invalid_lifecycle_request")}})};
				request.Scope = scope->Text;
			}
			if (const ScriptValue *snapshot = Field(value, "checkpoint_id"); snapshot != nullptr) {
				if (snapshot->Tag != ValueTag::String || snapshot->Text.empty() ||
					snapshot->Text.size() > 256 || snapshot->Text.find('\0') != std::string::npos)
					return {"invalid_argument", Map({{"status", String("invalid_lifecycle_request")}})};
				request.CheckpointId = snapshot->Text;
			}
			auto count = [&](std::string_view name, uint64_t &out) {
				const ScriptValue *field = Field(value, name);
				return field != nullptr && field->Tag == ValueTag::String && Ticket(field->Text, out);
			};
			if (!known ||
				(!inspect && (!BoundedStringField(value, "operation_id", 128, request.OperationId) ||
							  !count("expected_tick", request.ExpectedTick) ||
							  !count("expected_world_epoch", request.ExpectedWorldEpoch) ||
							  !count("expected_world_version", request.ExpectedWorldVersion))))
				return {"invalid_argument", Map({{"status", String("invalid_lifecycle_request")}})};
			if (request.Operation == "step") {
				const ScriptValue *interval = Field(value, "dt_ns");
				uint64_t denominator = 0;
				if (interval == nullptr || !HasOnlyFields(*interval, {"numerator", "denominator"}))
					return {"invalid_argument", Map({{"status", String("invalid_lifecycle_request")}})};
				const auto intervalCount = [&](std::string_view name, uint64_t &out) {
					const ScriptValue *field = Field(*interval, name);
					return field != nullptr && field->Tag == ValueTag::String && Ticket(field->Text, out);
				};
				if (!intervalCount("numerator", request.DtNumeratorNanoseconds) ||
					!intervalCount("denominator", denominator) || request.DtNumeratorNanoseconds == 0 ||
					denominator == 0 || denominator > std::numeric_limits<uint32_t>::max())
					return {"invalid_argument", Map({{"status", String("invalid_lifecycle_request")}})};
				request.DtDenominator = static_cast<uint32_t>(denominator);
			}
			uint64_t ticket = 0;
			std::string detail;
			if (!bridge->Queue(worldName, request, ticket, detail))
				return {
					"rejected", Map({{"status", String("lifecycle_rejected")}, {"reason", String(detail)}})
				};
			return {"ok", Map({{"status", String("queued")}, {"ticket", String(Decimal(ticket))}})};
		}

		DataSceneResult PollLifecycle(
			const std::shared_ptr<DataLifecycleBridge> &bridge,
			std::string_view worldName,
			std::string_view text
		) {
			if (bridge == nullptr) return LifecycleUnavailable();
			uint64_t ticket = 0;
			if (!Ticket(text, ticket))
				return {"invalid_argument", Map({{"status", String("invalid_lifecycle_ticket")}})};
			DataLifecycleBridgeReply reply;
			std::string detail;
			if (!bridge->Poll(worldName, ticket, reply, detail))
				return {
					"unknown",
					Map({{"status", String("unknown_lifecycle_ticket")}, {"reason", String(detail)}})
				};
			return {
				"ok",
				Map(
					{{"status", String(reply.Status)},
					 {"detail", String(reply.Detail)},
					 {"instance_id", String(reply.InstanceId)},
					 {"snapshot_id", String(reply.SnapshotId)},
					 {"checkpoint_id", String(reply.CheckpointId)},
					 {"tick", String(reply.Tick)},
					 {"time_nanoseconds", String(reply.TimeNanoseconds)},
					 {"version", String(reply.Version)},
					 {"epoch", String(reply.Epoch)}}
				)
			};
		}

		DataSceneResult ReleaseLifecycle(
			const std::shared_ptr<DataLifecycleBridge> &bridge,
			std::string_view worldName,
			std::string_view text
		) {
			if (bridge == nullptr) return LifecycleUnavailable();
			uint64_t ticket = 0;
			std::string detail;
			if (!Ticket(text, ticket))
				return {"invalid_argument", Map({{"status", String("invalid_lifecycle_ticket")}})};
			if (!bridge->Release(worldName, ticket, detail))
				return {
					"rejected",
					Map({{"status", String("lifecycle_release_rejected")}, {"reason", String(detail)}})
				};
			return {"ok", Map({{"status", String("released")}})};
		}

		bool StableId(const ecs::Store &store, ecs::Entity entity, std::string &out) {
			ecs::AttributeValue value;
			if (!ecs::GetAttribute(store, entity, core::Name(DATA_SCENE_ID_ATTRIBUTE), value) ||
				value.Type != ecs::PropertyType::String || value.String.empty()) {
				return false;
			}
			out = value.String;
			return true;
		}

		ScriptValue EntityRecord(
			const ecs::Store &store,
			ecs::Entity entity,
			std::string_view id,
			const std::unordered_map<uint64_t, std::string> &ids
		) {
			const ecs::ClassInfo &classInfo = ecs::Classes::Describe(store.ClassOf(entity));
			std::vector<std::pair<std::string, ScriptValue>> entries{
				{"id", String(id)},
				{"name", String(store.InstanceNameOf(entity).Text())},
				{"class", String(classInfo.Name.Text())},
			};
			if (const auto parent = ids.find(store.ParentOf(entity).Id); parent != ids.end()) {
				entries.emplace_back("parent_id", String(parent->second));
			}
			if (const auto *transform = store.Get<scene::Transform>(entity); transform != nullptr) {
				entries.emplace_back("transform", Frame(transform->Frame));
			}
			if (const auto *visual = store.Get<scene::Visual>(entity); visual != nullptr) {
				entries.emplace_back(
					"material",
					Map({
						{"mesh", String(visual->Mesh.Text())},
						{"tint", Colour(visual->Tint)},
						{"transparency", Number(visual->Transparency)},
						{"visible", Boolean(visual->Visible)},
						{"casts_shadow", Boolean(visual->CastShadow)},
					})
				);
			}
			if (const auto *surface = store.Get<scene::Surface>(entity); surface != nullptr) {
				std::vector<std::pair<std::string, ScriptValue>> material{
					{"name", String(surface->Material.Text())}
				};
				if (const auto *table = store.Resource<scene::SurfaceTable>(); table != nullptr) {
					if (const auto *properties = table->Find(surface->Material); properties != nullptr) {
						material.emplace_back("friction", Number(properties->Friction));
						material.emplace_back("restitution", Number(properties->Restitution));
					}
				}
				entries.emplace_back("surface_material", Map(std::move(material)));
			}
			if (const auto *appearance = store.Get<scene::SurfaceAppearance>(entity); appearance != nullptr) {
				entries.emplace_back(
					"pbr",
					Map({
						{"base_color_map", String(appearance->ColourMap.Text())},
						{"normal_map", String(appearance->NormalMap.Text())},
						{"roughness_map", String(appearance->RoughnessMap.Text())},
						{"metalness_map", String(appearance->MetalnessMap.Text())},
						{"emissive_map", String(appearance->EmissiveMap.Text())},
						{"base_color", Colour(appearance->Colour)},
						{"emissive_tint", Colour(appearance->EmissiveTint)},
						{"emissive_strength", Number(appearance->EmissiveStrength)},
						{"alpha_cutoff", Number(appearance->AlphaCutoff)},
					})
				);
			}
			if (const auto *light = store.Get<scene::Light>(entity); light != nullptr) {
				entries.emplace_back(
					"light",
					Map({
						{"kind", String(LightKindName(light->Kind))},
						{"color", Colour(light->Colour)},
						{"brightness", Number(light->Brightness)},
						{"range_metres", Number(light->Range)},
						{"angle_degrees", Number(light->Angle)},
						{"enabled", Boolean(light->Enabled)},
						{"shadows", Boolean(light->Shadows)},
					})
				);
			}
			if (const auto *skeleton = store.Get<scene::Skeleton>(entity); skeleton != nullptr) {
				entries.emplace_back(
					"skeleton",
					Map({
						{"rig", String(skeleton->Rig.Text())},
						{"joint_count", Number(skeleton->JointCount)},
						{"pose_scale", Number(skeleton->PoseScale)},
					})
				);
			}
			if (const auto *bone = store.Get<scene::Bone>(entity); bone != nullptr) {
				entries.emplace_back(
					"bone",
					Map({
						{"joint", Number(bone->Joint)},
						{"parent_joint", Number(bone->ParentJoint)},
						{"rest", Frame(bone->Rest)},
						{"transform", Frame(bone->Transform)},
						{"inverse_bind", Frame(bone->InverseBind)},
						{"world_frame", Frame(bone->WorldFrame)},
					})
				);
			}
			if (const auto *body = store.Get<scene::RigidBody>(entity); body != nullptr) {
				entries.emplace_back(
					"physics",
					Map({
						{"mass_kg", Number(body->Mass)},
						{"linear_damping_per_second", Number(body->LinearDamping)},
						{"angular_damping_per_second", Number(body->AngularDamping)},
						{"body_kind", String(scene::Describe(body->Kind))},
						{"simulated", Boolean(store.Has<scene::Simulated>(entity))},
						{"awake", Boolean(store.Has<scene::Motion>(entity))},
					})
				);
			}
			if (const auto *motion = store.Get<scene::Motion>(entity); motion != nullptr) {
				entries.emplace_back(
					"motion",
					Map({
						{"linear_mps", Vector(motion->Linear)},
						{"angular_radps", Vector(motion->Angular)},
					})
				);
			}
			if (const auto *body = store.Get<scene::RigidBody>(entity); body != nullptr) {
				entries.emplace_back(
					"rigid_body",
					Map({
						{"mass_kg", Number(body->Mass)},
						{"linear_damping_fraction_per_second", Number(body->LinearDamping)},
						{"angular_damping_fraction_per_second", Number(body->AngularDamping)},
						{"simulated", Boolean(store.Has<scene::Simulated>(entity))},
					})
				);
			}
			return Map(std::move(entries));
		}

		ScriptValue ContactRecords(
			const physics::PhysicsWorld &world, const std::unordered_map<uint64_t, std::string> &ids
		) {
			std::vector<ScriptValue> records;
			records.reserve(world.Manifolds().size());
			for (const physics::ContactManifold &manifold : world.Manifolds()) {
				auto endpoint = [&](ecs::Entity entity) {
					if (const auto found = ids.find(entity.Id); found != ids.end())
						return String(found->second);
					return String("");
				};
				std::vector<ScriptValue> points;
				points.reserve(manifold.PointCount);
				for (size_t index = 0; index < manifold.PointCount; index++) {
					const physics::ContactPoint &point = manifold.Points[index];
					points.push_back(Map({
						{"position_metres", Vector(point.Position)},
						{"penetration_metres", Number(point.Penetration)},
						{"feature", String(Decimal(point.Feature))},
					}));
				}
				records.push_back(Map({
					{"a_id", endpoint(manifold.A)},
					{"b_id", endpoint(manifold.B)},
					{"a_has_stable_id", Boolean(ids.contains(manifold.A.Id))},
					{"b_has_stable_id", Boolean(ids.contains(manifold.B.Id))},
					{"normal", Vector(manifold.Normal)},
					{"trigger", Boolean(manifold.Trigger)},
					{"points", Array(std::move(points))},
				}));
			}
			return Array(std::move(records));
		}

		void ServiceCapabilities(ScriptCall &call) {
			DataSceneResult result = GetCapabilities(call.World());
			const auto bridge = call.DataCapture();
			const bool available = bridge != nullptr && bridge->Capabilities().Available;
			for (auto &[name, value] : result.Value.Entries)
				if (name == "render_capture") value = Boolean(available);
			call.ReturnValue(std::move(result.Value));
		}
		void ServiceSnapshot(ScriptCall &call) {
			size_t limit = MAX_DATA_SCENE_ENTITIES;
			if (call.Arguments() > 0) {
				const double requested = call.AsNumber(0);
				if (!(requested >= 0.0) || requested > static_cast<double>(MAX_DATA_SCENE_ENTITIES) ||
					static_cast<double>(static_cast<size_t>(requested)) != requested) {
					call.ReturnValue(Map({{"status", String("invalid_limit")}}));
					return;
				}
				limit = static_cast<size_t>(requested);
			}
			call.ReturnValue(GetSceneSnapshot(call.World(), limit).Value);
		}
		void ServiceCamera(ScriptCall &call) {
			call.ReturnValue(GetCameraRenderingData(call.World(), call.AsInstance(0)).Value);
		}
		void ServiceImage(ScriptCall &call) {
			call.ReturnValue(GetEditableImageMetadata(call.World(), call.AsInstance(0)).Value);
		}
		void ServiceChannels(ScriptCall &call) {
			call.ReturnValue(CaptureChannels(call.DataCapture()).Value);
		}
		void ServiceCapture(ScriptCall &call) {
			ScriptValue request;
			CodecStatus status = CodecStatus::Ok;
			if (!call.ReadValue(0, request, status)) {
				call.ReturnValue(
					Map({{"status", String("invalid_capture_request")}, {"reason", String(Describe(status))}})
				);
				return;
			}
			call.ReturnValue(QueueCapture(call.DataCapture(), call.World().Name(), request).Value);
		}
		void ServicePollCapture(ScriptCall &call) {
			call.ReturnValue(PollCapture(call.DataCapture(), call.World().Name(), call.AsString(0)).Value);
		}
		void ServiceCancelCapture(ScriptCall &call) {
			call.ReturnValue(CancelCapture(call.DataCapture(), call.World().Name(), call.AsString(0)).Value);
		}
		void ServiceCaptureBuffer(ScriptCall &call) {
			ReadCaptureBuffer(call);
		}
		void ServiceReleaseCapture(ScriptCall &call) {
			call.ReturnValue(ReleaseCapture(call.DataCapture(), call.World().Name(), call.AsString(0)).Value);
		}
		void ServiceSetCaptureDriver(ScriptCall &call) {
			if (call.IsNil(0)) {
				if (auto *previous = call.World().ResourceMutable<DataCaptureDriver>();
					previous != nullptr && previous->Callback.Valid())
					call.ReleaseHostCallback(previous->Callback);
				call.World().RemoveResource<DataCaptureDriver>();
				call.ReturnValue(Map({{"status", String("released")}}));
				return;
			}
			const HostCallback replacement = call.RetainHostCallback(0);
			if (!replacement.Valid()) {
				call.ReturnValue(Map({{"status", String("invalid_driver")}}));
				return;
			}
			if (auto *previous = call.World().ResourceMutable<DataCaptureDriver>();
				previous != nullptr && previous->Callback.Valid())
				call.ReleaseHostCallback(previous->Callback);
			call.World().SetResource(DataCaptureDriver{replacement});
			call.ReturnValue(Map({{"status", String("registered")}}));
		}
		void ServiceRequestLifecycle(ScriptCall &call) {
			ScriptValue request;
			CodecStatus status = CodecStatus::Ok;
			if (!call.ReadValue(0, request, status)) {
				call.ReturnValue(Map({{"status", String("invalid_lifecycle_request")}}));
				return;
			}
			call.ReturnValue(QueueLifecycle(call.DataLifecycle(), call.World().Name(), request).Value);
		}
		void ServicePollLifecycle(ScriptCall &call) {
			call.ReturnValue(
				PollLifecycle(call.DataLifecycle(), call.World().Name(), call.AsString(0)).Value
			);
		}
		void ServiceReleaseLifecycle(ScriptCall &call) {
			call.ReturnValue(
				ReleaseLifecycle(call.DataLifecycle(), call.World().Name(), call.AsString(0)).Value
			);
		}
		void ServiceResources(ScriptCall &call) {
			call.ReturnValue(GetResources(call.World()).Value);
		}
		void ServiceRaycast(ScriptCall &call) {
			ScriptValue request;
			CodecStatus status = CodecStatus::Ok;
			if (!call.ReadValue(0, request, status)) {
				call.ReturnValue(Map({{"status", String("invalid_raycast_query")}}));
				return;
			}
			call.ReturnValue(QueryRay(call.World(), request).Value);
		}
		void ServiceAabb(ScriptCall &call) {
			ScriptValue request;
			CodecStatus status = CodecStatus::Ok;
			if (!call.ReadValue(0, request, status)) {
				call.ReturnValue(Map({{"status", String("invalid_aabb_query")}}));
				return;
			}
			call.ReturnValue(QueryAabb(call.World(), request).Value);
		}
		void ServiceObb(ScriptCall &call) {
			ScriptValue request;
			CodecStatus status = CodecStatus::Ok;
			if (!call.ReadValue(0, request, status)) {
				call.ReturnValue(Map({{"status", String("invalid_obb_query")}}));
				return;
			}
			call.ReturnValue(QueryObb(call.World(), request).Value);
		}

		constexpr std::array<ServiceMethod, 18> DATA_SCENE_METHODS{{
			{"GetCapabilities", ServiceCapabilities},
			{"GetSceneSnapshot", ServiceSnapshot},
			{"GetCameraRenderingData", ServiceCamera},
			{"GetEditableImageMetadata", ServiceImage},
			{"GetCaptureChannels", ServiceChannels},
			{"Capture", ServiceCapture},
			{"PollCapture", ServicePollCapture},
			{"CancelCapture", ServiceCancelCapture},
			{"GetCaptureBuffer", ServiceCaptureBuffer},
			{"ReleaseCapture", ServiceReleaseCapture},
			{"SetCaptureDriver", ServiceSetCaptureDriver},
			{"RequestLifecycle", ServiceRequestLifecycle},
			{"PollLifecycle", ServicePollLifecycle},
			{"ReleaseLifecycle", ServiceReleaseLifecycle},
			{"GetResources", ServiceResources},
			{"Raycast", ServiceRaycast},
			{"OverlapAABB", ServiceAabb},
			{"OverlapOBB", ServiceObb},
		}};
	}

	DataSceneResult GetCapabilities(const ecs::Store &) {
		return {
			"ok",
			Map({
				{"status", String("ok")},
				{"schema_version", String("data-scene/v1")},
				{"scene_snapshot", Boolean(true)},
				{"camera_metadata", Boolean(true)},
				{"editable_image_rgba8", Boolean(true)},
				{"durable_resources", Boolean(false)},
				{"render_capture", Boolean(false)},
				{"checkpoint", Boolean(false)},
			})
		};
	}

	DataSceneResult GetSceneSnapshot(ecs::Store &store, size_t limit) {
		if (limit > MAX_DATA_SCENE_ENTITIES)
			return {
				"resource_limit",
				Map({
					{"status", String("resource_limit")},
					{"limit", Number(MAX_DATA_SCENE_ENTITIES)},
				})
			};
		std::vector<std::pair<ecs::Entity, std::string>> selected;
		std::unordered_map<uint64_t, std::string> ids;
		std::unordered_set<std::string> seen;
		enum class SnapshotFault { None, Limit, IdTooLong, Duplicate };
		SnapshotFault fault = SnapshotFault::None;
		std::string duplicateId;
		size_t unlabelled = 0;
		store.Each<const ecs::InstanceName>([&](ecs::Entity entity, const ecs::InstanceName &) {
			if (fault != SnapshotFault::None) return;
			std::string id;
			if (!StableId(store, entity, id)) {
				unlabelled++;
				return;
			}
			if (id.size() > MAX_DATA_SCENE_ID_BYTES) {
				fault = SnapshotFault::IdTooLong;
				duplicateId = id;
				return;
			}
			if (selected.size() == limit) {
				fault = SnapshotFault::Limit;
				return;
			}
			if (!seen.emplace(id).second) {
				fault = SnapshotFault::Duplicate;
				duplicateId = id;
				return;
			}
			ids.emplace(entity.Id, id);
			selected.emplace_back(entity, std::move(id));
		});
		if (fault != SnapshotFault::None) {
			const char *status = fault == SnapshotFault::Limit		 ? "resource_limit"
								 : fault == SnapshotFault::IdTooLong ? "invalid_identity"
																	 : "identity_conflict";
			std::vector<std::pair<std::string, ScriptValue>> result{{"status", String(status)}};
			if (!duplicateId.empty()) result.emplace_back("id", String(duplicateId));
			return {status, Map(std::move(result))};
		}
		std::sort(selected.begin(), selected.end(), [](const auto &left, const auto &right) {
			return left.second < right.second;
		});
		std::vector<ScriptValue> entities;
		entities.reserve(selected.size());
		for (const auto &[entity, id] : selected)
			entities.push_back(EntityRecord(store, entity, id, ids));
		const ecs::WorldTime time = store.Time();
		std::vector<std::pair<std::string, ScriptValue>> result{
			{"status", String("ok")},
			{"schema_version", String("data-scene/v1")},
			{"tick", String(Decimal(time.Tick))},
			{"elapsed_seconds", Number(time.Elapsed)},
			{"fixed_delta_seconds", Number(time.Delta)},
			{"coverage", String("explicitly_identified_subset")},
			{"unlabelled_instances", Number(unlabelled)},
			{"entities", Array(std::move(entities))},
		};
		if (const auto *clock = physics::PhysicsClockOf(store); clock != nullptr) {
			result.emplace_back(
				"physics_clock",
				Map({
					{"paused", Boolean(clock->Paused)},
					{"rate_hz", Number(clock->Rate)},
					{"steps", String(Decimal(clock->Steps))},
					{"dropped_steps", String(Decimal(clock->DroppedSteps))},
				})
			);
		}
		if (ecs::Components::Find(core::Name("physics.PhysicsWorld")).IsValid())
			if (const auto *world = store.Resource<physics::PhysicsWorld>(); world != nullptr)
				result.emplace_back("contacts", ContactRecords(*world, ids));
		return {"ok", Map(std::move(result))};
	}

	DataSceneResult GetCameraRenderingData(const ecs::Store &store, ecs::Entity entity) {
		const auto *camera = store.Get<scene::Camera>(entity);
		const auto *transform = store.Get<scene::Transform>(entity);
		if (camera == nullptr || transform == nullptr)
			return {"invalid_argument", Map({{"status", String("invalid_camera")}})};
		std::string id;
		if (!StableId(store, entity, id))
			return {"identity_required", Map({{"status", String("identity_required")}})};
		return {
			"ok",
			Map({
				{"status", String("ok")},
				{"id", String(id)},
				{"world_from_camera", Frame(transform->Frame)},
				{"vertical_fov_radians", Number(camera->FieldOfViewRadians)},
				{"near_metres", Number(camera->NearPlane)},
				{"far_metres", Number(camera->FarPlane)},
				{"requested_width", Number(camera->ImageWidth)},
				{"requested_height", Number(camera->ImageHeight)},
				{"coordinate_convention", String("right-handed, Y-up, Vulkan depth 0..1")},
			})
		};
	}

	DataSceneResult GetEditableImageMetadata(const ecs::Store &store, ecs::Entity entity) {
		const auto *image = store.Get<scene::EditableImage>(entity);
		if (image == nullptr)
			return {"invalid_argument", Map({{"status", String("invalid_editable_image")}})};
		if (image->Width == 0 || image->Height == 0 || image->Width > scene::MAXIMUM_EDITABLE_IMAGE_PIXELS ||
			image->Height > scene::MAXIMUM_EDITABLE_IMAGE_PIXELS ||
			static_cast<uint64_t>(image->Width) * image->Height > scene::MAXIMUM_EDITABLE_IMAGE_PIXELS) {
			return {"invalid_data", Map({{"status", String("invalid_rgba8_dimensions")}})};
		}
		const size_t expected = static_cast<size_t>(image->Width) * image->Height * 4;
		if (image->Pixels.size() != expected)
			return {"invalid_data", Map({{"status", String("invalid_rgba8_storage")}})};
		return {
			"ok",
			Map({
				{"status", String("ok")},
				{"width", Number(image->Width)},
				{"height", Number(image->Height)},
				{"byte_length", Number(expected)},
				{"format", String("rgba8")},
				{"row_order", String("top_left")},
				{"color_space", String("linear")},
				{"alpha", String("straight")},
				{"copy_semantics", String("copied")},
				{"revision", Number(image->Revision)},
			})
		};
	}

	DataSceneResult GetCaptureChannels(const ecs::Store &) {
		return CaptureUnavailable();
	}

	DataSceneResult GetCaptureChannels(const ecs::Store &, const std::shared_ptr<DataCaptureBridge> &bridge) {
		return CaptureChannels(bridge);
	}

	DataSceneResult GetResources(const ecs::Store &) {
		return {
			"unsupported",
			Map({
				{"status", String("capability_unsupported")},
				{"feature", String("durable_resources")},
				{"resources", Array({})},
				{"reason", String("this service has no durable resource owner")},
			})
		};
	}

	const ServiceSurface &DataSceneServiceSurface() {
		static const ServiceSurface SURFACE = [] {
			ServiceSurface surface;
			surface.Name = "DataSceneService";
			surface.Methods = DATA_SCENE_METHODS;
			return surface;
		}();
		return SURFACE;
	}
}
