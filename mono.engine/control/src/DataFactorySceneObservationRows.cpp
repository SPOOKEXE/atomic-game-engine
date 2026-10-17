#include "DataFactoryMcpAdapter.hpp"

namespace engine::control {
	using nlohmann::json;

	void AddDataFactorySceneObservationRows(Surface &surface, world::DataFactorySession &session) {
		using namespace data_factory_detail;
		surface.Add(
			Tool{
				"get_collider_bev",
				"Returns a snapshot-bound bird's-eye collider-contact grid. Rows are z-major from minimum Z "
				"to "
				"maximum Z; columns then run from minimum X to maximum X. Each cell is occupied, empty, or "
				"explicitly unknown when physics cannot prove a negative answer.",
				[] {
					const json xz{
						{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 2}, {"maxItems", 2}
					};
					const json lifecycle{
						{"type", "object"},
						{"additionalProperties", false},
						{"properties",
						 {{"tick", {{"type", "integer"}, {"minimum", 0}}},
						  {"world_epoch", {{"type", "integer"}, {"minimum", 0}}},
						  {"world_version", {{"type", "integer"}, {"minimum", 0}}}}},
						{"required", {"tick", "world_epoch", "world_version"}}
					};
					return json{
						{"type", "object"},
						{"additionalProperties", false},
						{"properties",
						 {{"schema_version", {{"const", "collider-bev/v1"}}},
						  {"world_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
						  {"lifecycle", lifecycle},
						  {"snapshot_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
						  {"xz_bounds_metres",
						   {{"type", "object"},
							{"additionalProperties", false},
							{"properties", {{"minimum", xz}, {"maximum", xz}}},
							{"required", {"minimum", "maximum"}}}},
						  {"y_minimum_metres", {{"type", "number"}}},
						  {"y_maximum_metres", {{"type", "number"}}},
						  {"rows", {{"type", "integer"}, {"minimum", 1}, {"maximum", 8}}},
						  {"columns", {{"type", "integer"}, {"minimum", 1}, {"maximum", 8}}}}},
						{"required",
						 {"schema_version",
						  "world_id",
						  "lifecycle",
						  "snapshot_id",
						  "xz_bounds_metres",
						  "y_minimum_metres",
						  "y_maximum_metres",
						  "rows",
						  "columns"}}
					};
				},
				[&session](const json &arguments, std::string &failure) -> json {
					using namespace data_factory_detail;
					if (!arguments.is_object() || !Only(
													  arguments,
													  {"schema_version",
													   "world_id",
													   "lifecycle",
													   "snapshot_id",
													   "xz_bounds_metres",
													   "y_minimum_metres",
													   "y_maximum_metres",
													   "rows",
													   "columns"},
													  failure
												  ))
						return nullptr;
					Request request;
					const json *field = nullptr;
					if (!Field(arguments, "schema_version", field, failure) || !field->is_string() ||
						field->get<std::string>() != "collider-bev/v1" ||
						!Field(arguments, "world_id", field, failure) ||
						!Text(*field, "world_id", request.InstanceId, failure) ||
						!Field(arguments, "snapshot_id", field, failure) ||
						!Text(*field, "snapshot_id", request.SnapshotId, failure)) {
						if (failure.empty())
							failure = Error("validation_failed", "schema_version must be collider-bev/v1");
						return nullptr;
					}
					const auto lifecycle = arguments.find("lifecycle");
					if (lifecycle == arguments.end() || !lifecycle->is_object() ||
						!Only(*lifecycle, {"tick", "world_epoch", "world_version"}, failure) ||
						!Field(*lifecycle, "tick", field, failure) ||
						!UInt(*field, "lifecycle.tick", request.Tick, failure) ||
						!Field(*lifecycle, "world_epoch", field, failure) ||
						!UInt(*field, "lifecycle.world_epoch", request.Epoch, failure) ||
						!Field(*lifecycle, "world_version", field, failure) ||
						!UInt(*field, "lifecycle.world_version", request.Version, failure)) {
						if (failure.empty())
							failure = Error(
								"validation_failed", "lifecycle requires tick, world_epoch and world_version"
							);
						return nullptr;
					}
					const auto finite = [](const json &value, float &out) {
						if (!value.is_number() || !std::isfinite(value.get<double>()) ||
							value.get<double>() < -std::numeric_limits<float>::max() ||
							value.get<double>() > std::numeric_limits<float>::max())
							return false;
						out = value.get<float>();
						return std::isfinite(out);
					};
					const json *bounds = nullptr;
					if (!Field(arguments, "xz_bounds_metres", bounds, failure) || !bounds->is_object() ||
						!Only(*bounds, {"minimum", "maximum"}, failure) || !bounds->contains("minimum") ||
						!bounds->contains("maximum") || !bounds->at("minimum").is_array() ||
						!bounds->at("maximum").is_array() || bounds->at("minimum").size() != 2 ||
						bounds->at("maximum").size() != 2) {
						if (failure.empty())
							failure = Error(
								"validation_failed", "xz_bounds_metres needs finite [x,z] minimum and maximum"
							);
						return nullptr;
					}
					script::DataSceneColliderBevRequest bev;
					if (!finite(bounds->at("minimum")[0], bev.MinimumXMetres) ||
						!finite(bounds->at("minimum")[1], bev.MinimumZMetres) ||
						!finite(bounds->at("maximum")[0], bev.MaximumXMetres) ||
						!finite(bounds->at("maximum")[1], bev.MaximumZMetres) ||
						!Field(arguments, "y_minimum_metres", field, failure) ||
						!finite(*field, bev.MinimumYMetres) ||
						!Field(arguments, "y_maximum_metres", field, failure) ||
						!finite(*field, bev.MaximumYMetres) || !Field(arguments, "rows", field, failure) ||
						!field->is_number_unsigned() || field->get<uint64_t>() > 8 ||
						!Field(arguments, "columns", field, failure) || !field->is_number_unsigned() ||
						field->get<uint64_t>() > 8) {
						if (failure.empty())
							failure = Error(
								"validation_failed",
								"BEV bounds must be finite and dimensions must be 1 through 8"
							);
						return nullptr;
					}
					bev.Rows = arguments.at("rows").get<uint8_t>();
					bev.Columns = arguments.at("columns").get<uint8_t>();
					if (bev.Rows == 0 || bev.Columns == 0 || bev.MinimumXMetres >= bev.MaximumXMetres ||
						bev.MinimumZMetres >= bev.MaximumZMetres ||
						bev.MinimumYMetres >= bev.MaximumYMetres) {
						failure =
							Error("validation_failed", "BEV bounds and Y slab must be strictly increasing");
						return nullptr;
					}
					const auto hasExtent = [](float minimum, float maximum) {
						return static_cast<float>((static_cast<double>(maximum) - minimum) * 0.5) > 0.0f;
					};
					const auto boundary = [](float minimum, float maximum, uint8_t index, uint8_t count) {
						if (index == 0) return minimum;
						if (index == count) return maximum;
						return static_cast<float>(
							static_cast<double>(minimum) +
							(static_cast<double>(maximum) - minimum) * static_cast<double>(index) / count
						);
					};
					if (!hasExtent(bev.MinimumYMetres, bev.MaximumYMetres)) {
						failure = Error("validation_failed", "Y slab half extent rounds to zero");
						return nullptr;
					}
					for (uint8_t row = 0; row < bev.Rows; ++row) {
						const float minimumZ =
							boundary(bev.MinimumZMetres, bev.MaximumZMetres, row, bev.Rows);
						const float maximumZ =
							boundary(bev.MinimumZMetres, bev.MaximumZMetres, row + 1, bev.Rows);
						for (uint8_t column = 0; column < bev.Columns; ++column) {
							const float minimumX =
								boundary(bev.MinimumXMetres, bev.MaximumXMetres, column, bev.Columns);
							const float maximumX =
								boundary(bev.MinimumXMetres, bev.MaximumXMetres, column + 1, bev.Columns);
							if (!hasExtent(minimumX, maximumX) || !hasExtent(minimumZ, maximumZ)) {
								failure = Error("validation_failed", "a BEV cell half extent rounds to zero");
								return nullptr;
							}
						}
					}
					if (!session.OwnsWorld(request.InstanceId)) {
						failure =
							Error("validation_failed", "world_id is not owned by this data-factory session");
						return nullptr;
					}
					if (!Preconditions(session, request, failure)) return nullptr;
					const world::DataFactoryReply barrier =
						session.RenderSnapshotBarrier(request.InstanceId, request.SnapshotId);
					if (barrier.Status != world::DataFactoryStatus::Ok) {
						failure = Error(world::Describe(barrier.Status), barrier.Detail);
						return nullptr;
					}
					json result;
					const world::WorldStatus entered = session.UniverseOf().Enter(
						session.UniverseOf().Find(core::Name(barrier.InstanceId)), [&](ecs::Store &store) {
							result = data_scene_detail::Result(script::ColliderBev(store, bev), failure);
						}
					);
					if (entered != world::WorldStatus::Ok && failure.empty())
						failure = Error("validation_failed", "scene is unavailable");
					if (!failure.empty()) return nullptr;
					result["world_id"] = barrier.InstanceId;
					result["lifecycle"] = {
						{"tick", barrier.Clock.Tick},
						{"world_epoch", barrier.WorldEpoch},
						{"world_version", barrier.WorldVersion}
					};
					result["snapshot_id"] = request.SnapshotId;
					return result;
				}
			}
		);

		surface.Add(
			Tool{
				"get_filled_occupancy",
				"Returns a bounded snapshot-bound voxel grid. A filled cell is proven only when one analytic "
				"collider contains its whole AABB. Collider unions, baked hulls and meshes remain explicitly "
				"unknown rather than being inferred.",
				[] {
					const json vector{
						{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}
					};
					const json lifecycle{
						{"type", "object"},
						{"additionalProperties", false},
						{"properties",
						 {{"tick", {{"type", "integer"}, {"minimum", 0}}},
						  {"world_epoch", {{"type", "integer"}, {"minimum", 0}}},
						  {"world_version", {{"type", "integer"}, {"minimum", 0}}}}},
						{"required", {"tick", "world_epoch", "world_version"}}
					};
					return json{
						{"type", "object"},
						{"additionalProperties", false},
						{"properties",
						 {{"schema_version", {{"const", "filled-occupancy/v1"}}},
						  {"world_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
						  {"lifecycle", lifecycle},
						  {"snapshot_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
						  {"minimum_metres", vector},
						  {"maximum_metres", vector},
						  {"columns", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4}}},
						  {"rows", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4}}},
						  {"layers", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4}}}}},
						{"required",
						 {"schema_version",
						  "world_id",
						  "lifecycle",
						  "snapshot_id",
						  "minimum_metres",
						  "maximum_metres",
						  "columns",
						  "rows",
						  "layers"}}
					};
				},
				[&session](const json &arguments, std::string &failure) -> json {
					using namespace data_factory_detail;
					if (!arguments.is_object() || !Only(
													  arguments,
													  {"schema_version",
													   "world_id",
													   "lifecycle",
													   "snapshot_id",
													   "minimum_metres",
													   "maximum_metres",
													   "columns",
													   "rows",
													   "layers"},
													  failure
												  ))
						return nullptr;
					Request request;
					const json *field = nullptr;
					if (!Field(arguments, "schema_version", field, failure) || !field->is_string() ||
						field->get<std::string>() != "filled-occupancy/v1" ||
						!Field(arguments, "world_id", field, failure) ||
						!Text(*field, "world_id", request.InstanceId, failure) ||
						!Field(arguments, "snapshot_id", field, failure) ||
						!Text(*field, "snapshot_id", request.SnapshotId, failure)) {
						if (failure.empty())
							failure =
								Error("validation_failed", "schema_version must be filled-occupancy/v1");
						return nullptr;
					}
					const auto lifecycle = arguments.find("lifecycle");
					if (lifecycle == arguments.end() || !lifecycle->is_object() ||
						!Only(*lifecycle, {"tick", "world_epoch", "world_version"}, failure) ||
						!Field(*lifecycle, "tick", field, failure) ||
						!UInt(*field, "lifecycle.tick", request.Tick, failure) ||
						!Field(*lifecycle, "world_epoch", field, failure) ||
						!UInt(*field, "lifecycle.world_epoch", request.Epoch, failure) ||
						!Field(*lifecycle, "world_version", field, failure) ||
						!UInt(*field, "lifecycle.world_version", request.Version, failure)) {
						if (failure.empty())
							failure = Error(
								"validation_failed", "lifecycle requires tick, world_epoch and world_version"
							);
						return nullptr;
					}
					script::DataSceneFilledOccupancyRequest occupancy;
					if (!arguments.contains("minimum_metres") || !arguments.contains("maximum_metres") ||
						!FiniteVector(arguments.at("minimum_metres"), occupancy.MinimumMetres) ||
						!FiniteVector(arguments.at("maximum_metres"), occupancy.MaximumMetres) ||
						!StrictFiniteBox(occupancy.MinimumMetres, occupancy.MaximumMetres)) {
						failure = Error(
							"validation_failed",
							"minimum_metres and maximum_metres must be finite strict vectors"
						);
						return nullptr;
					}
					const auto dimension = [&](std::string_view name, uint8_t &out) {
						const auto found = arguments.find(name);
						if (found == arguments.end() || !found->is_number_unsigned() ||
							found->get<uint64_t>() == 0 || found->get<uint64_t>() > 4)
							return false;
						out = found->get<uint8_t>();
						return true;
					};
					if (!dimension("columns", occupancy.Columns) || !dimension("rows", occupancy.Rows) ||
						!dimension("layers", occupancy.Layers)) {
						failure = Error(
							"validation_failed", "columns, rows and layers must be integers from 1 through 4"
						);
						return nullptr;
					}
					if (!session.OwnsWorld(request.InstanceId)) {
						failure =
							Error("validation_failed", "world_id is not owned by this data-factory session");
						return nullptr;
					}
					if (!Preconditions(session, request, failure)) return nullptr;
					const world::DataFactoryReply barrier =
						session.RenderSnapshotBarrier(request.InstanceId, request.SnapshotId);
					if (barrier.Status != world::DataFactoryStatus::Ok) {
						failure = Error(world::Describe(barrier.Status), barrier.Detail);
						return nullptr;
					}
					json result;
					const world::WorldStatus entered = session.UniverseOf().Enter(
						session.UniverseOf().Find(core::Name(barrier.InstanceId)), [&](ecs::Store &store) {
							result =
								data_scene_detail::Result(script::FilledOccupancy(store, occupancy), failure);
						}
					);
					if (entered != world::WorldStatus::Ok && failure.empty())
						failure = Error("validation_failed", "scene is unavailable");
					if (!failure.empty()) return nullptr;
					result["world_id"] = barrier.InstanceId;
					result["lifecycle"] = {
						{"tick", barrier.Clock.Tick},
						{"world_epoch", barrier.WorldEpoch},
						{"world_version", barrier.WorldVersion}
					};
					result["snapshot_id"] = request.SnapshotId;
					return result;
				}
			}
		);

		surface.Add(
			Tool{
				"get_signed_distance_field",
				"Samples a bounded world-space signed-distance grid at cell centres against one retained "
				"paused "
				"snapshot. Distances are metres, negative inside and positive outside. Exact values require "
				"one "
				"authored analytic box, sphere or cylinder; unions and baked geometry are returned as "
				"unknown.",
				[] {
					const json vector{
						{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}
					};
					const json lifecycle{
						{"type", "object"},
						{"additionalProperties", false},
						{"properties",
						 {{"tick", {{"type", "integer"}, {"minimum", 0}}},
						  {"world_epoch", {{"type", "integer"}, {"minimum", 0}}},
						  {"world_version", {{"type", "integer"}, {"minimum", 0}}}}},
						{"required", {"tick", "world_epoch", "world_version"}}
					};
					return json{
						{"type", "object"},
						{"additionalProperties", false},
						{"properties",
						 {{"schema_version", {{"const", "signed-distance-field/v1"}}},
						  {"world_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
						  {"lifecycle", lifecycle},
						  {"snapshot_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
						  {"minimum_metres", vector},
						  {"maximum_metres", vector},
						  {"columns", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4}}},
						  {"rows", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4}}},
						  {"layers", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4}}}}},
						{"required",
						 {"schema_version",
						  "world_id",
						  "lifecycle",
						  "snapshot_id",
						  "minimum_metres",
						  "maximum_metres",
						  "columns",
						  "rows",
						  "layers"}}
					};
				},
				[&session](const json &arguments, std::string &failure) -> json {
					using namespace data_factory_detail;
					if (!arguments.is_object() || !Only(
													  arguments,
													  {"schema_version",
													   "world_id",
													   "lifecycle",
													   "snapshot_id",
													   "minimum_metres",
													   "maximum_metres",
													   "columns",
													   "rows",
													   "layers"},
													  failure
												  ))
						return nullptr;
					Request request;
					const json *field = nullptr;
					if (!Field(arguments, "schema_version", field, failure) || !field->is_string() ||
						field->get<std::string>() != "signed-distance-field/v1" ||
						!Field(arguments, "world_id", field, failure) ||
						!Text(*field, "world_id", request.InstanceId, failure) ||
						!Field(arguments, "snapshot_id", field, failure) ||
						!Text(*field, "snapshot_id", request.SnapshotId, failure)) {
						if (failure.empty())
							failure =
								Error("validation_failed", "schema_version must be signed-distance-field/v1");
						return nullptr;
					}
					const auto lifecycle = arguments.find("lifecycle");
					if (lifecycle == arguments.end() || !lifecycle->is_object() ||
						!Only(*lifecycle, {"tick", "world_epoch", "world_version"}, failure) ||
						!Field(*lifecycle, "tick", field, failure) ||
						!UInt(*field, "lifecycle.tick", request.Tick, failure) ||
						!Field(*lifecycle, "world_epoch", field, failure) ||
						!UInt(*field, "lifecycle.world_epoch", request.Epoch, failure) ||
						!Field(*lifecycle, "world_version", field, failure) ||
						!UInt(*field, "lifecycle.world_version", request.Version, failure)) {
						if (failure.empty())
							failure = Error(
								"validation_failed", "lifecycle requires tick, world_epoch and world_version"
							);
						return nullptr;
					}
					script::DataSceneSignedDistanceFieldRequest sdf;
					if (!arguments.contains("minimum_metres") || !arguments.contains("maximum_metres") ||
						!FiniteVector(arguments.at("minimum_metres"), sdf.MinimumMetres) ||
						!FiniteVector(arguments.at("maximum_metres"), sdf.MaximumMetres) ||
						!StrictFiniteBox(sdf.MinimumMetres, sdf.MaximumMetres)) {
						failure = Error(
							"validation_failed",
							"minimum_metres and maximum_metres must be finite strict vectors"
						);
						return nullptr;
					}
					const auto dimension = [&](std::string_view name, uint8_t &out) {
						const auto found = arguments.find(name);
						if (found == arguments.end() || !found->is_number_unsigned() ||
							found->get<uint64_t>() == 0 || found->get<uint64_t>() > 4)
							return false;
						out = found->get<uint8_t>();
						return true;
					};
					if (!dimension("columns", sdf.Columns) || !dimension("rows", sdf.Rows) ||
						!dimension("layers", sdf.Layers)) {
						failure = Error(
							"validation_failed", "columns, rows and layers must be integers from 1 through 4"
						);
						return nullptr;
					}
					const auto boundary = [](float minimum, float maximum, uint8_t index, uint8_t count) {
						if (index == 0) return minimum;
						if (index == count) return maximum;
						return static_cast<float>(
							static_cast<double>(minimum) +
							(static_cast<double>(maximum) - minimum) * static_cast<double>(index) / count
						);
					};
					const auto cellsDistinct = [&](float minimum, float maximum, uint8_t count) {
						for (uint8_t index = 0; index < count; ++index) {
							const float width = boundary(minimum, maximum, index + 1, count) -
												boundary(minimum, maximum, index, count);
							if (!(static_cast<float>(static_cast<double>(width) * 0.5) > 0.0f)) return false;
						}
						return true;
					};
					const auto centre = [](float minimum, float maximum, uint8_t index, uint8_t count) {
						return static_cast<float>(
							static_cast<double>(minimum) + (static_cast<double>(maximum) - minimum) *
															   (static_cast<double>(index) + 0.5) / count
						);
					};
					const auto centresDistinct = [&](float minimum, float maximum, uint8_t count) {
						float previous = centre(minimum, maximum, 0, count);
						for (uint8_t index = 1; index < count; ++index) {
							const float current = centre(minimum, maximum, index, count);
							if (!(previous < current)) return false;
							previous = current;
						}
						return true;
					};
					if (!cellsDistinct(sdf.MinimumMetres.X, sdf.MaximumMetres.X, sdf.Columns) ||
						!cellsDistinct(sdf.MinimumMetres.Y, sdf.MaximumMetres.Y, sdf.Layers) ||
						!cellsDistinct(sdf.MinimumMetres.Z, sdf.MaximumMetres.Z, sdf.Rows) ||
						!centresDistinct(sdf.MinimumMetres.X, sdf.MaximumMetres.X, sdf.Columns) ||
						!centresDistinct(sdf.MinimumMetres.Y, sdf.MaximumMetres.Y, sdf.Layers) ||
						!centresDistinct(sdf.MinimumMetres.Z, sdf.MaximumMetres.Z, sdf.Rows)) {
						failure = Error(
							"validation_failed", "signed-distance-field cells collapse at float32 precision"
						);
						return nullptr;
					}
					if (!session.OwnsWorld(request.InstanceId)) {
						failure =
							Error("validation_failed", "world_id is not owned by this data-factory session");
						return nullptr;
					}
					if (!Preconditions(session, request, failure)) return nullptr;
					const world::DataFactoryReply barrier =
						session.RenderSnapshotBarrier(request.InstanceId, request.SnapshotId);
					if (barrier.Status != world::DataFactoryStatus::Ok) {
						failure = Error(world::Describe(barrier.Status), barrier.Detail);
						return nullptr;
					}
					json result;
					const world::WorldStatus entered = session.UniverseOf().Enter(
						session.UniverseOf().Find(core::Name(barrier.InstanceId)), [&](ecs::Store &store) {
							result =
								data_scene_detail::Result(script::SignedDistanceField(store, sdf), failure);
						}
					);
					if (entered != world::WorldStatus::Ok && failure.empty())
						failure = Error("validation_failed", "scene is unavailable");
					if (!failure.empty()) return nullptr;
					result["world_id"] = barrier.InstanceId;
					result["lifecycle"] = {
						{"tick", barrier.Clock.Tick},
						{"world_epoch", barrier.WorldEpoch},
						{"world_version", barrier.WorldVersion}
					};
					result["snapshot_id"] = request.SnapshotId;
					return result;
				}
			}
		);

		surface.Add(
			Tool{
				"get_authored_navmesh_path",
				"Finds a bounded authored-walkable surface route in one retained paused snapshot. A route is "
				"returned only for supported authored geometry and a clear zero-radius corridor; every other "
				"case is explicitly unknown.",
				[] {
					const json vector{
						{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}
					};
					const json lifecycle{
						{"type", "object"},
						{"additionalProperties", false},
						{"properties",
						 {{"tick", {{"type", "integer"}, {"minimum", 0}}},
						  {"world_epoch", {{"type", "integer"}, {"minimum", 0}}},
						  {"world_version", {{"type", "integer"}, {"minimum", 0}}}}},
						{"required", {"tick", "world_epoch", "world_version"}}
					};
					return json{
						{"type", "object"},
						{"additionalProperties", false},
						{"properties",
						 {{"schema_version", {{"const", "authored-navmesh-path/v1"}}},
						  {"world_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
						  {"lifecycle", lifecycle},
						  {"snapshot_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
						  {"start_metres", vector},
						  {"goal_metres", vector},
						  {"vertical_tolerance_metres",
						   {{"type", "number"}, {"minimum", 0}, {"maximum", 10}}}}},
						{"required",
						 {"schema_version",
						  "world_id",
						  "lifecycle",
						  "snapshot_id",
						  "start_metres",
						  "goal_metres"}}
					};
				},
				[&session](const json &arguments, std::string &failure) -> json {
					using namespace data_factory_detail;
					if (!arguments.is_object() || !Only(
													  arguments,
													  {"schema_version",
													   "world_id",
													   "lifecycle",
													   "snapshot_id",
													   "start_metres",
													   "goal_metres",
													   "vertical_tolerance_metres"},
													  failure
												  ))
						return nullptr;
					Request request;
					const json *field = nullptr;
					if (!Field(arguments, "schema_version", field, failure) || !field->is_string() ||
						field->get<std::string>() != "authored-navmesh-path/v1" ||
						!Field(arguments, "world_id", field, failure) ||
						!Text(*field, "world_id", request.InstanceId, failure) ||
						!Field(arguments, "snapshot_id", field, failure) ||
						!Text(*field, "snapshot_id", request.SnapshotId, failure)) {
						if (failure.empty())
							failure =
								Error("validation_failed", "schema_version must be authored-navmesh-path/v1");
						return nullptr;
					}
					const auto lifecycle = arguments.find("lifecycle");
					if (lifecycle == arguments.end() || !lifecycle->is_object() ||
						!Only(*lifecycle, {"tick", "world_epoch", "world_version"}, failure) ||
						!Field(*lifecycle, "tick", field, failure) ||
						!UInt(*field, "lifecycle.tick", request.Tick, failure) ||
						!Field(*lifecycle, "world_epoch", field, failure) ||
						!UInt(*field, "lifecycle.world_epoch", request.Epoch, failure) ||
						!Field(*lifecycle, "world_version", field, failure) ||
						!UInt(*field, "lifecycle.world_version", request.Version, failure)) {
						if (failure.empty())
							failure = Error(
								"validation_failed", "lifecycle requires tick, world_epoch and world_version"
							);
						return nullptr;
					}
					script::DataSceneNavmeshPathRequest navmesh;
					if (!arguments.contains("start_metres") || !arguments.contains("goal_metres") ||
						!FiniteVector(arguments.at("start_metres"), navmesh.StartMetres) ||
						!FiniteVector(arguments.at("goal_metres"), navmesh.GoalMetres)) {
						failure =
							Error("validation_failed", "start_metres and goal_metres must be finite vectors");
						return nullptr;
					}
					if (const auto tolerance = arguments.find("vertical_tolerance_metres");
						tolerance != arguments.end()) {
						if (!tolerance->is_number() ||
							!std::isfinite(navmesh.VerticalToleranceMetres = tolerance->get<float>()) ||
							navmesh.VerticalToleranceMetres < 0.0f ||
							navmesh.VerticalToleranceMetres > 10.0f) {
							failure = Error(
								"validation_failed", "vertical_tolerance_metres must be finite from 0 to 10"
							);
							return nullptr;
						}
					}
					if (!session.OwnsWorld(request.InstanceId)) {
						failure =
							Error("validation_failed", "world_id is not owned by this data-factory session");
						return nullptr;
					}
					if (!Preconditions(session, request, failure)) return nullptr;
					const world::DataFactoryReply barrier =
						session.RenderSnapshotBarrier(request.InstanceId, request.SnapshotId);
					if (barrier.Status != world::DataFactoryStatus::Ok) {
						failure = Error(world::Describe(barrier.Status), barrier.Detail);
						return nullptr;
					}
					json result;
					const world::WorldStatus entered = session.UniverseOf().Enter(
						session.UniverseOf().Find(core::Name(barrier.InstanceId)), [&](ecs::Store &store) {
							result = data_scene_detail::Result(
								script::FindAuthoredNavmeshPath(store, navmesh), failure
							);
						}
					);
					if (entered != world::WorldStatus::Ok && failure.empty())
						failure = Error("validation_failed", "scene is unavailable");
					if (!failure.empty()) return nullptr;
					result["world_id"] = barrier.InstanceId;
					result["lifecycle"] = {
						{"tick", barrier.Clock.Tick},
						{"world_epoch", barrier.WorldEpoch},
						{"world_version", barrier.WorldVersion}
					};
					result["snapshot_id"] = request.SnapshotId;
					return result;
				}
			}
		);

		surface.Add(
			Tool{
				"get_collider_occupancy",
				"Tests up to 32 named finite world-space AABBs against the exact completed, retained "
				"all-systems-paused snapshot. Boundary contact counts as collider contact. This is not a "
				"filled-volume test: unavailable physics, broad-phase overflow, and mesh or hull candidates "
				"remain explicitly incomplete.",
				[] {
					const json vector{
						{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}
					};
					const json lifecycle{
						{"type", "object"},
						{"additionalProperties", false},
						{"properties",
						 {{"tick", {{"type", "integer"}, {"minimum", 0}}},
						  {"world_epoch", {{"type", "integer"}, {"minimum", 0}}},
						  {"world_version", {{"type", "integer"}, {"minimum", 0}}}}},
						{"required", {"tick", "world_epoch", "world_version"}}
					};
					return json{
						{"type", "object"},
						{"properties",
						 {{"schema_version", {{"const", "collider-occupancy/v1"}}},
						  {"world_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
						  {"lifecycle", lifecycle},
						  {"snapshot_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
						  {"probes",
						   {{"type", "array"},
							{"minItems", 1},
							{"maxItems", 32},
							{"items",
							 {{"type", "object"},
							  {"additionalProperties", false},
							  {"properties",
							   {{"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
								{"minimum_metres", vector},
								{"maximum_metres", vector}}},
							  {"required", {"name", "minimum_metres", "maximum_metres"}}}}}}}},
						{"required", {"schema_version", "world_id", "lifecycle", "snapshot_id", "probes"}},
						{"additionalProperties", false}
					};
				},
				[&session](const json &arguments, std::string &failure) -> json {
					using namespace data_factory_detail;
					if (!arguments.is_object() ||
						!Only(
							arguments,
							{"schema_version", "world_id", "lifecycle", "snapshot_id", "probes"},
							failure
						)) {
						if (failure.empty())
							failure = Error("validation_failed", "arguments must be an object");
						return nullptr;
					}
					Request request;
					const json *field = nullptr;
					if (!Field(arguments, "schema_version", field, failure) || !field->is_string() ||
						field->get<std::string>() != "collider-occupancy/v1" ||
						!Field(arguments, "world_id", field, failure) ||
						!Text(*field, "world_id", request.InstanceId, failure)) {
						if (failure.empty())
							failure =
								Error("validation_failed", "schema_version must be collider-occupancy/v1");
						return nullptr;
					}
					const auto lifecycle = arguments.find("lifecycle");
					if (lifecycle == arguments.end() || !lifecycle->is_object() ||
						!Only(*lifecycle, {"tick", "world_epoch", "world_version"}, failure) ||
						!Field(*lifecycle, "tick", field, failure) ||
						!UInt(*field, "lifecycle.tick", request.Tick, failure) ||
						!Field(*lifecycle, "world_epoch", field, failure) ||
						!UInt(*field, "lifecycle.world_epoch", request.Epoch, failure) ||
						!Field(*lifecycle, "world_version", field, failure) ||
						!UInt(*field, "lifecycle.world_version", request.Version, failure)) {
						if (failure.empty())
							failure = Error(
								"validation_failed", "lifecycle requires tick, world_epoch and world_version"
							);
						return nullptr;
					}
					const json *snapshot = nullptr;
					if (!Field(arguments, "snapshot_id", snapshot, failure) ||
						!Text(*snapshot, "snapshot_id", request.SnapshotId, failure))
						return nullptr;
					const auto probesField = arguments.find("probes");
					if (probesField == arguments.end() || !probesField->is_array() || probesField->empty() ||
						probesField->size() > 32) {
						failure = Error("validation_failed", "probes must contain 1 through 32 named AABBs");
						return nullptr;
					}
					std::array<core::AABB, 32> probes;
					std::array<std::string, 32> names;
					std::unordered_set<std::string> uniqueNames;
					for (size_t index = 0; index < probesField->size(); ++index) {
						const json &probe = (*probesField)[index];
						if (!probe.is_object() ||
							!Only(probe, {"name", "minimum_metres", "maximum_metres"}, failure) ||
							!probe.contains("name") ||
							!Text(probe.at("name"), "probe name", names[index], failure) ||
							!uniqueNames.emplace(names[index]).second) {
							if (failure.empty())
								failure = Error("validation_failed", "probe names must be unique");
							return nullptr;
						}
						core::Vector3 minimum, maximum;
						if (!probe.contains("minimum_metres") || !probe.contains("maximum_metres") ||
							!FiniteVector(probe.at("minimum_metres"), minimum) ||
							!FiniteVector(probe.at("maximum_metres"), maximum) ||
							!StrictFiniteBox(minimum, maximum)) {
							failure = Error(
								"validation_failed",
								"each probe needs finite strict minimum and maximum vectors"
							);
							return nullptr;
						}
						probes[index] = {minimum, maximum};
					}
					if (!Preconditions(session, request, failure)) return nullptr;
					const world::DataFactoryReply barrier =
						session.RenderSnapshotBarrier(request.InstanceId, request.SnapshotId);
					if (barrier.Status != world::DataFactoryStatus::Ok) {
						failure = Error(world::Describe(barrier.Status), barrier.Detail);
						return nullptr;
					}
					std::array<physics::ColliderOccupancy, 32> occupancy;
					json answers = json::array();
					world::Universe &universe = session.UniverseOf();
					const world::WorldStatus entered =
						universe.Enter(universe.Find(core::Name(barrier.InstanceId)), [&](ecs::Store &store) {
							physics::ColliderOccupancyBatch(
								store,
								std::span{probes}.first(probesField->size()),
								std::span{occupancy}.first(probesField->size())
							);
							for (size_t index = 0; index < probesField->size(); ++index) {
								const physics::ColliderOccupancy &answer = occupancy[index];
								bool witnessIdentityAvailable = false;
								json witnessId = nullptr;
								bool ambiguousIdentity = false;
								if (answer.WitnessAvailable) {
									ecs::AttributeValue value;
									if (ecs::GetAttribute(
											store,
											answer.Witness,
											core::Name(script::DATA_SCENE_ID_ATTRIBUTE),
											value
										) &&
										value.Type == ecs::PropertyType::String && !value.String.empty() &&
										value.String.size() <= script::MAX_DATA_SCENE_ID_BYTES &&
										value.String.find('\0') == std::string::npos &&
										OccupancyUtf8(value.String)) {
										size_t matches = 0;
										store.Each<const ecs::InstanceName>([&](ecs::Entity entity,
																				const ecs::InstanceName &) {
											ecs::AttributeValue candidate;
											if (ecs::GetAttribute(
													store,
													entity,
													core::Name(script::DATA_SCENE_ID_ATTRIBUTE),
													candidate
												) &&
												candidate.Type == ecs::PropertyType::String &&
												candidate.String == value.String)
												matches++;
										});
										ambiguousIdentity = matches != 1;
										if (!ambiguousIdentity) {
											witnessIdentityAvailable = true;
											witnessId = value.String;
										}
									}
								}
								const bool rowAvailable =
									answer.Available && (answer.OverlapFound || answer.Complete);
								const json overlap = rowAvailable ? json(answer.OverlapFound) : json(nullptr);
								const json reason =
									rowAvailable || answer.Why == physics::ColliderOccupancy::Reason::None
										? json(nullptr)
										: json(OccupancyReason(answer.Why));
								answers.push_back(
									{{"name", names[index]},
									 {"minimum_metres", (*probesField)[index].at("minimum_metres")},
									 {"maximum_metres", (*probesField)[index].at("maximum_metres")},
									 {"available", rowAvailable},
									 {"overlap_found", overlap},
									 {"witness_id", std::move(witnessId)},
									 {"witness_identity_available", witnessIdentityAvailable},
									 {"complete", answer.Complete},
									 {"reason", reason}}
								);
							}
						});
					if (entered != world::WorldStatus::Ok) {
						failure = Error("validation_failed", "scene is unavailable");
						return nullptr;
					}
					json result{
						{"schema_version", "collider-occupancy/v1"},
						{"world_id", barrier.InstanceId},
						{"lifecycle",
						 {{"tick", barrier.Clock.Tick},
						  {"world_epoch", barrier.WorldEpoch},
						  {"world_version", barrier.WorldVersion}}},
						{"snapshot_id", request.SnapshotId},
						{"probes", std::move(answers)}
					};
					if (result.dump().size() > 64u * 1024u) {
						failure = Error(
							"resource_limit", "collider occupancy exceeds the 65536-byte response limit"
						);
						return nullptr;
					}
					return result;
				}
			}
		);
	}
}
