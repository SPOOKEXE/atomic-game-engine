#include <engine/assets/Animation.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Animation.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/MeshCatalogue.hpp>
#include <engine/scene/Skinning.hpp>
#include <engine/script/DataSceneService.hpp>
#include <engine/script/RigExport.hpp>

#include <algorithm>
#include <array>
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
		constexpr uint64_t MAX_EXACT_SCRIPT_INTEGER = (uint64_t{1} << 53u) - 1u;
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

		bool Seconds(float value, uint64_t &numerator, uint64_t &denominator) {
			if (!std::isfinite(value) || value < 0.0f) return false;
			const uint32_t bits = std::bit_cast<uint32_t>(value);
			const uint32_t exponent = (bits >> 23u) & 0xFFu;
			uint64_t significand = exponent == 0 ? bits & 0x7FFFFFu : (1u << 23u) | (bits & 0x7FFFFFu);
			int shift = exponent == 0 ? -149 : static_cast<int>(exponent) - 150;
			if (significand == 0) {
				numerator = 0;
				denominator = 1;
				return true;
			}
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
			return true;
		}

		bool Tick(float seconds, uint64_t tickNumerator, uint64_t tickDenominator, uint64_t &tick) {
			uint64_t numerator = 0, denominator = 0;
			if (!Seconds(seconds, numerator, denominator)) return false;
			const uint64_t first = std::gcd(numerator, tickNumerator);
			numerator /= first;
			tickNumerator /= first;
			const uint64_t second = std::gcd(tickDenominator, denominator);
			tickDenominator /= second;
			denominator /= second;
			if (denominator != 1 || tickNumerator != 1 ||
				numerator > MAX_EXACT_SCRIPT_INTEGER / tickDenominator)
				return false;
			// ScriptValue stores numbers as double, so keep every exported tick exactly representable.
			tick = numerator * tickDenominator;
			return true;
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
		if (tickNumerator > MAX_EXACT_SCRIPT_INTEGER || tickDenominator > MAX_EXACT_SCRIPT_INTEGER)
			return Refusal(
				"invalid_tick_duration", "world tick duration cannot be represented exactly by ScriptValue"
			);
		std::vector<ScriptValue> entities;
		entities.reserve(rigs.size());
		size_t totalBones = 0;
		size_t totalKeypoints = 0;
		size_t totalAnimationKeys = 0;
		size_t totalSkinVertices = 0;
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
			std::vector<std::pair<std::string, ScriptValue>> clips;
			std::unordered_set<std::string> clipIds;
			bool invalidClip = false;
			const char *clipFailure = "invalid_clip";
			store.Each<scene::AnimationClip>([&](Entity clipEntity, const scene::AnimationClip &clip) {
				if (invalidClip || clip.Rig != skeleton->Rig) return;
				if (clips.size() == MAX_RIG_EXPORT_CLIPS_PER_ENTITY) {
					invalidClip = true;
					clipFailure = "resource_limit";
					return;
				}
				if (clip.Buffer == ecs::NULL_ENTITY) {
					invalidClip = true;
					clipFailure = "asset_only_clip";
					return;
				}
				const scene::AnimationBuffer *buffer = store.Get<scene::AnimationBuffer>(clip.Buffer);
				if (buffer == nullptr) {
					invalidClip = true;
					clipFailure = "dangling_clip_buffer";
					return;
				}
				if (buffer->Data.empty() || buffer->Data.size() > MAX_RIG_EXPORT_ANIMATION_BYTES) {
					invalidClip = true;
					clipFailure = "resource_limit";
					return;
				}
				std::string clipId;
				if (!StableId(store, clipEntity, clipId) || !clipIds.emplace(clipId).second) {
					invalidClip = true;
					clipFailure = "invalid_clip_id";
					return;
				}
				const std::string_view name = store.InstanceNameOf(clipEntity).Text();
				if (!Text(name, MAX_RIG_EXPORT_ID_BYTES)) {
					invalidClip = true;
					clipFailure = "invalid_clip_name";
					return;
				}
				core::ByteReader reader(buffer->Data);
				assets::AnimationData animation;
				if (!assets::Animation::Read(reader, animation) || !reader.AtEnd()) {
					invalidClip = true;
					clipFailure = "malformed_clip_buffer";
					return;
				}
				uint64_t endTick = 0;
				if (!Tick(animation.Duration, tickNumerator, tickDenominator, endTick)) {
					invalidClip = true;
					clipFailure = "unaligned_clip_time";
					return;
				}
				std::vector<std::pair<std::string, ScriptValue>> channels;
				for (const assets::AnimationChannel &channel : animation.Channels) {
					if (channel.Joint >= slots.size() || slots[channel.Joint] == nullptr ||
						channel.Keys.empty()) {
						invalidClip = true;
						clipFailure = "invalid_clip_channel";
						return;
					}
					if (channel.Keys.size() > MAX_RIG_EXPORT_KEYS_PER_CHANNEL ||
						totalAnimationKeys > MAX_RIG_EXPORT_TOTAL_ANIMATION_KEYS - channel.Keys.size()) {
						invalidClip = true;
						clipFailure = "resource_limit";
						return;
					}
					std::vector<ScriptValue> translations, rotations;
					translations.reserve(channel.Keys.size());
					rotations.reserve(channel.Keys.size());
					uint64_t previousTick = 0;
					bool firstKey = true;
					for (const assets::AnimationKeyframe &key : channel.Keys) {
						uint64_t keyTick = 0, secondsNumerator = 0, secondsDenominator = 0;
						ScriptValue checked;
						if (!Tick(key.Time, tickNumerator, tickDenominator, keyTick) || keyTick > endTick ||
							!Seconds(key.Time, secondsNumerator, secondsDenominator) ||
							(!firstKey && keyTick <= previousTick) || !Frame(key.Transform, checked)) {
							invalidClip = true;
							clipFailure = "invalid_clip_key";
							return;
						}
						firstKey = false;
						previousTick = keyTick;
						const ScriptValue time = Map({
							{"tick", Number(keyTick)},
							{"seconds_numerator", Number(secondsNumerator)},
							{"seconds_denominator", Number(secondsDenominator)},
						});
						translations.push_back(Map({
							{"time", time},
							{"value",
							 Array(
								 {Number(key.Transform.Position.X),
								  Number(key.Transform.Position.Y),
								  Number(key.Transform.Position.Z)}
							 )},
						}));
						rotations.push_back(Map({
							{"time", time},
							{"value",
							 Array(
								 {Number(key.Transform.QuaternionX),
								  Number(key.Transform.QuaternionY),
								  Number(key.Transform.QuaternionZ),
								  Number(key.Transform.QuaternionW)}
							 )},
						}));
					}
					totalAnimationKeys += channel.Keys.size();
					for (const auto &[property, keys] :
						 std::array<std::pair<const char *, const std::vector<ScriptValue> *>, 2>{
							 {{"rotation", &rotations}, {"translation", &translations}}
						 }) {
						const std::string channelId =
							clipId + ":joint:" + std::to_string(channel.Joint) + ":" + property;
						if (!Text(channelId, MAX_RIG_EXPORT_ID_BYTES) ||
							channels.size() == MAX_RIG_EXPORT_CHANNELS_PER_CLIP) {
							invalidClip = true;
							clipFailure = "invalid_channel_id";
							return;
						}
						channels.emplace_back(
							channelId,
							Map({
								{"channel_id", String(channelId)},
								{"joint_slot", Number(channel.Joint)},
								{"property", String(property)},
								{"keys", Array(*keys)},
							})
						);
					}
				}
				if (invalidClip) return;
				std::sort(channels.begin(), channels.end(), [](const auto &left, const auto &right) {
					return left.first < right.first;
				});
				std::vector<ScriptValue> exportedChannels;
				exportedChannels.reserve(channels.size());
				for (auto &[ignored, value] : channels) {
					(void)ignored;
					exportedChannels.push_back(std::move(value));
				}
				clips.emplace_back(
					clipId,
					Map({
						{"clip_id", String(clipId)},
						{"name", String(name)},
						{"start_tick", Number(0)},
						{"end_tick", Number(endTick)},
						{"channels", Array(std::move(exportedChannels))},
					})
				);
			});
			if (invalidClip) return Refusal(clipFailure, "buffered animation clip cannot be exported");
			std::sort(clips.begin(), clips.end(), [](const auto &left, const auto &right) {
				return left.first < right.first;
			});
			std::vector<ScriptValue> exportedClips;
			exportedClips.reserve(clips.size());
			for (auto &[ignored, value] : clips) {
				(void)ignored;
				exportedClips.push_back(std::move(value));
			}
			ScriptValue exportedSkinning;
			const scene::Visual *visual = store.Get<scene::Visual>(rig);
			scene::MeshSkinning skinning;
			const bool sourceKnown = visual != nullptr && visual->Mesh.IsValid() &&
									 engine::scene::SkinningOf(store, visual->Mesh, skinning);
			const auto unavailableSkinning = [&](std::string_view reason) {
				exportedSkinning = Map({
					{"available", Boolean(false)},
					{"unavailable_reason", String(reason)},
					{"mesh_id", ScriptValue{}},
					{"position_space", String("mesh_object")},
					{"joint_index_space", String("skeleton_palette_slot")},
					{"weight_encoding", String("uint16_unorm")},
					{"weight_denominator", Number(65535)},
					{"normalization", String("unavailable")},
					{"vertices", Array({})},
				});
			};
			if (visual == nullptr || !visual->Mesh.IsValid()) {
				unavailableSkinning("skinned drawable has no stable mesh ID");
			} else if (!Text(visual->Mesh.Text(), MAX_RIG_EXPORT_ID_BYTES)) {
				unavailableSkinning("mesh ID must be valid UTF-8 text within 512 bytes");
			} else if (!sourceKnown) {
				unavailableSkinning("mesh skinning source is unavailable in this world");
			} else if (skinning.JointCount == 0) {
				unavailableSkinning("mesh has no authored skin palette");
			} else if (skinning.JointCount != skeleton->JointCount) {
				unavailableSkinning("mesh skin palette does not match the skeleton joint count");
			} else if (skinning.Vertices.size() > MAX_RIG_EXPORT_SKIN_VERTICES ||
					   skinning.Vertices.size() > MAX_RIG_EXPORT_TOTAL_SKIN_VERTICES - totalSkinVertices) {
				unavailableSkinning("mesh skinning vertices exceed the 1024 response limit");
			} else {
				totalSkinVertices += skinning.Vertices.size();
				std::vector<ScriptValue> vertices;
				vertices.reserve(skinning.Vertices.size());
				for (size_t vertexIndex = 0; vertexIndex < skinning.Vertices.size(); ++vertexIndex) {
					const scene::MeshSkinningVertex &vertex = skinning.Vertices[vertexIndex];
					std::vector<ScriptValue> influences;
					influences.reserve(vertex.Weights.size());
					for (size_t influence = 0; influence < vertex.Weights.size(); ++influence) {
						const uint16_t slot = vertex.Joints[influence];
						influences.push_back(Map({
							{"joint_id", String(entityId + ":joint:" + std::to_string(slot))},
							{"joint_slot", Number(slot)},
							{"weight", Number(vertex.Weights[influence])},
						}));
					}
					vertices.push_back(Map(
						{{"vertex_index", Number(vertexIndex)}, {"influences", Array(std::move(influences))}}
					));
				}
				exportedSkinning = Map({
					{"available", Boolean(true)},
					{"unavailable_reason", ScriptValue{}},
					{"mesh_id", String(visual->Mesh.Text())},
					{"position_space", String("mesh_object")},
					{"joint_index_space", String("skeleton_palette_slot")},
					{"weight_encoding", String("uint16_unorm")},
					{"weight_denominator", Number(65535)},
					{"normalization",
					 String("each weighted vertex sums to 65535; zero-weight influences are retained")},
					{"vertices", Array(std::move(vertices))},
				});
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
				{"skinning", std::move(exportedSkinning)},
				{"clips", Array(std::move(exportedClips))},
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
