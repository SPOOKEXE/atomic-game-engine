#include <engine/ecs/Store.hpp>
#include <engine/render/Animation.hpp>
#include <engine/scene/Animation.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Skinning.hpp>
#include <engine/scene/UpdateCadence.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace engine::render {
	namespace {
		constexpr size_t PRIORITIES = static_cast<size_t>(scene::AnimationPriority::Override) + 1;

		struct PlayingTrack {
			const scene::AnimationTrack *State = nullptr;
			const assets::AnimationData *Clip = nullptr;
		};

		const scene::UpdateCadence *
		PolicyFor(const ecs::Store &store, ecs::Entity animator, ecs::Entity rig) {
			if (const auto *policy = store.Get<scene::UpdateCadence>(animator)) return policy;
			for (ecs::Entity walk = rig; walk != ecs::NULL_ENTITY; walk = store.ParentOf(walk)) {
				if (const auto *policy = store.Get<scene::UpdateCadence>(walk)) return policy;
			}
			return nullptr;
		}

		bool PlayerOwned(const ecs::Store &store, ecs::Entity subject) {
			for (ecs::Entity walk = subject; walk != ecs::NULL_ENTITY; walk = store.ParentOf(walk)) {
				const auto *character = store.Get<scene::Character>(walk);
				const auto *owner = store.Get<scene::NetworkOwner>(walk);
				if ((character != nullptr && character->Owner != ecs::NULL_ENTITY) ||
					(owner != nullptr && owner->Player != ecs::NULL_ENTITY))
					return true;
			}
			return scene::PlayerOwning(store, subject) != ecs::NULL_ENTITY;
		}

		struct Layer {
			core::Vector3 Position;
			glm::vec4 Rotation{};
			glm::vec4 Reference{};
			float Weight = 0.0f;
			bool HasReference = false;
		};

		bool Same(const core::CFrame &left, const core::CFrame &right) {
			return left.Position == right.Position && left.QuaternionX == right.QuaternionX &&
				   left.QuaternionY == right.QuaternionY && left.QuaternionZ == right.QuaternionZ &&
				   left.QuaternionW == right.QuaternionW;
		}

		float ClipTime(const scene::AnimationTrack &track, const assets::AnimationData &clip) {
			if (!std::isfinite(track.TimePosition)) {
				return 0.0f;
			}
			if (!track.Looped) {
				return std::clamp(track.TimePosition, 0.0f, clip.Duration);
			}
			float time = std::fmod(track.TimePosition, clip.Duration);
			return time < 0.0f ? time + clip.Duration : time;
		}

		const assets::AnimationChannel *ChannelFor(const assets::AnimationData &clip, uint16_t joint) {
			const auto found = std::lower_bound(
				clip.Channels.begin(),
				clip.Channels.end(),
				joint,
				[](const assets::AnimationChannel &channel, uint16_t wanted) {
					return channel.Joint < wanted;
				}
			);
			return found != clip.Channels.end() && found->Joint == joint ? &*found : nullptr;
		}

		core::CFrame Sample(const assets::AnimationChannel &channel, float time) {
			const auto after = std::upper_bound(
				channel.Keys.begin(),
				channel.Keys.end(),
				time,
				[](float wanted, const assets::AnimationKeyframe &key) { return wanted < key.Time; }
			);
			if (after == channel.Keys.begin()) {
				return after->Transform;
			}
			if (after == channel.Keys.end()) {
				return channel.Keys.back().Transform;
			}
			const assets::AnimationKeyframe &first = *(after - 1);
			const float alpha = (time - first.Time) / (after->Time - first.Time);
			return first.Transform.Lerp(after->Transform, alpha);
		}

		void Accumulate(Layer &layer, const core::CFrame &sample, float weight) {
			glm::vec4 rotation{
				sample.QuaternionX, sample.QuaternionY, sample.QuaternionZ, sample.QuaternionW
			};
			if (!layer.HasReference) {
				layer.Reference = rotation;
				layer.HasReference = true;
			} else if (glm::dot(layer.Reference, rotation) < 0.0f) {
				rotation = -rotation;
			}
			layer.Position = layer.Position + sample.Position * weight;
			layer.Rotation += rotation * weight;
			layer.Weight += weight;
		}

		core::CFrame PoseOf(uint16_t joint, const std::vector<PlayingTrack> &tracks) {
			std::array<Layer, PRIORITIES> layers{};
			for (const PlayingTrack &playing : tracks) {
				const assets::AnimationChannel *channel = ChannelFor(*playing.Clip, joint);
				const float weight = std::clamp(playing.State->Weight, 0.0f, 1.0f);
				if (channel == nullptr || weight <= 0.0f) {
					continue;
				}
				Accumulate(
					layers[static_cast<size_t>(playing.State->Priority)],
					Sample(*channel, ClipTime(*playing.State, *playing.Clip)),
					weight
				);
			}

			core::CFrame pose;
			for (const Layer &layer : layers) {
				if (layer.Weight <= 0.0f) {
					continue;
				}
				const glm::vec4 rotation = glm::length(layer.Rotation) > 1e-6f
											   ? glm::normalize(layer.Rotation)
											   : glm::vec4{0, 0, 0, 1};
				const core::CFrame blended(
					layer.Position * (1.0f / layer.Weight),
					glm::quat(rotation.w, rotation.x, rotation.y, rotation.z)
				);
				pose = pose.Lerp(blended, std::min(layer.Weight, 1.0f));
			}
			return pose;
		}

		const assets::AnimationData *FindBufferedAnimation(ecs::Store &store, ecs::Entity entity) {
			const scene::AnimationBuffer *source = store.Get<scene::AnimationBuffer>(entity);
			if (source == nullptr || source->Data.empty()) {
				return nullptr;
			}
			if (!store.HasResource<AnimationCatalogue>()) {
				store.SetResource(AnimationCatalogue{});
			}

			AnimationCatalogue::BufferedClip &cached =
				store.ResourceMutable<AnimationCatalogue>()->Buffers[entity];
			if (!cached.Loaded || cached.Revision != source->Revision) {
				cached.Revision = source->Revision;
				cached.Loaded = true;
				cached.Clip.reset();
				core::ByteReader reader(source->Data);
				assets::AnimationData decoded;
				if (assets::Animation::Read(reader, decoded) && reader.AtEnd()) {
					cached.Clip = std::move(decoded);
				}
			}
			return cached.Clip.has_value() ? &*cached.Clip : nullptr;
		}
	}

	bool RecordAnimation(ecs::Store &store, const core::Name &name, const assets::AnimationData &clip) {
		if (!name.IsValid() || !clip.IsValid()) {
			return false;
		}
		if (!store.HasResource<AnimationCatalogue>()) {
			store.SetResource(AnimationCatalogue{});
		}
		store.ResourceMutable<AnimationCatalogue>()->Clips[name.Id()] = clip;
		return true;
	}

	const assets::AnimationData *FindAnimation(const ecs::Store &store, const core::Name &name) {
		const AnimationCatalogue *catalogue = store.Resource<AnimationCatalogue>();
		if (catalogue == nullptr || !name.IsValid()) {
			return nullptr;
		}
		const auto found = catalogue->Clips.find(name.Id());
		return found == catalogue->Clips.end() ? nullptr : &found->second;
	}

	size_t EvaluateAnimations(ecs::Store &store) {
		size_t written = 0;
		uint64_t evaluated = 0;
		auto *history = store.ResourceMutable<scene::PoseCadenceHistory>();
		if (history != nullptr) {
			std::erase_if(history->Poses, [&](const auto &entry) {
				return !store.Alive(entry.first) || store.Get<scene::Animator>(entry.first) == nullptr;
			});
			history->Evaluated = 0;
			history->Skipped = 0;
			history->Promotions = 0;
			history->MaximumAgeTicks = 0;
			history->CapacityRefused = 0;
			history->Unavailable = 0;
		}
		store.Each<const scene::Animator>([&](ecs::Entity animator, const scene::Animator &driver) {
			const ecs::Entity rig = scene::RigFor(store, animator);
			const scene::Skeleton *skeleton = store.Get<scene::Skeleton>(rig);
			if (skeleton == nullptr || !std::isfinite(skeleton->PoseScale) || skeleton->PoseScale <= 0) {
				return;
			}

			const auto *policy = PolicyFor(store, animator, rig);
			scene::DomainCadenceState *cadenceState = nullptr;
			scene::CadenceDecision admission;
			if (policy != nullptr && scene::ValidUpdateCadence(*policy)) {
				if (history == nullptr) {
					store.SetResource(scene::PoseCadenceHistory{});
					history = store.ResourceMutable<scene::PoseCadenceHistory>();
				}
				auto found = history->Poses.find(animator);
				if (found == history->Poses.end() &&
					history->Poses.size() < scene::PoseCadenceHistory::MAXIMUM_POSES)
					found = history->Poses.try_emplace(animator).first;
				if (found == history->Poses.end()) {
					history->CapacityRefused++;
				} else {
					cadenceState = &found->second;
					uint32_t critical = 0;
					if (PlayerOwned(store, rig) || PlayerOwned(store, animator) ||
						(policy->Owner != ecs::NULL_ENTITY && PlayerOwned(store, policy->Owner)))
						critical |= static_cast<uint32_t>(scene::UpdateCriticality::PlayerOwned);
					if (!driver.EvaluationThrottled || driver.RootMotion)
						critical |= static_cast<uint32_t>(scene::UpdateCriticality::Required);
					admission = scene::CadenceDue(
						*policy,
						*cadenceState,
						scene::UpdateDomain::VisualPose,
						store.Time().Tick,
						policy->Revision,
						critical
					);
					history->MaximumAgeTicks = std::max(history->MaximumAgeTicks, admission.AgeTicks);
					// Full-fidelity poses remain responsive to edits between fixed ticks.
					const bool fullFidelity =
						admission.CriticalReasons != 0 ||
						policy->Domains[static_cast<size_t>(scene::UpdateDomain::VisualPose)].IntervalTicks ==
							1 ||
						!policy->Identity.IsValid();
					if (!admission.Due && !fullFidelity) {
						history->Skipped++;
						return;
					}
				}
			}

			std::vector<PlayingTrack> tracks;
			bool missingClip = false;
			store.EachDescendant(animator, [&](ecs::Entity entity) {
				const scene::AnimationTrack *track = store.Get<scene::AnimationTrack>(entity);
				if (track == nullptr || !track->Playing || track->Clip == ecs::NULL_ENTITY) {
					return;
				}
				const scene::AnimationClip *reference = store.Get<scene::AnimationClip>(track->Clip);
				if (reference == nullptr || !scene::ClipFitsRig(*reference, *skeleton)) {
					return;
				}
				const assets::AnimationData *clip = reference->Buffer != ecs::NULL_ENTITY
														? FindBufferedAnimation(store, reference->Buffer)
														: FindAnimation(store, reference->Asset);
				if (clip != nullptr) {
					tracks.push_back(PlayingTrack{track, clip});
				} else {
					missingClip = true;
				}
			});
			// A pending streamed clip cannot turn retained bones into a successful update.
			if (cadenceState != nullptr && missingClip) {
				history->Unavailable++;
				return;
			}

			store.EachDescendant(rig, [&](ecs::Entity entity) {
				const scene::Bone *bone = store.Get<scene::Bone>(entity);
				if (bone == nullptr || bone->Joint >= skeleton->JointCount) {
					return;
				}
				core::CFrame pose = PoseOf(bone->Joint, tracks);
				pose.Position = pose.Position * skeleton->PoseScale;
				if (!Same(pose, bone->Transform)) {
					store.GetMutable<scene::Bone>(entity)->Transform = pose;
					written++;
				}
			});
			evaluated++;
			if (cadenceState != nullptr && admission.Due) {
				if (admission.CriticalReasons != 0 && cadenceState->CriticalReasons == 0)
					history->Promotions++;
				(void)scene::CommitUpdate(
					*cadenceState, admission, store.Time().Tick, policy->Revision, policy->Revision, true
				);
			}
		});
		if (history != nullptr) history->Evaluated = evaluated;
		return written;
	}
}
