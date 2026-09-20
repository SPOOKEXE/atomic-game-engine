#pragma once

// Shared lifecycle fence for read-only data-factory observations. A read is
// still an observation of a particular completed world revision, so it must
// not silently answer from a newer or different factory-owned scene.
// @tier L13 · shared

#include <engine/world/DataFactory.hpp>

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace engine::control::data_factory_read_fence {

	using nlohmann::json;

	// Reads an unsigned lifecycle counter without narrowing it.
	inline bool UInt(const json &value, std::string_view name, uint64_t &out, std::string &failure) {
		if (!value.is_number_unsigned()) {
			failure = "validation_failed: " + std::string(name) + " must be an unsigned integer";
			return false;
		}
		out = value.get<uint64_t>();
		return true;
	}

	// Extracts the bounded scene identity shared by session-bound read tools.
	inline bool InstanceId(const json &arguments, std::string &out, std::string &failure) {
		const auto field = arguments.find("instance_id");
		if (field == arguments.end() || !field->is_string()) {
			failure = "validation_failed: instance_id must name a scene";
			return false;
		}
		out = field->get<std::string>();
		if (out.empty() || out.size() > 128 || out.find('\0') != std::string::npos) {
			failure = "validation_failed: instance_id must be a non-empty identifier of at most 128 bytes";
			return false;
		}
		return true;
	}

	// Recognizes the three fields that bind a read to one completed world revision.
	inline bool IsExpectedRevisionField(std::string_view name) {
		return name == "expected_tick" || name == "expected_world_epoch" || name == "expected_world_version";
	}

	// Serializes the lifecycle revision currently observed by the factory session.
	inline json Current(const world::DataFactoryReply &reply) {
		return {
			{"instance_id", reply.InstanceId},
			{"current_tick", reply.Clock.Tick},
			{"current_world_epoch", reply.WorldEpoch},
			{"current_world_version", reply.WorldVersion},
		};
	}

	// Confirms session ownership and rejects reads whose expected revision is no longer current.
	inline bool Validate(
		world::DataFactorySession *session,
		std::string_view instanceId,
		const json &options,
		json &result,
		std::string &failure
	) {
		if (session == nullptr) return true;
		if (!session->OwnsWorld(instanceId)) {
			result = {
				{"status", "validation_failed"},
				{"instance_id", std::string(instanceId)},
				{"detail", "instance_id is not owned by this data-factory session"}
			};
			failure = "validation_failed: instance_id is not owned by this data-factory session";
			return false;
		}

		uint64_t tick = 0;
		uint64_t epoch = 0;
		uint64_t version = 0;
		if (!options.contains("expected_tick") ||
			!UInt(options["expected_tick"], "options.expected_tick", tick, failure) ||
			!options.contains("expected_world_epoch") ||
			!UInt(options["expected_world_epoch"], "options.expected_world_epoch", epoch, failure) ||
			!options.contains("expected_world_version") ||
			!UInt(options["expected_world_version"], "options.expected_world_version", version, failure)) {
			if (failure.empty()) failure = "validation_failed: expected lifecycle revision is required";
			return false;
		}

		const world::DataFactoryReply current = session->Inspect(instanceId);
		result = Current(current);
		if (current.Status != world::DataFactoryStatus::Ok) {
			result["status"] = world::Describe(current.Status);
			result["detail"] = current.Detail;
			failure = world::Describe(current.Status) + std::string(": ") + current.Detail;
			return false;
		}
		if (current.Clock.Tick == tick && current.WorldEpoch == epoch && current.WorldVersion == version) {
			result = nullptr;
			return true;
		}
		result["status"] = "version_conflict";
		result["detail"] = "read requires the current completed lifecycle revision";
		result["expected_tick"] = tick;
		result["expected_world_epoch"] = epoch;
		result["expected_world_version"] = version;
		failure = "version_conflict: read requires the current completed lifecycle revision";
		return false;
	}
}
