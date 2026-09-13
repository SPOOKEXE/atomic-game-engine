#include <engine/core/Name.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Skinning.hpp>
#include <engine/script/DataSceneService.hpp>
#include <engine/script/RigExport.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <unordered_set>
#include <utility>

namespace engine::script {
	using ecs::Entity;

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
		ScriptValue Boolean(bool value) {
			ScriptValue out(value ? ValueTag::True : ValueTag::False);
			out.Boolean = value;
			return out;
		}
		ScriptValue Array(std::vector<ScriptValue> values) {
			ScriptValue out(ValueTag::Array);
			out.Items = std::move(values);
			return out;
		}
		ScriptValue Map(std::vector<std::pair<std::string, ScriptValue>> values) {
			ScriptValue out(ValueTag::Map);
			out.Entries = std::move(values);
			return out;
		}

		bool Utf8(std::string_view value) {
			for (size_t index = 0; index < value.size();) {
				const uint8_t first = static_cast<uint8_t>(value[index++]);
				if (first < 0x80) continue;
				const unsigned extra = first >= 0xC2 && first <= 0xDF	? 1
									   : first >= 0xE0 && first <= 0xEF ? 2
									   : first >= 0xF0 && first <= 0xF4 ? 3
																		: 4;
				if (extra == 4 || index + extra > value.size()) return false;
				uint32_t codepoint = first & ((1u << (7 - extra)) - 1u);
				for (unsigned part = 0; part < extra; ++part) {
					const uint8_t next = static_cast<uint8_t>(value[index++]);
					if ((next & 0xC0u) != 0x80u) return false;
					codepoint = (codepoint << 6u) | (next & 0x3Fu);
				}
				if ((extra == 1 && codepoint < 0x80) || (extra == 2 && codepoint < 0x800) ||
					(extra == 3 && codepoint < 0x10000) || codepoint > 0x10FFFF ||
					(codepoint >= 0xD800 && codepoint <= 0xDFFF))
					return false;
			}
			return true;
		}

		bool Text(std::string_view value, size_t maximum) {
			return !value.empty() && value.size() <= maximum && value.find('\0') == std::string_view::npos &&
				   Utf8(value);
		}

		bool StableId(const ecs::Store &store, Entity entity, std::string &id) {
			ecs::AttributeValue value;
			if (!ecs::GetAttribute(store, entity, core::Name(DATA_SCENE_ID_ATTRIBUTE), value) ||
				value.Type != ecs::PropertyType::String ||
				!Text(value.String, MAX_RIG_EXPORT_ENTITY_ID_BYTES))
				return false;
			id = value.String;
			return true;
		}

		bool Frame(const core::CFrame &frame, ScriptValue &out) {
			const double norm = std::hypot(
				std::hypot(frame.QuaternionX, frame.QuaternionY),
				std::hypot(frame.QuaternionZ, frame.QuaternionW)
			);
			if (!std::isfinite(frame.Position.X) || !std::isfinite(frame.Position.Y) ||
				!std::isfinite(frame.Position.Z) || !std::isfinite(norm) || std::abs(norm - 1.0) > 1e-5)
				return false;
			out = Map({
				{"translation",
				 Array({Number(frame.Position.X), Number(frame.Position.Y), Number(frame.Position.Z)})},
				{"rotation_xyzw",
				 Array(
					 {Number(frame.QuaternionX),
					  Number(frame.QuaternionY),
					  Number(frame.QuaternionZ),
					  Number(frame.QuaternionW)}
				 )},
				{"scale", Array({Number(1), Number(1), Number(1)})},
			});
			return true;
		}

		bool TickRate(float delta, uint64_t &numerator, uint64_t &denominator) {
			if (!std::isfinite(delta) || delta <= 0.0f) return false;
			const uint32_t bits = std::bit_cast<uint32_t>(delta);
			const uint32_t exponent = (bits >> 23u) & 0xFFu;
			uint64_t significand = exponent == 0 ? bits & 0x7FFFFFu : (1u << 23u) | (bits & 0x7FFFFFu);
			int shift = exponent == 0 ? -149 : static_cast<int>(exponent) - 150;
			if (significand == 0) return false;
			while ((significand & 1u) == 0) {
				significand >>= 1u;
				shift++;
			}
			if (shift >= 0) {
				if (shift >= 64 || significand > std::numeric_limits<uint64_t>::max() >> shift) return false;
				numerator = significand << shift;
				denominator = 1;
			} else {
				if (shift <= -64) return false;
				numerator = significand;
				denominator = uint64_t{1} << -shift;
			}
			const uint64_t divisor = std::gcd(numerator, denominator);
			numerator /= divisor;
			denominator /= divisor;
			return numerator != 0;
		}

