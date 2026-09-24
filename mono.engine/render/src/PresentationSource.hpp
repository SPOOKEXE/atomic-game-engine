#pragma once

// One ECS source as seen by world and viewport presentation collectors.

#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/scene/Visibility.hpp>

#include <tuple>

namespace engine::render {
	namespace presentation_source_detail {
		template <class... Types> struct ComponentList {};

		template <class Left, class Right> struct Append;

		template <class... Left, class... Right>
		struct Append<ComponentList<Left...>, ComponentList<Right...>> {
			using Type = ComponentList<Left..., Right...>;
		};

		template <class Left, class Right> using AppendType = typename Append<Left, Right>::Type;
	}

	struct PresentationSource {
		enum class LocalTransparencyMode {
			Include,
			Ignore,
		};

		using WorldFields = presentation_source_detail::ComponentList<
			scene::Transform,
			scene::PreviousTransform,
			scene::Bounds,
			scene::Visual,
			scene::SurfaceAppearance,
			scene::Tags,
			scene::LocalTransparency>;
		using WorldFilters = presentation_source_detail::ComponentList<scene::Rendered>;
		using WorldDrawable = presentation_source_detail::AppendType<WorldFields, WorldFilters>;
		using RiggedWorldFields = presentation_source_detail::
			AppendType<WorldFields, presentation_source_detail::ComponentList<scene::CharacterLimb>>;
		using ViewportFields =
			presentation_source_detail::ComponentList<scene::Transform, scene::Bounds, scene::Visual>;
		using RenderStateFields = presentation_source_detail::
			ComponentList<scene::LODAuto, scene::LODCustom, scene::LODSettings, scene::RenderEffects>;

		template <class... Components>
		static bool HasAll(
			const ecs::Store &store,
			ecs::Entity entity,
			presentation_source_detail::ComponentList<Components...>
		) {
			return (store.Has<Components>(entity) && ...);
		}

		template <class... Components>
		static size_t
		CountMatching(ecs::Store &store, presentation_source_detail::ComponentList<Components...>) {
			return store.CountMatching<Components...>();
		}

		template <class... Fields, class... Required>
		static auto Query(
			ecs::Store &store,
			presentation_source_detail::ComponentList<Fields...>,
			presentation_source_detail::ComponentList<Required...>
		) {
			return store.Query<const Fields...>().template With<Required...>();
		}

		static bool IsWorldDrawable(const ecs::Store &store, ecs::Entity entity) {
			return HasAll(store, entity, WorldDrawable{});
		}

		static bool IsViewportDrawable(const ecs::Store &store, ecs::Entity entity) {
			return HasAll(store, entity, ViewportFields{});
		}

		static size_t CountWorldDrawables(ecs::Store &store) {
			return CountMatching(store, WorldDrawable{});
		}

		static auto QueryWorldDrawables(ecs::Store &store) {
			return Query(store, WorldFields{}, WorldFilters{});
		}

		static auto QueryRiggedWorldDrawables(ecs::Store &store) {
			return Query(store, RiggedWorldFields{}, WorldFilters{});
		}

		template <class... Components>
		static auto ReadComponents(
			const ecs::Store &store,
			ecs::Entity entity,
			presentation_source_detail::ComponentList<Components...>
		) {
			return std::tuple<const Components *...>{store.Get<Components>(entity)...};
		}

		static void
		ApplyOptionalRenderState(const ecs::Store &store, ecs::Entity entity, scene::DrawInstance &instance) {
			const auto inputs = ReadComponents(store, entity, RenderStateFields{});
			std::apply(
				[&](auto... components) { scene::ApplyDrawRenderState(instance, components...); }, inputs
			);
		}

		// Builds the shared geometry row from already resolved query columns.
		static scene::DrawInstance MakeDrawInstance(
			const core::CFrame &frame,
			const scene::Bounds &bounds,
			const scene::Visual &visual,
			const scene::SurfaceAppearance *appearance,
			const scene::Tags *tags,
			ecs::Entity entity,
			const scene::LocalTransparency *local,
			const scene::CharacterLimb *limb
		) {
			return scene::MakeDrawInstance(frame, bounds, visual, appearance, tags, entity.Id, local, limb);
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

			scene::DrawInstance instance = MakeDrawInstance(
				frame,
				*bounds,
				*visual,
				store.Get<scene::SurfaceAppearance>(entity),
				store.Get<scene::Tags>(entity),
				entity,
				transparencyMode == LocalTransparencyMode::Include
					? store.Get<scene::LocalTransparency>(entity)
					: nullptr,
				store.Get<scene::CharacterLimb>(entity)
			);
			ApplyOptionalRenderState(store, entity, instance);
			return instance;
		}
	};
}
