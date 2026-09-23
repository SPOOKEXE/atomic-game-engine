#pragma once

#include <engine/render/InterfacePass.hpp>
#include <engine/render/WorldView.hpp>
#include <engine/world/Universe.hpp>

namespace client {
	template <class Abort>
	bool BindNamedCaptureWorldView(
		engine::ecs::Store &store,
		const engine::render::WorldViewBinding &binding,
		const engine::core::Vector2 &extent,
		engine::render::View &view,
		engine::render::WorldViewFrame &frame,
		engine::render::WorldCameraFrame &camera,
		Abort &&abort
	) {
		engine::render::CollectWorldView(store, view.WorldName, frame);
		view.Portals = frame.Portals;
		engine::render::CollectWorldCamera(store, view, extent, camera, true);
		if (engine::render::BindWorldView(frame, camera, binding, view)) return true;
		abort();
		return false;
	}

	template <class Abort>
	bool BindNamedCapturePresentation(
		engine::ecs::Store &store,
		const engine::render::WorldViewBinding &binding,
		const engine::core::Vector2 &extent,
		engine::render::View &view,
		engine::render::WorldViewFrame &frame,
		engine::render::WorldCameraFrame &camera,
		engine::render::InterfacePass &interface,
		Abort &&abort
	) {
		if (!BindNamedCaptureWorldView(
				store, binding, extent, view, frame, camera, std::forward<Abort>(abort)
			))
			return false;
		interface.Submit(camera, extent, extent);
		return true;
	}

	// Rebuilds camera-dependent presentation data after a capture selected a
	// named camera. The caller commits the view only when this binding succeeds.
	template <class Abort>
	bool BindNamedCaptureWorldView(
		engine::world::Universe &universe,
		engine::world::WorldId world,
		const engine::render::WorldViewBinding &binding,
		const engine::core::Vector2 &extent,
		engine::render::View &view,
		engine::render::WorldViewFrame &frame,
		engine::render::WorldCameraFrame &camera,
		Abort &&abort
	) {
		bool bound = false;
		const auto status = universe.Enter(world, [&](engine::ecs::Store &store) {
			auto worldBinding = binding;
			worldBinding.Identity = store.Identity();
			bound = BindNamedCaptureWorldView(store, worldBinding, extent, view, frame, camera, [] {});
		});
		if (status == engine::world::WorldStatus::Ok && bound) return true;
		abort();
		return false;
	}

	template <class Abort>
	bool BindNamedCapturePresentation(
		engine::world::Universe &universe,
		engine::world::WorldId world,
		const engine::render::WorldViewBinding &binding,
		const engine::core::Vector2 &extent,
		engine::render::View &view,
		engine::render::WorldViewFrame &frame,
		engine::render::WorldCameraFrame &camera,
		engine::render::InterfacePass &interface,
		Abort &&abort
	) {
		bool bound = false;
		const auto status = universe.Enter(world, [&](engine::ecs::Store &store) {
			auto worldBinding = binding;
			worldBinding.Identity = store.Identity();
			bound =
				BindNamedCapturePresentation(store, worldBinding, extent, view, frame, camera, interface, [] {
				});
		});
		if (status == engine::world::WorldStatus::Ok && bound) return true;
		abort();
		return false;
	}
}
