#pragma once

// Builds the one current-tick packet a paused data-factory world may publish.

#include <engine/core/types/CFrame.hpp>
#include <engine/render/WorldPresentation.hpp>
#include <engine/scene/Components.hpp>
#include <engine/world/World.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace client {
	class Compositor;

	// A copied paused-world packet whose geometry was accepted by the compositor.
	struct PausedDataFactoryPresentation {
		engine::core::CFrame Frame;
		engine::scene::Camera Camera;
		uint64_t Tick = 0;
		std::vector<engine::render::DataCaptureObjectLabel> ObjectLabels;
		std::vector<engine::render::DataCaptureSemanticLabel> SemanticLabels;
		std::vector<engine::render::DataCapturePartLabel> PartLabels;
		bool ObjectLabelsValid = true;
		bool SemanticLabelsValid = true;
		bool PartLabelsValid = true;
	};

	// Rebuilds and publishes a paused world's current-tick draw list without
	// running its full presentation phases. Labels return only with an accepted
	// publication, so they cannot describe a retained compositor frame.
	std::optional<PausedDataFactoryPresentation> PublishPausedDataFactoryPresentation(
		engine::ecs::Store &store, Compositor &views, engine::world::WorldId world
	);
}
