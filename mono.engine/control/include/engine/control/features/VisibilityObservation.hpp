#pragma once

#include <engine/control/Surface.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace engine::control::features {
	struct VisibilityObservationReply {
		std::string World;
		uint64_t Entity = 0;
		std::string State;
		std::string Cause;
	};
	struct VisibilitySnapshotReply {
		uint64_t Frame = 0;
		size_t ViewSlot = 0;
		std::string World;
		bool Valid = false;
		size_t Dropped = 0;
		bool DroppedExact = true;
		std::vector<VisibilityObservationReply> Observations;
	};

	inline Feature VisibilityObservations(std::function<VisibilitySnapshotReply()> snapshot) {
		return {
			.Name = "visibility-observations", .Install = [snapshot = std::move(snapshot)](Surface &surface) {
				surface.Add({
					.Name = "visibility_observations",
					.Description =
						"Returns bounded per-object facts from the completed main camera submission.",
					.Schema =
						[] {
							return nlohmann::json{
								{"type", "object"},
								{"properties",
								 nlohmann::json{
									 {"world", {{"type", "string"}}}, {"entity", {{"type", "integer"}}}
								 }}
							};
						},
					.Call =
						[snapshot](const nlohmann::json &arguments, std::string &failure) {
							if (!arguments.is_object()) {
								failure = "arguments must be an object";
								return nlohmann::json(nullptr);
							}
							if (arguments.contains("world") && !arguments["world"].is_string()) {
								failure = "world must be a string";
								return nlohmann::json(nullptr);
							}
							if (arguments.contains("entity") && !arguments["entity"].is_number_unsigned() &&
								(!arguments["entity"].is_number_integer() ||
								 arguments["entity"].get<int64_t>() < 0)) {
								failure = "entity must be an unsigned integer";
								return nlohmann::json(nullptr);
							}
							const std::string world = arguments.value("world", std::string{});
							const uint64_t entity = arguments.value("entity", uint64_t(0));
							const VisibilitySnapshotReply view = snapshot();
							nlohmann::json rows = nlohmann::json::array();
							constexpr size_t MAX_REPLY = 256;
							bool truncated = view.Dropped > 0;
							for (const VisibilityObservationReply &row : view.Observations) {
								if ((!world.empty() && row.World != world) ||
									(entity != 0 && row.Entity != entity))
									continue;
								if (rows.size() == MAX_REPLY) {
									truncated = true;
									break;
								}
								rows.push_back(
									{{"world", row.World},
									 {"entity", row.Entity},
									 {"state", row.State},
									 {"cause", row.Cause}}
								);
							}
							return nlohmann::json{
								{"valid", view.Valid},
								{"frame", view.Frame},
								{"viewSlot", view.ViewSlot},
								{"world", view.World},
								{"observations", std::move(rows)},
								{"truncated", truncated},
								{"dropped", view.Dropped},
								{"dropped_exact", view.DroppedExact}
							};
						},
				});
			}
		};
	}
}
