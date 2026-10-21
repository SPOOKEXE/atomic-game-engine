#pragma once

#include <engine/ecs/Store.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/SurfaceCameras.hpp>

#include <client/Scene.hpp>
#include <cmath>
#include <limits>
#include <optional>
#include <span>

namespace client {
	// Raster and visibility poses share one route, including its change of distance units.
	// Distinct poses require a valid lens with a finite, positive NearPlane.
	inline engine::world::WorldId ResolvePortalEyeView(
		engine::world::Universe &universe,
		engine::world::WorldId inputWorld,
		engine::world::WorldId visualWorld,
		engine::render::View &view,
		engine::render::PortalImageHost *images = nullptr,
		engine::render::PortalImageHost::Time now = std::chrono::steady_clock::now(),
		engine::world::WorldId topologyOwner = {}
	) {
		const auto sourceVisibility = view.VisibilityCameraFrame();
		auto visibility = sourceVisibility;
		auto lens = view.Camera;
		const auto selected = ResolveCameraPortalWorld(
			universe, inputWorld, visualWorld, visibility, lens, images, now, topologyOwner
		);
		if (!selected.IsValid()) return selected;
		const bool coincident = view.CameraFrame.Position == sourceVisibility.Position &&
								view.CameraFrame.Rotation() == sourceVisibility.Rotation();
		if (coincident) {
			view.CameraFrame = visibility;
		} else {
			auto relative = sourceVisibility.ToObjectSpace(view.CameraFrame);
			relative.Position = relative.Position * (lens.NearPlane / view.Camera.NearPlane);
			view.CameraFrame = visibility * relative;
		}
		view.VisibilityFrame = visibility;
		view.Camera = lens;
		return selected;
	}

	inline std::optional<engine::core::Vector3> PortalHandoffBody(const engine::ecs::Store &store) {
		const auto *local = store.Resource<engine::scene::LocalPlayer>();
		if (local == nullptr || !store.Alive(local->Instance)) return std::nullopt;
		const auto *rig =
			store.Get<engine::scene::Character>(engine::scene::CharacterOf(store, local->Instance));
		if (rig == nullptr || rig->Owner != local->Instance || !store.Alive(rig->Root)) return std::nullopt;
		const auto *root = store.Get<engine::scene::Transform>(rig->Root);
		if (root == nullptr) return std::nullopt;
		return root->Frame.Position;
	}

	// A body can be admitted before its following eye crosses the paired pane.
	// Keep looking through the old world only for that interval and that endpoint.
	inline std::optional<engine::scene::SeamTransform> TrailingPortalEye(
		std::span<const engine::scene::PortalSeam> seams,
		engine::core::Name previousWorld,
		const engine::core::Vector3 &eye,
		const engine::core::Vector3 &body
	) {
		const engine::scene::PortalSeam *nearest = nullptr;
		float nearestDistance = std::numeric_limits<float>::infinity();
		bool ambiguous = false;
		for (const auto &seam : seams) {
			if (!seam.Crosses || seam.DestinationWorld != previousWorld) continue;
			const float eyeSide = engine::scene::SeamOffset(seam, eye);
			const float bodySide = engine::scene::SeamOffset(seam, body);
			if (!(eyeSide * bodySide < 0.0f)) continue;
			const float fraction = eyeSide / (eyeSide - bodySide);
			const auto crossing = eye + (body - eye) * fraction;
			if (engine::scene::SeamDistance(seam, crossing) > 0.01f) continue;
			const float distance = std::abs(eyeSide);
			if (distance + 0.01f < nearestDistance) {
				nearest = &seam;
				nearestDistance = distance;
				ambiguous = false;
			} else if (std::abs(distance - nearestDistance) <= 0.01f) {
				ambiguous = true;
			}
		}
		if (nearest == nullptr || ambiguous) return std::nullopt;
		return engine::scene::SeamMapping(*nearest);
	}
}
