#include "PresentationSourceDamage.hpp"

#include "PresentationSource.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Skinning.hpp>
#include <engine/scene/Visibility.hpp>

namespace engine::render {

	namespace {
		using ecs::Entity;
		using ecs::Store;
		using scene::Bone;
		using scene::Bounds;
		using scene::CharacterLimb;
		using scene::LocalTransparency;
		using scene::LODAuto;
		using scene::LODCustom;
		using scene::LODSettings;
		using scene::PreviousTransform;
		using scene::Rendered;
		using scene::RenderEffects;
		using scene::Skeleton;
		using scene::SurfaceAppearance;
		using scene::Tags;
		using scene::Transform;
		using scene::Visual;

		template <class Component, class Relevant>
		bool SourceRevisionChanged(Store &store, DrawList &drawList, uint64_t &seen, Relevant &&relevant) {
			store.Observe<Component>();
			const uint64_t revision = store.ComponentChangeVersion<Component>();
			if (!drawList.SourcesReady || revision == seen) {
				seen = revision;
				return false;
			}
			seen = revision;

			bool visited = false;
			bool changed = false;
			store.EachChanged<Component>([&](Entity entity, Component &) {
				visited = true;
				changed |= relevant(entity);
			});
			// A world may have ticked while it was not presented. Its monotonic
			// revision survives, but the row bits do not, so an empty walk is not
			// proof that the cached list is current.
			return changed || !visited;
		}

		template <class Component>
		bool SourceRevisionAdvanced(Store &store, DrawList &drawList, uint64_t &seen) {
			store.Observe<Component>();
			const uint64_t revision = store.ComponentChangeVersion<Component>();
			const bool changed = drawList.SourcesReady && revision != seen;
			seen = revision;
			return changed;
		}
	}

	PresentationSourceDamage CollectPresentationSourceDamage(
		ecs::Store &store, DrawList &drawList, size_t matching, size_t skeletons, size_t bones
	) {
		PresentationSourceDamage damage;
		damage.Full = !drawList.SourcesReady || matching != drawList.SourceEntityCount ||
					  skeletons != drawList.SkeletonCount || bones != drawList.BoneCount;
		const auto drawable = [&store](ecs::Entity entity) {
			return PresentationSource::IsWorldDrawable(store, entity);
		};

		// Pose columns only affect the interpolated frame or the skin palette.
		// A camera orbit writes Transform every frame but the camera has no draw
		// row. Filtering the changed walk avoids rebuilding static rows for it.
		damage.Pose |=
			SourceRevisionChanged<Transform>(store, drawList, drawList.Revisions.Transform, drawable);
		damage.Pose |= SourceRevisionChanged<PreviousTransform>(
			store, drawList, drawList.Revisions.PreviousTransform, drawable
		);
		damage.Full |= SourceRevisionChanged<Bounds>(store, drawList, drawList.Revisions.Bounds, drawable);
		damage.Full |= SourceRevisionChanged<Visual>(store, drawList, drawList.Revisions.Visual, drawable);
		damage.Full |= SourceRevisionChanged<SurfaceAppearance>(
			store, drawList, drawList.Revisions.SurfaceAppearance, drawable
		);
		damage.Full |= SourceRevisionChanged<Tags>(store, drawList, drawList.Revisions.Tags, drawable);
		damage.Full |= SourceRevisionChanged<LocalTransparency>(
			store, drawList, drawList.Revisions.LocalTransparency, drawable
		);
		damage.Full |=
			SourceRevisionChanged<CharacterLimb>(store, drawList, drawList.Revisions.CharacterLimb, drawable);
		damage.Full |= SourceRevisionChanged<LODAuto>(store, drawList, drawList.Revisions.LODAuto, drawable);
		damage.Full |=
			SourceRevisionChanged<LODCustom>(store, drawList, drawList.Revisions.LODCustom, drawable);
		damage.Full |=
			SourceRevisionChanged<LODSettings>(store, drawList, drawList.Revisions.LODSettings, drawable);
		damage.Full |=
			SourceRevisionChanged<RenderEffects>(store, drawList, drawList.Revisions.RenderEffects, drawable);
		damage.Pose |= SourceRevisionAdvanced<Skeleton>(store, drawList, drawList.Revisions.Skeleton);
		damage.Pose |= SourceRevisionAdvanced<Bone>(store, drawList, drawList.Revisions.Bone);

		store.Observe<Rendered>();
		const uint64_t renderedRevision = store.ComponentChangeVersion<Rendered>();
		damage.Full |= drawList.SourcesReady && renderedRevision != drawList.Revisions.Rendered;
		drawList.Revisions.Rendered = renderedRevision;
		drawList.SourceEntityCount = matching;
		drawList.SkeletonCount = skeletons;
		drawList.BoneCount = bones;
		drawList.SourcesReady = true;
		return damage;
	}
}
