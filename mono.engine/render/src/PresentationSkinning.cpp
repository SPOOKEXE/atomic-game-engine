#include <engine/core/Profiling.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/render/WorldPresentation.hpp>
#include <engine/scene/Skinning.hpp>

#include <cmath>
#include <limits>

namespace engine::render {
	void CollectSkinPalettes(ecs::Store &store, DrawList &drawList) {
		ENGINE_PROFILE_CAT("build skin palettes", engine::core::ProfileCategory::Simulation);
		drawList.JointFrames.clear();
		for (scene::DrawInstance &instance : drawList.Instances) {
			instance.SkinFirst = 0;
			instance.SkinCount = 0;
			const ecs::Entity source(instance.Source);
			const scene::Skeleton *skeleton = store.Get<scene::Skeleton>(source);
			if (skeleton == nullptr || skeleton->JointCount == 0 ||
				skeleton->JointCount > scene::MAX_JOINTS || !std::isfinite(skeleton->PoseScale) ||
				skeleton->PoseScale <= 0)
				continue;
			instance.SkinFirst = static_cast<uint32_t>(drawList.JointFrames.size());
			instance.SkinCount = skeleton->JointCount;
			drawList.JointFrames.resize(drawList.JointFrames.size() + skeleton->JointCount);
			store.EachDescendant(source, [&](ecs::Entity descendant) {
				const scene::Bone *bone = store.Get<scene::Bone>(descendant);
				if (bone != nullptr && bone->Joint < skeleton->JointCount) {
					auto frame = instance.Frame.Inverse() * scene::SkinningFrameOf(*bone);
					frame.Position = frame.Position * (1.0f / skeleton->PoseScale);
					drawList.JointFrames[instance.SkinFirst + bone->Joint] = frame;
				}
			});
		}
	}

	void RebaseSkinPalettes(
		std::span<scene::DrawInstance> instances,
		std::span<const core::CFrame> source,
		std::vector<core::CFrame> &destination
	) {
		for (scene::DrawInstance &instance : instances) {
			const uint64_t end = static_cast<uint64_t>(instance.SkinFirst) + instance.SkinCount;
			if (instance.SkinCount == 0 || end > source.size() ||
				destination.size() > std::numeric_limits<uint32_t>::max() - instance.SkinCount) {
				instance.SkinFirst = 0;
				instance.SkinCount = 0;
				continue;
			}
			const size_t first = instance.SkinFirst;
			instance.SkinFirst = static_cast<uint32_t>(destination.size());
			destination.insert(destination.end(), source.begin() + first, source.begin() + end);
		}
	}
}