		RigExportResult Refusal(const char *status, std::string_view detail) {
			return {status, Map({{"status", String(status)}, {"detail", String(detail)}})};
		}
	}

	RigExportResult GetRigExport(
		ecs::Store &store, std::string_view exportId, const std::vector<std::string> &entityIds, size_t limit
	) {
		if (!Text(exportId, MAX_RIG_EXPORT_ID_BYTES))
			return Refusal(
				"invalid_export_id", "export_id must be a stable non-empty string within 512 bytes"
			);
		if (limit > MAX_RIG_EXPORT_ENTITIES) return Refusal("resource_limit", "entity limit exceeds 256");
		if (entityIds.size() > MAX_RIG_EXPORT_ENTITIES)
			return Refusal("resource_limit", "selection exceeds 256 entity IDs");
		std::unordered_set<std::string> wanted;
		for (const std::string &id : entityIds) {
			if (!Text(id, MAX_RIG_EXPORT_ENTITY_ID_BYTES) || !wanted.emplace(id).second)
				return Refusal(
					"invalid_selection", "selection IDs must be unique stable strings within 512 bytes"
				);
		}

		std::vector<std::pair<Entity, std::string>> rigs;
		std::unordered_set<std::string> seen;
		bool identityConflict = false;
		bool invalidIdentity = false;
		bool overLimit = false;
		bool scanStopped = false;
		store.Each<scene::Skeleton>([&](Entity entity, const scene::Skeleton &) {
			if (scanStopped) return;
			std::string id;
			if (!StableId(store, entity, id)) {
				ecs::AttributeValue attribute;
				invalidIdentity =
					invalidIdentity ||
					(ecs::GetAttribute(store, entity, core::Name(DATA_SCENE_ID_ATTRIBUTE), attribute) &&
					 attribute.Type == ecs::PropertyType::String);
				return;
			}
			if (!wanted.empty() && !wanted.contains(id)) return;
			if (!seen.emplace(id).second) {
				identityConflict = true;
				return;
			}
			if (rigs.size() == limit) {
				overLimit = true;
				scanStopped = true;
				return;
			}
			rigs.emplace_back(entity, std::move(id));
		});
		if (identityConflict)
			return Refusal("identity_conflict", "two skeletons carry the same stable entity ID");
		if (invalidIdentity)
			return Refusal(
				"invalid_identity", "skeleton entity ID must be valid UTF-8 text within 500 bytes"
			);
		if (overLimit)
			return Refusal("resource_limit", "selected skeleton count exceeds the requested limit");
		if (!wanted.empty() && rigs.size() != wanted.size())
			return Refusal("unknown_selection", "a selected entity is not an identified skeleton");
		if (rigs.empty()) return Refusal("no_rigs", "the scene has no identified skeletons");
		std::sort(rigs.begin(), rigs.end(), [](const auto &left, const auto &right) {
			return left.second < right.second;
		});

		uint64_t tickNumerator = 0, tickDenominator = 0;
		if (!TickRate(store.Time().Delta, tickNumerator, tickDenominator))
			return Refusal("invalid_tick_duration", "world tick duration is not a positive finite rational");
		std::vector<ScriptValue> entities;
		entities.reserve(rigs.size());
		size_t totalBones = 0;
		size_t totalKeypoints = 0;
		for (const auto &[rig, entityId] : rigs) {
			const scene::Skeleton *skeleton = store.Get<scene::Skeleton>(rig);
			if (skeleton == nullptr || !skeleton->Rig.IsValid() || skeleton->JointCount == 0 ||
				skeleton->JointCount > MAX_RIG_EXPORT_BONES)
				return Refusal("invalid_skeleton", "skeleton needs a named rig and 1 through 1024 joints");
			if (!Text(skeleton->Rig.Text(), MAX_RIG_EXPORT_ID_BYTES))
				return Refusal("invalid_rig_id", "rig ID must be valid UTF-8 text within 512 bytes");
			if (skeleton->JointCount > MAX_RIG_EXPORT_TOTAL_BONES - totalBones)
				return Refusal("resource_limit", "exported joints exceed the 1024 response bound");
			totalBones += skeleton->JointCount;
			std::vector<const scene::Bone *> slots(skeleton->JointCount);
			bool duplicateSlot = false;
			store.Each<const scene::Bone>([&](Entity boneEntity, const scene::Bone &bone) {
				if (scene::SkeletonOf(store, boneEntity) != rig) return;
				if (bone.Joint >= slots.size() || slots[bone.Joint] != nullptr) {
					duplicateSlot = true;
					return;
				}
				slots[bone.Joint] = &bone;
			});
			if (duplicateSlot)
				return Refusal("invalid_skeleton", "skeleton contains duplicate or out-of-range joint slots");
			std::vector<ScriptValue> joints;
			joints.reserve(slots.size());
			for (size_t slot = 0; slot < slots.size(); ++slot) {
				const scene::Bone *bone = slots[slot];
				if (bone == nullptr || (bone->ParentJoint != scene::NO_JOINT && bone->ParentJoint >= slot))
					return Refusal(
						"invalid_skeleton", "skeleton joints must densely cover slots with earlier parents"
					);
				ScriptValue rest, current, inverseBind, world;
				const core::CFrame currentFrame = bone->Rest * bone->Transform;
				if (!Frame(bone->Rest, rest) || !Frame(currentFrame, current) ||
					!Frame(bone->InverseBind, inverseBind) || !Frame(bone->WorldFrame, world))
					return Refusal("invalid_frame", "bone frame is non-finite or has a non-unit quaternion");
				joints.push_back(Map({
					{"joint_id", String(entityId + ":joint:" + std::to_string(slot))},
					{"slot", Number(slot)},
					{"parent_joint_id",
					 bone->ParentJoint == scene::NO_JOINT
						 ? ScriptValue{}
						 : String(entityId + ":joint:" + std::to_string(bone->ParentJoint))},
					{"rest_frame", std::move(rest)},
					{"current_frame", std::move(current)},
					{"inverse_bind_frame", std::move(inverseBind)},
					{"world_frame", std::move(world)},
				}));
			}
			std::vector<std::pair<std::string, const scene::RigKeypoint *>> keypoints;
			bool duplicateKeypoint = false;
			bool invalidKeypoint = false;
			store.Each<const scene::RigKeypoint>([&](Entity pointEntity, const scene::RigKeypoint &point) {
				if (scene::SkeletonOf(store, pointEntity) != rig) return;
				if (!point.Keypoint.IsValid() || !Text(point.Keypoint.Text(), MAX_RIG_EXPORT_ID_BYTES) ||
					point.Joint >= slots.size() || slots[point.Joint] == nullptr) {
					invalidKeypoint = true;
					return;
				}
				keypoints.emplace_back(std::string(point.Keypoint.Text()), &point);
			});
			if (invalidKeypoint)
				return Refusal(
					"invalid_keypoint", "keypoints need a unique UTF-8 name and an existing skeleton joint"
				);
			if (keypoints.size() > MAX_RIG_EXPORT_KEYPOINTS ||
				keypoints.size() > MAX_RIG_EXPORT_TOTAL_KEYPOINTS - totalKeypoints)
				return Refusal("resource_limit", "exported keypoints exceed the 1024 response bound");
			std::sort(keypoints.begin(), keypoints.end(), [](const auto &left, const auto &right) {
				return left.first < right.first;
			});
			for (size_t index = 1; index < keypoints.size(); ++index)
				if (keypoints[index - 1].first == keypoints[index].first) duplicateKeypoint = true;
			if (duplicateKeypoint)
				return Refusal("invalid_keypoint", "two keypoints on one skeleton carry the same name");
			totalKeypoints += keypoints.size();
			std::vector<ScriptValue> exportedKeypoints;
			exportedKeypoints.reserve(keypoints.size());
			for (const auto &[name, point] : keypoints) {
				const std::string keypointId = entityId + ":keypoint:" + name;
				if (!Text(keypointId, MAX_RIG_EXPORT_ID_BYTES))
					return Refusal(
						"invalid_keypoint_id",
						"composed keypoint ID must be valid UTF-8 text within 512 bytes"
					);
				ScriptValue checked;
				const core::CFrame worldFrame = slots[point->Joint]->WorldFrame * point->Frame;
				if (!Frame(point->Frame, checked) || !Frame(worldFrame, checked))
					return Refusal(
						"invalid_frame", "keypoint frame is non-finite or has a non-unit quaternion"
					);
				exportedKeypoints.push_back(Map({
					{"keypoint_id", String(keypointId)},
					{"name", String(name)},
					{"state", String("present")},
					{"position",
					 Array(
						 {Number(worldFrame.Position.X),
						  Number(worldFrame.Position.Y),
						  Number(worldFrame.Position.Z)}
					 )},
					{"missing_reason", ScriptValue{}},
				}));
			}
			entities.push_back(Map({
				{"entity_id", String(entityId)},
				{"rig_id", String(skeleton->Rig.Text())},
				{"coordinate",
				 Map(
					 {{"handedness", String("right")},
					  {"up_axis", String("+Y")},
					  {"forward_axis", String("-Z")},
					  {"linear_unit", String("m")},
					  {"meters_per_unit", Number(1)}}
				 )},
				{"joints", Array(std::move(joints))},
				{"keypoints", Array(std::move(exportedKeypoints))},
				{"skinning",
				 Map(
					 {{"available", Boolean(false)},
					  {"unavailable_reason",
					   String("engine skeleton rows do not retain per-vertex skin weights")},
					  {"vertices", Array({})}}
				 )},
				{"clips", Array({})},
			}));
		}
		return {
			"ok",
			Map({
				{"schema", String("data-rig/v1")},
				{"export_id", String(exportId)},
				{"tick_seconds_numerator", Number(tickNumerator)},
				{"tick_seconds_denominator", Number(tickDenominator)},
				{"entities", Array(std::move(entities))},
			})
		};
	}
}
