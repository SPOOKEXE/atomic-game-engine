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

		const scene::Transform *Transform = nullptr;
		const scene::PreviousTransform *PreviousTransform = nullptr;
		const scene::Bounds *Bounds = nullptr;
		const scene::Visual *Visual = nullptr;
		const scene::SurfaceAppearance *Appearance = nullptr;
		const scene::Tags *Tags = nullptr;
		const scene::LocalTransparency *Transparency = nullptr;
		const scene::CharacterLimb *Limb = nullptr;
		const scene::LODAuto *AutomaticLod = nullptr;
		const scene::LODCustom *CustomLod = nullptr;
		const scene::LODSettings *LodSettings = nullptr;
		const scene::RenderEffects *Effects = nullptr;

		static PresentationSource Of(const ecs::Store &store, ecs::Entity entity) {
			return {
				.Transform = store.Get<scene::Transform>(entity),
				.PreviousTransform = store.Get<scene::PreviousTransform>(entity),
				.Bounds = store.Get<scene::Bounds>(entity),
				.Visual = store.Get<scene::Visual>(entity),
				.Appearance = store.Get<scene::SurfaceAppearance>(entity),
				.Tags = store.Get<scene::Tags>(entity),
				.Transparency = store.Get<scene::LocalTransparency>(entity),
				.Limb = store.Get<scene::CharacterLimb>(entity),
				.AutomaticLod = store.Get<scene::LODAuto>(entity),
				.CustomLod = store.Get<scene::LODCustom>(entity),
				.LodSettings = store.Get<scene::LODSettings>(entity),
				.Effects = store.Get<scene::RenderEffects>(entity),
			};
		}

		static bool IsWorldDrawable(const ecs::Store &store, ecs::Entity entity) {
			return store.Has<scene::Transform>(entity) && store.Has<scene::PreviousTransform>(entity) &&
				   store.Has<scene::Bounds>(entity) && store.Has<scene::Visual>(entity) &&
				   store.Has<scene::SurfaceAppearance>(entity) && store.Has<scene::Tags>(entity) &&
				   store.Has<scene::LocalTransparency>(entity) && store.Has<scene::Rendered>(entity);
		}

		bool IsViewportDrawable() const {
			return Transform != nullptr && Bounds != nullptr && Visual != nullptr;
		}

		scene::DrawInstance MakeDrawInstance(
			ecs::Entity entity,
			const core::CFrame &frame,
			LocalTransparencyMode transparencyMode = LocalTransparencyMode::Include
		) const {
			return scene::MakeDrawInstance(
				frame,
				*Bounds,
				*Visual,
				Appearance,
				Tags,
				entity.Id,
				transparencyMode == LocalTransparencyMode::Include ? Transparency : nullptr,
				Limb,
				AutomaticLod,
				CustomLod,
				LodSettings,
				Effects
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
