#include "PresentationSourceRows.hpp"

#include "PresentationSource.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Visibility.hpp>

#include <algorithm>
#include <atomic>

namespace engine::render {
	namespace {
		bool SameFrame(const core::CFrame &left, const core::CFrame &right) {
			return left.Position == right.Position && left.QuaternionX == right.QuaternionX &&
				   left.QuaternionY == right.QuaternionY && left.QuaternionZ == right.QuaternionZ &&
				   left.QuaternionW == right.QuaternionW;
		}
	}

	size_t RefreshPresentationSourceRows(ecs::Store &store, DrawList &drawList, float alpha, size_t grain) {
		using scene::Bounds;
		using scene::CharacterLimb;
		using scene::LocalTransparency;
		using scene::PreviousTransform;
		using scene::SurfaceAppearance;
		using scene::Tags;
		using scene::Transform;
		using scene::Visual;

		std::atomic_bool hasInterpolation = false;
		std::atomic_bool sourceOrderChanged = false;
		scene::DrawInstance *const out = drawList.Instances.data();
		const size_t capacity = drawList.Instances.size();
		const auto write = [out, capacity, alpha, &hasInterpolation, &sourceOrderChanged](
							   size_t base,
							   size_t first,
							   size_t rows,
							   const ecs::Entity *entities,
							   const Transform *transforms,
							   const PreviousTransform *previous
						   ) {
			const size_t at = base + first;
			if (at >= capacity) return;
			rows = std::min(rows, capacity - at);
			bool foundInterpolation = false;
			for (size_t row = 0; row < rows; row++) {
				if (out[at + row].Source != entities[row].Id) {
					sourceOrderChanged.store(true, std::memory_order_relaxed);
					continue;
				}
				const bool moving = !SameFrame(previous[row].Frame, transforms[row].Frame);
				foundInterpolation |= moving;
				out[at + row].Frame =
					moving ? previous[row].Frame.NLerp(transforms[row].Frame, alpha) : transforms[row].Frame;
			}
			if (foundInterpolation) hasInterpolation.store(true, std::memory_order_relaxed);
		};

		const size_t loose =
			PresentationSource::QueryWorldDrawables(store).Without<CharacterLimb>().EachBatchEntitiesParallel(
				[&write](
					size_t first,
					size_t rows,
					const ecs::Entity *entities,
					const Transform *transforms,
					const PreviousTransform *previous,
					const Bounds *,
					const Visual *,
					const SurfaceAppearance *,
					const Tags *,
					const LocalTransparency *
				) { write(0, first, rows, entities, transforms, previous); },
				grain
			);
		const size_t rigged = PresentationSource::QueryRiggedWorldDrawables(store).EachBatchEntitiesParallel(
			[&write, loose](
				size_t first,
				size_t rows,
				const ecs::Entity *entities,
				const Transform *transforms,
				const PreviousTransform *previous,
				const Bounds *,
				const Visual *,
				const SurfaceAppearance *,
				const Tags *,
				const LocalTransparency *,
				const CharacterLimb *
			) { write(loose, first, rows, entities, transforms, previous); },
			grain
		);
		drawList.HasInterpolation = hasInterpolation.load(std::memory_order_relaxed);
		return sourceOrderChanged.load(std::memory_order_relaxed) ? 0 : loose + rigged;
	}
}
