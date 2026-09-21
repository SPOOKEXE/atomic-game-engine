#include "LodPreview.hpp"

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/LevelOfDetail.hpp>
#include <engine/scene/MeshCatalogue.hpp>

#include <glm/vec4.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <studio/Projection.hpp>

namespace studio {
	namespace {
		constexpr float NEAR_W = 1e-4f;

		float ProjectedArea(
			const PanelProjection &panel, const engine::core::CFrame &frame, engine::core::Vector3 half
		) {
			if (!panel.IsValid()) {
				return 0.0f;
			}

			const engine::core::Vector3 right = frame.RightVector();
			const engine::core::Vector3 up = frame.UpVector();
			const engine::core::Vector3 forward = frame.LookVector();
			const engine::core::Vector3 extent{
				std::abs(right.X) * half.X + std::abs(up.X) * half.Y + std::abs(forward.X) * half.Z,
				std::abs(right.Y) * half.X + std::abs(up.Y) * half.Y + std::abs(forward.Y) * half.Z,
				std::abs(right.Z) * half.X + std::abs(up.Z) * half.Y + std::abs(forward.Z) * half.Z,
			};

			float minimumX = std::numeric_limits<float>::infinity();
			float minimumY = std::numeric_limits<float>::infinity();
			float maximumX = -std::numeric_limits<float>::infinity();
			float maximumY = -std::numeric_limits<float>::infinity();
			for (const float x : {-extent.X, extent.X}) {
				for (const float y : {-extent.Y, extent.Y}) {
					for (const float z : {-extent.Z, extent.Z}) {
						const engine::core::Vector3 point = frame.Position + engine::core::Vector3{x, y, z};
						const glm::vec4 clip = panel.Matrix * glm::vec4(point.X, point.Y, point.Z, 1.0f);
						if (clip.w <= NEAR_W) {
							// Match lod-select.comp: a box crossing the eye plane is
							// effectively screen-filling, so it keeps its finest level.
							return std::numeric_limits<float>::max();
						}
						const float ndcX = std::clamp(clip.x / clip.w, -1.0f, 1.0f);
						const float ndcY = std::clamp(clip.y / clip.w, -1.0f, 1.0f);
						minimumX = std::min(minimumX, ndcX);
						minimumY = std::min(minimumY, ndcY);
						maximumX = std::max(maximumX, ndcX);
						maximumY = std::max(maximumY, ndcY);
					}
				}
			}
			return (maximumX - minimumX) * panel.ImageSize.x * 0.5f * (maximumY - minimumY) *
				   panel.ImageSize.y * 0.5f;
		}

		uint8_t DistanceCap(float distance, const std::array<float, 3> &bands) {
			for (uint8_t level = 0; level < bands.size(); level++) {
				if (!(distance >= bands[level])) {
					return level;
				}
			}
			return static_cast<uint8_t>(bands.size());
		}
	}

	float CenteredLodLabelX(float objectMinimumX, float objectMaximumX, float labelWidth) {
		return (objectMinimumX + objectMaximumX - labelWidth) * 0.5f;
	}

	bool ShouldDrawActiveLodLabel(const engine::ecs::Store &store, engine::ecs::Entity instance) {
		const auto *visual = store.Get<engine::scene::Visual>(instance);
		const engine::ecs::ClassId meshPart = engine::ecs::Classes::Find(engine::core::Name("MeshPart"));
		return visual != nullptr && visual->Visible && store.IsA(instance, meshPart);
	}

	std::optional<uint8_t> ActiveLodForViewport(
		const engine::ecs::Store &store,
		const engine::ecs::Entity instance,
		const PanelProjection &panel,
		const std::array<float, 3> &distanceBands
	) {
		const auto *visual = store.Get<engine::scene::Visual>(instance);
		const auto *bounds = store.Get<engine::scene::Bounds>(instance);
		const auto *transform = store.Get<engine::scene::Transform>(instance);
		const auto *automatic = store.Get<engine::scene::AutoMeshLOD>(instance);
		const auto *custom = store.Get<engine::scene::CustomMeshLOD>(instance);
		const auto *catalogue = store.Resource<engine::scene::MeshCatalogue>();
		if (!visual || !bounds || !transform || !catalogue || !visual->Mesh.IsValid()) {
			return std::nullopt;
		}

		engine::scene::LevelOfDetail lod = engine::scene::ResolveMeshLOD(visual->Mesh, automatic, custom);
		if (catalogue->Find(visual->Mesh) == 0) {
			return std::nullopt;
		}
		if (lod.Levels <= 1) {
			return uint8_t{0};
		}
		for (uint8_t level = 1; level < lod.Levels; level++) {
			if (catalogue->Find(engine::scene::LevelMesh(lod, visual->Mesh, level)) == 0) {
				// The renderer keeps using the resident prefix while an automatic
				// ladder rebuilds. Report that same fallback instead of making the
				// label disappear during a world switch or property edit.
				lod.Levels = level;
				break;
			}
		}

		const float area = ProjectedArea(panel, transform->Frame, bounds->HalfExtent);
		const uint8_t selected = engine::scene::SelectLevel(lod, *catalogue, visual->Mesh, area);
		const float distance = (transform->Frame.Position - panel.Eye).Magnitude();
		return std::min(selected, DistanceCap(distance, distanceBands));
	}
}
