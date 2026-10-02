#pragma once
#include <engine/imagegraph/VerletReplay.hpp>

#include <cmath>
namespace engine::imagegraph::detail {
	inline bool
	ValidSourceVerletCollider(const VerletCollider &collider, bool requireEllipseDenominator = true) {
		if (collider.Shape < 0 || collider.Shape > 1) return false;
		const auto &area = collider.Region;
		for (const double item : {area.CenterX, area.CenterY, area.HalfWidth, area.HalfHeight})
			if (!std::isfinite(item)) return false;
		if (collider.Shape == 0 || !requireEllipseDenominator) return true;
		const double x = area.HalfWidth * area.HalfWidth, y = area.HalfHeight * area.HalfHeight;
		return std::isfinite(x) && std::isfinite(y) && x > 0 && y > 0;
	}
	// The source reuses its collider cursor inside the point loop, skipping later
	// colliders or attempting an undefined point access. Preserve both outcomes.
	inline bool ApplySourceVerletColliders(VerletMesh &mesh, std::span<const VerletCollider> colliders) {
		for (size_t index = 0; index < colliders.size(); ++index) {
			const auto collider = colliders[index];
			for (size_t visited = 0; visited < mesh.Points.size(); ++visited) {
				if (index >= mesh.Points.size()) return false;
				auto &point = mesh.Points[index++];
				if (point.Rest) continue;
				const auto &area = collider.Region;
				bool inside;
				if (collider.Shape == 0)
					inside = point.Position.X >= area.CenterX - area.HalfWidth &&
							 point.Position.X <= area.CenterX + area.HalfWidth &&
							 point.Position.Y >= area.CenterY - area.HalfHeight &&
							 point.Position.Y <= area.CenterY + area.HalfHeight;
				else {
					const double dx = point.Position.X - area.CenterX, dy = point.Position.Y - area.CenterY;
					inside = dx * dx / (area.HalfWidth * area.HalfWidth) +
								 dy * dy / (area.HalfHeight * area.HalfHeight) <=
							 1;
				}
				if (inside) point.Position = point.BeforePrevious;
			}
		}
		return true;
	}
}
