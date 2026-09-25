#pragma once

// Recording helpers the transfer service calls at its declared points. Each one
// is a no-op in a world without a portal observation log.

#include <engine/ecs/Store.hpp>
#include <engine/script/PortalObservation.hpp>

#include <string_view>

namespace engine::script::portal_observation {

	// Registers the log resource type. Idempotent.
	void Register();

	// Creates an empty log unless one exists.
	void Prepare(ecs::Store &store);

	// Fills a stamp for `subject`, whose trace comes from its own or `root`'s
	// `scene::ObservationTrace`, else the world default. Null without a log.
	PortalObservationLog *Begin(ecs::Store &store, ecs::Entity subject, ecs::Entity root, PortalObservationStamp &stamp);

	// Records a handoff stage change with a peer world.
	void Handoff(
		ecs::Store &store,
		ecs::Entity subject,
		ecs::Entity root,
		PortalHandoffEvent event,
		uint64_t transfer,
		uint64_t inputTick,
		std::string_view peer
	);

	// Records one input applied, scheduled or skipped.
	void Input(
		ecs::Store &store,
		ecs::Entity subject,
		PortalInputRoute route,
		uint64_t inputTick,
		const core::Vector3 &direction
	);
}
