// Client-local control providers that borrow renderer and data-factory host state.

#include "ControlHooks.hpp"

#include <engine/control/features/VisibilityObservation.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/VisibilityObservation.hpp>

namespace client {

	engine::control::features::VisibilitySnapshotReply
	VisibilityObservationSnapshot(const engine::render::Renderer &renderer) {
		const engine::render::VisibilitySnapshot snapshot = renderer.Visibility();
		engine::control::features::VisibilitySnapshotReply reply;
		reply.Frame = snapshot.Frame;
		reply.ViewSlot = snapshot.ViewSlot;
		reply.World = std::string(snapshot.World.Text());
		reply.Valid = snapshot.Valid;
		reply.Dropped = snapshot.Dropped;
		reply.DroppedExact = snapshot.DroppedExact;
		for (const engine::render::VisibilityObservation &row : snapshot.Observations) {
			reply.Observations.push_back(
				{std::string(row.World.Text()),
				 row.Entity,
				 engine::render::Describe(row.State),
				 engine::render::Describe(row.Cause)}
			);
		}
		return reply;
	}

	engine::control::HookLease ActivateVisibilityObservationHook(
		engine::control::HookRegistry &hooks,
		std::function<engine::control::features::VisibilitySnapshotReply()> snapshot,
		std::string &failure
	) {
		if (!snapshot) {
			failure = "visibility snapshot provider is required";
			return {};
		}
		return hooks.Activate(
			{
				.Id = "client.visibility-observation",
				.Revision = "v1",
				.Purpose = "Reads the most recent completed renderer visibility snapshot.",
				.Dependencies = {},
				.Limits = {},
			},
			[snapshot = std::move(snapshot)](engine::control::HookRegistration &registration) {
				registration.Add(engine::control::features::VisibilityObservationsTool(snapshot));
			},
			failure
		);
	}

	engine::control::HookLease ActivateTemporalSampleHook(
		engine::control::HookRegistry &hooks,
		engine::world::Universe &universe,
		engine::world::DataFactorySession &session,
		std::string &failure
	) {
		return hooks.Activate(
			{
				.Id = "client.temporal-sample",
				.Revision = "v1",
				.Purpose = "Reads retained camera and object poses from a data-factory snapshot.",
				.Dependencies = {},
				.Limits = {},
			},
			[&universe, &session](engine::control::HookRegistration &registration) {
				registration.Add(engine::control::TemporalSampleTool(universe, session));
			},
			failure
		);
	}

	engine::control::HookLease ActivateRigExportHook(
		engine::control::HookRegistry &hooks,
		engine::world::Universe &universe,
		engine::world::DataFactorySession &session,
		std::string &failure
	) {
		return hooks.Activate(
			{
				.Id = "client.rig-export",
				.Revision = "v1",
				.Purpose = "Exports bounded data-rig records from a data-factory world.",
				.Dependencies = {},
				.Limits = {},
			},
			[&universe, &session](engine::control::HookRegistration &registration) {
				registration.Add(engine::control::RigExportTool(universe, &session));
			},
			failure
		);
	}

	engine::control::HookLease ActivateDataAudioObservationHook(
		engine::control::HookRegistry &hooks,
		engine::world::Universe &universe,
		std::shared_ptr<engine::script::DataAudioObservationBridge> bridge,
		engine::world::DataFactorySession &session,
		std::string &failure
	) {
		if (!bridge) {
			failure = "audio observation bridge is required";
			return {};
		}
		return hooks.Activate(
			{
				.Id = "client.audio-observation",
				.Revision = "v1",
				.Purpose = "Reads copied audio observations and bounded waveform chunks.",
				.Dependencies = {},
				.Limits = {},
			},
			[&universe,
			 bridge = std::move(bridge),
			 &session](engine::control::HookRegistration &registration) {
				auto rows = engine::control::DataAudioObservationTools(universe, bridge, &session);
				registration.Add(std::move(rows.Metadata));
				registration.Add(std::move(rows.Observation));
				registration.Add(std::move(rows.Waveform));
			},
			failure
		);
	}
}
