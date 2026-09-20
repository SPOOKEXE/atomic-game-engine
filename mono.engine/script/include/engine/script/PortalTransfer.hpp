#pragma once

#include <engine/core/types/CFrame.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/scene/PortalTransfer.hpp>
#include <engine/scene/SurfaceCameras.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace engine::ecs {
	class Store;
	class Scheduler;
}

namespace engine::core {
	class ByteWriter;
	class ByteReader;
}

namespace engine::script {

	// arch-crossing: a receipt names its origin and incarnation, never a local entity.
	struct PortalTransferId {
		// Stable source-world name that owns the transfer.
		std::string SourceWorld;
		// Source-world incarnation that rejects stale receipts.
		uint64_t SourceIncarnation = 0;
		// Per-incarnation sequence assigned by the source transfer service.
		uint64_t Sequence = 0;
		// Compares the complete cross-world transfer identity.
		bool operator==(const PortalTransferId &) const = default;
	};

	// A portal transfer stage value.
	enum class PortalTransferStage : uint8_t { Preparing, Committing, Committed, Refused, Cancelling };

	// Completed destination state in destination coordinates. The enclosing receipt owns identity.
	struct PortalTransferMotion {
		// Destination-world incarnation that completed the transfer.
		uint64_t DestinationIncarnation = 0;
		// Destination simulation tick that produced this motion sample.
		uint64_t DestinationTick = 0;
		// Source input tick incorporated into the destination state.
		uint64_t InputTick = 0;
		// World-space transform.
		core::CFrame Frame;
		// World-space linear velocity at the completed destination tick.
		core::Vector3 Linear;
		// World-space angular velocity at the completed destination tick.
		core::Vector3 Angular;
		// Walk speed.
		float WalkSpeed = 0;
		// Jump speed.
		float JumpSpeed = 0;
		// Whether the destination character was grounded at the sampled tick.
		bool Grounded = false;
		// Completed world time, independent of input coalescing and transport delay.
		double SimulationSeconds = 0;
	};

	// Shared bounded codec for host-bus and authenticated play-session samples.
	bool ValidPortalTransferMotion(const PortalTransferMotion &motion);
	// Encodes a bounded completed-motion acknowledgement.
	bool WritePortalTransferMotion(core::ByteWriter &writer, const PortalTransferMotion &motion);
	// Reads portal transfer motion.
	bool ReadPortalTransferMotion(core::ByteReader &reader, PortalTransferMotion &motion);

	// Cross-world receipt that tracks a portal transfer through its lifecycle.
	struct PortalTransferReceipt {
		// Stable identifier.
		PortalTransferId Id;
		// Destination world.
		std::string DestinationWorld;
		// Current lifecycle stage of the transfer receipt.
		PortalTransferStage Stage = PortalTransferStage::Preparing;
		// Seam transform that maps source state into the destination world.
		scene::SeamTransform Through;
		// Human-readable refusal or lifecycle diagnostic.
		std::string Diagnostic;
		// Record kind discriminator.
		scene::PortalBodyKind Kind = scene::PortalBodyKind::Player;
		// Destination control assignment, not a completed physics pose acknowledgement.
		uint64_t AcknowledgedInputTick = 0;
		// Optional completed destination motion acknowledgement.
		std::optional<PortalTransferMotion> Motion;
	};

	// Registers portal transfer components.
	void RegisterPortalTransferComponents();
	// Host control must assign a fresh nonzero incarnation on world recreation.
	// A snapshot restore retains the stored incarnation and must not reconfigure.
	bool
	ConfigurePortalTransfers(ecs::Store &store, uint64_t incarnation, bool requirePlayerAdmission = false);
	// Host admission names the exact destination incarnation before source retirement.
	bool
	AdmitPortalPlayerTransfer(ecs::Store &store, const PortalTransferId &id, uint64_t destinationIncarnation);
	// Keep source authority paused until the destination acknowledges reservation removal.
	bool
	CancelPortalPlayerTransfer(ecs::Store &store, const PortalTransferId &id, std::string_view diagnostic);
	// Returns the current source-world incarnation used for new receipt ids.
	uint64_t PortalTransferIncarnation(const ecs::Store &store);
	// Starts a player transfer after capturing its bounded portal body.
	bool BeginPortalTransfer(
		ecs::Store &store,
		ecs::Entity player,
		std::string_view destination,
		const scene::SeamTransform &through,
		PortalTransferId &out,
		std::string &failure,
		std::optional<scene::PortalBodySweep> sweep = std::nullopt
	);
	// Starts a movable-object transfer after capturing its bounded subtree.
	bool BeginPortalObjectTransfer(
		ecs::Store &store,
		ecs::Entity object,
		std::string_view destination,
		const scene::SeamTransform &through,
		PortalTransferId &out,
		std::string &failure,
		std::optional<scene::PortalBodySweep> sweep = std::nullopt
	);
	// Advances pending transfer stages at the world's tick boundary.
	void PumpPortalTransfers(ecs::Store &store);
	// Registers portal transfer systems.
	void RegisterPortalTransferSystems(ecs::Scheduler &scheduler);
	// Returns a snapshot of locally retained transfer receipts.
	std::vector<PortalTransferReceipt> PortalTransferReceipts(const ecs::Store &store);
	// Transfers a player through a portal.
	std::optional<PortalTransferReceipt>
	PortalTransferOfPlayer(const ecs::Store &store, ecs::Entity sourcePlayer);
	// Resolves the exact committed receipt inside the destination store only.
	ecs::Entity PortalTransferPlayer(const ecs::Store &store, const PortalTransferId &id);
	// Queues validated source-space movement for this player's transfer. Delivery
	// is acknowledged after destination control assignment; no source entity crosses the bus.
	// inputTick preserves the client clock; zero denotes unstamped host/local control.
	// stepSeconds is the duration of that clock's tick; zero denotes unknown timing.
	bool ForwardPortalPlayerMove(
		ecs::Store &store,
		ecs::Entity sourcePlayer,
		const core::Vector3 &direction,
		bool jump,
		uint64_t inputTick = 0,
		double stepSeconds = 0
	);
	// Native destination input takes over permanently from the previous host.
	void ClosePortalPlayerMoveForwarding(ecs::Store &store, ecs::Entity destinationPlayer);

	// A portal input disposition value.
	enum class PortalInputDisposition : uint8_t { Immediate, Queued, Refused };
	// Queues timed native input against the last forwarded physics assignment.
	// Untimed local control cancels pending input and applies immediately.
	PortalInputDisposition SchedulePortalPlayerMove(
		ecs::Store &store,
		ecs::Entity player,
		const core::Vector3 &direction,
		bool jump,
		uint64_t inputTick,
		double stepSeconds
	);
	// Actual applied input for a live rig with a forwarded or native portal clock.
	std::optional<uint64_t> AppliedPortalPlayerInput(const ecs::Store &store, ecs::Entity player);
	// Transfers an object through a portal.
	std::optional<PortalTransferReceipt>
	PortalTransferOfObject(const ecs::Store &store, ecs::Entity sourceObject);
	// Returns the destination object entity for a committed receipt, when local.
	ecs::Entity PortalTransferObject(const ecs::Store &store, const PortalTransferId &id);

}
