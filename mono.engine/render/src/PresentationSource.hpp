#pragma once

// One ECS source as seen by presentation collectors. World collection requires
// every field; a ViewportFrame intentionally needs only its geometry fields.

#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/scene/Visibility.hpp>

namespace engine::render {

	struct PresentationSource {
		enum class LocalTransparencyMode {
			Include,
			Ignore,
		};

		static bool IsWorldDrawable(const ecs::Store &store, ecs::Entity entity) {
			return store.Has<scene::Transform>(entity) && store.Has<scene::PreviousTransform>(entity) &&
				   store.Has<scene::Bounds>(entity) && store.Has<scene::Visual>(entity) &&
				   store.Has<scene::SurfaceAppearance>(entity) && store.Has<scene::Tags>(entity) &&
				   store.Has<scene::LocalTransparency>(entity) && store.Has<scene::Rendered>(entity);
		}

		static bool IsViewportDrawable(const ecs::Store &store, ecs::Entity entity) {
			return store.Has<scene::Transform>(entity) && store.Has<scene::Bounds>(entity) &&
				   store.Has<scene::Visual>(entity);
		}

		static scene::DrawInstance MakeDrawInstance(
			const ecs::Store &store,
			ecs::Entity entity,
			const core::CFrame &frame,
			LocalTransparencyMode transparencyMode = LocalTransparencyMode::Include
		) {
			const auto *bounds = store.Get<scene::Bounds>(entity);
			const auto *visual = store.Get<scene::Visual>(entity);
			if (bounds == nullptr || visual == nullptr) {
				return {};
			}
			return scene::MakeDrawInstance(
				frame,
				*bounds,
				*visual,
				store.Get<scene::SurfaceAppearance>(entity),
				store.Get<scene::Tags>(entity),
				entity.Id,
				transparencyMode == LocalTransparencyMode::Include
					? store.Get<scene::LocalTransparency>(entity)
					: nullptr,
				store.Get<scene::CharacterLimb>(entity),
				store.Get<scene::LODAuto>(entity),
				store.Get<scene::LODCustom>(entity),
				store.Get<scene::LODSettings>(entity),
				store.Get<scene::RenderEffects>(entity)
			);
		}

		static void
		ApplyOptionalRenderState(const ecs::Store &store, ecs::Entity entity, scene::DrawInstance &instance) {
			scene::ApplyDrawRenderState(
				instance,
				store.Get<scene::LODAuto>(entity),
				store.Get<scene::LODCustom>(entity),
				store.Get<scene::LODSettings>(entity),
				store.Get<scene::RenderEffects>(entity)
			);
		}
	};
}
