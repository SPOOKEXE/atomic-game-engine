#pragma once

// Which camera a world is looked at from, and the matrices that follow from it.
//
// `Camera` is a component because a world may hold several of them — a
// spectator, a cutscene, a security monitor — and under the instance model a
// camera is an instance, which is a row. This is the other half: a resource
// naming the live one, so "where is the camera" is a lookup rather than a walk
// over every camera row asking which is in charge.
//
// **This holds no `render::Camera` and must never hold one.** That type is
// `client` tier: a server-tier host resolves a view for a hosted world and
// cannot name it, and reaching for it would put presentation in the type a
// headless world writes. What crosses is a `CFrame`, three floats and the
// matrices below — all of which are arithmetic, not device state.
//
// The matrices are cached rather than derived at every read because the
// consumers are a culling pass, a draw-list build and an overlay, and three
// passes recomputing one inverse per frame is three chances for two of them to
// disagree about which tick's camera they were looking at.
//
// @tier L7 · shared

#include <engine/core/types/CFrame.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/scene/Components.hpp>

#include <glm/mat4x4.hpp>

#include <cstdint>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {

	// A camera's view and projection, already resolved.
	//
	// Column-major and Vulkan-convention: Y-up clip space and depth mapped to
	// `0..1`, which is what `GLM_FORCE_DEPTH_ZERO_TO_ONE` in `core`'s build
	// pins for the whole engine. A projection built under a different setting
	// is a depth buffer that is subtly inverted, and it reads as a z-fighting
	// bug rather than as a matrix mistake.
	//
	// @since v0.4
	struct CameraMatrices {
		// World space to view space: the inverse of the camera's frame.
		glm::mat4 View{1.0f};

		// View space to clip space.
		glm::mat4 Projection{1.0f};

		// `Projection * View`, kept because every consumer wants the product
		// and a second multiply per pass is a second chance to get the order
		// backwards.
		glm::mat4 ViewProjection{1.0f};
	};

	// The world's live camera.
	//
	// A resource: there is one of it, and nothing iterates it.
	//
	// @since v0.4
	struct ActiveCamera {
		// The entity carrying the live `Camera` and `Transform`.
		//
		// A handle rather than a copy of the values, so there is exactly one
		// place a camera's field of view is written and no way for the live
		// camera to disagree with the row it came from.
		ecs::Entity Entity;

		// Width over height of whatever this world is being drawn into.
		//
		// Written by the consumer — a window, an offscreen target, a mirror's
		// texture — because a world has no idea how big anybody's screen is.
		// Kept here rather than passed to `ResolveActiveCamera` so that
		// resolving stays a plain `void(Store &)` the scheduler can register.
		float AspectRatio = 1.0f;

		// Explicit padding, so the object representation a snapshot writes
		// holds no uninitialised bytes.
		uint32_t Reserved = 0;

		// What `ResolveActiveCamera` last computed. Identity until it runs.
		//
		// Braced rather than left bare so that "identity until it runs" is what
		// the type says and not only what this comment says. An aggregate
		// initialiser naming the entity and the aspect ratio — which is every
		// caller — leaves this member behind, and a member with no default is
		// one `-Wmissing-field-initializers` is right to call out.
		CameraMatrices Matrices{};
	};

	// Builds the matrices for one camera at one placement.
	//
	// The one place the engine decides what a camera's matrices are. Every
	// consumer that built its own would be a consumer that can disagree about
	// handedness, clip depth or the order of the product.
	//
	// A zero or negative `aspectRatio` yields identity matrices rather than a
	// projection full of infinities: a window reports zero height while it is
	// minimised, and that is an ordinary frame rather than an error.
	//
	// @param frame       The camera's world-space placement.
	// @param camera      Field of view and clipping distances.
	// @param aspectRatio Width over height of the target.
	// @return The resolved view, projection and their product.
	CameraMatrices ResolveCamera(const core::CFrame &frame, const Camera &camera, float aspectRatio);

	// Refreshes the world's `ActiveCamera` matrices from the row it names.
	//
	// A `void(Store &)` and nothing else, so it registers as an ordinary
	// system — which is the reason `AspectRatio` is a field above rather than a
	// second argument here.
	//
	// Leaves the matrices exactly as they were when there is no `ActiveCamera`
	// resource, when the entity it names is dead, or when that entity carries
	// no `Camera` or no `Transform`. A stale view is a frame drawn from where
	// the camera was; clearing to identity would be a frame drawn from inside
	// the origin with nothing in it, which looks like a renderer fault and
	// sends the search to the wrong module.
	//
	// @param store The world to resolve in.
	void ResolveActiveCamera(ecs::Store &store);
}
